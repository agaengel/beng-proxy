/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Stock.hxx"
#include "Client.hxx"
#include "Handler.hxx"
#include "pool.hxx"
#include "io/Logger.hxx"
#include "util/Cancellable.hxx"

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>

#include <string.h>

struct NfsStockConnection;

struct NfsStockRequest final
    : boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::normal_link>>,
      Cancellable {
    NfsStockConnection &connection;

    struct pool &pool;
    NfsStockGetHandler &handler;

    NfsStockRequest(NfsStockConnection &_connection, struct pool &_pool,
                    NfsStockGetHandler &_handler,
                    CancellablePointer &cancel_ptr)
        :connection(_connection), pool(_pool),
         handler(_handler) {
        pool_ref(&pool);
        cancel_ptr = *this;
    }

    /* virtual methods from class Cancellable */
    void Cancel() noexcept override;
};

struct NfsStockConnection final
    : NfsClientHandler,
      boost::intrusive::set_base_hook<boost::intrusive::link_mode<boost::intrusive::normal_link>> {

    NfsStock &stock;

    struct pool &pool;

    const char *key;

    NfsClient *client;

    CancellablePointer cancel_ptr;

    boost::intrusive::list<NfsStockRequest,
                           boost::intrusive::constant_time_size<false>> requests;

    NfsStockConnection(NfsStock &_stock, struct pool &_pool,
                       const char *_key)
        :stock(_stock), pool(_pool), key(_key), client(nullptr) {}

    void Remove(NfsStockRequest &r) {
        requests.erase(requests.iterator_to(r));
    }

    /* virtual methods from NfsClientHandler */
    void OnNfsClientReady(NfsClient &client) override;
    void OnNfsMountError(std::exception_ptr ep) override;
    void OnNfsClientClosed(std::exception_ptr ep) override;

    struct Compare {
        bool operator()(const NfsStockConnection &a, const NfsStockConnection &b) const {
            return strcmp(a.key, b.key) < 0;
        }

        bool operator()(const NfsStockConnection &a, const char *b) const {
            return strcmp(a.key, b) < 0;
        }

        bool operator()(const char *a, const NfsStockConnection &b) const {
            return strcmp(a, b.key) < 0;
        }
    };
};

struct NfsStock final {
    EventLoop &event_loop;
    struct pool &pool;

    /**
     * Maps server name to #NfsStockConnection.
     */
    typedef boost::intrusive::set<NfsStockConnection,
                                  boost::intrusive::compare<NfsStockConnection::Compare>,
                                  boost::intrusive::constant_time_size<false>> ConnectionMap;
    ConnectionMap connections;

    NfsStock(EventLoop &_event_loop, struct pool &_pool)
        :event_loop(_event_loop), pool(_pool) {}

    ~NfsStock();

    void Get(struct pool &pool,
             const char *server, const char *export_name,
             NfsStockGetHandler &handler,
             CancellablePointer &cancel_ptr);

    void Remove(NfsStockConnection &c) {
        connections.erase(connections.iterator_to(c));
    }
};

/*
 * NfsClientHandler
 *
 */

void
NfsStockConnection::OnNfsClientReady(NfsClient &_client)
{
    assert(client == nullptr);

    client = &_client;

    requests.clear_and_dispose([&_client](NfsStockRequest *request){
            request->handler.OnNfsStockReady(_client);
            DeleteUnrefPool(request->pool, request);
        });
}

void
NfsStockConnection::OnNfsMountError(std::exception_ptr ep)
{
    assert(!stock.connections.empty());

    requests.clear_and_dispose([&ep](NfsStockRequest *request){
            request->handler.OnNfsStockError(ep);
            DeleteUnrefPool(request->pool, request);
        });

    stock.Remove(*this);
    DeleteUnrefTrashPool(pool, this);
}

void
NfsStockConnection::OnNfsClientClosed(std::exception_ptr ep)
{
    assert(requests.empty());
    assert(!stock.connections.empty());

    LogConcat(1, key, "NFS connection closed: ", ep);

    stock.Remove(*this);
    DeleteUnrefTrashPool(pool, this);
}

/*
 * async operation
 *
 */

void
NfsStockRequest::Cancel() noexcept
{
    connection.Remove(*this);
    DeleteUnrefPool(pool, this);

    // TODO: abort client if all requests are gone?
}

/*
 * public
 *
 */

NfsStock *
nfs_stock_new(EventLoop &event_loop, struct pool &pool)
{
    return new NfsStock(event_loop, pool);
}

NfsStock::~NfsStock()
{
    connections.clear_and_dispose([this](NfsStockConnection *connection){
        if (connection->client != nullptr)
            nfs_client_free(connection->client);
        else
            connection->cancel_ptr.Cancel();

        assert(connection->requests.empty());
        DeleteUnrefTrashPool(connection->pool, connection);
        });
}

void
nfs_stock_free(NfsStock *stock)
{
    delete stock;
}

inline void
NfsStock::Get(struct pool &caller_pool,
              const char *server, const char *export_name,
              NfsStockGetHandler &handler,
              CancellablePointer &cancel_ptr)
{
    const char *key = p_strcat(&caller_pool, server, ":", export_name,
                               nullptr);

    ConnectionMap::insert_commit_data hint;
    auto result = connections.insert_check(key,
                                           NfsStockConnection::Compare(),
                                           hint);
    const bool is_new = result.second;
    NfsStockConnection *connection;
    if (is_new) {
        struct pool *c_pool = pool_new_libc(&pool, "nfs_stock_connection");
        connection =
            NewFromPool<NfsStockConnection>(*c_pool, *this, *c_pool,
                                            p_strdup(c_pool, key));

        connections.insert_commit(*connection, hint);
    } else {
        connection = &*result.first;
        if (connection->client != nullptr) {
            /* already connected */
            handler.OnNfsStockReady(*connection->client);
            return;
        }
    }

    auto request = NewFromPool<NfsStockRequest>(caller_pool, *connection,
                                                caller_pool, handler,
                                                cancel_ptr);
    connection->requests.push_front(*request);

    if (is_new)
        nfs_client_new(connection->stock.event_loop, connection->pool,
                       server, export_name,
                       *connection, connection->cancel_ptr);
}

void
nfs_stock_get(NfsStock *stock, struct pool *pool,
              const char *server, const char *export_name,
              NfsStockGetHandler &handler,
              CancellablePointer &cancel_ptr)
{
    stock->Get(*pool, server, export_name, handler, cancel_ptr);
}
