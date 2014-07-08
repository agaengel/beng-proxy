/*
 * Global declarations.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_INSTANCE_HXX
#define BENG_PROXY_INSTANCE_HXX

#include "config.hxx"
#include "shutdown_listener.h"
#include "net/ServerSocket.hxx"

#include <inline/list.h>

#include <forward_list>

#include <event.h>

struct instance {
    struct pool *pool;

    struct config config;

    struct event_base *event_base;

    uint64_t http_request_counter;

    std::forward_list<ServerSocket> listeners;

    struct list_head connections;
    unsigned num_connections;

    bool should_exit;
    struct shutdown_listener shutdown_listener;
    struct event sighup_event;

    /* child management */
    struct event respawn_event;
    struct list_head workers;
    unsigned num_workers;

    /**
     * The configured control channel server (see --control-listen),
     * NULL if none was configured.
     */
    struct control_server *control_server;

    /**
     * The implicit per-process control server.  It listens on a local
     * socket "@beng-proxy:PID" and will accept connections only from
     * root or the beng-proxy user.
     */
    struct control_local *local_control_server;

    /* stock */
    struct tcache *translate_cache;
    struct balancer *balancer;
    struct hstock *tcp_stock;
    struct tcp_balancer *tcp_balancer;
    struct memcached_stock *memcached_stock;

    /* cache */
    struct http_cache *http_cache;

    struct filter_cache *filter_cache;

    struct lhttp_stock *lhttp_stock;
    struct fcgi_stock *fcgi_stock;

    struct hstock *was_stock;

    struct hstock *delegate_stock;

    struct nfs_stock *nfs_stock;
    struct nfs_cache *nfs_cache;

    struct stock *pipe_stock;

    struct resource_loader *resource_loader;
};

struct client_connection;

void
init_signals(struct instance *instance);

void
deinit_signals(struct instance *instance);

void
all_listeners_event_add(struct instance *instance);

void
all_listeners_event_del(struct instance *instance);

void
instance_fork_cow(struct instance *instance, bool inherit);

#endif
