/*
 * Process CM4all commands in a HTML stream, e.g. embeddings.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "processor.h"
#include "parser.h"
#include "strutil.h"
#include "replace.h"
#include "embed.h"
#include "uri.h"
#include "args.h"
#include "widget.h"
#include "growing-buffer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct processor *processor_t;

struct processor {
    struct istream output;
    istream_t input;

    struct widget *widget;
    const struct processor_env *env;
    unsigned options;

    struct replace replace;

    struct parser parser;
    enum {
        TAG_NONE,
        TAG_EMBED,
        TAG_A,
        TAG_FORM,
        TAG_IMG,
    } tag;
    struct widget *embedded_widget;
};

static inline processor_t
istream_to_processor(istream_t istream)
{
    return (processor_t)(((char*)istream) - offsetof(struct processor, output));
}

static void
processor_output_stream_read(istream_t istream)
{
    processor_t processor = istream_to_processor(istream);

    if (processor->input != NULL)
        istream_read(processor->input);
    else
        replace_read(&processor->replace);
}

static void
processor_close(processor_t processor);

static void
processor_output_stream_close(istream_t istream)
{
    processor_t processor = istream_to_processor(istream);

    processor_close(processor);
}

static const struct istream processor_output_stream = {
    .read = processor_output_stream_read,
    .close = processor_output_stream_close,
};

static size_t
processor_input_data(const void *data, size_t length, void *ctx)
{
    processor_t processor = ctx;
    size_t nbytes;

    assert(processor != NULL);
    assert(data != NULL);
    assert(length > 0);

    processor->parser.position = processor->replace.source_length;

    nbytes = replace_feed(&processor->replace, data, length);
    if (nbytes == 0)
        return 0;

    parser_feed(&processor->parser, (const char*)data, nbytes);

    if (!processor->replace.quiet &&
        processor->replace.source_length >= 8 * 1024 * 1024) {
        fprintf(stderr, "file too large for processor\n");
        processor_close(processor);
        return 0;
    }

    return (size_t)nbytes;
}

static void
processor_input_eof(void *ctx)
{
    processor_t processor = ctx;

    assert(processor != NULL);
    assert(processor->input != NULL);

    processor->input->handler = NULL;
    pool_unref(processor->input->pool);
    processor->input = NULL;

    replace_eof(&processor->replace);
}

static void
processor_input_free(void *ctx)
{
    processor_t processor = ctx;

    assert(processor->input != NULL);

    pool_unref(processor->input->pool);
    processor->input = NULL;

    processor_close(processor); /* XXX */
}

static const struct istream_handler processor_input_handler = {
    .data = processor_input_data,
    .eof = processor_input_eof,
    .free = processor_input_free,
};


istream_t
processor_new(pool_t pool, istream_t istream,
              struct widget *widget,
              const struct processor_env *env,
              unsigned options)
{
    processor_t processor;

    assert(istream != NULL);
    assert(istream->handler == NULL);
    assert(widget != NULL);

#ifdef NDEBUG
    pool_ref(pool);
#else
    pool = pool_new_linear(pool, "processor", 16384);
#endif

    processor = p_malloc(pool, sizeof(*processor));

    processor->output = processor_output_stream;
    processor->output.pool = pool;

    processor->input = istream;
    istream->handler = &processor_input_handler;
    istream->handler_ctx = processor;
    pool_ref(processor->input->pool);

    processor->widget = widget;
    processor->env = env;
    processor->options = options;

    replace_init(&processor->replace, pool, &processor->output,
                 (options & PROCESSOR_QUIET) != 0);

    parser_init(&processor->parser);

    return &processor->output;
}

static void
processor_close(processor_t processor)
{
    assert(processor != NULL);

    processor->replace.output = NULL;
    replace_destroy(&processor->replace);

    if (processor->input != NULL) {
        pool_t pool = processor->input->pool;
        istream_free(&processor->input);
        pool_unref(pool);
    }

    istream_invoke_free(&processor->output);

    pool_unref(processor->output.pool);
}

