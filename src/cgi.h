/*
 * Run a CGI script.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_CGI_H
#define __BENG_CGI_H

#include <http/method.h>

#include <glib.h>

struct pool;
struct istream;
struct strmap;
struct http_response_handler;
struct async_operation_ref;
struct jail_params;

G_GNUC_CONST
static inline GQuark
cgi_quark(void)
{
    return g_quark_from_static_string("cgi");
}

/**
 * @param params environment variables for the child process
 */
void
cgi_new(struct pool *pool, const struct jail_params *jail,
        const char *interpreter, const char *action,
        const char *path,
        http_method_t method, const char *uri,
        const char *script_name, const char *path_info,
        const char *query_string,
        const char *document_root,
        const char *remote_addr,
        struct strmap *headers, struct istream *body,
        const char *const params[], unsigned num_params,
        const struct http_response_handler *handler,
        void *handler_ctx,
        struct async_operation_ref *async_ref);

#endif
