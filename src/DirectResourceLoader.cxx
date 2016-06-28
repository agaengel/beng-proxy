/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "DirectResourceLoader.hxx"
#include "ResourceAddress.hxx"
#include "filtered_socket.hxx"
#include "http_request.hxx"
#include "http_response.hxx"
#include "file_request.hxx"
#include "file_address.hxx"
#include "lhttp_request.hxx"
#include "http_address.hxx"
#include "http_headers.hxx"
#include "cgi/cgi_glue.hxx"
#include "cgi_address.hxx"
#include "fcgi/Request.hxx"
#include "fcgi/Remote.hxx"
#include "nfs_address.hxx"
#include "was/was_glue.hxx"
#include "ajp/ajp_request.hxx"
#include "header_writer.hxx"
#include "pipe_filter.hxx"
#include "delegate/Address.hxx"
#include "delegate/HttpRequest.hxx"
#include "strmap.hxx"
#include "istream/istream.hxx"
#include "ssl/ssl_client.hxx"
#include "gerrno.h"
#include "pool.hxx"
#include "util/ConstBuffer.hxx"

#ifdef HAVE_LIBNFS
#include "nfs_request.hxx"
#endif

#include <socket/parser.h>

#include <string.h>
#include <stdlib.h>

class SslSocketFilterFactory final : public SocketFilterFactory {
    struct pool &pool;
    EventLoop &event_loop;
    const char *const host;

public:
    SslSocketFilterFactory(struct pool &_pool,
                           EventLoop &_event_loop,
                           const char *_host)
        :pool(_pool), event_loop(_event_loop), host(_host) {}

    void *CreateFilter(GError **error_r) override {
        return ssl_client_create(&pool, event_loop, host, error_r);
    }
};

static inline GQuark
resource_loader_quark(void)
{
    return g_quark_from_static_string("resource_loader");
}

static const char *
extract_remote_addr(const StringMap *headers)
{
    const char *xff = strmap_get_checked(headers, "x-forwarded-for");
    if (xff == nullptr)
        return nullptr;

    /* extract the last host name in X-Forwarded-For */
    const char *p = strrchr(xff, ',');
    if (p == nullptr)
        p = xff;
    else
        ++p;

    while (*p == ' ')
        ++p;

    return p;
}

static const char *
extract_remote_ip(struct pool *pool, const StringMap *headers)
{
    const char *p = extract_remote_addr(headers);
    if (p == nullptr)
        return p;

    size_t length;
    const char *endptr;
    const char *q = socket_extract_hostname(p, &length, &endptr);
    if (q == p && length == strlen(p))
        return p;

    return p_strndup(pool, q, length);
}

static const char *
extract_server_name(struct pool *pool, const StringMap *headers,
                    unsigned *port_r)
{
    const char *p = strmap_get_checked(headers, "host");
    if (p == nullptr)
        return nullptr;

    const char *colon = strchr(p, ':');
    if (colon == nullptr)
        return p;

    if (strchr(colon + 1, ':') != nullptr)
        /* XXX handle IPv6 addresses properly */
        return p;

    char *endptr;
    unsigned port = strtoul(colon + 1, &endptr, 10);
    if (endptr > colon + 1 && *endptr == 0)
        *port_r = port;

    return p_strndup(pool, p, colon - p);
}

