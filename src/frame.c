/*
 * Pick the output of a single widget for displaying it in an IFRAME.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "embed.h"
#include "widget-http.h"
#include "processor.h"
#include "widget.h"
#include "widget-resolver.h"
#include "global.h"

#include <daemon/log.h>

#include <assert.h>

struct frame_class_looup {
    pool_t pool;
    struct processor_env *env;
    struct widget *widget;

    struct http_response_handler_ref handler;
    struct async_operation_ref *async_ref;
};

static inline GQuark
widget_quark(void)
{
    return g_quark_from_static_string("widget");
}

static void
frame_class_lookup_callback(void *ctx)
{
    struct frame_class_looup *fcl = ctx;

    if (fcl->widget->class == NULL) {
        GError *error =
            g_error_new(widget_quark(), 0,
                        "lookup of widget class '%s' for '%s' failed",
                        fcl->widget->class_name, widget_path(fcl->widget));
        http_response_handler_invoke_abort(&fcl->handler, error);
        return;
    }

    embed_frame_widget(fcl->pool, fcl->env, fcl->widget,
                       fcl->handler.handler, fcl->handler.ctx,
                       fcl->async_ref);
}

static void
frame_top_widget(pool_t pool, struct processor_env *env,
                 struct widget *widget,
                 const struct http_response_handler *handler,
                 void *handler_ctx,
                 struct async_operation_ref *async_ref)
{
    assert(widget->from_request.proxy);

    if (!widget_check_host(widget, env->untrusted_host)) {
        daemon_log(4, "untrusted host name mismatch\n");
        http_response_handler_direct_message(handler, handler_ctx,
                                             pool, HTTP_STATUS_FORBIDDEN,
                                             "Forbidden");
        return;
    }

    if (widget->class->stateful) {
        struct session *session = session_get(env->session_id);
        if (session != NULL) {
            widget_sync_session(widget, session);
            session_put(session);
        }
    }

    widget_http_request(pool, widget, env,
                        handler, handler_ctx, async_ref);
}

static void
frame_parent_widget(pool_t pool, struct processor_env *env,
                    struct widget *widget,
                    const struct http_response_handler *handler,
                    void *handler_ctx,
                    struct async_operation_ref *async_ref)
{
    if (!widget_class_is_container(widget->class,
                                   widget_get_view_name(widget))) {
        /* this widget cannot possibly be the parent of a framed
           widget if it is not a container */

        if (env->request_body != NULL)
            istream_free(&env->request_body);

        GError *error =
            g_error_new(widget_quark(), 0,
                        "frame within non-container requested");
        http_response_handler_direct_abort(handler, handler_ctx, error);
        return;
    }

    if (widget->class->stateful) {
        struct session *session = session_get(env->session_id);
        if (session != NULL) {
            widget_sync_session(widget, session);
            session_put(session);
        }
    }

    if (env->request_body != NULL && widget->from_request.focus_ref == NULL) {
        /* the request body is not consumed yet, but the focus is not
           within the frame: discard the body, because it cannot ever
           be used */
        assert(!istream_has_handler(env->request_body));

        daemon_log(4, "discarding non-framed request body\n");

        istream_free(&env->request_body);
    }

    widget_http_request(pool, widget, env,
                        handler, handler_ctx, async_ref);
}

void
embed_frame_widget(pool_t pool, struct processor_env *env,
                   struct widget *widget,
                   const struct http_response_handler *handler,
                   void *handler_ctx,
                   struct async_operation_ref *async_ref)
{
    assert(pool != NULL);
    assert(env != NULL);
    assert(widget != NULL);
    assert(widget->from_request.proxy || widget->from_request.proxy_ref != NULL);

    if (widget->class == NULL) {
        struct frame_class_looup *fcl = p_malloc(pool, sizeof(*fcl));
        fcl->pool = pool;
        fcl->env = env;
        fcl->widget = widget;
        http_response_handler_set(&fcl->handler, handler, handler_ctx);
        fcl->async_ref = async_ref;
        widget_resolver_new(pool, env->pool, widget,
                            global_translate_cache,
                            frame_class_lookup_callback, fcl, async_ref);
        return;
    }

    if (widget->from_request.proxy)
        /* this widget is being proxied */
        frame_top_widget(pool, env, widget,
                         handler, handler_ctx, async_ref);
    else
        /* only partial match: this is the parent of the frame
           widget */
        frame_parent_widget(pool, env, widget,
                            handler, handler_ctx, async_ref);
}
