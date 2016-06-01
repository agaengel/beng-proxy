#include "session.hxx"
#include "session_manager.hxx"
#include "cookie_jar.hxx"
#include "shm/dpool.hxx"
#include "crash.hxx"
#include "event/Loop.hxx"

#include <inline/compiler.h>

#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(int argc gcc_unused, char **argv gcc_unused) {
    EventLoop event_loop;

    crash_global_init();
    session_manager_init(event_loop, 1200, 0, 0);
    session_manager_event_del();

    int fds[2];
    (void)pipe(fds);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        event_loop.Reinit();
        session_manager_init(event_loop, 1200, 0, 0);

        auto *session = session_new();
        (void)write(fds[1], &session->id, sizeof(session->id));
        session_put(session);
    } else {
        session_manager_event_add();

        close(fds[1]);

        int status;
        pid_t pid2 = wait(&status);
        assert(pid2 == pid);
        assert(WIFEXITED(status));
        assert(WEXITSTATUS(status) == 0);

        SessionId session_id;
        (void)read(fds[0], &session_id, sizeof(session_id));

        SessionLease session(session_id);
        assert(session);
        assert(session->id == session_id);
    }

    session_manager_deinit();
    crash_global_deinit();
}
