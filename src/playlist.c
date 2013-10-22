#include "file.h"
#include "queue.h"

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

// How many ms to wait to check whether anything is added to the playlist yet.
// also when the buffers are full
#define NOOP_DELAY 5

typedef struct GrooveSinkPrivate {
    GrooveQueue *audioq;
    int audioq_buf_count;
    int audioq_size; // in bytes
    int min_audioq_size; // in bytes
} GrooveSinkPrivate;

typedef struct SinkStack {
    GrooveSink *sink;
    struct SinkStack *next;
} SinkStack;

typedef struct SinkMap {
    SinkStack *stack_head;
    AVFilterContext *aformat_ctx;
    AVFilterContext *abuffersink_ctx;
    struct SinkMap *next;
} SinkMap;

typedef struct GroovePlaylistPrivate {
    SDL_Thread *thread_id;
    int abort_request;

    AVPacket audio_pkt_temp;
    AVFrame *in_frame;
    int paused;
    int last_paused;

    int in_sample_rate;
    uint64_t in_channel_layout;
    enum AVSampleFormat in_sample_fmt;
    AVRational in_time_base;

    char strbuf[512];
    AVFilterGraph *filter_graph;
    AVFilterContext *abuffer_ctx;
    AVFilterContext *volume_ctx;
    AVFilterContext *asplit_ctx;

    // this mutex applies to the variables in this block
    SDL_mutex *decode_head_mutex;
    // pointer to current playlist item being decoded
    GroovePlaylistItem *decode_head;
    // desired volume for the volume filter
    double volume;
    // set to 1 to trigger a rebuild
    int rebuild_filter_graph_flag;
    // map audio format to list of sinks
    // for each map entry, use the first sink in the stack as the example
    // of the audio format in that stack
    SinkMap *sink_map;
    int sink_map_count;

    // the value for volume that was used to construct the filter graph
    double filter_volume;

    // only touched by decode_thread, tells whether we have sent the end_of_q_sentinel
    int sent_end_of_q;

    GroovePlaylistItem *purge_item; // set temporarily
} GroovePlaylistPrivate;

typedef struct GrooveBufferPrivate {
    AVFrame *frame;
    int ref_count;
    SDL_mutex *mutex;
} GrooveBufferPrivate;

// this is used to tell the difference between a buffer underrun
// and the end of the playlist.
static GrooveBuffer *end_of_q_sentinel = NULL;

static int frame_size(const AVFrame *frame) {
    return av_get_channel_layout_nb_channels(frame->channel_layout) *
        av_get_bytes_per_sample(frame->format) *
        frame->nb_samples;
}

static GrooveBuffer * frame_to_groove_buffer(GroovePlaylist *playlist, GrooveSink *sink, AVFrame *frame) {
    GrooveBuffer *buffer = av_mallocz(sizeof(GrooveBuffer));
    GrooveBufferPrivate *b = av_mallocz(sizeof(GrooveBufferPrivate));

    if (!buffer || !b) {
        av_free(buffer);
        av_free(b);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate buffer: out of memory");
        return NULL;
    }

    buffer->internals = b;

    b->mutex = SDL_CreateMutex();

    if (!b->mutex) {
        av_free(buffer);
        av_free(b);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex: out of memory\n");
        return NULL;
    }

    GroovePlaylistPrivate *p = playlist->internals;
    GrooveFile *file = p->decode_head->file;
    GrooveFilePrivate *f = file->internals;

    buffer->item = p->decode_head;
    buffer->pos = f->audio_clock;

    buffer->data = frame->extended_data;
    buffer->frame_count = frame->nb_samples;
    buffer->format.channel_layout = frame->channel_layout;
    buffer->format.sample_fmt = frame->format;
    buffer->format.sample_rate = frame->sample_rate;
    buffer->size = frame_size(frame);

    b->frame = frame;

    return buffer;
}


