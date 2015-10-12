/*
 * Convert an input and an output pipe to a duplex socket.
 *
 * This code is used in the test cases to convert stdin/stdout to a
 * single socket.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "duplex.hxx"
#include "system/fd-util.h"
#include "system/fd_util.h"
#include "event/event2.h"
#include "event/Callback.hxx"
#include "buffered_io.hxx"
#include "pool.hxx"
#include "fb_pool.hxx"
#include "SliceFifoBuffer.hxx"

#include <inline/compiler.h>
#include <daemon/log.h>

#include <sys/socket.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

struct Duplex {
    int read_fd;
    int write_fd;
    int sock_fd;
    bool sock_eof;

    SliceFifoBuffer from_read, to_write;

    struct event2 read_event, write_event, sock_event;

    void Destroy();
    bool CheckDestroy();

    void ReadEventCallback();
    void WriteEventCallback();
    void SocketEventCallback(evutil_socket_t fd, short events);
};

void
Duplex::Destroy()
{
    if (read_fd >= 0) {
        event2_set(&read_event, 0);

        if (read_fd > 2)
            close(read_fd);

        read_fd = -1;
    }

    if (write_fd >= 0) {
        event2_set(&write_event, 0);

        if (write_fd > 2)
            close(write_fd);

        write_fd = -1;
    }

    if (sock_fd >= 0) {
        event2_set(&sock_event, 0);
        event2_commit(&sock_event);

        close(sock_fd);
        sock_fd = -1;
    }

    from_read.Free(fb_pool_get());
    to_write.Free(fb_pool_get());
}

bool
Duplex::CheckDestroy()
{
    if (read_fd < 0 && sock_eof && from_read.IsEmpty() && to_write.IsEmpty()) {
        Destroy();
        return true;
    } else
        return false;
}

inline void
Duplex::ReadEventCallback()
{
    event2_reset(&read_event);

    ssize_t nbytes = read_to_buffer(read_fd, from_read, INT_MAX);
    if (nbytes == -1) {
        daemon_log(1, "failed to read: %s\n", strerror(errno));
        Destroy();
        return;
    }

    if (nbytes == 0) {
        close(read_fd);
        read_fd = -1;
        if (CheckDestroy())
            return;
    }

    if (nbytes > 0)
        event2_or(&sock_event, EV_WRITE);

    if (read_fd >= 0 && !from_read.IsFull())
        event2_or(&read_event, EV_READ);
}

inline void
Duplex::WriteEventCallback()
{
    event2_reset(&write_event);

    ssize_t nbytes = write_from_buffer(write_fd, to_write);
    if (nbytes == -1) {
        Destroy();
        return;
    }

    if (nbytes > 0 && !sock_eof)
        event2_or(&sock_event, EV_READ);

    if (!to_write.IsEmpty())
        event2_or(&write_event, EV_WRITE);
}

inline void
Duplex::SocketEventCallback(evutil_socket_t fd, short events)
{
    event2_lock(&sock_event);
    event2_occurred_persist(&sock_event, events);

    if ((events & EV_READ) != 0) {
        ssize_t nbytes = recv_to_buffer(fd, to_write, INT_MAX);
        if (nbytes == -1) {
            daemon_log(1, "failed to read: %s\n", strerror(errno));
            Destroy();
            return;
        }

        if (nbytes == 0) {
            sock_eof = true;
            if (CheckDestroy())
                return;
        }

        if (likely(nbytes > 0))
            event2_or(&write_event, EV_WRITE);

        if (!to_write.IsFull())
            event2_or(&sock_event, EV_READ);
    }

    if ((events & EV_WRITE) != 0) {
        ssize_t nbytes = send_from_buffer(fd, from_read);
        if (nbytes == -1) {
            Destroy();
            return;
        }

        if (nbytes > 0 && read_fd >= 0)
            event2_or(&read_event, EV_READ);

        if (!from_read.IsEmpty())
            event2_or(&sock_event, EV_WRITE);
    }

    event2_unlock(&sock_event);
}

int
duplex_new(struct pool *pool, int read_fd, int write_fd)
{
    assert(pool != nullptr);
    assert(read_fd >= 0);
    assert(write_fd >= 0);

    int fds[2];
    if (socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
        return -1;

    if (fd_set_nonblock(fds[1], 1) < 0) {
        int save_errno = errno;
        close(fds[0]);
        close(fds[1]);
        errno = save_errno;
        return -1;
    }

    auto duplex = NewFromPool<Duplex>(*pool);
    duplex->read_fd = read_fd;
    duplex->write_fd = write_fd;
    duplex->sock_fd = fds[0];
    duplex->sock_eof = false;

    duplex->from_read.Allocate(fb_pool_get());
    duplex->to_write.Allocate(fb_pool_get());

    event2_init(&duplex->read_event, read_fd,
                MakeSimpleEventCallback(Duplex, ReadEventCallback), duplex,
                nullptr);
    event2_set(&duplex->read_event, EV_READ);

    event2_init(&duplex->write_event, write_fd,
                MakeSimpleEventCallback(Duplex, WriteEventCallback), duplex,
                nullptr);

    event2_init(&duplex->sock_event, duplex->sock_fd,
                MakeEventCallback(Duplex, SocketEventCallback), duplex,
                nullptr);
    event2_persist(&duplex->sock_event);
    event2_set(&duplex->sock_event, EV_READ);

    return fds[1];
}
