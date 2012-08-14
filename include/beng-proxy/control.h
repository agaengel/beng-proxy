/*
 * Definitions for the beng-proxy remote control protocol.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_CONTROL_H
#define BENG_PROXY_CONTROL_H

#include <stdint.h>

enum beng_control_command {
    CONTROL_NOP = 0,

    /**
     * Drop items from the translation cache.
     */
    CONTROL_TCACHE_INVALIDATE = 1,

    /**
     * Re-enable the specified node after a failure, remove all
     * failure/fade states.
     *
     * The payload is the node name according to lb.conf, followed by
     * a colon and the port number.
     */
    CONTROL_ENABLE_NODE = 2,

    /**
     * Fade out the specified node, preparing for its shutdown: the
     * node will only be used for pre-existing sessions that refer
     * to it.
     *
     * The payload is the node name according to lb.conf, followed by
     * a colon and the port number.
     */
    CONTROL_FADE_NODE = 3,

    /**
     * Get the status of the specified node.
     *
     * The payload is the node name according to lb.conf, followed by
     * a colon and the port number.
     *
     * The server then sends a response to the source IP.  Its payload
     * is the node name and port, a null byte, and a string describing
     * the worker status.  Possible values: "ok", "error", "fade".
     */
    CONTROL_NODE_STATUS = 4,

    /**
     * Dump all memory pools.
     */
    CONTROL_DUMP_POOLS = 5,
};

struct beng_control_header {
    uint16_t length;
    uint16_t command;
};

/**
 * This magic number precedes every UDP packet.
 */
static const uint32_t control_magic = 0x63046101;

#endif
