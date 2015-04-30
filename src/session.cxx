/*
 * Session management.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "session.hxx"
#include "cookie_jar.hxx"
#include "shm/dpool.hxx"
#include "shm/dbuffer.hxx"
#include "expiry.h"
#include "crash.hxx"
#include "expiry.h"

#include <daemon/log.h>

#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <event.h>

#define SESSION_TTL_NEW 120

inline
Session::Session(struct dpool *_pool)
    :pool(_pool),
     expires(expiry_touch(SESSION_TTL_NEW)),
     counter(1),
     is_new(true),
     cookie_sent(false), cookie_received(false),
     realm(nullptr),
     translate(nullptr),
     user(nullptr),
     user_expires(0),
     language(nullptr),
     cookies(cookie_jar_new(*_pool))
{
    lock_init(&lock);
}

inline
Session::Session(struct dpool *_pool, const Session &src)
    :pool(_pool),
     id(src.id),
     expires(src.expires),
     counter(1),
     is_new(src.is_new),
     cookie_sent(src.cookie_sent), cookie_received(src.cookie_received),
     realm(d_strdup(pool, src.realm)),
     translate(DupBuffer(pool, src.translate)),
     user(d_strdup_checked(pool, src.user)),
     user_expires(0),
     language(d_strdup_checked(pool, src.language)),
     cookies(src.cookies->Dup(*pool))
{
    lock_init(&lock);
}

inline
Session::~Session()
{
    lock_destroy(&lock);
}

Session *
session_allocate(struct dpool *pool)
{
    return NewFromPool<Session>(pool, pool);
}

void
session_destroy(Session *session)
{
    DeleteDestroyPool(*session->pool, session);
}

/**
 * Calculates the score for purging the session: higher score means
 * more likely to be purged.
 */
unsigned
session_purge_score(const Session *session)
{
    if (session->is_new)
        return 1000;

    if (!session->cookie_received)
        return 50;

    if (session->user == nullptr)
        return 20;

    return 1;
}

void
session_clear_translate(Session *session)
{
    assert(crash_in_unsafe());
    assert(session != nullptr);

    if (!session->translate.IsEmpty()) {
        d_free(session->pool, session->translate.data);
        session->translate = nullptr;
    }
}

void
session_clear_user(Session *session)
{
    assert(crash_in_unsafe());
    assert(session != nullptr);

    if (session->user != nullptr) {
        d_free(session->pool, session->user);
        session->user = nullptr;
    }
}

void
session_clear_language(Session *session)
{
    assert(crash_in_unsafe());
    assert(session != nullptr);

    if (session->language != nullptr) {
        d_free(session->pool, session->language);
        session->language = nullptr;
    }
}

bool
session_set_translate(Session *session, ConstBuffer<void> translate)
{
    assert(crash_in_unsafe());
    assert(session != nullptr);
    assert(!translate.IsNull());

    if (!session->translate.IsNull() &&
        session->translate.size == translate.size &&
        memcmp(session->translate.data, translate.data, translate.size) == 0)
        /* same value as before: no-op */
        return true;

    session_clear_translate(session);

    session->translate = DupBuffer(session->pool, translate);
    return !session->translate.IsNull();
}

bool
session_set_user(Session *session, const char *user, unsigned max_age)
{
    assert(crash_in_unsafe());
    assert(session != nullptr);
    assert(user != nullptr);

    if (session->user == nullptr || strcmp(session->user, user) != 0) {
        session_clear_user(session);

        session->user = d_strdup(session->pool, user);
        if (session->user == nullptr)
            return false;
    }

    if (max_age == (unsigned)-1)
        /* never expires */
        session->user_expires = 0;
    else if (max_age == 0)
        /* expires immediately, use only once */
        session->user_expires = 1;
    else
        session->user_expires = expiry_touch(max_age);

    return true;
}

bool
session_set_language(Session *session, const char *language)
{
    assert(crash_in_unsafe());
    assert(session != nullptr);
    assert(language != nullptr);

    if (session->language != nullptr && strcmp(session->language, language) == 0)
        /* same value as before: no-op */
        return true;

    session_clear_language(session);

    session->language = d_strdup(session->pool, language);
    return session->language != nullptr;
}

static WidgetSession::Set
widget_session_map_dup(struct dpool *pool, const WidgetSession::Set &src,
                       Session *session, WidgetSession *parent);

