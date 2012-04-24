/*
 * Manage cookies sent by the widget server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "cookie_client.h"
#include "cookie_jar.h"
#include "strref2.h"
#include "strref-pool.h"
#include "strref-dpool.h"
#include "strmap.h"
#include "http-string.h"
#include "tpool.h"
#include "dpool.h"
#include "expiry.h"

#include <inline/list.h>

#include <stdlib.h>
#include <string.h>

static bool
domain_matches(const char *domain, const char *match)
{
    size_t domain_length = strlen(domain);
    size_t match_length = strlen(match);

    return domain_length >= match_length &&
        strcasecmp(domain + domain_length - match_length, match) == 0 &&
        (domain_length == match_length || /* "a.b" matches "a.b" */
         match[0] == '.' || /* "a.b" matches ".b" */
         /* "a.b" matches "b" (implicit dot according to RFC 2965
            3.2.2): */
         (domain_length > match_length &&
          domain[domain_length - match_length - 1] == '.'));
}

static bool
path_matches(const char *path, const char *match)
{
    return match == NULL || memcmp(path, match, strlen(match)) == 0;
}

static void
cookie_list_delete_match(struct dpool *dpool, struct list_head *head,
                         const char *domain, const char *path,
                         const struct strref *name)
{
    struct cookie *cookie, *next;

    assert(domain != NULL);

    for (cookie = (struct cookie *)head->next;
         &cookie->siblings != head; cookie = next) {
        next = (struct cookie *)cookie->siblings.next;

        if (domain_matches(domain, cookie->domain) &&
            (cookie->path == NULL
             ? path == NULL
             : path_matches(cookie->path, path)) &&
            strref_cmp2(&cookie->name, name) == 0) {
            list_remove(&cookie->siblings);
            cookie_free(dpool, cookie);
        }
    }
}

static struct cookie *
parse_next_cookie(struct dpool *pool, struct strref *input)
{
    struct strref name, value;

    http_next_name_value(tpool, input, &name, &value, false);
    if (strref_is_empty(&name))
        return NULL;

    struct cookie *cookie = d_malloc(pool, sizeof(*cookie));
    if (cookie == NULL)
        /* out of memory */
        return NULL;

    strref_set_dup_d(pool, &cookie->name, &name);
    if (strref_is_empty(&cookie->name)) {
        /* out of memory */
        d_free(pool, cookie);
        return NULL;
    }

    strref_set_dup_d(pool, &cookie->value, &value);
    if (strref_is_empty(&cookie->value)) {
        /* out of memory */
        strref_free_d(pool, &cookie->name);
        d_free(pool, cookie);
        return NULL;
    }

    cookie->domain = NULL;
    cookie->path = NULL;
    cookie->expires = 0;

    strref_ltrim(input);
    while (!strref_is_empty(input) && input->data[0] == ';') {
        strref_skip(input, 1);

        http_next_name_value(tpool, input, &name, &value, false);
        if (strref_lower_cmp_literal(&name, "domain") == 0)
            cookie->domain = strref_dup_d(pool, &value);
        else if (strref_lower_cmp_literal(&name, "path") == 0)
            cookie->path = strref_dup_d(pool, &value);
        else if (strref_lower_cmp_literal(&name, "max-age") == 0) {
            unsigned long seconds;
            char *endptr;

            seconds = strtoul(strref_dup(tpool, &value), &endptr, 10);
            if (*endptr == 0) {
                if (seconds == 0)
                    cookie->expires = (time_t)-1;
                else
                    cookie->expires = expiry_touch(seconds);
            }
        }

        strref_ltrim(input);
    }

    return cookie;
}

