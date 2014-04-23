#include "processor.h"
#include "penv.hxx"
#include "uri-parser.h"
#include "inline_widget.hxx"
#include "widget.h"
#include "widget_class.hxx"
#include "rewrite_uri.hxx"
#include "istream_file.h"
#include "istream.h"

#include <event.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

static bool is_eof;


/*
 * emulate missing libraries
 *
 */

const struct widget_class root_widget_class = {
    .views = {
        .address = {
            .type = RESOURCE_ADDRESS_NONE,
        },
    },
    .stateful = false,
};

struct tcache *global_translate_cache;

struct istream *
embed_inline_widget(struct pool *pool,
                    gcc_unused struct processor_env *env,
                    gcc_unused bool plain_text,
                    struct widget *widget)
{
    const char *s = widget_path(widget);
    if (s == nullptr)
        s = "widget";

    return istream_string_new(pool, s);
}

struct widget_session *
widget_get_session(gcc_unused struct widget *widget,
                   gcc_unused struct session *session,
                   gcc_unused bool create)
{
    return nullptr;
}

enum uri_mode
parse_uri_mode(G_GNUC_UNUSED const struct strref *s)
{
    return URI_MODE_DIRECT;
}

struct istream *
rewrite_widget_uri(gcc_unused struct pool *pool,
                   gcc_unused struct pool *widget_pool,
                   gcc_unused struct tcache *translate_cache,
                   gcc_unused const char *absolute_uri,
                   gcc_unused const struct parsed_uri *external_uri,
                   gcc_unused const char *site_name,
                   gcc_unused const char *untrusted_host,
                   gcc_unused struct strmap *args,
                   gcc_unused struct widget *widget,
                   gcc_unused session_id_t session_id,
                   gcc_unused const struct strref *value,
                   gcc_unused enum uri_mode mode,
                   gcc_unused bool stateful,
                   gcc_unused const char *view,
                   gcc_unused const struct escape_class *escape)
{
    return nullptr;
}


/*
 * istream handler
 *
 */

static size_t
my_istream_data(const void *data, size_t length, void *ctx)
{
    ssize_t nbytes;

    (void)ctx;

    nbytes = write(1, data, length);
    if (nbytes < 0) {
        fprintf(stderr, "failed to write to stdout: %s\n",
                strerror(errno));
        exit(2);
    }

    if (nbytes == 0) {
        fprintf(stderr, "failed to write to stdout\n");
        exit(2);
    }

    return (size_t)nbytes;
}

static void
my_istream_eof(void *ctx)
{
    (void)ctx;
    fprintf(stderr, "in my_istream_eof()\n");
    is_eof = true;
}

static void gcc_noreturn
my_istream_abort(G_GNUC_UNUSED GError *error, G_GNUC_UNUSED void *ctx)
{
    g_error_free(error);

    exit(2);
}

static const struct istream_handler my_istream_handler = {
    .data = my_istream_data,
    .eof = my_istream_eof,
    .abort = my_istream_abort,
};

int main(int argc, char **argv) {
    struct event_base *event_base;
    struct pool *pool;
    const char *uri;
    bool ret;
    struct parsed_uri parsed_uri;
    struct widget widget;
    struct processor_env env;

    (void)argc;
    (void)argv;

    event_base = event_init();

    pool = pool_new_libc(nullptr, "root");

    uri = "/beng.html";
    ret = uri_parse(&parsed_uri, uri);
    if (!ret) {
        fprintf(stderr, "uri_parse() failed\n");
        exit(2);
    }

    widget_init(&widget, pool, &root_widget_class);

    processor_env_init(pool, &env,
                       nullptr, nullptr,
                       "localhost:8080",
                       "localhost:8080",
                       "/beng.html",
                       "http://localhost:8080/beng.html",
                       &parsed_uri,
                       nullptr,
                       0xdeadbeef,
                       HTTP_METHOD_GET, nullptr);

    struct istream *result =
        processor_process(pool,
                          istream_file_new(pool, "/dev/stdin", (off_t)-1),
                          &widget, &env, PROCESSOR_CONTAINER);
    istream_handler_set(result, &my_istream_handler, nullptr, 0);

    if (!is_eof)
        event_dispatch();

    pool_unref(pool);
    pool_commit();
    pool_recycler_clear();

    event_base_free(event_base);
}