// decode one audio packet and return its uncompressed size
static int audio_decode_frame(GroovePlaylist *playlist, GrooveFile *file) {
    GroovePlaylistPrivate * p = playlist->internals;
    GrooveFilePrivate * f = file->internals;

    AVPacket *pkt = &f->audio_pkt;
    AVCodecContext *dec = f->audio_st->codec;

    AVPacket *pkt_temp = &p->audio_pkt_temp;
    *pkt_temp = *pkt;

    // update the audio clock with the pts if we can
    if (pkt->pts != AV_NOPTS_VALUE)
        f->audio_clock = av_q2d(f->audio_st->time_base) * pkt->pts;

    int max_data_size = 0;
    int len1, got_frame;
    int new_packet = 1;
    AVFrame *in_frame = p->in_frame;

    // NOTE: the audio packet can contain several frames
    while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
        avcodec_get_frame_defaults(in_frame);
        new_packet = 0;

        len1 = avcodec_decode_audio4(dec, in_frame, &got_frame, pkt_temp);
        if (len1 < 0) {
            // if error, we skip the frame
            pkt_temp->size = 0;
            return -1;
        }

        pkt_temp->data += len1;
        pkt_temp->size -= len1;

        if (!got_frame) {
            // stop sending empty packets if the decoder is finished
            if (!pkt_temp->data && dec->codec->capabilities & CODEC_CAP_DELAY)
                return 0;
            continue;
        }

        // push the audio data from decoded frame into the filtergraph
        int err = av_buffersrc_write_frame(p->abuffer_ctx, in_frame);
        if (err < 0) {
            av_strerror(err, p->strbuf, sizeof(p->strbuf));
            av_log(NULL, AV_LOG_ERROR, "error writing frame to buffersrc: %s\n",
                    p->strbuf);
            return -1;
        }

        // for each data format in the sink map, pull filtered audio from its
        // buffersink, turn it into a GrooveBuffer and then increment the ref
        // count for each sink in that stack.
        SinkMap *map_item = p->sink_map;
        double clock_adjustment = 0;
        while (map_item) {
            GrooveSink *example_sink = map_item->stack_head->sink;
            int data_size = 0;
            for (;;) {
                AVFrame *oframe = av_frame_alloc();
                int err = av_buffersink_get_frame(map_item->abuffersink_ctx, oframe);
                if (err == AVERROR_EOF || err == AVERROR(EAGAIN))
                    break;
                if (err < 0) {
                    av_log(NULL, AV_LOG_ERROR, "error reading buffer from buffersink\n");
                    return -1;
                }
                GrooveBuffer *buffer = frame_to_groove_buffer(playlist, example_sink, oframe);
                if (!buffer)
                    return -1;
                data_size += buffer->size;
                SinkStack *stack_item = map_item->stack_head;
                while (stack_item) {
                    GrooveSink *sink = stack_item->sink;
                    GrooveSinkPrivate *s = sink->internals;
                    if (groove_queue_put(s->audioq, buffer) < 0) {
                        av_log(NULL, AV_LOG_ERROR, "unable to put buffer in queue\n");
                    } else {
                        groove_buffer_ref(buffer);
                    }
                    stack_item = stack_item->next;
                }
                // do a ref/unref to trigger cleanup if there were no refs
                groove_buffer_ref(buffer);
                groove_buffer_unref(buffer);
            }
            if (data_size > max_data_size) {
                max_data_size = data_size;
                clock_adjustment = data_size / (double)example_sink->bytes_per_sec;
            }
            map_item = map_item->next;
        }

        // if no pts, then estimate it
        if (pkt->pts == AV_NOPTS_VALUE)
            f->audio_clock += clock_adjustment;
        return max_data_size;
    }
    return max_data_size;
}

