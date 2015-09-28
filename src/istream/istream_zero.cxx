/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "istream_zero.hxx"
#include "istream_internal.hxx"
#include "pool.hxx"
#include "util/Cast.hxx"

#include <limits.h>

struct ZeroIstream {
    struct istream stream;
};

static inline ZeroIstream *
istream_to_zero(struct istream *istream)
{
    return &ContainerCast2(*istream, &ZeroIstream::stream);
}

static off_t
istream_zero_available(gcc_unused struct istream *istream, bool partial)
{
    return partial
        ? INT_MAX
        : -1;
}

static off_t
istream_zero_skip(struct istream *istream gcc_unused, off_t length)
{
    return length;
}

static void
istream_zero_read(struct istream *istream)
{
    ZeroIstream *zero = istream_to_zero(istream);
    static char buffer[1024];

    istream_invoke_data(&zero->stream, buffer, sizeof(buffer));
}

static void
istream_zero_close(struct istream *istream)
{
    ZeroIstream *zero = istream_to_zero(istream);

    istream_deinit(&zero->stream);
}

static const struct istream_class istream_zero = {
    .available = istream_zero_available,
    .skip = istream_zero_skip,
    .read = istream_zero_read,
    .close = istream_zero_close,
};

struct istream *
istream_zero_new(struct pool *pool)
{
    auto zero = NewFromPool<ZeroIstream>(*pool);
    istream_init(&zero->stream, &istream_zero, pool);
    return &zero->stream;
}
