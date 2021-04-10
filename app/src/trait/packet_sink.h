#ifndef SC_PACKET_SINK
#define SC_PACKET_SINK

#include <assert.h>
#include <stdbool.h>
#include <libavcodec/avcodec.h>

/**
 * Packet sink trait.
 *
 * Component able to receive AVPackets should implement this trait.
 */
struct sc_packet_sink {
    const struct sc_packet_sink_ops *ops;
};

struct sc_packet_sink_ops {
    bool (*open)(struct sc_packet_sink *sink, const AVCodec *codec);
    void (*close)(struct sc_packet_sink *sink);
    bool (*push)(struct sc_packet_sink *sink, const AVPacket *packet);
};

#endif
