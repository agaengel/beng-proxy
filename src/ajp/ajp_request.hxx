/*
 * High level AJP client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_AJP_REQUEST_HXX
#define BENG_PROXY_AJP_REQUEST_HXX

#include <http/method.h>

struct pool;
class EventLoop;
class Istream;
struct TcpBalancer;
struct HttpAddress;
struct StringMap;
struct http_response_handler;
struct async_operation_ref;

/**
 * @param session_sticky a portion of the session id that is used to
 * select the worker; 0 means disable stickiness
 */
void
ajp_stock_request(struct pool *pool, EventLoop &event_loop,
                  TcpBalancer *tcp_balancer,
                  unsigned session_sticky,
                  const char *protocol, const char *remote_addr,
                  const char *remote_host, const char *server_name,
                  unsigned server_port, bool is_ssl,
                  http_method_t method,
                  const HttpAddress *uwa,
                  StringMap *headers, Istream *body,
                  const struct http_response_handler *handler,
                  void *handler_ctx,
                  struct async_operation_ref *async_ref);

#endif
