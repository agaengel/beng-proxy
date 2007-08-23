/*
 * HTTP server implementation.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "http-server.h"
#include "fifo-buffer.h"
#include "strutil.h"
#include "compiler.h"
#include "buffered-io.h"
#include "header-parser.h"
#include "header-writer.h"

#ifdef __linux
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *http_status_to_string_data[][20] = {
    [2] = {
        [HTTP_STATUS_OK - 200] = "200 OK",
        [HTTP_STATUS_CREATED - 200] = "201 Created",
        [HTTP_STATUS_NO_CONTENT - 200] = "204 No Content",
        [HTTP_STATUS_PARTIAL_CONTENT - 200] = "206 Partial Content",
    },
    [4] = {
        [HTTP_STATUS_BAD_REQUEST - 400] = "400 Bad Request",
        [HTTP_STATUS_UNAUTHORIZED - 400] = "401 Unauthorized",
        [HTTP_STATUS_FORBIDDEN - 400] = "403 Forbidden",
        [HTTP_STATUS_NOT_FOUND - 400] = "404 Not Found",
        [HTTP_STATUS_METHOD_NOT_ALLOWED - 400] = "405 Method Not Allowed",
    },
    [5] = {
        [HTTP_STATUS_INTERNAL_SERVER_ERROR - 500] = "500 Internal Server Error",
        [HTTP_STATUS_NOT_IMPLEMENTED - 500] = "501 Not Implemented",
        [HTTP_STATUS_BAD_GATEWAY - 500] = "502 Bad Gateway",
        [HTTP_STATUS_SERVICE_UNAVAILABLE - 500] = "503 Service Unavailable",
        [HTTP_STATUS_GATEWAY_TIMEOUT - 500] = "504 Gateway Timeout",
        [HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED - 500] = "505 HTTP Version Not Supported",
    },
};

struct http_server_connection {
    pool_t pool;

    /*
    struct sockaddr_storage remote_addr;
    socklen_t remote_addrlen;
    */

    /* I/O */
    int fd;
    struct event event;
    fifo_buffer_t input, output;

    /* callback */
    http_server_callback_t callback;
    void *callback_ctx;

    /* request */
    struct {
        enum {
            READ_START,
            READ_HEADERS,
            READ_BODY,
            READ_END
        } read_state;
        struct http_server_request *request;
    } request;

    /* response */
    struct {
        int writing;
        enum {
            WRITE_STATUS,
            WRITE_HEADERS,
            WRITE_BODY,
            WRITE_POST
        } write_state;
        int blocking;
        int chunked;
        char status_buffer[64];
        char content_length_buffer[32];
        istream_t status;
        struct header_writer header_writer;
        istream_t body;
    } response;

    /* connection settings */
    int keep_alive;
#ifdef __linux
    int cork;
#endif
};

static struct http_server_request *
http_server_request_new(http_server_connection_t connection)
{
    pool_t pool;
    struct http_server_request *request;

    assert(connection != NULL);

    pool = pool_new_linear(connection->pool, "http_server_request", 16384);
    request = p_malloc(pool, sizeof(*request));
    request->pool = pool;
    request->connection = connection;
    request->headers = strmap_new(pool, 64);

    return request;
}

static void
http_server_request_free(struct http_server_request **request_r)
{
    struct http_server_request *request = *request_r;
    *request_r = NULL;

    pool_unref(request->pool);
}

static inline int
http_server_connection_valid(http_server_connection_t connection)
{
    return connection->fd >= 0;
}

static inline void
http_server_cork(http_server_connection_t connection)
{
    assert(connection != NULL);
    assert(connection->fd >= 0);

    if (!connection->cork) {
        connection->cork = 1;
#ifdef __linux
        setsockopt(connection->fd, IPPROTO_TCP, TCP_CORK,
                   &connection->cork, sizeof(connection->cork));
#endif
    }
}

static inline void
http_server_uncork(http_server_connection_t connection)
{
    assert(connection != NULL);

    if (connection->cork) {
        assert(connection->fd >= 0);
        connection->cork = 0;
#ifdef __linux
        setsockopt(connection->fd, IPPROTO_TCP, TCP_CORK,
                   &connection->cork, sizeof(connection->cork));
#endif
    }
}

