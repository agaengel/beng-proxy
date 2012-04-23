/*
 * Write a session to a file.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "session_read.h"
#include "session_file.h"
#include "session.h"
#include "dhashmap.h"
#include "dpool.h"
#include "cookie_jar.h"

#include <stdint.h>

static bool
read_8(FILE *file, uint8_t *value_r)
{
    return fread(value_r, sizeof(*value_r), 1, file) == 1;
}

static bool
read_bool(FILE *file, bool *value_r)
{
    uint8_t t;
    if (!read_8(file, &t))
        return false;
    *value_r = t != 0;
    return true;
}

static bool
read_16(FILE *file, uint16_t *value_r)
{
    return fread(value_r, sizeof(*value_r), 1, file) == 1;
}

static bool
read_32(FILE *file, uint32_t *value_r)
{
    return fread(value_r, sizeof(*value_r), 1, file) == 1;
}

static bool
read_64(FILE *file, uint64_t *value_r)
{
    return fread(value_r, sizeof(*value_r), 1, file) == 1;
}

static bool
read_time(FILE *file, time_t *value_r)
{
    uint64_t t;
    if (!read_64(file, &t))
        return false;

    *value_r = (time_t)t;
    return true;
}

static bool
read_buffer(FILE *file, void *data, size_t length)
{
    return fread(data, 1, length, file) == length;
}

static bool
read_session_id(FILE *file, session_id_t *id)
{
    return read_buffer(file, id, sizeof(*id));
}

static bool
read_string(FILE *file, struct dpool *pool, char **s_r)
{
    assert(pool != NULL);
    assert(s_r != NULL);

    uint16_t length;
    if (!read_16(file, &length))
        return false;

    if (length == (uint16_t)-1) {
        *s_r = NULL;
        return true;
    }

    char *s = d_malloc(pool, length + 1);
    if (s == NULL)
        return false;

    if (!read_buffer(file, s, length))
        return false;

    s[length] = 0;
    *s_r = s;
    return true;
}

static bool
read_string_const(FILE *file, struct dpool *pool, const char **s_r)
{
    char *s;
    if (!read_string(file, pool, &s))
        return false;

    *s_r = s;
    return true;
}

static bool
read_strref(FILE *file, struct dpool *pool, struct strref *s)
{
    assert(pool != NULL);
    assert(s != NULL);

    uint16_t length;
    if (!read_16(file, &length))
        return false;

    if (length == (uint16_t)-1) {
        strref_null(s);
        return true;
    }

    if (length == 0) {
        strref_set_empty(s);
        return true;
    }

    char *p = d_malloc(pool, length);
    if (p == NULL)
        return false;

    if (!read_buffer(file, p, length))
        return false;

    strref_set(s, p, length);
    return true;
}

static bool
expect_32(FILE *file, uint32_t expected)
{
    uint32_t value;

    return read_32(file, &value) && value == expected;
}

uint32_t
session_read_magic(FILE *file)
{
    uint32_t magic;
    return read_32(file, &magic) ? magic : 0;
}

bool
session_read_file_header(FILE *file)
{
    const struct session *session = NULL;

    return expect_32(file, MAGIC_FILE) && expect_32(file, sizeof(*session));
}

static bool
read_widget_sessions(FILE *file, struct session *session,
                     struct widget_session *parent,
                     struct dhashmap **widgets_r);

static bool
do_read_widget_session(FILE *file, struct session *session,
                       struct widget_session *ws)
{
    struct dpool *pool = session->pool;

    return read_string_const(file, pool, &ws->id) &&
        read_widget_sessions(file, session, ws, &ws->children) &&
        read_string(file, pool, &ws->path_info) &&
        read_string(file, pool, &ws->query_string) &&
        expect_32(file, MAGIC_END_OF_RECORD);
}

static struct widget_session *
read_widget_session(FILE *file, struct session *session)
{
    struct widget_session *ws = widget_session_allocate(session);
    if (ws == NULL)
        return NULL;

    if (!do_read_widget_session(file, session, ws))
        return NULL;

    return ws;
}

static bool
read_widget_sessions(FILE *file, struct session *session,
                     struct widget_session *parent,
                     struct dhashmap **widgets_r)
{
    struct dhashmap *widgets = NULL;

    while (true) {
        uint32_t magic;
        if (!read_32(file, &magic))
            return false;

        if (magic == MAGIC_END_OF_LIST) {
            *widgets_r = widgets;
            return true;
        } else if (magic != MAGIC_WIDGET_SESSION)
            return false;

        struct widget_session *ws = read_widget_session(file, session);
        if (ws == NULL)
            return false;

        ws->parent = parent;

        if (widgets == NULL) {
            widgets = dhashmap_new(session->pool, 17);
            if (widgets == NULL)
                return false;
        }

        dhashmap_put(widgets, ws->id, ws);
    }
}

static bool
do_read_cookie(FILE *file, struct dpool *pool, struct cookie *cookie)
{
    assert(cookie != NULL);

    return read_strref(file, pool, &cookie->name) &&
        read_strref(file, pool, &cookie->value) &&
        read_string_const(file, pool, &cookie->domain) &&
        read_string_const(file, pool, &cookie->path) &&
        read_time(file, &cookie->expires) &&
        expect_32(file, MAGIC_END_OF_RECORD);
}

static struct cookie *
read_cookie(FILE *file, struct dpool *pool)
{
    struct cookie *cookie = d_malloc(pool, sizeof(*cookie));
    if (cookie == NULL || !do_read_cookie(file, pool, cookie))
        return NULL;

    return cookie;
}

static bool
read_cookie_jar(FILE *file, struct dpool *pool, struct cookie_jar *jar)
{
    while (true) {
        uint32_t magic;
        if (!read_32(file, &magic))
            return false;

        if (magic == MAGIC_END_OF_LIST)
            return true;
        else if (magic != MAGIC_COOKIE)
            return false;

        struct cookie *cookie = read_cookie(file, pool);
        if (cookie == NULL)
            return false;

        cookie_jar_add(jar, cookie);
    }
}

static bool
do_read_session(FILE *file, struct dpool *pool, struct session *session)
{
    assert(session != NULL);

    session->cookies = cookie_jar_new(pool);
    if (session->cookies == NULL)
        return false;

    return read_session_id(file, &session->id) &&
        read_time(file, &session->expires) &&
        read_32(file, &session->counter) &&
        read_bool(file, &session->new) &&
        read_bool(file, &session->cookie_sent) &&
        read_bool(file, &session->cookie_received) &&
        read_string_const(file, pool, &session->realm) &&
        read_string_const(file, pool, &session->translate) &&
        read_string_const(file, pool, &session->user) &&
        read_time(file, &session->user_expires) &&
        read_string_const(file, pool, &session->language) &&
        read_widget_sessions(file, session, NULL, &session->widgets) &&
        read_cookie_jar(file, pool, session->cookies) &&
        expect_32(file, MAGIC_END_OF_RECORD);
}

struct session *
session_read(FILE *file, struct dpool *pool)
{
    struct session *session = session_allocate(pool);
    if (session == NULL || !do_read_session(file, pool, session))
        return NULL;

    return session;
}
