#include "cgi.h"
#include "async.h"
#include "http-response.h"
#include "child.h"
#include "direct.h"

#include <inline/compiler.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <event.h>

struct context {
    struct async_operation_ref async_ref;

    unsigned data_blocking;
    bool close_response_body_early, close_response_body_late, close_response_body_data;
    bool body_read, no_content;
    int fd;
    bool released, aborted;
    http_status_t status;

    istream_t body;
    off_t body_data;
    bool body_eof, body_abort, body_closed;
};


/*
 * istream handler
 *
 */

static size_t
my_istream_data(const void *data __attr_unused, size_t length, void *ctx)
{
    struct context *c = ctx;

    c->body_data += length;

    if (c->close_response_body_data) {
        c->body_closed = true;
        istream_free_handler(&c->body);
        children_shutdown();
        return 0;
    }

    if (c->data_blocking) {
        --c->data_blocking;
        return 0;
    }

    return length;
}

static void
my_istream_eof(void *ctx)
{
    struct context *c = ctx;

    c->body = NULL;
    c->body_eof = true;

    children_shutdown();
}

static void
my_istream_abort(GError *error, void *ctx)
{
    struct context *c = ctx;

    g_error_free(error);

    c->body = NULL;
    c->body_abort = true;

    children_shutdown();
}

static const struct istream_handler my_istream_handler = {
    .data = my_istream_data,
    .eof = my_istream_eof,
    .abort = my_istream_abort,
};


/*
 * http_response_handler
 *
 */

static void
my_response(http_status_t status, struct strmap *headers __attr_unused,
            istream_t body,
            void *ctx)
{
    struct context *c = ctx;

    assert(!c->no_content || body == NULL);

    c->status = status;

    if (c->close_response_body_early) {
        istream_close_unused(body);
        children_shutdown();
    } else if (body != NULL)
        istream_assign_handler(&c->body, body, &my_istream_handler, c, 0);

    if (c->close_response_body_late) {
        c->body_closed = true;
        istream_free_handler(&c->body);
        children_shutdown();
    }

    if (c->body_read) {
        assert(body != NULL);
        istream_read(body);
    }

    if (c->no_content)
        children_shutdown();
}

static void
my_response_abort(GError *error, void *ctx)
{
    struct context *c = ctx;

    g_printerr("%s\n", error->message);
    g_error_free(error);

    c->aborted = true;

    children_shutdown();
}

static const struct http_response_handler my_response_handler = {
    .response = my_response,
    .abort = my_response_abort,
};


/*
 * tests
 *
 */

static void
test_normal(pool_t pool, struct context *c)
{
    const char *path;

    path = getenv("srcdir");
    if (path != NULL)
        path = p_strcat(pool, path, "/demo/cgi-bin/env.py", NULL);
    else
        path = "./demo/cgi-bin/env.py";

    cgi_new(pool, false, NULL, NULL,
            path,
            HTTP_METHOD_GET, "/",
            "env.py", NULL, NULL, "/var/www",
            NULL, NULL,
            &my_response_handler, c,
            &c->async_ref);

    pool_unref(pool);
    pool_commit();

    event_dispatch();

    assert(c->status == HTTP_STATUS_OK);
    assert(c->body == NULL);
    assert(c->body_eof);
    assert(!c->body_abort);
}

static void
test_close_early(pool_t pool, struct context *c)
{
    const char *path;

    path = getenv("srcdir");
    if (path != NULL)
        path = p_strcat(pool, path, "/demo/cgi-bin/env.py", NULL);
    else
        path = "./demo/cgi-bin/env.py";

    c->close_response_body_early = true;

    cgi_new(pool, false, NULL, NULL,
            path,
            HTTP_METHOD_GET, "/",
            "env.py", NULL, NULL, "/var/www",
            NULL, NULL,
            &my_response_handler, c,
            &c->async_ref);

    pool_unref(pool);
    pool_commit();

    event_dispatch();

    assert(c->status == HTTP_STATUS_OK);
    assert(c->body == NULL);
    assert(!c->body_eof);
    assert(!c->body_abort);
}

static void
test_close_late(pool_t pool, struct context *c)
{
    const char *path;

    path = getenv("srcdir");
    if (path != NULL)
        path = p_strcat(pool, path, "/demo/cgi-bin/env.py", NULL);
    else
        path = "./demo/cgi-bin/env.py";

    c->close_response_body_late = true;

    cgi_new(pool, false, NULL, NULL,
            path,
            HTTP_METHOD_GET, "/",
            "env.py", NULL, NULL, "/var/www",
            NULL, NULL,
            &my_response_handler, c,
            &c->async_ref);

    pool_unref(pool);
    pool_commit();

    event_dispatch();

    assert(c->status == HTTP_STATUS_OK);
    assert(c->body == NULL);
    assert(!c->body_eof);
    assert(c->body_abort || c->body_closed);
}