// abuffer -> volume -> asplit for each audio format
//                     -> aformat -> abuffersink
static int init_filter_graph(GroovePlaylist *playlist, GrooveFile *file) {
    GroovePlaylistPrivate *p = playlist->internals;
    GrooveFilePrivate *f = file->internals;

    // destruct old graph
    avfilter_graph_free(&p->filter_graph);

    // create new graph
    p->filter_graph = avfilter_graph_alloc();
    if (!p->filter_graph) {
        av_log(NULL, AV_LOG_ERROR, "unable to create filter graph: out of memory\n");
        return -1;
    }

    AVFilter *abuffer = avfilter_get_by_name("abuffer");
    AVFilter *volume = avfilter_get_by_name("volume");
    AVFilter *asplit = avfilter_get_by_name("asplit");
    AVFilter *aformat = avfilter_get_by_name("aformat");
    AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    int err;
    // create abuffer filter
    AVCodecContext *avctx = f->audio_st->codec;
    AVRational time_base = f->audio_st->time_base;
    snprintf(p->strbuf, sizeof(p->strbuf),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64, 
            time_base.num, time_base.den, avctx->sample_rate,
            av_get_sample_fmt_name(avctx->sample_fmt),
            avctx->channel_layout);
    av_log(NULL, AV_LOG_INFO, "abuffer: %s\n", p->strbuf);
    // save these values so we can compare later and check
    // whether we have to reconstruct the graph
    p->in_sample_rate = avctx->sample_rate;
    p->in_channel_layout = avctx->channel_layout;
    p->in_sample_fmt = avctx->sample_fmt;
    p->in_time_base = time_base;
    err = avfilter_graph_create_filter(&p->abuffer_ctx, abuffer,
            NULL, p->strbuf, NULL, p->filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error initializing abuffer filter\n");
        return err;
    }
    // as we create filters, this points the next source to link to
    AVFilterContext *audio_src_ctx = p->abuffer_ctx;

    // save the volume value so we can compare later and check
    // whether we have to reconstruct the graph
    p->filter_volume = p->volume;
    // if volume is not equal to 1.0, create volume filter
    double vol = p->volume;
    if (vol > 1.0) vol = 1.0;
    if (vol < 0.0) vol = 0.0;
    if (vol == 1.0) {
        p->volume_ctx = NULL;
    } else {
        snprintf(p->strbuf, sizeof(p->strbuf), "volume=%f", vol);
        av_log(NULL, AV_LOG_INFO, "volume: %s\n", p->strbuf);
        err = avfilter_graph_create_filter(&p->volume_ctx, volume, NULL,
                p->strbuf, NULL, p->filter_graph);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "error initializing volume filter\n");
            return err;
        }
        err = avfilter_link(audio_src_ctx, 0, p->volume_ctx, 0);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to link filters\n");
            return err;
        }
        audio_src_ctx = p->volume_ctx;
    }

    // if only one sink, no need for asplit
    if (p->sink_map_count < 2) {
        p->asplit_ctx = NULL;
    } else {
        snprintf(p->strbuf, sizeof(p->strbuf), "%d", p->sink_map_count);
        av_log(NULL, AV_LOG_INFO, "asplit: %s\n", p->strbuf);
        err = avfilter_graph_create_filter(&p->asplit_ctx, asplit,
                NULL, p->strbuf, NULL, p->filter_graph);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to create asplit filter\n");
            return err;
        }
        err = avfilter_link(audio_src_ctx, 0, p->asplit_ctx, 0);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to link to asplit\n");
            return err;
        }
        audio_src_ctx = p->asplit_ctx;
    }

    // for each audio format, create aformat and abuffersink filters
    SinkMap *map_item = p->sink_map;
    int pad_index = 0;
    while (map_item) {
        GrooveSink *example_sink = map_item->stack_head->sink;
        GrooveAudioFormat *audio_format = &example_sink->audio_format;

        // create aformat filter
        snprintf(p->strbuf, sizeof(p->strbuf),
                "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%"PRIx64,
                av_get_sample_fmt_name(audio_format->sample_fmt),
                audio_format->sample_rate, audio_format->channel_layout);
        av_log(NULL, AV_LOG_INFO, "aformat: %s\n", p->strbuf);
        err = avfilter_graph_create_filter(&map_item->aformat_ctx, aformat,
                NULL, p->strbuf, NULL, p->filter_graph);
        if (err < 0) {
            av_strerror(err, p->strbuf, sizeof(p->strbuf));
            av_log(NULL, AV_LOG_ERROR, "unable to create aformat filter: %s\n",
                    p->strbuf);
            return err;
        }
        err = avfilter_link(audio_src_ctx, pad_index, map_item->aformat_ctx, 0);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to link filters\n");
            return err;
        }

        // create abuffersink filter
        err = avfilter_graph_create_filter(&map_item->abuffersink_ctx, abuffersink,
                NULL, NULL, NULL, p->filter_graph);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to create abuffersink filter\n");
            return err;
        }
        err = avfilter_link(map_item->aformat_ctx, 0, map_item->abuffersink_ctx, 0);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to link filters\n");
            return err;
        }

        pad_index += 1;
        map_item = map_item->next;
    }

    err = avfilter_graph_config(p->filter_graph, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error configuring the filter graph\n");
        return err;
    }

    p->rebuild_filter_graph_flag = 0;

    return 0;
}

