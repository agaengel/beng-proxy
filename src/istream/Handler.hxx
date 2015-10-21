/*
 * Asynchronous input stream API.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef ISTREAM_HANDLER_HXX
#define ISTREAM_HANDLER_HXX

#include "FdType.hxx"
#include "glibfwd.hxx"

#include <sys/types.h>

/**
 * These special values may be returned from
 * istream_handler::direct().
 */
enum istream_result {
    /**
     * No more data available in the specified socket.
     */
    ISTREAM_RESULT_EOF = 0,

    /**
     * I/O error, errno set.
     */
    ISTREAM_RESULT_ERRNO = -1,

    /**
     * Writing would block, callee is responsible for registering an
     * event and calling istream_read().
     */
    ISTREAM_RESULT_BLOCKING = -2,

    /**
     * The stream has ben closed.  This state supersedes all other
     * states.
     */
    ISTREAM_RESULT_CLOSED = -3,
};

/** data sink for an istream */
struct istream_handler {
    /**
     * Data is available as a buffer.
     * This function must return 0 if it has closed the stream.
     *
     * @param data the buffer
     * @param length the number of bytes available in the buffer, greater than 0
     * @param ctx the istream_handler context pointer
     * @return the number of bytes consumed, 0 if writing would block
     * (caller is responsible for registering an event) or if the
     * stream has been closed
     */
    size_t (*data)(const void *data, size_t length, void *ctx);

    /**
     * Data is available in a file descriptor.
     * This function must return 0 if it has closed the stream.
     *
     * @param type what kind of file descriptor?
     * @param fd the file descriptor
     * @param max_length don't read more than this number of bytes
     * @param ctx the istream_handler context pointer
     * @return the number of bytes consumed, or one of the
     * #istream_result values
     */
    ssize_t (*direct)(FdType type, int fd, size_t max_length,
                      void *ctx);

    /**
     * End of file encountered.
     *
     * @param ctx the istream_handler context pointer
     */
    void (*eof)(void *ctx);

    /**
     * The istream has ended unexpectedly, e.g. an I/O error.
     *
     * The method close() will not result in a call to this callback,
     * since the caller is assumed to be the istream handler.
     *
     * @param error a GError describing the error condition, must be
     * freed by the callee
     * @param ctx the istream_handler context pointer
     */
    void (*abort)(GError *error, void *ctx);
};

#endif