static void
http_server_response_eof(http_server_connection_t connection)
{
    assert(connection->request.read_state != READ_START &&
           connection->request.read_state != READ_HEADERS);
    assert(connection->request.request != NULL);
    assert(connection->response.writing);
    assert(connection->response.write_state == WRITE_POST);
    assert(connection->response.status == NULL);
    assert(connection->response.body == NULL);

    pool_ref(connection->pool);

    if (connection->request.read_state == READ_BODY) {
        /* XXX discard rest of body? */
    }

    connection->request.read_state = READ_START;
    connection->response.writing = 0;

    http_server_request_free(&connection->request.request);

    if (!connection->keep_alive)
        /* keepalive disabled and response is finished: we must close
           the connection */
        http_server_connection_close(connection);

    pool_unref(connection->pool);
}

static void
http_server_response_read(http_server_connection_t connection)
{
    ssize_t nbytes;

    assert(connection->request.read_state != READ_START &&
           connection->request.read_state != READ_HEADERS);
    assert(connection->request.request != NULL);
    assert(connection->response.writing);
    assert(connection->response.write_state != WRITE_POST);

    switch (connection->response.write_state) {
    case WRITE_STATUS:
        assert(connection->response.status != NULL);
        istream_read(connection->response.status);

        break;

    case WRITE_HEADERS:
        nbytes = header_writer_run(&connection->response.header_writer);
        if (nbytes == 0) {
            if (connection->response.body == NULL) {
                connection->response.write_state = WRITE_POST;
                http_server_response_eof(connection);
            } else {
                connection->response.write_state = WRITE_BODY;
            }
        }

        break;

    case WRITE_BODY:
        assert(connection->response.body != NULL);
#ifdef __linux
        if (connection->response.chunked)
            istream_read(connection->response.body);
        else
            istream_direct(connection->response.body);
#else
        istream_read(connection->response.body);
#endif

        break;

    case WRITE_POST:
        assert(0);
    }
}

static void
http_server_response_read_loop(http_server_connection_t connection)
{
    struct http_server_request *request = connection->request.request;
    unsigned old_state;

    do {
        old_state = connection->response.write_state;
        http_server_response_read(connection);
    } while (connection->request.request == request &&
             connection->response.write_state != old_state);
}

static void
http_server_try_response(http_server_connection_t connection)
{
    const void *buffer;
    size_t length;
    ssize_t nbytes;

    assert(connection != NULL);
    assert(connection->fd >= 0);

    connection->response.blocking = 0;

    while ((buffer = fifo_buffer_read(connection->output, &length)) != NULL) {
        http_server_cork(connection);

        nbytes = write(connection->fd, buffer, length);
        if (unlikely(nbytes < 0 && errno != EAGAIN)) {
            perror("write error on HTTP connection");
            http_server_connection_close(connection);
            break;
        } else if (nbytes > 0) {
            fifo_buffer_consume(connection->output, (size_t)nbytes);
            if (!connection->response.writing ||
                connection->response.write_state == WRITE_POST)
                break;

            connection->response.blocking = (size_t)nbytes < length;
            if (connection->response.blocking)
                break;

            http_server_response_read(connection);
            if (!http_server_connection_valid(connection))
                break;
        } else {
            break;
        }
    }

    if (connection->response.writing &&
        connection->response.write_state != WRITE_POST &&
        !connection->response.blocking)
        http_server_response_read_loop(connection);

    http_server_uncork(connection);

    if (!connection->keep_alive &&
        connection->response.writing &&
        connection->response.write_state == WRITE_POST &&
        fifo_buffer_empty(connection->output))
        /* keepalive disabled and response is finished: we must close
           the connection */
        http_server_connection_close(connection);
}

