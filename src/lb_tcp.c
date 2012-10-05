/*
 * Handler for raw TCP connections.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "lb_tcp.h"
#include "lb_connection.h"
#include "lb_instance.h"
#include "lb_config.h"
#include "lb_log.h"
#include "tcp-balancer.h"
#include "istream-socket.h"
#include "sink-socket.h"
#include "client-balancer.h"
#include "client-socket.h"
#include "istream-socket.h"
#include "sink-socket.h"
#include "address_sticky.h"

#include <unistd.h>

/*
 * first istream_socket handler
 *
 */

static void
first_istream_socket_read(void *ctx)
{
    struct lb_connection *connection = ctx;

    sink_socket_read(connection->tcp.peers[0].sink);
}

static void
first_istream_socket_close(void *ctx)
{
    struct lb_connection *connection = ctx;

    (void)connection;
}

static bool
first_istream_socket_error(int error, void *ctx)
{
    struct lb_connection *connection = ctx;

    lb_connection_log_errno(3, connection, "Receive failed", error);
    lb_connection_close(connection);
    return false;
}

static bool
first_istream_socket_depleted(void *ctx)
{
    struct lb_connection *connection = ctx;

    (void)connection;
    return true;
}

static bool
first_istream_socket_finished(void *ctx)
{
    struct lb_connection *connection = ctx;

    lb_connection_close(connection);
    return false;
}

static const struct istream_socket_handler first_istream_socket_handler = {
    .read = first_istream_socket_read,
    .close = first_istream_socket_close,
    .error = first_istream_socket_error,
    .depleted = first_istream_socket_depleted,
    .finished = first_istream_socket_finished,
};

/*
 * second istream_socket handler
 *
 */

static void
second_istream_socket_read(void *ctx)
{
    struct lb_connection *connection = ctx;

    sink_socket_read(connection->tcp.peers[1].sink);
}

static void
second_istream_socket_close(void *ctx)
{
    struct lb_connection *connection = ctx;

    (void)connection;
}

static bool
second_istream_socket_error(int error, void *ctx)
{
    struct lb_connection *connection = ctx;

    lb_connection_log_errno(3, connection, "Receive failed", error);
    lb_connection_close(connection);
    return false;
}

static bool
second_istream_socket_depleted(void *ctx)
{
    struct lb_connection *connection = ctx;

    (void)connection;
    return true;
}

static bool
second_istream_socket_finished(void *ctx)
{
    struct lb_connection *connection = ctx;

    lb_connection_close(connection);
    return false;
}

static const struct istream_socket_handler second_istream_socket_handler = {
    .read = second_istream_socket_read,
    .close = second_istream_socket_close,
    .error = second_istream_socket_error,
    .depleted = second_istream_socket_depleted,
    .finished = second_istream_socket_finished,
};

/*
 * first sink_socket handler
 *
 */

static void
first_sink_input_eof(void *ctx)
{
    struct lb_connection *connection = ctx;

    lb_connection_close(connection);
}

static void
first_sink_input_error(GError *error, void *ctx)
{
    struct lb_connection *connection = ctx;

    connection->tcp.peers[0].sink = NULL;

    lb_connection_log_gerror(3, connection, "Error", error);
    g_error_free(error);

    lb_connection_close(connection);
}

static bool
first_sink_send_error(int error, void *ctx)
{
    struct lb_connection *connection = ctx;

    connection->tcp.peers[0].sink = NULL;

    lb_connection_log_errno(3, connection, "Send failed", error);
    lb_connection_close(connection);
    return false;
}

static const struct sink_socket_handler first_sink_socket_handler = {
    .input_eof = first_sink_input_eof,
    .input_error = first_sink_input_error,
    .send_error = first_sink_send_error,
};

/*
 * second sink_socket handler
 *
 */

static void
second_sink_input_eof(void *ctx)
{
    struct lb_connection *connection = ctx;

    lb_connection_close(connection);
}

static void
second_sink_input_error(GError *error, void *ctx)
{
    struct lb_connection *connection = ctx;

    connection->tcp.peers[1].sink = NULL;

    lb_connection_log_gerror(3, connection, "Error", error);
    g_error_free(error);

    lb_connection_close(connection);
}