static bool
apply_next_cookie(struct cookie_jar *jar, struct strref *input,
                  const char *domain, const char *path)
{
    assert(domain != NULL);

    struct cookie *cookie = parse_next_cookie(jar->pool, input);
    if (cookie == NULL)
        return false;

    if (cookie->domain == NULL) {
        cookie->domain = d_strdup(jar->pool, domain);
        if (cookie->domain == NULL) {
            /* out of memory */
            cookie_free(jar->pool, cookie);
            return false;
        }
    } else if (!domain_matches(domain, cookie->domain)) {
        /* discard if domain mismatch */
        cookie_free(jar->pool, cookie);
        return false;
    }

    if (path != NULL && cookie->path != NULL &&
        !path_matches(path, cookie->path)) {
        /* discard if path mismatch */
        cookie_free(jar->pool, cookie);
        return false;
    }

    /* delete the old cookie */
    cookie_list_delete_match(jar->pool, &jar->cookies, cookie->domain,
                             cookie->path,
                             &cookie->name);

    /* add the new one */

    if (cookie->expires == (time_t)-1)
        /* discard expired cookie */
        cookie_free(jar->pool, cookie);
    else
        list_add(&cookie->siblings, &jar->cookies);

    return true;
}

void
cookie_jar_set_cookie2(struct cookie_jar *jar, const char *value,
                       const char *domain, const char *path)
{
    struct strref input;
    struct pool_mark mark;

    strref_set_c(&input, value);

    pool_mark(tpool, &mark);

    while (1) {
        if (!apply_next_cookie(jar, &input, domain, path))
            break;

        if (strref_is_empty(&input)) {
            pool_rewind(tpool, &mark);
            return;
        }

        if (input.data[0] != ',')
            break;

        strref_skip(&input, 1);
        strref_ltrim(&input);
    }

    pool_rewind(tpool, &mark);

    /* XXX log error */
}

char *
cookie_jar_http_header_value(struct cookie_jar *jar,
                             const char *domain, const char *path,
                             struct pool *pool)
{
    static const size_t buffer_size = 4096;
    char *buffer;
    struct cookie *cookie, *next;
    size_t length;
    struct pool_mark mark;
    struct timespec now;
    int ret;
    char *value;

    assert(domain != NULL);
    assert(path != NULL);

    if (list_empty(&jar->cookies))
        return NULL;

    pool_mark(tpool, &mark);
    buffer = p_malloc(tpool, buffer_size);

    length = 0;

    ret = clock_gettime(CLOCK_MONOTONIC, &now);
    if (ret < 0)
        return NULL;

    for (cookie = (struct cookie *)jar->cookies.next;
         &cookie->siblings != &jar->cookies;
         cookie = next) {
        next = (struct cookie *)cookie->siblings.next;
        if (cookie->expires != 0 && cookie->expires < now.tv_sec) {
            cookie_delete(jar, cookie);
            continue;
        }

        if (!domain_matches(domain, cookie->domain) ||
            !path_matches(path, cookie->path))
            continue;

        if (buffer_size - length < cookie->name.length + 1 + 1 + cookie->value.length * 2 + 1 + 2)
            break;

        if (length > 0) {
            buffer[length++] = ';';
            buffer[length++] = ' ';
        }

        memcpy(buffer + length, cookie->name.data, cookie->name.length);
        length += cookie->name.length;
        buffer[length++] = '=';
        if (http_must_quote_token(&cookie->value))
            length += http_quote_string(buffer + length, &cookie->value);
        else {
            memcpy(buffer + length, cookie->value.data, cookie->value.length);
            length += cookie->value.length;
        }
    }

    if (length > 0)
        value = p_strndup(pool, buffer, length);
    else
        value = NULL;

    pool_rewind(tpool, &mark);
    return value;
}

void
cookie_jar_http_header(struct cookie_jar *jar,
                       const char *domain, const char *path,
                       struct strmap *headers, struct pool *pool)
{
    char *cookie = cookie_jar_http_header_value(jar, domain, path, pool);

    if (cookie != NULL) {
        strmap_add(headers, "cookie2", "$Version=\"1\"");
        strmap_add(headers, "cookie", cookie);
    }
}
