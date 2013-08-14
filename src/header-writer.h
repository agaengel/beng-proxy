/*
 * Write HTTP headers into a fifo_buffer_t.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_HEADER_WRITER_H
#define __BENG_HEADER_WRITER_H

struct pool;
struct strmap;
struct growing_buffer;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Begin writing a header line.  After this, you may write the value.
 * Call header_write_finish() when you're done.
 */
void
header_write_begin(struct growing_buffer *gb, const char *name);

/**
 * Finish the current header line.
 *
 * @see header_write_begin().
 */
void
header_write_finish(struct growing_buffer *gb);

void
header_write(struct growing_buffer *gb, const char *key, const char *value);

void
headers_copy_one(const struct strmap *in, struct growing_buffer *out,
                 const char *key);

void
headers_copy(struct strmap *in, struct growing_buffer *out,
             const char *const* keys);

void
headers_copy_all(struct strmap *in, struct growing_buffer *out);

struct growing_buffer *
headers_dup(struct pool *pool, struct strmap *in);

#ifdef __cplusplus
}
#endif

#endif