static int maybe_init_filter_graph(GroovePlaylist *playlist, GrooveFile *file) {
    GroovePlaylistPrivate *p = playlist->internals;
    GrooveFilePrivate *f = file->internals;
    AVCodecContext *avctx = f->audio_st->codec;
    AVRational time_base = f->audio_st->time_base;

    // if the input format stuff has changed, then we need to re-build the graph
    if (!p->filter_graph || p->rebuild_filter_graph_flag ||
        p->in_sample_rate != avctx->sample_rate ||
        p->in_channel_layout != avctx->channel_layout ||
        p->in_sample_fmt != avctx->sample_fmt ||
        p->in_time_base.num != time_base.num ||
        p->in_time_base.den != time_base.den ||
        p->volume != p->filter_volume)
    {
        return init_filter_graph(playlist, file);
    }

    return 0;
}

static int every_sink(GroovePlaylist *playlist, int (*func)(GrooveSink *), int default_value) {
    GroovePlaylistPrivate *p = playlist->internals;
    SinkMap *map_item = p->sink_map;
    while (map_item) {
        SinkStack *stack_item = map_item->stack_head;
        while (stack_item) {
            GrooveSink *sink = stack_item->sink;
            int value = func(sink);
            if (value != default_value)
                return value;
            stack_item = stack_item->next;
        }
        map_item = map_item->next;
    }
    return default_value;
}

static int sink_is_full(GrooveSink *sink) {
    GrooveSinkPrivate *s = sink->internals;
    return s->audioq_size >= s->min_audioq_size;
}

static int every_sink_full(GroovePlaylist *playlist) {
    return every_sink(playlist, sink_is_full, 1);
}

static int sink_signal_end(GrooveSink *sink) {
    GrooveSinkPrivate *s = sink->internals;
    groove_queue_put(s->audioq, end_of_q_sentinel);
    return 0;
}

static void every_sink_signal_end(GroovePlaylist *playlist) {
    every_sink(playlist, sink_signal_end, 0);
}

static int sink_flush(GrooveSink *sink) {
    GrooveSinkPrivate *s = sink->internals;

    groove_queue_flush(s->audioq);
    if (sink->flush)
        sink->flush(sink);

    return 0;
}

static void every_sink_flush(GroovePlaylist *playlist) {
    every_sink(playlist, sink_flush, 0);
}

static int decode_one_frame(GroovePlaylist *playlist, GrooveFile *file) {
    GroovePlaylistPrivate *p = playlist->internals;
    GrooveFilePrivate * f = file->internals;
    AVPacket *pkt = &f->audio_pkt;

    // might need to rebuild the filter graph if certain things changed
    if (maybe_init_filter_graph(playlist, file) < 0)
        return -1;

    // abort_request is set if we are destroying the file
    if (f->abort_request)
        return -1;

    // handle pause requests
    // only read p->paused once so that we don't need a mutex
    int paused = p->paused;
    if (paused != p->last_paused) {
        p->last_paused = paused;
        if (paused) {
            av_read_pause(f->ic);
        } else {
            av_read_play(f->ic);
        }
    }

    // handle seek requests
    SDL_LockMutex(f->seek_mutex);
    if (f->seek_pos >= 0) {
        if (av_seek_frame(f->ic, f->audio_stream_index, f->seek_pos, 0) < 0) {
            av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", f->ic->filename);
        } else if (f->seek_flush) {
            every_sink_flush(playlist);
        }
        avcodec_flush_buffers(f->audio_st->codec);
        f->seek_pos = -1;
        f->eof = 0;
    }
    SDL_UnlockMutex(f->seek_mutex);

    if (f->eof) {
        if (f->audio_st->codec->codec->capabilities & CODEC_CAP_DELAY) {
            av_init_packet(pkt);
            pkt->data = NULL;
            pkt->size = 0;
            pkt->stream_index = f->audio_stream_index;
            if (audio_decode_frame(playlist, file) > 0) {
                // keep flushing
                return 0;
            }
        }
        // this file is complete. move on
        return -1;
    }
    int err = av_read_frame(f->ic, pkt);
    if (err < 0) {
        // treat all errors as EOF, but log non-EOF errors.
        if (err != AVERROR_EOF) {
            av_log(NULL, AV_LOG_WARNING, "error reading frames\n");
        }
        f->eof = 1;
        return 0;
    }
    if (pkt->stream_index != f->audio_stream_index) {
        // we're only interested in the One True Audio Stream
        av_free_packet(pkt);
        return 0;
    }
    audio_decode_frame(playlist, file);
    av_free_packet(pkt);
    return 0;
}

