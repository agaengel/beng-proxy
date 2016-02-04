/*
 * Launch and manage "Local HTTP" child processes.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "child_stock.hxx"
#include "child_socket.hxx"
#include "stock/MapStock.hxx"
#include "stock/Stock.hxx"
#include "stock/Class.hxx"
#include "stock/Item.hxx"
#include "child_manager.hxx"
#include "gerrno.h"
#include "system/sigutil.h"
#include "pool.hxx"

#include <glib.h>

#include <string>

#include <assert.h>
#include <unistd.h>
#include <sched.h>

struct ChildStockItem final : HeapStockItem {
    const std::string key;

    const ChildStockClass *const cls;

    ChildSocket socket;
    pid_t pid = -1;

    bool busy = true;

    ChildStockItem(CreateStockItem c,
                   const char *_key,
                   const ChildStockClass &_cls)
        :HeapStockItem(c),
         key(_key),
         cls(&_cls) {}

    ~ChildStockItem() override;

    /* virtual methods from class StockItem */
    bool Borrow(gcc_unused void *ctx) override {
        assert(!busy);
        busy = true;

        return true;
    }

    bool Release(gcc_unused void *ctx) override {
        assert(busy);
        busy = false;

        /* reuse this item only if the child process hasn't exited */
        return pid > 0;
    }
};

static void
child_stock_child_callback(int status gcc_unused, void *ctx)
{
    auto *item = (ChildStockItem *)ctx;

    item->pid = -1;

    if (!item->busy)
        item->InvokeIdleDisconnect();
}

struct ChildStockArgs {
    const char *key;
    void *info;
    const ChildStockClass *cls;
    int fd;
    sigset_t *signals;
};

gcc_noreturn
static int
child_stock_fn(void *ctx)
{
    const auto *args = (const ChildStockArgs *)ctx;
    const int fd = args->fd;

    install_default_signal_handlers();
    leave_signal_section(args->signals);

    dup2(fd, 0);
    close(fd);

    int result = args->cls->run(args->key, args->info);
    _exit(result);
}

static pid_t
child_stock_start(const char *key, void *info,
                  int clone_flags,
                  const ChildStockClass *cls,
                  int fd, GError **error_r)
{
    /* avoid race condition due to libevent signal handler in child
       process */
    sigset_t signals;
    enter_signal_section(&signals);

    ChildStockArgs args = {
        key, info,
        cls,
        fd,
        &signals,
    };

    char stack[8192];

    int pid = clone(child_stock_fn, stack + sizeof(stack),
                    clone_flags, &args);
    if (pid < 0) {
        set_error_errno_msg(error_r, "clone() failed");
        leave_signal_section(&signals);
        return -1;
    }

    leave_signal_section(&signals);

    close(fd);

    return pid;
}

/*
 * stock class
 *
 */

static void
child_stock_create(void *stock_ctx,
                   gcc_unused struct pool &parent_pool, CreateStockItem c,
                   const char *key, void *info,
                   gcc_unused struct pool &caller_pool,
                   gcc_unused struct async_operation_ref &async_ref)
{
    const auto *cls = (const ChildStockClass *)stock_ctx;

    GError *error = nullptr;

    auto *item = new ChildStockItem(c, key, *cls);

    int socket_type = cls->socket_type != nullptr
        ? cls->socket_type(key, info)
        : SOCK_STREAM;

    int fd = item->socket.Create(socket_type, &error);
    if (fd < 0) {
        item->InvokeCreateError(error);
        return;
    }

    int clone_flags = SIGCHLD;
    if (cls->clone_flags != nullptr)
        clone_flags = cls->clone_flags(key, info, clone_flags);

    pid_t pid = item->pid = child_stock_start(key, info, clone_flags,
                                              cls, fd, &error);
    if (pid < 0) {
        item->InvokeCreateError(error);
        return;
    }

    child_register(pid, key, child_stock_child_callback, item);

    item->InvokeCreateSuccess();
}

ChildStockItem::~ChildStockItem()
{
    if (pid >= 0)
        child_kill_signal(pid, cls->shutdown_signal);

    if (socket.IsDefined())
        socket.Unlink();
}

static constexpr StockClass child_stock_class = {
    .create = child_stock_create,
};


/*
 * interface
 *
 */

StockMap *
child_stock_new(struct pool *pool, unsigned limit, unsigned max_idle,
                const ChildStockClass *cls)
{
    assert(cls != nullptr);
    assert(cls->shutdown_signal != 0);
    assert(cls->run != nullptr);

    union {
        const ChildStockClass *in;
        void *out;
    } u = { .in = cls };

    return hstock_new(*pool, child_stock_class, u.out, limit, max_idle);
}

const char *
child_stock_item_key(const StockItem *_item)
{
    const auto *item = (const ChildStockItem *)_item;

    return item->key.c_str();
}

int
child_stock_item_connect(const StockItem *_item, GError **error_r)
{
    const auto *item = (const ChildStockItem *)_item;

    return item->socket.Connect(error_r);
}

void
child_stock_put(StockMap *hstock, StockItem *_item,
                bool destroy)
{
    auto *item = (ChildStockItem *)_item;

    hstock_put(*hstock, item->key.c_str(), *item, destroy);
}
