#include "istream.h"
#include "widget.h"
#include "widget-class.h"
#include "processor.h"
#include "uri-parser.h"
#include "session.h"
#include "inline-widget.h"
#include "widget-registry.h"
#include "uri-address.h"
#include "global.h"
#include "crash.h"

#include <stdlib.h>
#include <stdio.h>

#define EXPECTED_RESULT "foo &c:url; <script><c:widget id=\"foo\" type=\"bar\"/></script> bar"

void
widget_class_lookup(pool_t pool __attr_unused, pool_t widget_pool __attr_unused,
                    struct tcache *translate_cache __attr_unused,
                    const char *widget_type __attr_unused,
                    widget_class_callback_t callback,
                    void *ctx,
                    struct async_operation_ref *async_ref __attr_unused)
{
    callback(NULL, ctx);
}

istream_t
embed_inline_widget(pool_t pool, struct processor_env *env __attr_unused,
                    struct widget *widget)
{
    return istream_string_new(pool, p_strdup(pool, widget->class_name));
}

static istream_t
create_input(pool_t pool)
{
    return istream_string_new(pool, "foo &c:url; <script><c:widget id=\"foo\" type=\"bar\"/></script> <c:widget id=\"foo\" type=\"bar\"/>");
}

static istream_t
create_test(pool_t pool, istream_t input)
{
    bool ret;
    const char *uri;
    static struct parsed_uri parsed_uri;
    static struct widget widget;
    static struct processor_env env;
    struct session *session;

    /* HACK, processor.c will ignore c:widget otherwise */
    global_translate_cache = (struct tcache *)(size_t)1;

    uri = "/beng.html";
    ret = uri_parse(&parsed_uri, uri);
    if (!ret)
        abort();

    widget_init(&widget, pool, &root_widget_class);

    crash_global_init();
    session_manager_init(1200, 0, 0);

    session = session_new();
    processor_env_init(pool, &env,
                       NULL, NULL,
                       "localhost:8080",
                       "localhost:8080",
                       "/beng.html",
                       "http://localhost:8080/beng.html",
                       &parsed_uri,
                       NULL,
                       session->id,
                       HTTP_METHOD_GET, NULL,
                       NULL);
    session_put(session);

    return processor_process(pool, input, &widget, &env, PROCESSOR_CONTAINER);
}

static void
cleanup(void)
{
    session_manager_deinit();
    crash_global_deinit();
}

#define FILTER_CLEANUP

#include "t-istream-filter.h"