static void audioq_put(GrooveQueue *queue, void *obj) {
    GrooveBuffer *buffer = obj;
    GrooveSink *sink = queue->context;
    GrooveSinkPrivate *s = sink->internals;
    if (buffer == end_of_q_sentinel)
        return;
    s->audioq_buf_count += 1;
    s->audioq_size += buffer->size;
}

static void audioq_get(GrooveQueue *queue, void *obj) {
    GrooveBuffer *buffer = obj;
    GrooveSink *sink = queue->context;
    GrooveSinkPrivate *s = sink->internals;
    if (buffer == end_of_q_sentinel)
        return;
    s->audioq_buf_count -= 1;
    s->audioq_size -= buffer->size;
}

static void audioq_cleanup(GrooveQueue *queue, void *obj) {
    GrooveBuffer *buffer = obj;
    GrooveSink *sink = queue->context;
    GrooveSinkPrivate *s = sink->internals;
    if (buffer == end_of_q_sentinel)
        return;
    s->audioq_buf_count -= 1;
    s->audioq_size -= buffer->size;
    groove_buffer_unref(buffer);
}

static int audioq_purge(GrooveQueue *queue, void *obj) {
    GrooveSink *sink = queue->context;
    GroovePlaylist *playlist = sink->playlist;
    GroovePlaylistPrivate *p = playlist->internals;
    GroovePlaylistItem *item = p->purge_item;
    GrooveBuffer *buffer = obj;
    return buffer->item == item;
}

// this thread is responsible for decoding and inserting buffers of decoded
// audio into each sink
static int decode_thread(void *arg) {
    GroovePlaylist *playlist = arg;
    GroovePlaylistPrivate *p = playlist->internals;

    while (!p->abort_request) {
        SDL_LockMutex(p->decode_head_mutex);

        // if we don't have anything to decode, wait until we do
        if (!p->decode_head) {
            if (!p->sent_end_of_q) {
                every_sink_signal_end(playlist);
                p->sent_end_of_q = 1;
            }
            SDL_UnlockMutex(p->decode_head_mutex);
            SDL_Delay(NOOP_DELAY);
            continue;
        }
        p->sent_end_of_q = 0;

        // if all sinks are filled up, no need to read more
        if (every_sink_full(playlist)) {
            SDL_UnlockMutex(p->decode_head_mutex);
            SDL_Delay(NOOP_DELAY);
            continue;
        }

        GrooveFile *file = p->decode_head->file;

        p->volume = p->decode_head->gain * playlist->volume;

        if (decode_one_frame(playlist, file) < 0) {
            p->decode_head = p->decode_head->next;
            // seek to beginning of next song
            if (p->decode_head) {
                GrooveFile *next_file = p->decode_head->file;
                GrooveFilePrivate *next_f = next_file->internals;
                SDL_LockMutex(next_f->seek_mutex);
                next_f->seek_pos = 0;
                next_f->seek_flush = 0;
                SDL_UnlockMutex(next_f->seek_mutex);
            }
        }

        SDL_UnlockMutex(p->decode_head_mutex);
    }

    return 0;
}

static int audio_formats_equal(const GrooveAudioFormat *a, const GrooveAudioFormat *b) {
    return a->sample_rate == b->sample_rate &&
        a->channel_layout == b->channel_layout &&
        a->sample_fmt == b->sample_fmt;
}

static int remove_sink_from_map(GrooveSink *sink) {
    GroovePlaylist *playlist = sink->playlist;
    GroovePlaylistPrivate *p = playlist->internals;

    SinkMap *map_item = p->sink_map;
    SinkMap *prev_map_item = NULL;
    while (map_item) {
        SinkMap *next_map_item = map_item->next;
        SinkStack *stack_item = map_item->stack_head;
        SinkStack *prev_stack_item = NULL;
        while (stack_item) {
            SinkStack *next_stack_item = stack_item->next;
            GrooveSink *item_sink = stack_item->sink;
            if (item_sink == sink) {
                av_free(stack_item);
                if (prev_stack_item) {
                    prev_stack_item->next = next_stack_item;
                } else if (next_stack_item) {
                    map_item->stack_head = next_stack_item;
                } else {
                    // the stack is empty; delete the map item
                    av_free(map_item);
                    p->sink_map_count -= 1;
                    if (prev_map_item) {
                        prev_map_item->next = next_map_item;
                    } else {
                        p->sink_map = next_map_item;
                    }
                }
                return 0;
            }

            prev_stack_item = stack_item;
            stack_item = next_stack_item;
        }
        prev_map_item = map_item;
        map_item = next_map_item;
    }

    return -1;
}

