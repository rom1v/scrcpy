#include "decoder.h"

#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <SDL2/SDL_events.h>
#include <unistd.h>

#include "events.h"
#include "recorder.h"
#include "video_buffer.h"
#include "util/buffer_util.h"
#include "util/log.h"

void
decoder_init(struct decoder *decoder, struct video_buffer *vb) {
    decoder->video_buffer = vb;
}

bool
decoder_open(struct decoder *decoder, const AVCodec *codec) {
    decoder->codec_ctx = avcodec_alloc_context3(codec);
    if (!decoder->codec_ctx) {
        LOGC("Could not allocate decoder context");
        return false;
    }

    if (avcodec_open2(decoder->codec_ctx, codec, NULL) < 0) {
        LOGE("Could not open codec");
        avcodec_free_context(&decoder->codec_ctx);
        return false;
    }

    decoder->frame = av_frame_alloc();
    if (!decoder->frame) {
        LOGE("Could not create decoder frame");
        avcodec_close(decoder->codec_ctx);
        avcodec_free_context(&decoder->codec_ctx);
        return false;
    }

    return true;
}

void
decoder_close(struct decoder *decoder) {
    av_frame_free(&decoder->frame);
    avcodec_close(decoder->codec_ctx);
    avcodec_free_context(&decoder->codec_ctx);
}

bool
decoder_push(struct decoder *decoder, const AVPacket *packet) {
    int ret;
    if ((ret = avcodec_send_packet(decoder->codec_ctx, packet)) < 0) {
        LOGE("Could not send video packet: %d", ret);
        return false;
    }
    ret = avcodec_receive_frame(decoder->codec_ctx, decoder->frame);
    if (!ret) {
        // a frame was received
        video_buffer_producer_offer_frame(decoder->video_buffer,
                                          &decoder->frame);
    } else if (ret != AVERROR(EAGAIN)) {
        LOGE("Could not receive video frame: %d", ret);
        return false;
    }
    return true;
}

void
decoder_interrupt(struct decoder *decoder) {
    video_buffer_interrupt(decoder->video_buffer);
}
