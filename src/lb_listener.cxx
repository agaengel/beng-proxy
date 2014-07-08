/*
 * Listener on a TCP port.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "lb_listener.hxx"
#include "lb_instance.hxx"
#include "lb_connection.hxx"
#include "lb_config.hxx"
#include "ssl_factory.hxx"
#include "address_envelope.hxx"
#include "util/Error.hxx"
#include "net/SocketDescriptor.hxx"
#include "net/SocketAddress.hxx"
#include "net/ServerSocket.hxx"

#include <daemon/log.h>

/*
 * listener_handler
 *
 */

static void
lb_listener_connected(SocketDescriptor &&fd, SocketAddress address,
                      void *ctx)
{
    struct lb_listener *listener = (struct lb_listener *)ctx;

    lb_connection_new(&listener->instance, &listener->config,
                      listener->ssl_factory,
                      fd.Steal(), address, address.GetSize());
}

static void
lb_listener_error(Error &&error, G_GNUC_UNUSED void *ctx)
{
    daemon_log(2, "%s\n", error.GetMessage());
}

const struct listener_handler lb_listener_handler = {
    .connected = lb_listener_connected,
    .error = lb_listener_error,
};

/*
 * constructor
 *
 */

bool
lb_listener::Setup(Error &error)
{
    assert(ssl_factory == nullptr);

    if (config.ssl) {
        /* prepare SSL support */

        ssl_factory = ssl_factory_new(config.ssl_config, true, error);
        if (ssl_factory == nullptr)
            return false;
    }

    const struct address_envelope *envelope = config.envelope;

    listener = new ServerSocket(lb_listener_handler, this);
    return listener->Listen(envelope->address.sa_family, SOCK_STREAM, 0,
                            SocketAddress(&envelope->address,
                                          envelope->length),
                            error);
}

lb_listener::~lb_listener()
{
    delete listener;

    if (ssl_factory != nullptr)
        ssl_factory_free(ssl_factory);
}

void
lb_listener_event_add(struct lb_listener *listener)
{
    listener->listener->AddEvent();
}

void
lb_listener_event_del(struct lb_listener *listener)
{
    listener->listener->RemoveEvent();
}
