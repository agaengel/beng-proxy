/*
 * High level FastCGI client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_FCGI_REQUEST_HXX
#define BENG_PROXY_FCGI_REQUEST_HXX

#include <http/method.h>

struct pool;
class EventLoop;
class Istream;
struct FcgiStock;
struct StringMap;
struct http_response_handler;
struct async_operation_ref;
struct ChildOptions;
template<typename T> struct ConstBuffer;

/**
 * @param jail run the FastCGI application with JailCGI?
 * @param args command-line arguments
 */
void
fcgi_request(struct pool *pool, EventLoop &event_loop,
             FcgiStock *fcgi_stock,
             const ChildOptions &options,
             const char *action,
             const char *path,
             ConstBuffer<const char *> args,
             http_method_t method, const char *uri,
             const char *script_name, const char *path_info,
             const char *query_string,
             const char *document_root,
             const char *remote_addr,
             StringMap *headers, Istream *body,
             ConstBuffer<const char *> params,
             int stderr_fd,
             const struct http_response_handler *handler,
             void *handler_ctx,
             struct async_operation_ref *async_ref);

#endif