static void
http_server_parse_request_line(http_server_connection_t connection,
                               const char *line, size_t length)
{
    const char *eol, *space;
    http_method_t method = HTTP_METHOD_NULL;

    assert(connection != NULL);
    assert(connection->request.read_state == READ_START);
    assert(connection->request.request == NULL);

    if (unlikely(length < 5)) {
        http_server_connection_close(connection);
        return;
    }

    eol = line + length;

    switch (line[0]) {
    case 'G':
        if (likely(line[1] == 'E' && line[2] == 'T' && line[3] == ' ')) {
                method = HTTP_METHOD_GET;
            line += 4;
        }
        break;

    case 'P':
        if (likely(line[1] == 'O' && line[2] == 'S' && line[3] == 'T' &&
                   line[4] == ' ')) {
            method = HTTP_METHOD_POST;
            line += 5;
        }
        break;

    case 'H':
        if (likely(line[1] == 'E' && line[2] == 'A' && line[3] == 'D' &&
                   line[4] == ' ')) {
            method = HTTP_METHOD_POST;
            line += 5;
        }
        break;
    }

    /* XXX: unknown method? */

    space = memchr(line, ' ', eol - line);
    if (unlikely(space == NULL))
        space = eol;

    connection->request.request = http_server_request_new(connection);
    connection->request.request->method = method;
    connection->request.request->uri = p_strndup(connection->request.request->pool, line, space - line);
    connection->request.read_state = READ_HEADERS;
}

static void
http_server_headers_finished(http_server_connection_t connection)
{
    const char *header_connection;

    header_connection = strmap_get(connection->request.request->headers, "connection");
    connection->keep_alive = header_connection != NULL &&
        strcasecmp(header_connection, "keep-alive") == 0;

    /* XXX body? */
    connection->request.read_state = READ_END;
    connection->callback(connection->request.request, connection->callback_ctx);
}

static void
http_server_handle_line(http_server_connection_t connection,
                        const char *line, size_t length)
{
    assert(connection->request.read_state == READ_START ||
           connection->request.read_state == READ_HEADERS);

    if (unlikely(connection->request.read_state == READ_START)) {
        assert(connection->request.request == NULL);

        http_server_parse_request_line(connection, line, length);
    } else if (likely(length > 0)) {
        assert(connection->request.read_state == READ_HEADERS);
        assert(connection->request.request != NULL);

        header_parse_line(connection->request.request->pool,
                          connection->request.request->headers,
                          line, length);
    } else {
        assert(connection->request.read_state == READ_HEADERS);
        assert(connection->request.request != NULL);

        http_server_headers_finished(connection);
    }
}

static int
http_server_parse_headers(http_server_connection_t connection)
{
    const char *buffer, *buffer_end, *start, *end, *next = NULL;
    size_t length;

    assert(connection->request.read_state == READ_START ||
           connection->request.read_state == READ_HEADERS);

    buffer = fifo_buffer_read(connection->input, &length);
    if (buffer == NULL)
        return 0;

    assert(length > 0);
    buffer_end = buffer + length;

    start = buffer;
    while ((end = memchr(start, '\n', buffer_end - start)) != NULL) {
        next = end + 1;
        --end;
        if (likely(*end == '\r'))
            --end;
        while (unlikely(end >= start && char_is_whitespace(*end)))
            --end;

        http_server_handle_line(connection, start, end - start + 1);
        if (connection->request.read_state != READ_HEADERS)
            break;

        start = next;
    }

    if (next == NULL)
        return 0;

    fifo_buffer_consume(connection->input, next - buffer);
    return 1;
}

static void
http_server_consume_input(http_server_connection_t connection)
{
    while (1) {
        if (connection->request.read_state == READ_START ||
            connection->request.read_state == READ_HEADERS) {
            if (http_server_parse_headers(connection) == 0)
                break;
        } else if (connection->request.read_state == READ_BODY) {
            /* XXX read body*/
        } else {
            break;
        }
    }
}

void
http_server_try_read(http_server_connection_t connection)
{
    void *buffer;
    size_t max_length;
    ssize_t nbytes;

    buffer = fifo_buffer_write(connection->input, &max_length);
    assert(buffer != NULL);

    assert(max_length > 0);

    nbytes = read(connection->fd, buffer, max_length);
    if (unlikely(nbytes < 0)) {
        if (errno == EAGAIN)
            return;
        perror("read error on HTTP connection");
        http_server_connection_close(connection);
        return;
    }

    if (unlikely(nbytes == 0)) {
        /* XXX */
        http_server_connection_close(connection);
        return;
    }

    fifo_buffer_append(connection->input, (size_t)nbytes);

    http_server_consume_input(connection);
}

static void
http_server_event_callback(int fd, short event, void *ctx);

