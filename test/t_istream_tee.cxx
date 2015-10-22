#include "istream/istream_tee.hxx"
#include "istream/istream_delayed.hxx"
#include "istream/istream_string.hxx"
#include "istream/istream.hxx"
#include "istream/sink_close.hxx"
#include "istream/sink_gstring.hxx"
#include "async.hxx"

#include <glib.h>

#include <event.h>
#include <string.h>

struct Context {
    GString *value = nullptr;

    bool eof = false, aborted = false;
};

struct BlockContext : Context {
    /* istream handler */

    size_t OnData(gcc_unused const void *data, gcc_unused size_t length) {
        // block
        return 0;
    }

    ssize_t OnDirect(gcc_unused FdType type, gcc_unused int fd,
                     gcc_unused size_t max_length) {
        gcc_unreachable();
    }

    void OnEof() {
        eof = true;
    }

    void OnError(GError *error) {
        g_error_free(error);
        aborted = true;
    }
};

/*
 * tests
 *
 */

static void
buffer_callback(GString *value, GError *error, void *_ctx)
{
    auto *ctx = (Context *)_ctx;

    assert(value != nullptr);
    assert(error == nullptr);

    ctx->value = value;
}

static void
test_block1(struct pool *pool)
{
    BlockContext ctx;
    struct async_operation_ref async_ref;

    struct istream *delayed = istream_delayed_new(pool);
    struct istream *tee = istream_tee_new(pool, delayed, false, false);
    struct istream *second = istream_tee_second(tee);

    istream_handler_set(tee, &MakeIstreamHandler<BlockContext>::handler, &ctx, 0);

    sink_gstring_new(pool, second, buffer_callback, &ctx, &async_ref);
    assert(ctx.value == nullptr);

    /* the input (istream_delayed) blocks */
    istream_read(second);
    assert(ctx.value == nullptr);

    /* feed data into input */
    istream_delayed_set(delayed, istream_string_new(pool, "foo"));
    assert(ctx.value == nullptr);

    /* the first output (block_istream_handler) blocks */
    istream_read(second);
    assert(ctx.value == nullptr);

    /* close the blocking output, this should release the "tee"
       object and restart reading (into the second output) */
    assert(!ctx.aborted && !ctx.eof);
    istream_free_handler(&tee);
    assert(!ctx.aborted && !ctx.eof);
    assert(ctx.value != nullptr);
    assert(strcmp(ctx.value->str, "foo") == 0);
    g_string_free(ctx.value, true);
}

static void
test_close_data(struct pool *pool)
{
    Context ctx;
    struct async_operation_ref async_ref;

    struct istream *tee =
        istream_tee_new(pool, istream_string_new(pool, "foo"), false, false);

    sink_close_new(tee);
    struct istream *second = istream_tee_second(tee);

    sink_gstring_new(pool, second, buffer_callback, &ctx, &async_ref);
    assert(ctx.value == nullptr);

    istream_read(second);

    /* at this point, sink_close has closed itself, and istream_tee
       should have passed the data to the sink_gstring */

    assert(ctx.value != nullptr);
    assert(strcmp(ctx.value->str, "foo") == 0);
    g_string_free(ctx.value, true);
}

/**
 * Close the second output after data has been consumed only by the
 * first output.  This verifies that istream_tee's "skip" attribute is
 * obeyed properly.
 */
static void
test_close_skipped(struct pool *pool)
{
    Context ctx;
    struct async_operation_ref async_ref;

    struct istream *input = istream_string_new(pool, "foo");
    struct istream *tee = istream_tee_new(pool, input, false, false);
    sink_gstring_new(pool, tee, buffer_callback, &ctx, &async_ref);

    struct istream *second = istream_tee_second(tee);
    sink_close_new(second);

    assert(ctx.value == nullptr);

    istream_read(input);

    assert(ctx.value != nullptr);
    assert(strcmp(ctx.value->str, "foo") == 0);
    g_string_free(ctx.value, true);
}


/*
 * main
 *
 */


int main(int argc, char **argv) {
    struct event_base *event_base;
    struct pool *root_pool;

    (void)argc;
    (void)argv;

    event_base = event_init();

    root_pool = pool_new_libc(nullptr, "root");

    /* run test suite */

    test_block1(root_pool);
    test_close_data(root_pool);
    test_close_skipped(root_pool);

    /* cleanup */

    pool_unref(root_pool);
    pool_commit();

    pool_recycler_clear();

    event_base_free(event_base);
}
