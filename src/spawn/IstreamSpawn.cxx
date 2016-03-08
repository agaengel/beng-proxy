/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "IstreamSpawn.hxx"
#include "Interface.hxx"
#include "Prepared.hxx"
#include "ExitListener.hxx"
#include "system/fd_util.h"
#include "system/fd-util.h"
#include "istream/istream.hxx"
#include "istream/istream_pointer.hxx"
#include "istream/istream_oo.hxx"
#include "buffered_io.hxx"
#include "direct.hxx"
#include "event/Event.hxx"
#include "gerrno.h"
#include "pool.hxx"
#include "fb_pool.hxx"
#include "SliceFifoBuffer.hxx"
#include "event/Callback.hxx"
#include "util/Cast.hxx"

#ifdef __linux
#include <fcntl.h>
#endif

#include <daemon/log.h>

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <limits.h>

struct SpawnIstream final : Istream, IstreamHandler, ExitListener {
    SpawnService &spawn_service;

    int output_fd;
    Event output_event;

    SliceFifoBuffer buffer;

    IstreamPointer input;
    int input_fd;
    Event input_event;

    int pid;

    SpawnIstream(SpawnService &_spawn_service,
                 struct pool &p,
                 Istream *_input, int _input_fd,
                 int _output_fd,
                 pid_t _pid);

    bool CheckDirect() const {
        return Istream::CheckDirect(FdType::FD_PIPE);
    }

    void Cancel();

    void FreeBuffer() {
        buffer.FreeIfDefined(fb_pool_get());
    }

    /**
     * Send data from the buffer.  Invokes the "eof" callback when the
     * buffer becomes empty and the pipe has been closed already.
     *
     * @return true if the caller shall read more data from the pipe
     */
    bool SendFromBuffer();

    void ReadFromOutput();

    void InputEventCallback() {
        input.Read();
    }

    void OutputEventCallback() {
        ReadFromOutput();
    }

    /* virtual methods from class Istream */

    void _Read() override;
    // TODO: implement int AsFd() override;
    void _Close() override;

    /* virtual methods from class IstreamHandler */
    size_t OnData(const void *data, size_t length) override;
    ssize_t OnDirect(FdType type, int fd, size_t max_length) override;
    void OnEof() override;
    void OnError(GError *error) override;

    /* virtual methods from class ExitListener */
    void OnChildProcessExit(int status) override;
};

void
SpawnIstream::Cancel()
{
    assert(output_fd >= 0);

    if (input.IsDefined()) {
        assert(input_fd >= 0);

        input_event.Delete();
        close(input_fd);
        input.Close();
    }

    output_event.Delete();

    close(output_fd);
    output_fd = -1;

    if (pid >= 0)
        spawn_service.KillChildProcess(pid);
}

inline bool
SpawnIstream::SendFromBuffer()
{
    assert(buffer.IsDefined());

    if (Istream::SendFromBuffer(buffer) == 0)
        return false;

    if (output_fd < 0) {
        if (buffer.IsEmpty()) {
            FreeBuffer();
            DestroyEof();
        }

        return false;
    }

    buffer.FreeIfEmpty(fb_pool_get());

    return true;
}

/*
 * input handler
 *
 */

inline size_t
SpawnIstream::OnData(const void *data, size_t length)
{
    assert(input_fd >= 0);

    ssize_t nbytes = write(input_fd, data, length);
    if (nbytes > 0)
        input_event.Add();
    else if (nbytes < 0) {
        if (errno == EAGAIN) {
            input_event.Add();
            return 0;
        }

        daemon_log(1, "write() to subprocess failed: %s\n",
                   strerror(errno));
        input_event.Delete();
        close(input_fd);
        input.ClearAndClose();
        return 0;
    }

    return (size_t)nbytes;
}

inline ssize_t
SpawnIstream::OnDirect(FdType type, int fd, size_t max_length)
{
    assert(input_fd >= 0);

    ssize_t nbytes = istream_direct_to_pipe(type, fd, input_fd, max_length);
    if (nbytes > 0)
        input_event.Add();
    else if (nbytes < 0) {
        if (errno == EAGAIN) {
            if (!fd_ready_for_writing(input_fd)) {
                input_event.Add();
                return ISTREAM_RESULT_BLOCKING;
            }

            /* try again, just in case connection->fd has become ready
               between the first splice() call and
               fd_ready_for_writing() */
            nbytes = istream_direct_to_pipe(type, fd, input_fd, max_length);
        }
    }

    return nbytes;
}

inline void
SpawnIstream::OnEof()
{
    assert(input.IsDefined());
    assert(input_fd >= 0);

    input_event.Delete();
    close(input_fd);

    input.Clear();
}

void
SpawnIstream::OnError(GError *error)
{
    assert(input.IsDefined());
    assert(input_fd >= 0);

    FreeBuffer();

    input_event.Delete();
    close(input_fd);
    input.Clear();

    Cancel();
    DestroyError(error);
}

/*
 * event for fork.output_fd
 */

