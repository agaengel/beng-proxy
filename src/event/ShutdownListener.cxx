/*
 * Listener for shutdown signals (SIGTERM, SIGINT, SIGQUIT).
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "ShutdownListener.hxx"

#include <daemon/log.h>

#include <signal.h>

inline void
ShutdownListener::SignalCallback(int signo)
{
    daemon_log(2, "caught signal %d, shutting down (pid=%d)\n",
               signo, (int)getpid());

    Disable();
    callback();
}

ShutdownListener::ShutdownListener(EventLoop &loop, Callback _callback)
    :event(loop, BIND_THIS_METHOD(SignalCallback)),
     callback(_callback)
{
    event.Add(SIGTERM);
    event.Add(SIGINT);
    event.Add(SIGQUIT);
}