static void
test_close_data(pool_t pool, struct context *c)
{
    const char *path;

    path = getenv("srcdir");
    if (path != NULL)
        path = p_strcat(pool, path, "/demo/cgi-bin/env.py", NULL);
    else
        path = "./demo/cgi-bin/env.py";
    c->close_response_body_data = true;

    cgi_new(pool, false, NULL, NULL,
            path,
            HTTP_METHOD_GET, "/",
            "env.py", NULL, NULL, "/var/www",
            NULL, NULL,
            &my_response_handler, c,
            &c->async_ref);

    pool_unref(pool);
    pool_commit();

    event_dispatch();

    assert(c->status == HTTP_STATUS_OK);
    assert(!c->body_eof);
    assert(!c->body_abort);
    assert(c->body_closed);
}

static void
test_post(pool_t pool, struct context *c)
{
    const char *path;

    path = getenv("srcdir");
    if (path != NULL)
        path = p_strcat(pool, path, "/demo/cgi-bin/cat.sh", NULL);
    else
        path = "./demo/cgi-bin/cat.sh";

    c->body_read = true;

    cgi_new(pool, false, NULL, NULL,
            path,
            HTTP_METHOD_POST, "/",
            "cat.sh", NULL, NULL, "/var/www",
            NULL, istream_file_new(pool, "Makefile", 8192),
            &my_response_handler, c,
            &c->async_ref);

    pool_unref(pool);
    pool_commit();

    event_dispatch();

    assert(c->status == HTTP_STATUS_OK);
    assert(c->body == NULL);
    assert(c->body_eof);
    assert(!c->body_abort);
}

static void
test_status(pool_t pool, struct context *c)
{
    const char *path;

    path = getenv("srcdir");
    if (path != NULL)
        path = p_strcat(pool, path, "/demo/cgi-bin/status.sh", NULL);
    else
        path = "./demo/cgi-bin/status.sh";

    c->body_read = true;

    cgi_new(pool, false, NULL, NULL,
            path,
            HTTP_METHOD_GET, "/",
            "status.sh", NULL, NULL, "/var/www",
            NULL, NULL,
            &my_response_handler, c,
            &c->async_ref);

    pool_unref(pool);
    pool_commit();

    event_dispatch();

    assert(c->status == HTTP_STATUS_CREATED);
    assert(c->body == NULL);
    assert(c->body_eof);
    assert(!c->body_abort);
}

static void
test_no_content(pool_t pool, struct context *c)
{
    const char *path;

    path = getenv("srcdir");
    if (path != NULL)
        path = p_strcat(pool, path, "/demo/cgi-bin/no_content.sh", NULL);
    else
        path = "./demo/cgi-bin/no_content.sh";

    c->no_content = true;

    cgi_new(pool, false, NULL, NULL,
            path,
            HTTP_METHOD_GET, "/",
            "no_content.sh", NULL, NULL, "/var/www",
            NULL, NULL,
            &my_response_handler, c,
            &c->async_ref);

    pool_unref(pool);
    pool_commit();

    event_dispatch();

    assert(c->status == HTTP_STATUS_NO_CONTENT);
    assert(c->body == NULL);
    assert(!c->body_eof);
    assert(!c->body_abort);
}


/*
 * main
 *
 */

static void
run_test(pool_t pool, void (*test)(pool_t pool, struct context *c)) {
    struct context c;

    memset(&c, 0, sizeof(c));

    children_init(pool);

    pool = pool_new_linear(pool, "test", 16384);
    test(pool, &c);
    pool_commit();
}

int main(int argc, char **argv) {
    struct event_base *event_base;
    pool_t pool;

    (void)argc;
    (void)argv;

    signal(SIGPIPE, SIG_IGN);

    direct_global_init();
    event_base = event_init();

    pool = pool_new_libc(NULL, "root");

    run_test(pool, test_normal);
    run_test(pool, test_close_early);
    run_test(pool, test_close_late);
    run_test(pool, test_close_data);
    run_test(pool, test_post);
    run_test(pool, test_status);
    run_test(pool, test_no_content);

    pool_unref(pool);
    pool_commit();
    pool_recycler_clear();

    event_base_free(event_base);
    direct_global_deinit();
}
