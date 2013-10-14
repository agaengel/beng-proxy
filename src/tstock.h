/*
 * Connection pooling for the translation server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_TSTOCK_H
#define BENG_PROXY_TSTOCK_H

struct pool;
struct tstock;
struct hstock;
struct translate_handler;
struct translate_request;
struct async_operation_ref;

#ifdef __cplusplus
extern "C" {
#endif

struct tstock *
tstock_new(struct pool *pool, struct hstock *tcp_stock, const char *socket_path);

void
tstock_translate(struct tstock *stock, struct pool *pool,
                 const struct translate_request *request,
                 const struct translate_handler *handler, void *ctx,
                 struct async_operation_ref *async_ref);

#ifdef __cplusplus
}
#endif

#endif