static inline processor_t
parser_to_processor(struct parser *parser)
{
    return (processor_t)(((char*)parser) - offsetof(struct processor, parser));
}

void
parser_element_start(struct parser *parser)
{
    processor_t processor = parser_to_processor(parser);

    if (parser->element_name_length == 7 &&
        memcmp(parser->element_name, "c:embed", 7) == 0) {
        processor->tag = TAG_EMBED;
        processor->embedded_widget = p_malloc(processor->output.pool,
                                              sizeof(*processor->embedded_widget));
        widget_init(processor->embedded_widget, NULL);

        list_add(&processor->embedded_widget->siblings,
                 &processor->widget->children);
        processor->embedded_widget->parent = processor->widget;
    } else if (parser->element_name_length == 1 &&
               parser->element_name[0] == 'a') {
        processor->tag = TAG_A;
    } else if (parser->element_name_length == 4 &&
               memcmp(parser->element_name, "form", 4) == 0) {
        processor->tag = TAG_FORM;
    } else if (parser->element_name_length == 3 &&
               memcmp(parser->element_name, "img", 3) == 0) {
        processor->tag = TAG_IMG;
    } else {
        processor->tag = TAG_NONE;
    }
}

static void
replace_attribute_value(processor_t processor, istream_t value)
{
    assert(processor->parser.state == PARSER_ATTR_VALUE ||
           processor->parser.state == PARSER_ATTR_VALUE_COMPAT);

    replace_add(&processor->replace,
                processor->parser.attr_value_start,
                processor->parser.attr_value_end,
                value);
}

static void
make_url_attribute_absolute(processor_t processor)
{
    const char *new_uri = uri_absolute(processor->output.pool,
                                       processor->widget->real_uri,
                                       processor->parser.attr_value,
                                       processor->parser.attr_value_length);
    if (new_uri != NULL)
        replace_attribute_value(processor,
                                istream_string_new(processor->output.pool,
                                                   new_uri));
}

static void
transform_url_attribute(processor_t processor, int focus)
{
    const char *new_uri = uri_absolute(processor->output.pool,
                                       processor->widget->real_uri,
                                       processor->parser.attr_value,
                                       processor->parser.attr_value_length);
    const char *args;

    if (new_uri == NULL)
        return;

    if (processor->widget->id == NULL ||
        processor->env->external_uri == NULL ||
        processor->widget->class == NULL ||
        !widget_class_includes_uri(processor->widget->class, new_uri)) {
        replace_attribute_value(processor,
                                istream_string_new(processor->output.pool,
                                                   new_uri));
        return;
    }

    if (!focus && memchr(processor->parser.attr_value, '?',
                         processor->parser.attr_value_length) != NULL)
        focus = 1;

    /* the URI is relative to the widget's base URI.  Convert the URI
       into an absolute URI to the template page on this server and
       add the appropriate args. */
    args = args_format(processor->output.pool, processor->env->args,
                       processor->widget->id,
                       new_uri + strlen(processor->widget->class->uri),
                       "focus",
                       focus ? processor->widget->id : NULL);

    new_uri = p_strncat(processor->output.pool,
                        processor->env->external_uri->base,
                        processor->env->external_uri->base_length,
                        ";", 1,
                        args, strlen(args),
                        NULL);

    replace_attribute_value(processor,
                            istream_string_new(processor->output.pool,
                                               new_uri));
}

static int
parse_bool(const char *p, size_t length)
{
    return length == 0 ||
        p[0] == '1' || p[0] == 'y' || p[0] == 'Y';
}