static bool
second_sink_send_error(int error, void *ctx)
{
    struct lb_connection *connection = ctx;

    connection->tcp.peers[1].sink = NULL;

    lb_connection_log_errno(3, connection, "Send failed", error);
    lb_connection_close(connection);
    return false;
}

static const struct sink_socket_handler second_sink_socket_handler = {
    .input_eof = second_sink_input_eof,
    .input_error = second_sink_input_error,
    .send_error = second_sink_send_error,
};

/*
 * stock_handler
 *
 */

static void
lb_tcp_stock_success(int fd, void *ctx)
{
    struct lb_connection *connection = ctx;

    async_ref_clear(&connection->tcp.connect);

    connection->tcp.peers[1].fd = fd;

    struct istream *istream =
        istream_socket_new(connection->pool,
                           connection->tcp.peers[0].fd,
                           connection->tcp.peers[0].type,
                           &first_istream_socket_handler, connection);
    istream = istream_pipe_new(connection->pool, istream,
                               connection->instance->pipe_stock);

    connection->tcp.peers[1].sink =
        sink_socket_new(connection->pool, istream, fd, ISTREAM_TCP,
                        &second_sink_socket_handler, connection);

    istream = istream_socket_new(connection->pool, fd, ISTREAM_TCP,
                                 &second_istream_socket_handler, connection);
    istream = istream_pipe_new(connection->pool, istream,
                               connection->instance->pipe_stock);

    connection->tcp.peers[0].sink =
        sink_socket_new(connection->pool, istream,
                        connection->tcp.peers[0].fd,
                        connection->tcp.peers[0].type,
                        &first_sink_socket_handler, connection);
}

static void
lb_tcp_stock_timeout(void *ctx)
{
    struct lb_connection *connection = ctx;

    close(connection->tcp.peers[0].fd);

    lb_connection_log_error(4, connection, "Connect error", "Timeout");
    lb_connection_remove(connection);
}

static void
lb_tcp_stock_error(GError *error, void *ctx)
{
    struct lb_connection *connection = ctx;

    close(connection->tcp.peers[0].fd);

    lb_connection_log_gerror(4, connection, "Connect error", error);
    g_error_free(error);

    lb_connection_remove(connection);
}

static const struct client_socket_handler lb_tcp_client_socket_handler = {
    .success = lb_tcp_stock_success,
    .timeout = lb_tcp_stock_timeout,
    .error = lb_tcp_stock_error,
};

/*
 * constructor
 *
 */

gcc_pure
static unsigned
lb_tcp_sticky(const struct lb_cluster_config *cluster,
              const struct sockaddr *remote_address)
{
    switch (cluster->address_list.sticky_mode) {
    case STICKY_NONE:
    case STICKY_FAILOVER:
        break;

    case STICKY_SOURCE_IP:
        return socket_address_sticky(remote_address);

    case STICKY_SESSION_MODULO:
    case STICKY_COOKIE:
    case STICKY_JVM_ROUTE:
        /* not implemented here */
        break;
    }

    return 0;
}

void
lb_tcp_new(struct lb_connection *connection, int fd,
           enum istream_direct fd_type,
           const struct sockaddr *remote_address)
{
    connection->tcp.peers[0].fd = fd;
    connection->tcp.peers[0].type = fd_type;

    unsigned session_sticky = lb_tcp_sticky(connection->listener->cluster,
                                            remote_address);

    client_balancer_connect(connection->pool, connection->instance->balancer,
                            session_sticky,
                            &connection->listener->cluster->address_list,
                            20,
                            &lb_tcp_client_socket_handler, connection,
                            &connection->tcp.connect);
}

void
lb_tcp_close(struct lb_connection *connection)
{
    if (async_ref_defined(&connection->tcp.connect))
        async_abort(&connection->tcp.connect);
    else {
        if (connection->tcp.peers[0].sink != NULL)
            sink_socket_close(connection->tcp.peers[0].sink);

        if (connection->tcp.peers[1].sink != NULL)
            sink_socket_close(connection->tcp.peers[1].sink);

        close(connection->tcp.peers[0].fd);
        close(connection->tcp.peers[1].fd);
    }
}
