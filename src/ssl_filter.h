/*
 * SSL/TLS filter.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_SSL_FILTER_H
#define BENG_PROXY_SSL_FILTER_H

#include "ssl_quark.h"

#include <openssl/ssl.h>

struct pool;
struct notify;
struct ssl_config;
struct ssl_filter;

/**
 * Create a new SSL filter.  It is run in a new thread.
 *
 * @param encrypted_fd the encrypted side of the filter
 * @param plain_fd the plain-text side of the filter (socketpair
 * to local service)
 */
struct ssl_filter *
ssl_filter_new(struct pool *pool, SSL_CTX *ssl_ctx,
               int encrypted_fd, int plain_fd,
               struct notify *notify,
               GError **error_r);

void
ssl_filter_free(struct ssl_filter *ssl);

#endif
