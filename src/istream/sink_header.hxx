/*
 * This istream filter reads a 32 bit header size from the stream,
 * reads it into a buffer and invokes a callback with the tail of the
 * stream.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_SINK_HEADER_HXX
#define BENG_PROXY_SINK_HEADER_HXX

#include <exception>

#include <stddef.h>

struct pool;
class Istream;
class CancellablePointer;

struct sink_header_handler {
    void (*done)(void *header, size_t length, Istream &tail, void *ctx);
    void (*error)(std::exception_ptr ep, void *ctx);
};

void
sink_header_new(struct pool &pool, Istream &input,
                const struct sink_header_handler &handler, void *ctx,
                CancellablePointer &cancel_ptr);

#endif
