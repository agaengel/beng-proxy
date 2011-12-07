/*
 * Widget declarations.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_WIDGET_REQUEST_H
#define BENG_PROXY_WIDGET_REQUEST_H

#include <glib.h>

#include <stdbool.h>
#include <stddef.h>

struct pool;
struct widget;
struct processor_env;
struct session;

/**
 * Copy parameters from the request to the widget.
 */
bool
widget_copy_from_request(struct widget *widget, struct processor_env *env,
                         GError **error_r);

/**
 * Synchronize the widget with its session.
 */
void
widget_sync_session(struct widget *widget, struct session *session);

/**
 * Overwrite request data, copy values from a HTTP redirect location.
 */
void
widget_copy_from_location(struct widget *widget, struct session *session,
                          const char *location, size_t location_length,
                          struct pool *pool);

#endif