gcc_malloc
static WidgetSession *
widget_session_dup(struct dpool *pool, const WidgetSession *src,
                   Session *session)
{
    assert(crash_in_unsafe());
    assert(src != nullptr);
    assert(src->id != nullptr);

    auto *dest = NewFromPool<WidgetSession>(pool);
    if (dest == nullptr)
        return nullptr;

    dest->id = d_strdup(pool, src->id);
    if (dest->id == nullptr)
        return nullptr;

    dest->children = widget_session_map_dup(pool, src->children,
                                            session, dest);

    if (src->path_info != nullptr) {
        dest->path_info = d_strdup(pool, src->path_info);
        if (dest->path_info == nullptr)
            return nullptr;
    } else
        dest->path_info = nullptr;

    if (src->query_string != nullptr) {
        dest->query_string = d_strdup(pool, src->query_string);
        if (dest->query_string == nullptr)
            return nullptr;
    } else
        dest->query_string = nullptr;

    return dest;
}

static WidgetSession::Set
widget_session_map_dup(struct dpool *pool, const WidgetSession::Set &src,
                       Session *session, WidgetSession *parent)
{
    assert(crash_in_unsafe());

    WidgetSession::Set dest;

    for (const auto &src_ws : src) {
        WidgetSession *dest_ws = widget_session_dup(pool, &src_ws, session);
        if (dest_ws == nullptr)
            break;

        dest_ws->parent = parent;
        dest_ws->session = session;

        dest.insert(*dest_ws);
    }

    return std::move(dest);
}

Session *
session_dup(struct dpool *pool, const Session *src)
{
    assert(crash_in_unsafe());

    auto *dest = NewFromPool<Session>(pool, pool, *src);
    if (dest == nullptr)
        return nullptr;

    dest->widgets = widget_session_map_dup(pool, src->widgets, dest, nullptr);
    return dest;
}

WidgetSession *
widget_session_allocate(Session *session)
{
    auto *ws = NewFromPool<WidgetSession>(session->pool);
    if (ws == nullptr)
        return nullptr;

    ws->session = session;
    return ws;
}

static WidgetSession *
hashmap_r_get_widget_session(Session *session, WidgetSession::Set &set,
                             const char *id, bool create)
{
    assert(crash_in_unsafe());
    assert(session != nullptr);
    assert(lock_is_locked(&session->lock));
    assert(id != nullptr);

    auto i = set.find(id, WidgetSession::Compare());
    if (i != set.end())
        return &*i;

    if (!create)
        return nullptr;

    auto *ws = widget_session_allocate(session);
    if (ws == nullptr)
        return nullptr;

    ws->parent = nullptr;
    ws->id = d_strdup(session->pool, id);
    if (ws->id == nullptr) {
        DeleteFromPool(session->pool, ws);
        return nullptr;
    }

    ws->path_info = nullptr;
    ws->query_string = nullptr;

    set.insert(*ws);
    return ws;
}

WidgetSession *
session_get_widget(Session *session, const char *id, bool create)
{
    assert(crash_in_unsafe());
    assert(session != nullptr);
    assert(id != nullptr);

    return hashmap_r_get_widget_session(session, session->widgets, id,
                                        create);
}

WidgetSession *
widget_session_get_child(WidgetSession *parent,
                         const char *id,
                         bool create)
{
    assert(crash_in_unsafe());
    assert(parent != nullptr);
    assert(parent->session != nullptr);
    assert(id != nullptr);

    return hashmap_r_get_widget_session(parent->session, parent->children,
                                        id, create);
}

static void
widget_session_free(struct dpool *pool, WidgetSession *ws)
{
    assert(crash_in_unsafe());

    d_free(pool, ws->id);

    if (ws->path_info != nullptr)
        d_free(pool, ws->path_info);

    if (ws->query_string != nullptr)
        d_free(pool, ws->query_string);

    DeleteFromPool(pool, ws);
}

static void
widget_session_clear_map(struct dpool *pool, WidgetSession::Set &set)
{
    assert(crash_in_unsafe());
    assert(pool != nullptr);

    set.clear_and_dispose([pool](WidgetSession *ws){
            widget_session_delete(pool, ws);
        });
}

void
widget_session_delete(struct dpool *pool, WidgetSession *ws)
{
    assert(crash_in_unsafe());
    assert(pool != nullptr);
    assert(ws != nullptr);

    widget_session_clear_map(pool, ws->children);

    widget_session_free(pool, ws);
}

void
session_delete_widgets(Session *session)
{
    assert(crash_in_unsafe());
    assert(session != nullptr);

    widget_session_clear_map(session->pool, session->widgets);
}
