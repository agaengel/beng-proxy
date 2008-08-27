/*
 * Filter a resource through an HTTP server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_FILTER_H
#define __BENG_FILTER_H

#include "pool.h"
#include "istream.h"

struct http_cache;
struct hstock;
struct fcgi_stock;
struct resource_address;
struct growing_buffer;
struct http_response_handler;
struct async_operation_ref;

void
filter_new(struct http_cache *cache,
           struct hstock *ajp_client_stock,
           struct fcgi_stock *fcgi_stock,
           pool_t pool,
           const struct resource_address *address,
           struct growing_buffer *headers,
           istream_t body,
           const struct http_response_handler *handler,
           void *handler_ctx,
           struct async_operation_ref *async_ref);

#endif