static int add_sink_to_map(GroovePlaylist *playlist, GrooveSink *sink) {
    GroovePlaylistPrivate *p = playlist->internals;

    SinkStack *stack_entry = av_mallocz(sizeof(SinkStack));
    stack_entry->sink = sink;

    if (!stack_entry)
        return -1;

    SinkMap *map_item = p->sink_map;
    while (map_item) {
        // if our sink matches the example sink from this map entry,
        // push our sink onto the stack and we're done
        GrooveSink *example_sink = map_item->stack_head->sink;
        if (audio_formats_equal(&example_sink->audio_format, &sink->audio_format)) {
            stack_entry->next = map_item->stack_head;
            map_item->stack_head = stack_entry;
            return 0;
        }
        map_item = map_item->next;
    }
    // we did not find somewhere to put it, so push it onto the stack.
    SinkMap *map_entry = av_mallocz(sizeof(SinkMap));
    map_entry->stack_head = stack_entry;
    if (!map_entry) {
        av_free(stack_entry);
        return -1;
    }
    if (p->sink_map) {
        map_entry->next = p->sink_map;
        p->sink_map = map_entry;
    } else {
        p->sink_map = map_entry;
    }
    p->sink_map_count += 1;
    return 0;
}

int groove_sink_detach(GrooveSink *sink) {
    GroovePlaylist *playlist = sink->playlist;

    if (!playlist)
        return -1;

    GrooveSinkPrivate *s = sink->internals;

    if (s->audioq) {
        groove_queue_abort(s->audioq);
        groove_queue_flush(s->audioq);
    }

    GroovePlaylistPrivate *p = playlist->internals;

    SDL_LockMutex(p->decode_head_mutex);
    int err = remove_sink_from_map(sink);
    SDL_UnlockMutex(p->decode_head_mutex);

    sink->playlist = NULL;

    return err;
}

int groove_sink_attach(GrooveSink *sink, GroovePlaylist *playlist) {
    GrooveSinkPrivate *s = sink->internals;

    // cache computed audio format stuff
    int channel_count = av_get_channel_layout_nb_channels(sink->audio_format.channel_layout);
    sink->bytes_per_sec = channel_count * sink->audio_format.sample_rate *
        av_get_bytes_per_sample(sink->audio_format.sample_fmt);

    s->min_audioq_size = sink->buffer_size * channel_count *
        av_get_bytes_per_sample(sink->audio_format.sample_fmt);
    av_log(NULL, AV_LOG_INFO, "audio queue size: %d\n", s->min_audioq_size);

    // add the sink to the entry that matches its audio format
    GroovePlaylistPrivate *p = playlist->internals;

    SDL_LockMutex(p->decode_head_mutex);
    int err = add_sink_to_map(playlist, sink);
    SDL_UnlockMutex(p->decode_head_mutex);

    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to attach device: out of memory\n");
        return err;
    }

    // in case we've called abort on the queue, reset
    groove_queue_reset(s->audioq);

    sink->playlist = playlist;

    return 0;
}

int groove_sink_get_buffer(GrooveSink *sink, GrooveBuffer **buffer, int block) {
    GrooveSinkPrivate *s = sink->internals;

    if (groove_queue_get(s->audioq, (void**)buffer, block) == 1) {
        if (*buffer == end_of_q_sentinel) {
            *buffer = NULL;
            return GROOVE_BUFFER_END;
        } else {
            return GROOVE_BUFFER_YES;
        }
    } else {
        *buffer = NULL;
        return GROOVE_BUFFER_NO;
    }
}

GroovePlaylist * groove_playlist_create() {
    GroovePlaylist * playlist = av_mallocz(sizeof(GroovePlaylist));
    GroovePlaylistPrivate * p = av_mallocz(sizeof(GroovePlaylistPrivate));
    if (!playlist || !p) {
        av_free(p);
        av_free(playlist);
        av_log(NULL, AV_LOG_ERROR, "Could not create playlist - out of memory\n");
        return NULL;
    }
    playlist->internals = p;

    // the one that the playlist can read
    playlist->volume = 1.0;
    // the other volume multiplied by the playlist item's gain
    p->volume = 1.0;

    p->decode_head_mutex = SDL_CreateMutex();
    if (!p->decode_head_mutex) {
        groove_playlist_destroy(playlist);
        av_log(NULL, AV_LOG_WARNING, "unable to create playlist: out of memory\n");
        return NULL;
    }

    p->in_frame = avcodec_alloc_frame();

    if (!p->in_frame) {
        groove_playlist_destroy(playlist);
        av_log(NULL, AV_LOG_ERROR, "unable to alloc frame: out of memory\n");
        return NULL;
    }

    p->thread_id = SDL_CreateThread(decode_thread, "decode", playlist);

    if (!p->thread_id) {
        groove_playlist_destroy(playlist);
        av_log(NULL, AV_LOG_ERROR, "Error creating playlist thread: Out of memory\n");
        return NULL;
    }

    return playlist;
}