void
SpawnIstream::ReadFromOutput()
{
    assert(output_fd >= 0);

    if (!CheckDirect()) {
        buffer.AllocateIfNull(fb_pool_get());

        ssize_t nbytes = read_to_buffer(output_fd, buffer, INT_MAX);
        if (nbytes == -2) {
            /* XXX should not happen */
        } else if (nbytes > 0) {
            if (Istream::SendFromBuffer(buffer) > 0) {
                buffer.FreeIfEmpty(fb_pool_get());
                output_event.Add();
            }
        } else if (nbytes == 0) {
            Cancel();

            if (buffer.IsEmpty()) {
                FreeBuffer();
                DestroyEof();
            }
        } else if (errno == EAGAIN) {
            buffer.FreeIfEmpty(fb_pool_get());
            output_event.Add();

            if (input.IsDefined())
                /* the CGI may be waiting for more data from stdin */
                input.Read();
        } else {
            GError *error =
                new_error_errno_msg("failed to read from sub process");
            FreeBuffer();
            Cancel();
            DestroyError(error);
        }
    } else {
        if (Istream::ConsumeFromBuffer(buffer) > 0)
            /* there's data left in the buffer, which must be consumed
               before we can switch to "direct" transfer */
            return;

        buffer.FreeIfDefined(fb_pool_get());

        /* at this point, the handler might have changed inside
           Istream::ConsumeFromBuffer(), and the new handler might not
           support "direct" transfer - check again */
        if (!CheckDirect()) {
            output_event.Add();
            return;
        }

        ssize_t nbytes = InvokeDirect(FdType::FD_PIPE, output_fd, INT_MAX);
        if (nbytes == ISTREAM_RESULT_BLOCKING ||
            nbytes == ISTREAM_RESULT_CLOSED) {
            /* -2 means the callback wasn't able to consume any data right
               now */
        } else if (nbytes > 0) {
            output_event.Add();
        } else if (nbytes == ISTREAM_RESULT_EOF) {
            FreeBuffer();
            Cancel();
            DestroyEof();
        } else if (errno == EAGAIN) {
            output_event.Add();

            if (input.IsDefined())
                /* the CGI may be waiting for more data from stdin */
                input.Read();
        } else {
            GError *error =
                new_error_errno_msg("failed to read from sub process");
            FreeBuffer();
            Cancel();
            DestroyError(error);
        }
    }
}


/*
 * istream implementation
 *
 */

void
SpawnIstream::_Read()
{
    if (buffer.IsEmpty() || SendFromBuffer())
        ReadFromOutput();
}

void
SpawnIstream::_Close()
{
    FreeBuffer();

    if (output_fd >= 0)
        Cancel();

    Destroy();
}

/*
 * child callback
 *
 */

void
SpawnIstream::OnChildProcessExit(gcc_unused int status)
{
    assert(pid >= 0);

    pid = -1;
}


/*
 * constructor
 *
 */

inline
SpawnIstream::SpawnIstream(SpawnService &_spawn_service,
                           struct pool &p,
                           Istream *_input, int _input_fd,
                           int _output_fd,
                           pid_t _pid)
    :Istream(p),
     spawn_service(_spawn_service),
     output_fd(_output_fd),
     input(_input, *this, ISTREAM_TO_PIPE),
     input_fd(_input_fd),
     pid(_pid)
{
    output_event.Set(output_fd, EV_READ,
                     MakeSimpleEventCallback(SpawnIstream, OutputEventCallback),
                     this);

    if (_input != nullptr) {
        input_event.Set(input_fd, EV_WRITE,
                        MakeSimpleEventCallback(SpawnIstream, InputEventCallback),
                        this);
        input_event.Add();
    }

    spawn_service.SetExitListener(pid, this);
}

int
SpawnChildProcess(struct pool *pool, const char *name,
                  Istream *input, Istream **output_r,
                  PreparedChildProcess &&prepared,
                  SpawnService &spawn_service,
                  GError **error_r)
{
    if (input != nullptr) {
        int fd = input->AsFd();
        if (fd >= 0) {
            prepared.SetStdin(fd);
            input = nullptr;
        }
    }

    int stdin_pipe = -1;
    if (input != nullptr) {
        int fds[2];
        if (pipe_cloexec(fds) < 0) {
            set_error_errno_msg(error_r, "pipe_cloexec() failed");
            input->CloseUnused();
            return -1;
        }

        prepared.stdin_fd = fds[0];
        stdin_pipe = fds[1];

        if (fd_set_nonblock(stdin_pipe, true) < 0) {
            set_error_errno_msg(error_r, "fcntl(O_NONBLOCK) failed");
            close(stdin_pipe);
            input->CloseUnused();
            return -1;
        }
    }

    int stdout_fds[2];
    if (pipe_cloexec(stdout_fds) < 0) {
        set_error_errno_msg(error_r, "pipe() failed");

        if (input != nullptr) {
            close(stdin_pipe);
            input->CloseUnused();
        }

        return -1;
    }

    const int stdout_pipe = stdout_fds[0];
    prepared.stdout_fd = stdout_fds[1];

    if (fd_set_nonblock(stdout_pipe, true) < 0) {
        set_error_errno_msg(error_r, "fcntl(O_NONBLOCK) failed");

        if (input != nullptr) {
            close(stdin_pipe);
            input->CloseUnused();
        }

        close(stdout_pipe);
        return -1;
    }

    const int pid = spawn_service.SpawnChildProcess(name, std::move(prepared),
                                                    nullptr, error_r);
    if (pid < 0) {
        if (input != nullptr) {
            close(stdin_pipe);
            input->CloseUnused();
        }

        close(stdout_pipe);
    } else {
        auto f = NewFromPool<SpawnIstream>(*pool, spawn_service, *pool,
                                           input, stdin_pipe,
                                           stdout_pipe,
                                           pid);

        /* XXX CLOEXEC */

        *output_r = f;
    }

    return pid;
}
