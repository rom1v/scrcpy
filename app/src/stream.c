#include "stream.h"

#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <SDL2/SDL_assert.h>
#include <SDL2/SDL_events.h>
#include <SDL2/SDL_mutex.h>
#include <SDL2/SDL_thread.h>
#include <unistd.h>

#include "compat.h"
#include "config.h"
#include "buffer_util.h"
#include "decoder.h"
#include "events.h"
#include "lock_util.h"
#include "log.h"
#include "recorder.h"

#define BUFSIZE 0x10000

#define HEADER_SIZE 12
#define NO_PTS UINT64_C(-1)

static bool
stream_recv_packet(struct stream *stream, void **out_data, size_t *out_size) {
    struct receiver_state *state = &stream->receiver_state;

    // The video stream contains raw packets, without time information. When we
    // record, we retrieve the timestamps separately, from a "meta" header
    // added by the server before each raw packet.
    //
    // The "meta" header length is 12 bytes:
    // [. . . . . . . .|. . . .]. . . . . . . . . . . . . . . ...
    //  <-------------> <-----> <-----------------------------...
    //        PTS        packet        raw packet
    //                    size
    //
    // It is followed by <packet_size> bytes containing the packet/frame.

    uint8_t header[HEADER_SIZE];
    ssize_t r = net_recv_all(stream->socket, header, HEADER_SIZE);
    if (r < HEADER_SIZE) {
        return false;
    }

    state->pts = buffer_read64be(header);
    uint32_t len = buffer_read32be(&header[8]);
    SDL_assert(len);

    //LOGD("len = %d", (int) len);

    void *buf = av_malloc(len);
    if (!buf) {
        LOGE("Could not allocate packet buffer");
        return false;
    }

    *out_data = buf;
    *out_size = len;

    r = net_recv_all(stream->socket, buf, len);
    if (r < len) {
        return false;
    }

    return true;
}

static void
notify_stopped(void) {
    SDL_Event stop_event;
    stop_event.type = EVENT_STREAM_STOPPED;
    SDL_PushEvent(&stop_event);
}

static bool
process_packet(struct stream *stream, AVPacket *packet) {
    uint64_t pts = stream->receiver_state.pts;
    if (stream->decoder && !decoder_push(stream->decoder, packet)) {
        return false;
    }

    if (stream->recorder) {
        packet->pts = pts;
        packet->dts = pts;
        LOGD("recording with pts = %d", (int) packet->pts);

        if (!recorder_write(stream->recorder, packet)) {
            LOGE("Could not write frame to output file");
            return false;
        }
    }

    return true;
}

static bool
stream_parse(struct stream *stream, void *data, size_t len) {
    SDL_assert(stream->receiver_state.pts != NO_PTS);

    uint8_t *in_data = data;
    int in_len = len;
    uint8_t *out_data = NULL;
    int out_len = 0;
    if (stream->receiver_state.pts != NO_PTS) {
        while (in_len) {
            int r = av_parser_parse2(stream->parser, stream->codec_ctx,
                                     &out_data, &out_len, in_data, in_len,
                                     AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1);
            //LOGD("av_parser_parse2 %d returned %d", (int) len, r);
            in_data += r;
            in_len -= r;
            //LOGD("r = %d, out_len = %d", (int) r, (int) out_len);
            if (out_len) {
                //LOGD("pts = %d", (int) stream->receiver_state.pts);
                AVPacket packet;
                av_init_packet(&packet);
                packet.data = out_data;
                packet.size = out_len;

                if (stream->parser->key_frame == 1) {
                    packet.flags |= AV_PKT_FLAG_KEY;
                }

                bool ok = process_packet(stream, &packet);
                av_packet_unref(&packet);

                if (!ok) {
                    LOGE("Could not process packet");
                    return false;
                }
            }
        }
    }

    return true;
}

static int
run_stream(void *data) {
    struct stream *stream = data;

    AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        LOGE("H.264 decoder not found");
        goto end;
    }

    stream->codec_ctx = avcodec_alloc_context3(codec);
    if (!stream->codec_ctx) {
        LOGC("Could not allocate codec context");
        goto end;
    }

    if (stream->decoder && !decoder_open(stream->decoder, codec)) {
        LOGE("Could not open decoder");
        goto finally_free_codec_ctx;
    }

    if (stream->recorder && !recorder_open(stream->recorder, codec)) {
        LOGE("Could not open recorder");
        goto finally_close_decoder;
    }

    stream->parser = av_parser_init(AV_CODEC_ID_H264);
    // We must only pass complete frames to av_parser_parse2()!
    // This allow to reduce the latency by 1 frame
    stream->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

    for (;;) {
        void *data;
        size_t len;
        bool ok = stream_recv_packet(stream, &data, &len);
        if (!ok) {
            // end of stream
            break;
        }

        ok = stream_parse(stream, data, len);
        if (!ok) {
            // cannot process packet (error already logged)
            break;
        }

        av_free(data);
    }

    LOGD("End of frames");

finally_close_recorder:
    if (stream->recorder) {
        recorder_close(stream->recorder);
    }
finally_close_decoder:
    if (stream->decoder) {
        decoder_close(stream->decoder);
    }
finally_free_codec_ctx:
    avcodec_free_context(&stream->codec_ctx);
end:
    notify_stopped();
    return 0;
}

void
stream_init(struct stream *stream, socket_t socket,
            struct decoder *decoder, struct recorder *recorder) {
    stream->socket = socket;
    stream->decoder = decoder,
    stream->recorder = recorder;
    stream->receiver_state.pts = NO_PTS;
}

bool
stream_start(struct stream *stream) {
    LOGD("Starting stream thread");

    stream->thread = SDL_CreateThread(run_stream, "stream", stream);
    if (!stream->thread) {
        LOGC("Could not start stream thread");
        return false;
    }
    return true;
}

void
stream_stop(struct stream *stream) {
    if (stream->decoder) {
        decoder_interrupt(stream->decoder);
    }
}

void
stream_join(struct stream *stream) {
    SDL_WaitThread(stream->thread, NULL);
}