static void
http_server_event_setup(http_server_connection_t connection)
{
    short event = 0;
    struct timeval tv;

    assert(connection != NULL);
    assert(connection->fd >= 0);
    assert(connection->input != NULL);
    assert(connection->output != NULL);

    if (connection->event.ev_events != 0)
        event_del(&connection->event);

    if ((connection->request.read_state == READ_START ||
         connection->request.read_state == READ_HEADERS ||
         connection->request.read_state == READ_BODY) &&
        !fifo_buffer_full(connection->input))
        event = EV_READ | EV_TIMEOUT;

    if ((connection->response.writing && connection->response.blocking) ||
        !fifo_buffer_empty(connection->output))
        event |= EV_WRITE | EV_TIMEOUT;

    if (event == 0) {
        connection->event.ev_events = 0;
        return;
    }

    tv.tv_sec = 30;
    tv.tv_usec = 0;

    event_set(&connection->event, connection->fd,
              event, http_server_event_callback, connection);
    event_add(&connection->event, &tv);
}

static void
http_server_event_callback(int fd, short event, void *ctx)
{
    http_server_connection_t connection = ctx;

    (void)fd;

    pool_ref(connection->pool);

    if (unlikely(event & EV_TIMEOUT)) {
        fprintf(stderr, "timeout\n");
        http_server_connection_close(connection);
    }

    if (http_server_connection_valid(connection) && (event & EV_WRITE) != 0)
        http_server_try_response(connection);

    if (http_server_connection_valid(connection) && (event & EV_READ) != 0)
        http_server_try_read(connection);

    if (likely(http_server_connection_valid(connection)))
        http_server_event_setup(connection);

    pool_unref(connection->pool);
    pool_commit();
}

http_server_connection_t
http_server_connection_new(pool_t pool, int fd,
                           http_server_callback_t callback, void *ctx)
{
    http_server_connection_t connection;

    assert(fd >= 0);
    assert(callback != NULL);

    connection = p_malloc(pool, sizeof(*connection));
    connection->pool = pool;
    connection->fd = fd;
    connection->callback = callback;
    connection->callback_ctx = ctx;
    connection->request.read_state = READ_START;
    connection->request.request = NULL;
    connection->response.writing = 0;
    connection->cork = 0;

    connection->input = fifo_buffer_new(pool, 4096);
    connection->output = fifo_buffer_new(pool, 4096);

    connection->event.ev_events = 0;
    http_server_event_setup(connection);

    return connection;
}

void
http_server_connection_close(http_server_connection_t connection)
{
    assert(connection != NULL);

    if (connection->fd >= 0) {
        event_del(&connection->event);
        close(connection->fd);
        connection->fd = -1;
    }

    connection->request.read_state = READ_START;
    connection->cork = 0;

    pool_ref(connection->pool);

    if (connection->request.read_state != READ_START) {
        assert(connection->request.request != NULL);
        http_server_request_free(&connection->request.request);

        if (connection->response.writing) {
            if (connection->response.status != NULL)
                istream_free(&connection->response.status);

            if (connection->response.body != NULL) {
                pool_t pool = connection->response.body->pool;
                istream_free(&connection->response.body);
                pool_unref(pool);
            }

            connection->response.writing = 0;
        }
    }

    if (connection->callback != NULL) {
        http_server_callback_t callback = connection->callback;
        void *callback_ctx = connection->callback_ctx;
        connection->callback = NULL;
        connection->callback_ctx = NULL;
        callback(NULL, callback_ctx);
    }

    pool_unref(connection->pool);
}

void
http_server_connection_free(http_server_connection_t *connection_r)
{
    http_server_connection_t connection = *connection_r;
    *connection_r = NULL;

    assert(connection != NULL);

    http_server_connection_close(connection);
}

static const char hex_digits[] = "0123456789abcdef";

