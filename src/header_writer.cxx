/*
 * Write HTTP headers into a fifo_buffer_t.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "header_writer.hxx"
#include "strmap.hxx"
#include "growing_buffer.hxx"

#include <http/header.h>

#include <assert.h>
#include <string.h>

void
header_write_begin(GrowingBuffer *gb, const char *name)
{
    assert(gb != nullptr);
    assert(name != nullptr);
    assert(*name != 0);

    size_t name_length = strlen(name);
    char *dest = (char *)gb->Write(name_length + 2);

    memcpy(dest, name, name_length);
    dest += name_length;
    *dest++ = ':';
    *dest++ = ' ';
}

void
header_write_finish(GrowingBuffer *gb)
{
    assert(gb != nullptr);

    gb->Write("\r\n", 2);
}

void
header_write(GrowingBuffer *gb, const char *key, const char *value)
{
    size_t key_length, value_length;

    assert(gb != nullptr);
    assert(key != nullptr);
    assert(value != nullptr);

    key_length = strlen(key);
    value_length = strlen(value);

    char *dest = (char *)
        gb->Write(key_length + 2 + value_length + 2);

    memcpy(dest, key, key_length);
    dest += key_length;
    *dest++ = ':';
    *dest++ = ' ';
    memcpy(dest, value, value_length);
    dest += value_length;
    *dest++ = '\r';
    *dest = '\n';
}

void
headers_copy_one(const struct strmap *in, GrowingBuffer *out,
                 const char *key)
{
    assert(in != nullptr);
    assert(out != nullptr);

    const char *value = in->Get(key);
    if (value != nullptr)
        header_write(out, key, value);
}

void
headers_copy(const struct strmap *in, GrowingBuffer *out,
             const char *const* keys)
{
    const char *value;

    for (; *keys != nullptr; ++keys) {
        value = in->Get(*keys);
        if (value != nullptr)
            header_write(out, *keys, value);
    }
}

void
headers_copy_all(const struct strmap *in, GrowingBuffer *out)
{
    assert(in != nullptr);
    assert(out != nullptr);

    for (const auto &i : *in)
        header_write(out, i.key, i.value);
}

void
headers_copy_most(const struct strmap *in, GrowingBuffer *out)
{
    assert(in != nullptr);
    assert(out != nullptr);

    for (const auto &i : *in)
        if (!http_header_is_hop_by_hop(i.key))
            header_write(out, i.key, i.value);
}

GrowingBuffer
headers_dup(struct pool *pool, const struct strmap *in)
{
    GrowingBuffer out(*pool, 2048);
    headers_copy_most(in, &out);
    return out;
}
