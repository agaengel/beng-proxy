/*
 * Listener on a TCP port.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_LISTENER_HXX
#define BENG_PROXY_LISTENER_HXX

#include "glibfwd.hxx"

#include <stddef.h>

struct sockaddr;
struct listener;

struct listener_handler {
    void (*connected)(int fd, const struct sockaddr *address,
                      size_t length, void *ctx);
    void (*error)(GError *error, void *ctx);
};

struct listener *
listener_new(int family, int socktype, int protocol,
             const struct sockaddr *address, size_t address_length,
             const struct listener_handler *handler, void *ctx,
             GError **error_r);

struct listener *
listener_tcp_port_new(int port,
                      const struct listener_handler *handler, void *ctx,
                      GError **error_r);

void
listener_free(struct listener **listener_r);

void
listener_event_add(struct listener *listener);

void
listener_event_del(struct listener *listener);

#endif