static size_t
http_server_send_chunk(http_server_connection_t connection,
                       const void *p, size_t length)
{
    char *dest;
    size_t max_length;

    assert(connection != NULL);
    assert(connection->fd >= 0);
    assert(p != NULL);
    assert(length > 0);


    dest = fifo_buffer_write(connection->output, &max_length);
    if (dest == NULL)
        return 0;

    if (max_length < 4 + 2 + 1 + 2 + 5)
        return 0;

    if (length > max_length - 4 - 2 - 2 - 5)
        length = max_length - 4 - 2 - 2 - 5;

    dest[0] = hex_digits[(length >> 12) & 0xf];
    dest[1] = hex_digits[(length >> 8) & 0xf];
    dest[2] = hex_digits[(length >> 4) & 0xf];
    dest[3] = hex_digits[length & 0xf];
    dest[4] = '\r';
    dest[5] = '\n';
    memcpy(dest + 4 + 2, p, length);
    dest[4 + 2 + length] = '\r';
    dest[4 + 2 + length + 1] = '\n';

    fifo_buffer_append(connection->output, 4 + 2 + length + 2);

    if ((connection->event.ev_events & EV_WRITE) == 0)
        http_server_event_setup(connection);

    return length;
}

static void
http_server_send_last_chunk(http_server_connection_t connection)
{
    char *dest;
    size_t max_length;

    dest = fifo_buffer_write(connection->output, &max_length);
    assert(dest != NULL && max_length >= 5); /* XXX */

    memcpy(dest, "0\r\n\r\n", 5);
    fifo_buffer_append(connection->output, 5);
}

size_t
http_server_send_status(http_server_connection_t connection, int status)
{
    char *dest;
    size_t length;

    assert(connection != NULL);
    assert(connection->fd >= 0);
    assert(status >= 100 && status < 600);

    dest = fifo_buffer_write(connection->output, &length);
    snprintf(dest, length,
             "HTTP/1.1 %d\r\nServer: beng-proxy " VERSION "\r\n",
             status);
    length = strlen(dest);
    fifo_buffer_append(connection->output, length);
    return length;
}

void
http_server_try_write(http_server_connection_t connection)
{
    assert(connection != NULL);
    assert(!connection->cork);

    http_server_try_response(connection);
}

static size_t
write_or_append(http_server_connection_t connection,
                const void *data, size_t length)
{
    void *dest;
    size_t max_length;
    ssize_t nbytes;

    assert(connection->fd >= 0);
    assert(connection->response.writing);

    if (length >= 1024 && fifo_buffer_empty(connection->output)) {
        nbytes = write(connection->fd, data, length);
        connection->response.blocking = (ssize_t)length < nbytes;
        if (nbytes >= 0)
            return (size_t)nbytes;

        if (errno == EAGAIN)
            return 0;

        perror("write error on HTTP connection");
        http_server_connection_close(connection);
        return 0;
    } else {
        dest = fifo_buffer_write(connection->output, &max_length);
        if (dest == NULL)
            return 0;

        if (length > max_length)
            length = max_length;

        memcpy(dest, data, length);
        fifo_buffer_append(connection->output, length);

        if ((connection->event.ev_events & EV_WRITE) == 0)
            http_server_event_setup(connection);

        return length;
    }
}


static size_t
http_server_status_data(const void *data, size_t length, void *ctx)
{
    http_server_connection_t connection = ctx;

    assert(connection->response.writing);
    assert(connection->response.write_state == WRITE_STATUS);

    return write_or_append(connection, data, length);
}

static void
http_server_status_eof(void *ctx)
{
    http_server_connection_t connection = ctx;

    assert(connection->response.writing);
    assert(connection->response.write_state == WRITE_STATUS);

    connection->response.status = NULL;
    connection->response.write_state = WRITE_HEADERS;
}

static void
http_server_status_free(void *ctx)
{
    http_server_connection_t connection = ctx;

    if (connection->response.write_state == WRITE_STATUS)
        http_server_connection_close(connection);
}

static const struct istream_handler http_server_status_handler = {
    .data = http_server_status_data,
    .eof = http_server_status_eof,
    .free = http_server_status_free,
};


static size_t
http_server_response_body_data(const void *data, size_t length, void *ctx)
{
    http_server_connection_t connection = ctx;

    assert(connection->response.writing);
    assert(connection->response.write_state != WRITE_POST);

    if (connection->response.write_state != WRITE_BODY)
        return 0;

    if (connection->response.chunked)
        return http_server_send_chunk(connection, data, length);
    else
        return write_or_append(connection, data, length);
}

