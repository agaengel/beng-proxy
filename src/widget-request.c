/*
 * Copy parameters from a request to the widget object.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "widget.h"
#include "session.h"
#include "processor.h"
#include "uri.h"
#include "args.h"

#include <string.h>
#include <assert.h>

static void
widget_to_session(struct widget_session *ws, const struct widget *widget)
{
    ws->path_info = widget->from_request.path_info == NULL
        ? NULL
        : p_strdup(ws->pool, widget->from_request.path_info);

    ws->query_string = strref_is_empty(&widget->from_request.query_string)
        ? NULL
        : strref_dup(ws->pool, &widget->from_request.query_string);
}

static void
session_to_widget(struct widget *widget, const struct widget_session *ws)
{
    widget->from_request.path_info = ws->path_info;

    if (ws->query_string != NULL)
        strref_set_c(&widget->from_request.query_string, ws->query_string);
}

void
widget_copy_from_request(struct widget *widget, struct processor_env *env)
{
    struct widget_session *ws;

    assert(widget != NULL);
    assert(widget->real_uri == NULL);
    assert(widget->from_request.path_info == NULL);
    assert(widget->from_request.path_info == NULL);
    assert(strref_is_empty(&widget->from_request.query_string));
    assert(widget->from_request.proxy_ref == NULL);
    assert(widget->from_request.focus_ref == NULL);
    assert(widget->from_request.method == HTTP_METHOD_GET);
    assert(widget->from_request.body == NULL);
    assert(!widget->from_request.proxy);

    /* is this widget being proxied? */

    if (widget->id != NULL && widget->parent != NULL &&
        widget->parent->from_request.proxy_ref != NULL &&
        strcmp(widget->id, widget->parent->from_request.proxy_ref->id) == 0) {
        widget->from_request.proxy_ref = widget->parent->from_request.proxy_ref->next;

        if (widget->from_request.proxy_ref == NULL)
            widget->from_request.proxy = 1;
        else
            widget->parent->from_request.proxy_ref = NULL;
    }

    /* are we focused? */

    if (widget->id != NULL && widget->parent != NULL &&
        widget->parent->from_request.focus_ref != NULL &&
        strcmp(widget->id, widget->parent->from_request.focus_ref->id) == 0 &&
        widget->parent->from_request.focus_ref->next == NULL) {
        /* we're in focus.  forward query string and request body. */
        widget->from_request.path_info = strmap_remove(env->args, "path");
        widget->from_request.query_string = env->external_uri->query;

        if (env->request_body != NULL) {
            /* XXX which method? */
            widget->from_request.method = HTTP_METHOD_POST;
            widget->from_request.body = env->request_body;
            env->request_body = NULL;
        }

        /* store query string in session */

        ws = widget_get_session(widget, 1);
        if (ws != NULL)
            widget_to_session(ws, widget);
    } else if (widget->id != NULL && widget->parent != NULL &&
               widget->parent->from_request.focus_ref != NULL &&
               strcmp(widget->id, widget->parent->from_request.focus_ref->id) == 0 &&
               widget->parent->from_request.focus_ref->next != NULL) {
        /* we are the parent (or grant-parent) of the focused widget.
           store the relative focus_ref. */

        widget->from_request.focus_ref = widget->parent->from_request.focus_ref->next;
        widget->parent->from_request.focus_ref = NULL;
        
        /* get query string from session */

        ws = widget_get_session(widget, 0);
        if (ws != NULL)
            session_to_widget(widget, ws);
    } else {
        /* get query string from session */

        ws = widget_get_session(widget, 0);
        if (ws != NULL)
            session_to_widget(widget, ws);
    }

    if (widget->from_request.path_info == NULL)
        widget->from_request.path_info = widget->path_info;

    assert(widget->from_request.path_info != NULL);
}

void
widget_copy_from_location(struct widget *widget, const char *location,
                          pool_t pool)
{
    struct widget_session *ws;
    const char *qmark;

    assert(widget != NULL);
    assert(widget != NULL);

    widget->from_request.method = HTTP_METHOD_GET;
    widget->from_request.body = NULL;

    qmark = strchr(location, '?');
    if (qmark == NULL) {
        widget->from_request.path_info = location;
        strref_clear(&widget->from_request.query_string);
    } else {
        widget->from_request.path_info
            = p_strndup(pool, location, qmark - location);
        strref_set_c(&widget->from_request.query_string,
                     qmark + 1);
    }

    ws = widget_get_session(widget, 1);
    if (ws != NULL)
        widget_to_session(ws, widget);
}
