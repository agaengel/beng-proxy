/*
 * Server side part of the "control" protocol.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_CONTROL_SERVER_H
#define BENG_PROXY_CONTROL_SERVER_H

#include "beng-proxy/control.h"

#include <glib.h>

#include <stddef.h>

struct pool;
struct sockaddr;
struct in_addr;
class SocketAddress;
struct ControlServer;

struct control_handler {
    /**
     * @return false if the datagram shall be discarded
     */
    bool (*raw)(const void *data, size_t length,
                SocketAddress address,
                int uid,
                void *ctx);

    void (*packet)(enum beng_control_command command,
                   const void *payload, size_t payload_length,
                   SocketAddress address,
                   void *ctx);

    void (*error)(GError *error, void *ctx);
};

G_GNUC_CONST
static inline GQuark
control_server_quark(void)
{
    return g_quark_from_static_string("control_server");
}

ControlServer *
control_server_new(SocketAddress address,
                   const struct control_handler *handler, void *ctx,
                   GError **error_r);

ControlServer *
control_server_new_port(const char *host_and_port, int default_port,
                        const struct in_addr *group,
                        const struct control_handler *handler, void *ctx,
                        GError **error_r);

void
control_server_free(ControlServer *cs);

void
control_server_enable(ControlServer *cs);

void
control_server_disable(ControlServer *cs);

/**
 * Replaces the socket.  The old one is closed, and the new one is now
 * owned by this object.
 */
void
control_server_set_fd(ControlServer *cs, int fd);

bool
control_server_reply(ControlServer *cs, struct pool *pool,
                     SocketAddress address,
                     enum beng_control_command command,
                     const void *payload, size_t payload_length,
                     GError **error_r);

void
control_server_decode(const void *data, size_t length,
                      SocketAddress address,
                      const struct control_handler *handler, void *handler_ctx);

#endif