#ifdef __linux
static ssize_t
http_server_response_body_direct(int fd, size_t max_length, void *ctx)
{
    http_server_connection_t connection = ctx;
    ssize_t nbytes;

    assert(connection->response.writing);
    assert(connection->response.write_state != WRITE_POST);

    connection->response.blocking = 0;

    if (connection->response.write_state != WRITE_BODY)
        return -2;

    nbytes = sendfile(connection->fd, fd, NULL, max_length);
    if (nbytes < 0 && errno == EAGAIN)
        return -2;

    if (nbytes > 0)
        connection->response.blocking = 1;

    return nbytes;
}
#endif

static void
http_server_response_body_eof(void *ctx)
{
    http_server_connection_t connection = ctx;

    assert(connection->response.writing);
    assert(connection->response.write_state != WRITE_POST);
    assert(connection->response.body != NULL);
    assert(connection->response.body->pool != NULL);

    pool_unref(connection->response.body->pool);

    connection->response.body = NULL;
    if (connection->response.write_state == WRITE_BODY)
        connection->response.write_state = WRITE_POST;

    if (connection->response.chunked) {
        if (connection->response.write_state == WRITE_POST) {
            http_server_send_last_chunk(connection);
        } else {
            /* XXX chunked body is empty before we were able to write */
            abort();
        }
    }

    http_server_response_eof(connection);
}

static void
http_server_response_body_free(void *ctx)
{
    http_server_connection_t connection = ctx;

    if (connection->response.write_state == WRITE_BODY)
        http_server_connection_close(connection);
}

static const struct istream_handler http_server_response_body_handler = {
    .data = http_server_response_body_data,
#ifdef __linux
    .direct = http_server_response_body_direct,
#endif
    .eof = http_server_response_body_eof,
    .free = http_server_response_body_free,
};


static const char *
http_status_to_string(http_status_t status)
{
    assert((status / 100) < sizeof(http_status_to_string_data) / sizeof(http_status_to_string_data[0]));
    assert(status % 100 < sizeof(http_status_to_string_data[0]) / sizeof(http_status_to_string_data[0][0]));

    return http_status_to_string_data[status / 100][status % 100];
}

static size_t
format_status_line(char *p, http_status_t status)
{
    const char *status_string;
    size_t length;

    assert(status >= 100 && status < 600);

    status_string = http_status_to_string(status);
    assert(status_string != NULL);
    length = strlen(status_string);

    memcpy(p, "HTTP/1.1 ", 9);
    memcpy(p + 9, status_string, length);
    length += 9;
    p[length++] = '\r';
    p[length++] = '\n';

    return length;
}

void
http_server_response(struct http_server_request *request,
                     http_status_t status, strmap_t headers,
                     off_t content_length, istream_t body)
{
    http_server_connection_t connection = request->connection;

    assert(connection->request.request == request);
    assert(!connection->response.writing);

    connection->response.status
        = istream_memory_new(request->pool,
                             connection->response.status_buffer,
                             format_status_line(connection->response.status_buffer,
                                                status));
    connection->response.status->handler = &http_server_status_handler;
    connection->response.status->handler_ctx = connection;

    if (headers == NULL)
        headers = strmap_new(request->pool, 16);

    if (content_length == (off_t)-1) {
        if (connection->keep_alive) {
            strmap_put(headers, "transfer-encoding", "chunked", 1);
            connection->response.chunked = 1;
        } else
            connection->response.chunked = 0;
    } else {
        snprintf(connection->response.content_length_buffer,
                 sizeof(connection->response.content_length_buffer),
                 "%lu", (unsigned long)content_length);
        strmap_put(headers, "content-length",
                   connection->response.content_length_buffer,
                   1);
        connection->response.chunked = 0;
    }

    strmap_put(headers, "connection",
               connection->keep_alive ? "keep-alive" : "close",
               1);

    header_writer_init(&connection->response.header_writer,
                       connection->output, headers);

    connection->response.body = body;
    if (connection->response.body != NULL) {
        pool_ref(connection->response.body->pool);
        connection->response.body->handler = &http_server_response_body_handler;
        connection->response.body->handler_ctx = connection;
    }

    connection->response.writing = 1;
    connection->response.write_state = WRITE_STATUS;

    http_server_try_response(connection);
}

void
http_server_send_message(struct http_server_request *request,
                         http_status_t status, const char *msg)
{
    size_t length = strlen(msg);

    http_server_response(request, status, NULL,
                         length,
                         istream_memory_new(request->pool, msg, length));
}
