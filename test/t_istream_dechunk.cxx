#include "istream/istream_dechunk.hxx"
#include "istream/istream_string.hxx"
#include "istream/istream.hxx"

#include <stdio.h>

#define EXPECTED_RESULT "foo"

static Istream *
create_input(struct pool *pool)
{
    return istream_string_new(pool, "3\r\nfoo\r\n0\r\n\r\n ");
}

class MyDechunkHandler final : public DechunkHandler {
    void OnDechunkEndSeen() override {}

    bool OnDechunkEnd() override {
        return false;
    }
};

static Istream *
create_test(EventLoop &event_loop, struct pool *pool, Istream *input)
{
    auto *handler = NewFromPool<MyDechunkHandler>(*pool);
    return istream_dechunk_new(*pool, *input, event_loop, *handler);
}

#include "t_istream_filter.hxx"