void groove_playlist_destroy(GroovePlaylist *playlist) {
    groove_playlist_clear(playlist);

    GroovePlaylistPrivate * p = playlist->internals;

    // wait for decode thread to finish
    p->abort_request = 1;
    SDL_WaitThread(p->thread_id, NULL);

    every_sink(playlist, groove_sink_detach, 0);

    avfilter_graph_free(&p->filter_graph);
    avcodec_free_frame(&p->in_frame);

    if (p->decode_head_mutex)
        SDL_DestroyMutex(p->decode_head_mutex);

    av_free(p);
    av_free(playlist);
}

void groove_playlist_play(GroovePlaylist *playlist) {
    GroovePlaylistPrivate * p = playlist->internals;
    // no mutex needed for this boolean flag
    p->paused = 0;
}

void groove_playlist_pause(GroovePlaylist *playlist) {
    GroovePlaylistPrivate * p = playlist->internals;
    // no mutex needed for this boolean flag
    p->paused = 1;
}

void groove_playlist_seek(GroovePlaylist *playlist, GroovePlaylistItem *item, double seconds) {
    GrooveFile * file = item->file;
    GrooveFilePrivate * f = file->internals;

    int64_t ts = seconds * f->audio_st->time_base.den / f->audio_st->time_base.num;
    if (f->ic->start_time != AV_NOPTS_VALUE)
        ts += f->ic->start_time;

    GroovePlaylistPrivate *p = playlist->internals;

    SDL_LockMutex(p->decode_head_mutex);
    SDL_LockMutex(f->seek_mutex);

    f->seek_pos = ts;
    f->seek_flush = 1;
    p->decode_head = item;

    SDL_UnlockMutex(f->seek_mutex);
    SDL_UnlockMutex(p->decode_head_mutex);
}

GroovePlaylistItem * groove_playlist_insert(GroovePlaylist *playlist, GrooveFile *file,
        double gain, GroovePlaylistItem *next)
{
    GroovePlaylistItem * item = av_mallocz(sizeof(GroovePlaylistItem));
    if (!item)
        return NULL;

    item->file = file;
    item->next = next;
    item->gain = gain;

    GroovePlaylistPrivate *p = playlist->internals;
    GrooveFilePrivate *f = file->internals;

    // lock decode_head_mutex so that decode_head cannot point to a new item
    // while we're screwing around with the queue
    SDL_LockMutex(p->decode_head_mutex);

    if (next) {
        if (next->prev) {
            item->prev = next->prev;
            item->prev->next = item;
            next->prev = item;
        } else {
            playlist->head = item;
        }
    } else if (!playlist->head) {
        playlist->head = item;
        playlist->tail = item;

        p->decode_head = playlist->head;

        SDL_LockMutex(f->seek_mutex);
        f->seek_pos = 0;
        f->seek_flush = 0;
        SDL_UnlockMutex(f->seek_mutex);
    } else {
        item->prev = playlist->tail;
        playlist->tail->next = item;
        playlist->tail = item;
    }

    SDL_UnlockMutex(p->decode_head_mutex);
    return item;
}

static int purge_sink(GrooveSink *sink) {
    GrooveSinkPrivate *s = sink->internals;

    groove_queue_purge(s->audioq);

    GroovePlaylist *playlist = sink->playlist;
    GroovePlaylistPrivate *p = playlist->internals;
    GroovePlaylistItem *item = p->purge_item;

    if (sink->purge)
        sink->purge(sink, item);

    return 0;
}

