/*
 * Serve HTTP requests from the local VFS.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "handler.h"
#include "request.h"
#include "connection.h"
#include "header-writer.h"
#include "widget.h"
#include "embed.h"
#include "frame.h"
#include "http-util.h"
#include "proxy-widget.h"
#include "session.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *const copy_headers[] = {
    "age",
    "etag",
    "content-encoding",
    "content-language",
    "content-md5",
    "content-range",
    "content-type",
    "last-modified",
    "retry-after",
    "vary",
    NULL,
};

static const char *const copy_headers_processed[] = {
    "etag",
    "content-language",
    "content-type",
    "vary",
    NULL,
};


static void
proxy_transfer_close(struct request *request)
{
    pool_t pool;

    assert(request != NULL);
    assert(request->request != NULL);
    assert(request->request->pool != NULL);

    pool = request->request->pool;

    if (request->url_stream != NULL) {
        url_stream_t url_stream = request->url_stream;
        request->url_stream = NULL;
        url_stream_close(url_stream);
    }

    pool_unref(pool);
}

static const char *
request_absolute_uri(struct http_server_request *request)
{
    const char *host = strmap_get(request->headers, "host");

    if (host == NULL)
        return NULL;

    return p_strcat(request->pool,
                    "http://",
                    host,
                    request->uri,
                    NULL);
}

static void 
proxy_response_response(http_status_t status, strmap_t headers,
                        off_t content_length, istream_t body,
                        void *ctx)
{
    struct request *request2 = ctx;
    struct http_server_request *request = request2->request;
    growing_buffer_t response_headers;

    (void)status;

    assert(request2->url_stream != NULL);
    request2->url_stream = NULL;

    response_headers = growing_buffer_new(request->pool, 2048);

    if (request2->translate.response->process) {
        struct widget *widget;
        unsigned processor_options = 0;

        /* XXX request body? */
        processor_env_init(request->pool, &request2->env,
                           request_absolute_uri(request),
                           &request2->uri,
                           request2->args,
                           request2->session,
                           request->headers,
                           0, NULL,
                           embed_widget_callback);
        if (request2->env.frame != NULL) { /* XXX */
            request2->env.widget_callback = frame_widget_callback;

            /* do not show the template contents if the browser is
               only interested in one particular widget for
               displaying the frame */
            processor_options |= PROCESSOR_QUIET;
        }

        widget = p_malloc(request->pool, sizeof(*widget));
        widget_init(widget, NULL);
        widget->from_request.session = session_get_widget(request2->env.session, request2->uri.base, 1);

        pool_ref(request->pool);

        body = processor_new(request->pool, body, widget, &request2->env,
                             processor_options);
        if (request2->env.frame != NULL) {
            /* XXX */
            widget_proxy_install(&request2->env, request, body);
            pool_unref(request->pool);
            proxy_transfer_close(request2);
            return;
        }

#ifndef NO_DEFLATE
        if (http_client_accepts_encoding(request->headers, "deflate")) {
            header_write(response_headers, "content-encoding", "deflate");
            body = istream_deflate_new(request->pool, body);
        }
#endif

        pool_unref(request->pool);

        content_length = (off_t)-1;

        headers_copy(headers, response_headers, copy_headers_processed);
    } else {
        headers_copy(headers, response_headers, copy_headers);
    }

    assert(!istream_has_handler(body));

    http_server_response(request, HTTP_STATUS_OK,
                         response_headers,
                         content_length, body);
}

static void 
proxy_response_free(void *ctx)
{
    struct request *request = ctx;

    request->url_stream = NULL;

    proxy_transfer_close(request);
}

static const struct http_client_response_handler proxy_response_handler = {
    .response = proxy_response_response,
    .free = proxy_response_free,
};


void
proxy_callback(struct request *request2)
{
    struct http_server_request *request = request2->request;
    const struct translate_response *tr = request2->translate.response;

    pool_ref(request->pool);

    request2->url_stream = url_stream_new(request->pool,
                                          request->method, tr->proxy, NULL,
                                          request->content_length, request->body,
                                          &proxy_response_handler, request2);
    if (request2->url_stream == NULL) {
        proxy_transfer_close(request2);
        http_server_send_message(request,
                                 HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                 "Internal server error");
    }
}