void
parser_attr_finished(struct parser *parser)
{
    processor_t processor = parser_to_processor(parser);

    switch (processor->tag) {
    case TAG_NONE:
        break;

    case TAG_EMBED:
        if (parser->attr_name_length == 4 &&
            memcmp(parser->attr_name, "href", 4) == 0)
            processor->embedded_widget->class
                = get_widget_class(processor->output.pool,
                                   p_strndup(processor->output.pool, parser->attr_value,
                                             parser->attr_value_length));
        else if (parser->attr_name_length == 2 &&
                 memcmp(parser->attr_name, "id", 2) == 0)
            processor->embedded_widget->id = p_strndup(processor->output.pool, parser->attr_value,
                                                       parser->attr_value_length);
        else if (parser->attr_name_length == 6 &&
                 memcmp(parser->attr_name, "iframe", 6) == 0)
            processor->embedded_widget->iframe = parse_bool(parser->attr_value,
                                                            parser->attr_value_length);
        else if (parser->attr_name_length == 5 &&
                 memcmp(parser->attr_name, "width", 5) == 0)
            processor->embedded_widget->width = p_strndup(processor->output.pool, parser->attr_value,
                                                          parser->attr_value_length);
        else if (parser->attr_name_length == 6 &&
                 memcmp(parser->attr_name, "height", 6) == 0)
            processor->embedded_widget->height = p_strndup(processor->output.pool, parser->attr_value,
                                                           parser->attr_value_length);
        break;

    case TAG_IMG:
        if (parser->attr_name_length == 3 &&
            memcmp(parser->attr_name, "src", 3) == 0)
            make_url_attribute_absolute(processor);
        break;

    case TAG_A:
        if (parser->attr_name_length == 4 &&
            memcmp(parser->attr_name, "href", 4) == 0)
            transform_url_attribute(processor, 0);
        break;

    case TAG_FORM:
        if (parser->attr_name_length == 6 &&
            memcmp(parser->attr_name, "action", 6) == 0)
            transform_url_attribute(processor, 1);
        break;
    }
}

static istream_t
embed_widget(pool_t pool, const struct processor_env *env, struct widget *widget)
{
    if (widget->class == NULL || widget->class->uri == NULL)
        return istream_string_new(pool, "Error: no widget class specified");

    widget->real_uri = widget->class->uri;

    if (widget->id != NULL) {
        const char *append = strmap_get(env->args, widget->id);
        if (append != NULL) {
            widget->append_uri = append;
            widget->real_uri = p_strcat(pool, widget->class->uri, append, NULL);
        }
    }

    return env->widget_callback(pool, env, widget);
}

static istream_t
embed_decorate(pool_t pool, istream_t istream, const struct widget *widget)
{
    growing_buffer_t tag;

    assert(istream != NULL);
    assert(istream->handler == NULL);

    tag = growing_buffer_new(pool, 256);
    growing_buffer_write_string(tag, "<div class='embed' style='overflow:auto; border:1px dotted red;");

    if (widget->width != NULL) {
        growing_buffer_write_string(tag, "width:");
        growing_buffer_write_string(tag, widget->width);
        growing_buffer_write_string(tag, ";");
    }

    if (widget->height != NULL) {
        growing_buffer_write_string(tag, "height:");
        growing_buffer_write_string(tag, widget->height);
        growing_buffer_write_string(tag, ";");
    }

    growing_buffer_write_string(tag, "'>");

    return istream_cat_new(pool,
                           growing_buffer_istream(tag),
                           istream,
                           istream_string_new(pool, "</div>"),
                           NULL);
}

static istream_t
embed_element_finished(processor_t processor)
{
    struct widget *widget;
    istream_t istream;

    widget = processor->embedded_widget;
    processor->embedded_widget = NULL;

    istream = embed_widget(processor->output.pool, processor->env, widget);
    if (istream != NULL && (processor->options & PROCESSOR_QUIET) == 0)
        istream = embed_decorate(processor->output.pool, istream, widget);

    return istream;
}

void
parser_element_finished(struct parser *parser, off_t end)
{
    processor_t processor = parser_to_processor(parser);

    if (processor->tag == TAG_EMBED) {
        istream_t istream = embed_element_finished(processor);
        replace_add(&processor->replace, processor->parser.element_offset,
                    end, istream);
    }
}