void groove_playlist_remove(GroovePlaylist *playlist, GroovePlaylistItem *item) {
    GroovePlaylistPrivate *p = playlist->internals;

    SDL_LockMutex(p->decode_head_mutex);

    // if it's currently being played, seek to the next item
    if (item == p->decode_head) {
        p->decode_head = item->next;
    }

    if (item->prev) {
        item->prev->next = item->next;
    } else {
        playlist->head = item->next;
    }
    if (item->next) {
        item->next->prev = item->prev;
    } else {
        playlist->tail = item->prev;
    }

    // in each sink,
    // we must be absolutely sure to purge the audio buffer queue
    // of references to item before freeing it at the bottom of this method
    p->purge_item = item;
    every_sink(playlist, purge_sink, 0);
    p->purge_item = NULL;

    SDL_UnlockMutex(p->decode_head_mutex);

    av_free(item);
}

void groove_playlist_clear(GroovePlaylist *playlist) {
    GroovePlaylistItem * node = playlist->head;
    if (!node) return;
    while (node) {
        groove_playlist_remove(playlist, node);
        node = node->next;
    }
}

int groove_playlist_count(GroovePlaylist *playlist) {
    GroovePlaylistItem * node = playlist->head;
    int count = 0;
    while (node) {
        count += 1;
        node = node->next;
    }
    return count;
}

void groove_playlist_set_gain(GroovePlaylist *playlist, GroovePlaylistItem *item,
        double gain)
{
    GroovePlaylistPrivate *p = playlist->internals;

    SDL_LockMutex(p->decode_head_mutex);
    item->gain = gain;
    if (item == p->decode_head) {
        p->volume = playlist->volume * p->decode_head->gain;
    }
    SDL_UnlockMutex(p->decode_head_mutex);
}

void groove_playlist_position(GroovePlaylist *playlist, GroovePlaylistItem **item,
        double *seconds)
{
    GroovePlaylistPrivate *p = playlist->internals;

    SDL_LockMutex(p->decode_head_mutex);
    if (item)
        *item = p->decode_head;

    if (seconds && p->decode_head) {
        GrooveFile *file = p->decode_head->file;
        GrooveFilePrivate * f = file->internals;
        *seconds = f->audio_clock;
    }
    SDL_UnlockMutex(p->decode_head_mutex);
}

void groove_playlist_set_volume(GroovePlaylist *playlist, double volume) {
    GroovePlaylistPrivate *p = playlist->internals;

    SDL_LockMutex(p->decode_head_mutex);
    playlist->volume = volume;
    p->volume = p->decode_head ? volume * p->decode_head->gain : volume;
    SDL_UnlockMutex(p->decode_head_mutex);
}

int groove_playlist_playing(GroovePlaylist *playlist) {
    GroovePlaylistPrivate *p = playlist->internals;
    return !p->paused;
}

GrooveSink * groove_sink_create() {
    GrooveSink *sink = av_mallocz(sizeof(GrooveSink));
    GrooveSinkPrivate *s = av_mallocz(sizeof(GrooveSinkPrivate));

    if (!sink || !s) {
        av_free(sink);
        av_free(s);
        av_log(NULL, AV_LOG_ERROR, "could not create sink: out of memory\n");
        return NULL;
    }

    sink->internals = s;
    sink->buffer_size = 8192;

    s->audioq = groove_queue_create();

    if (!s->audioq) {
        groove_sink_destroy(sink);
        av_log(NULL, AV_LOG_ERROR, "could not create audio buffer: out of memory\n");
        return NULL;
    }

    s->audioq->context = sink;
    s->audioq->cleanup = audioq_cleanup;
    s->audioq->put = audioq_put;
    s->audioq->get = audioq_get;
    s->audioq->purge = audioq_purge;

    return sink;
}

void groove_sink_destroy(GrooveSink *sink) {
    if (!sink)
        return;

    GrooveSinkPrivate *s = sink->internals;

    if (s->audioq)
        groove_queue_destroy(s->audioq);

    av_free(s);
    av_free(sink);
}

void groove_buffer_ref(GrooveBuffer *buffer) {
    GrooveBufferPrivate *b = buffer->internals;

    SDL_LockMutex(b->mutex);
    b->ref_count += 1;
    SDL_UnlockMutex(b->mutex);
}

void groove_buffer_unref(GrooveBuffer *buffer) {
    if (!buffer)
        return;

    GrooveBufferPrivate *b = buffer->internals;

    SDL_LockMutex(b->mutex);
    b->ref_count -= 1;
    int free = b->ref_count == 0;
    SDL_UnlockMutex(b->mutex);

    if (free) {
        SDL_DestroyMutex(b->mutex);
        av_frame_free(&b->frame);
        av_free(b);
        av_free(buffer);
    }

}