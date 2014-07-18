/*
 * HTTP server implementation.
 *
 * istream implementation for the request body.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "http_server_internal.hxx"
#include "pool.hxx"
#include "istream-internal.h"
#include "util/Cast.hxx"

BufferedResult
http_server_feed_body(struct http_server_connection *connection,
                      const void *data, size_t length)
{
    assert(connection != nullptr);
    assert(connection->request.read_state == http_server_connection::Request::BODY);
    assert(connection->request.request->body != nullptr);
    assert(!connection->response.pending_drained);

    /* checking request.request->body and not request.body_reader,
       because the dechunker might be attached to the
       http_body_reader */
    if (!istream_has_handler(connection->request.request->body))
        /* the handler is not yet connected */
        return BufferedResult::BLOCKING;

    const ScopePoolRef ref(*connection->pool TRACE_ARGS);

    size_t nbytes = connection->request.body_reader.FeedBody(data, length);
    if (nbytes == 0) {
        return connection->socket.IsValid()
            ? BufferedResult::BLOCKING
            : BufferedResult::CLOSED;
    }

    connection->request.bytes_received += nbytes;
    connection->socket.Consumed(nbytes);

    if (connection->request.read_state == http_server_connection::Request::BODY &&
        connection->request.body_reader.IsEOF()) {
        connection->request.read_state = http_server_connection::Request::END;

        /* re-enable the event, to detect client disconnect while
           we're processing the request */
        connection->socket.ScheduleReadNoTimeout(false);

        connection->request.body_reader.DeinitEOF();
        if (!http_server_connection_valid(connection))
            return BufferedResult::CLOSED;
    }

    return nbytes == length
        ? BufferedResult::OK
        : BufferedResult::PARTIAL;
}

static inline struct http_server_connection *
response_stream_to_connection(struct istream *istream)
{
    auto &body = HttpBodyReader::FromStream(*istream);
    return ContainerCast(&body, struct http_server_connection,
                         request.body_reader);
}

static off_t
http_server_request_stream_available(struct istream *istream, bool partial)
{
    struct http_server_connection *connection = response_stream_to_connection(istream);

    assert(http_server_connection_valid(connection));
    assert(connection->request.read_state == http_server_connection::Request::BODY);
    assert(!connection->response.pending_drained);

    return connection->request.body_reader.GetAvailable(connection->socket,
                                                        partial);
}

static void
http_server_request_stream_read(struct istream *istream)
{
    struct http_server_connection *connection = response_stream_to_connection(istream);

    assert(http_server_connection_valid(connection));
    assert(connection->request.read_state == http_server_connection::Request::BODY);
    assert(istream_has_handler(&connection->request.body_reader.GetStream()));
    assert(connection->request.request->body != nullptr);
    assert(istream_has_handler(connection->request.request->body));
    assert(!connection->response.pending_drained);

    if (connection->request.in_handler)
        /* avoid recursion */
        return;

    if (!http_server_maybe_send_100_continue(connection))
        return;

    connection->socket.Read(connection->request.body_reader.RequireMore());
}

static void
http_server_request_stream_close(struct istream *istream)
{
    struct http_server_connection *connection = response_stream_to_connection(istream);

    if (connection->request.read_state == http_server_connection::Request::END)
        return;

    assert(connection->request.read_state == http_server_connection::Request::BODY);
    assert(!connection->request.body_reader.IsEOF());
    assert(!connection->response.pending_drained);

    if (!connection->socket.IsValid() ||
        !connection->socket.IsConnected()) {
        /* this happens when there's an error on the socket while
           reading the request body before the response gets
           submitted, and this HTTP server library invokes the
           handler's abort method; the handler will free the request
           body, but the socket is already closed */
        assert(connection->request.request == nullptr);
    }

    connection->request.read_state = http_server_connection::Request::END;

    if (connection->request.request != nullptr)
        connection->request.request->body = nullptr;

    connection->keep_alive = false;

    connection->request.body_reader.Deinit();
}

const struct istream_class http_server_request_stream = {
    .available = http_server_request_stream_available,
    .read = http_server_request_stream_read,
    .close = http_server_request_stream_close,
};