void
DirectResourceLoader::SendRequest(struct pool &pool,
                                  unsigned session_sticky,
                                  http_method_t method,
                                  const ResourceAddress &address,
                                  http_status_t status, StringMap &&headers,
                                  Istream *body,
                                  gcc_unused const char *body_etag,
                                  const struct http_response_handler &handler,
                                  void *handler_ctx,
                                  struct async_operation_ref &async_ref)
{
    switch (address.type) {
        const FileAddress *file;
        const CgiAddress *cgi;
#ifdef HAVE_LIBNFS
        const NfsAddress *nfs;
#endif
        int stderr_fd;
        const char *server_name;
        unsigned server_port;
        const SocketFilter *filter;
        SocketFilterFactory *filter_factory;

    case ResourceAddress::Type::NONE:
        break;

    case ResourceAddress::Type::LOCAL:
        if (body != nullptr)
            /* static files cannot receive a request body, close it */
            body->CloseUnused();

        file = &address.GetFile();
        if (file->delegate != nullptr) {
            if (delegate_stock == nullptr) {
                GError *error = g_error_new_literal(resource_loader_quark(), 0,
                                                    "No delegate stock");
                handler.InvokeAbort(handler_ctx, error);
                return;
            }

            delegate_stock_request(event_loop, *delegate_stock, pool,
                                   file->delegate->delegate,
                                   file->delegate->child_options,
                                   file->path,
                                   file->content_type,
                                   &handler, handler_ctx,
                                   async_ref);
            return;
        }

        static_file_get(event_loop, pool, file->path,
                        file->content_type,
                        &handler, handler_ctx);
        return;

    case ResourceAddress::Type::NFS:
#ifdef HAVE_LIBNFS
        nfs = &address.GetNfs();
        if (body != nullptr)
            /* NFS files cannot receive a request body, close it */
            body->CloseUnused();

        nfs_request(pool, *nfs_cache,
                    nfs->server, nfs->export_name,
                    nfs->path, nfs->content_type,
                    &handler, handler_ctx, &async_ref);
#else
        handler.InvokeAbort(handler_ctx,
                            g_error_new_literal(resource_loader_quark(), 0,
                                                "libnfs disabled"));
#endif
        return;

    case ResourceAddress::Type::PIPE:
        cgi = &address.GetCgi();
        pipe_filter(spawn_service, event_loop, &pool,
                    cgi->path, cgi->args,
                    cgi->options,
                    status, std::move(headers), body,
                    &handler, handler_ctx);
        return;

    case ResourceAddress::Type::CGI:
        cgi_new(spawn_service, event_loop, &pool,
                method, &address.GetCgi(),
                extract_remote_ip(&pool, &headers),
                &headers, body,
                &handler, handler_ctx, &async_ref);
        return;

    case ResourceAddress::Type::FASTCGI:
        cgi = &address.GetCgi();

        if (cgi->options.stderr_path != nullptr) {
            stderr_fd = cgi->options.OpenStderrPath();
            if (stderr_fd < 0) {
                int code = errno;
                GError *error =
                    g_error_new(errno_quark(), code, "open('%s') failed: %s",
                                cgi->options.stderr_path,
                                g_strerror(code));
                handler.InvokeAbort(handler_ctx, error);
                return;
            }
        } else
            stderr_fd = -1;

        if (cgi->address_list.IsEmpty())
            fcgi_request(&pool, event_loop, fcgi_stock,
                         cgi->options,
                         cgi->action,
                         cgi->path,
                         cgi->args,
                         method, cgi->GetURI(&pool),
                         cgi->script_name,
                         cgi->path_info,
                         cgi->query_string,
                         cgi->document_root,
                         extract_remote_ip(&pool, &headers),
                         &headers, body,
                         cgi->params,
                         stderr_fd,
                         &handler, handler_ctx, &async_ref);
        else
            fcgi_remote_request(&pool, event_loop, tcp_balancer,
                                &cgi->address_list,
                                cgi->path,
                                method, cgi->GetURI(&pool),
                                cgi->script_name,
                                cgi->path_info,
                                cgi->query_string,
                                cgi->document_root,
                                extract_remote_ip(&pool, &headers),
                                &headers, body,
                                cgi->params,
                                stderr_fd,
                                &handler, handler_ctx, &async_ref);
        return;

    case ResourceAddress::Type::WAS:
        cgi = &address.GetCgi();
        was_request(pool, *was_stock, cgi->options,
                    cgi->action,
                    cgi->path,
                    cgi->args,
                    method, cgi->GetURI(&pool),
                    cgi->script_name,
                    cgi->path_info,
                    cgi->query_string,
                    headers, body,
                    cgi->params,
                    handler, handler_ctx, async_ref);
        return;

    case ResourceAddress::Type::HTTP:
        switch (address.GetHttp().protocol) {
        case HttpAddress::Protocol::HTTP:
            if (address.GetHttp().ssl) {
                filter = &ssl_client_get_filter();
                filter_factory = NewFromPool<SslSocketFilterFactory>(pool, pool,
                                                                     event_loop,
                                                                     /* TODO: only host */
                                                                     address.GetHttp().host_and_port);
            } else {
                filter = nullptr;
                filter_factory = nullptr;
            }

            http_request(pool, event_loop, *tcp_balancer, session_sticky,
                         filter, filter_factory,
                         method, address.GetHttp(),
                         HttpHeaders(headers), body,
                         handler, handler_ctx, async_ref);
            break;

        case HttpAddress::Protocol::AJP:
            server_port = 80;
            server_name = extract_server_name(&pool, &headers, &server_port);
            ajp_stock_request(pool, event_loop, *tcp_balancer,
                              session_sticky,
                              "http", extract_remote_ip(&pool, &headers),
                              nullptr,
                              server_name, server_port,
                              false,
                              method, address.GetHttp(),
                              headers, body,
                              handler, handler_ctx, async_ref);
            break;
        }

        return;

    case ResourceAddress::Type::LHTTP:
        lhttp_request(pool, event_loop, *lhttp_stock,
                      address.GetLhttp(),
                      method, HttpHeaders(headers), body,
                      handler, handler_ctx, async_ref);
        return;
    }

    /* the resource could not be located, abort the request */

    if (body != nullptr)
        body->CloseUnused();

    GError *error = g_error_new_literal(resource_loader_quark(), 0,
                                        "Could not locate resource");
    handler.InvokeAbort(handler_ctx, error);
}
