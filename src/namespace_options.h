/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_NAMESPACE_OPTIONS_H
#define BENG_PROXY_NAMESPACE_OPTIONS_H

#include <inline/compiler.h>

#include <stdbool.h>

struct pool;

struct namespace_options {
    /**
     * Start the child process in a new user namespace?
     */
    bool enable_user;

    /**
     * Start the child process in a new PID namespace?
     */
    bool enable_pid;

    /**
     * Start the child process in a new network namespace?
     */
    bool enable_network;

    bool enable_mount;

    /**
     * Mount a new /proc?
     */
    bool mount_proc;

    const char *pivot_root;
};

void
namespace_options_init(struct namespace_options *options);

void
namespace_options_copy(struct pool *pool, struct namespace_options *dest,
                       const struct namespace_options *src);

gcc_pure
int
namespace_options_clone_flags(const struct namespace_options *options,
                              int flags);

void
namespace_options_unshare(const struct namespace_options *options);

void
namespace_options_setup(const struct namespace_options *options);

char *
namespace_options_id(const struct namespace_options *options, char *p);

#endif
