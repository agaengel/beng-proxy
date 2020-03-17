/*
 * Copyright 2007-2019 CM4all GmbH
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

#include "HttpConnection.hxx"
#include "RLogger.hxx"
#include "Config.hxx"
#include "Check.hxx"
#include "ClusterConfig.hxx"
#include "ListenerConfig.hxx"
#include "Goto.txx"
#include "ForwardHttpRequest.hxx"
#include "TranslationHandler.hxx"
#include "Instance.hxx"
#include "Cookie.hxx"
#include "http_server/http_server.hxx"
#include "http/IncomingRequest.hxx"
#include "http_server/Handler.hxx"
#include "http_server/Error.hxx"
#include "access_log/Glue.hxx"
#include "pool/pool.hxx"
#include "pool/UniquePtr.hxx"
#include "address_string.hxx"
#include "fs/FilteredSocket.hxx"
#include "fs/ThreadSocketFilter.hxx"
#include "fs/Ptr.hxx"
#include "thread/Pool.hxx"
#include "uri/Verify.hxx"
#include "ssl/Filter.hxx"
#include "net/SocketProtocolError.hxx"
#include "net/StaticSocketAddress.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "io/FdType.hxx"
#include "system/Error.hxx"
#include "util/Exception.hxx"

#include <assert.h>

inline
LbHttpConnection::LbHttpConnection(PoolPtr &&_pool, LbInstance &_instance,
				   const LbListenerConfig &_listener,
				   const LbGoto &_destination,
				   SocketAddress _client_address)
	:PoolHolder(std::move(_pool)), instance(_instance), listener(_listener),
	 initial_destination(_destination),
	 client_address(address_to_string(pool, _client_address)),
	 logger(*this)
{
	if (client_address == nullptr)
		client_address = "unknown";
}

gcc_pure
static int
HttpServerLogLevel(std::exception_ptr e)
{
	try {
		FindRetrowNested<HttpServerSocketError>(e);
	} catch (const HttpServerSocketError &) {
		e = std::current_exception();

		/* some socket errors caused by our client are less
		   important */

		try {
			FindRetrowNested<std::system_error>(e);
		} catch (const std::system_error &se) {
			if (IsErrno(se, ECONNRESET))
				return 4;
		}

		try {
			FindRetrowNested<SocketProtocolError>(e);
		} catch (...) {
			return 4;
		}
	}

	return 2;
}

/*
 * public
 *
 */

LbHttpConnection *
NewLbHttpConnection(LbInstance &instance,
		    const LbListenerConfig &listener,
		    const LbGoto &destination,
		    SslFactory *ssl_factory,
		    UniqueSocketDescriptor &&fd, SocketAddress address)
{
	assert(listener.destination.GetProtocol() == LbProtocol::HTTP);

	/* determine the local socket address */
	StaticSocketAddress local_address = fd.GetLocalAddress();

	auto fd_type = FdType::FD_TCP;

	SslFilter *ssl_filter = nullptr;
	SocketFilterPtr filter;

	if (ssl_factory != nullptr) {
		ssl_filter = ssl_filter_new(*ssl_factory);
		filter.reset(new ThreadSocketFilter(instance.event_loop,
						    thread_pool_get_queue(instance.event_loop),
						    &ssl_filter_get_handler(*ssl_filter)));
	}

	auto pool = pool_new_linear(instance.root_pool, "http_connection", 2048);

	auto *connection = NewFromPool<LbHttpConnection>(std::move(pool), instance,
							 listener, destination,
							 address);
	connection->ssl_filter = ssl_filter;

	instance.http_connections.push_back(*connection);

	connection->http = http_server_connection_new(connection->GetPool(),
						      UniquePoolPtr<FilteredSocket>::Make(connection->GetPool(),
											  instance.event_loop,
											  std::move(fd), fd_type,
											  std::move(filter)),
						      local_address.IsDefined()
						      ? (SocketAddress)local_address
						      : nullptr,
						      address,
						      false,
						      *connection);
	return connection;
}

void
LbHttpConnection::Destroy()
{
	assert(!instance.http_connections.empty());

	auto &connections = instance.http_connections;
	connections.erase(connections.iterator_to(*this));

	this->~LbHttpConnection();
}

void
LbHttpConnection::CloseAndDestroy()
{
	assert(listener.destination.GetProtocol() == LbProtocol::HTTP);
	assert(http != nullptr);

	http_server_connection_close(http);

	Destroy();
}

void
LbHttpConnection::SendError(IncomingHttpRequest &request, std::exception_ptr ep)
{
	const char *msg = listener.verbose_response
		? p_strdup(request.pool, GetFullMessage(ep).c_str())
		: "Bad gateway";

	request.SendMessage(HTTP_STATUS_BAD_GATEWAY, msg);
}

void
LbHttpConnection::LogSendError(IncomingHttpRequest &request,
			       std::exception_ptr ep)
{
	logger(2, ep);
	SendError(request, ep);
}

static void
SendResponse(IncomingHttpRequest &request,
	     const LbSimpleHttpResponse &response)
{
	assert(response.IsDefined());

	request.SendSimpleResponse(response.status,
				   response.location.empty() ? nullptr : response.location.c_str(),
				   response.message.empty() ? nullptr : response.message.c_str());
}

/*
 * http connection handler
 *
 */

void
LbHttpConnection::RequestHeadersFinished(IncomingHttpRequest &request) noexcept
{
	++instance.http_request_counter;

	request.logger = NewFromPool<LbRequestLogger>(request.pool,
						      instance, request);
}

void
LbHttpConnection::HandleHttpRequest(IncomingHttpRequest &request,
				    const StopwatchPtr &,
				    CancellablePointer &cancel_ptr) noexcept
{
	if (!uri_path_verify_quick(request.uri)) {
		request.body.Clear();
		request.SendMessage(HTTP_STATUS_BAD_REQUEST, "Malformed request URI");
		return;
	}

	auto &rl = *(LbRequestLogger *)request.logger;

	if (instance.config.global_http_check &&
	    instance.config.global_http_check->Match(request.uri, rl.host) &&
	    instance.config.global_http_check->MatchClientAddress(request.remote_address)) {
		request.body.Clear();

		if (instance.config.global_http_check->Check())
			request.SendMessage(HTTP_STATUS_OK,
					    instance.config.global_http_check->success_message.c_str());
		else
			request.SendSimpleResponse(HTTP_STATUS_NOT_FOUND,
						   nullptr, nullptr);

		return;
	}

	HandleHttpRequest(initial_destination, request, cancel_ptr);
}

void
LbHttpConnection::HandleHttpRequest(const LbGoto &destination,
				    IncomingHttpRequest &request,
				    CancellablePointer &cancel_ptr)
{
	const auto &goto_ = destination.FindRequestLeaf(request);
	if (goto_.response != nullptr) {
		request.body.Clear();
		SendResponse(request, *goto_.response);
		return;
	}

	if (goto_.lua != nullptr) {
		InvokeLua(*goto_.lua, request, cancel_ptr);
		return;
	}

	if (goto_.translation != nullptr) {
		AskTranslationServer(*goto_.translation, request, cancel_ptr);
		return;
	}

	if (goto_.resolve_connect != nullptr) {
		ResolveConnect(goto_.resolve_connect, request, cancel_ptr);
		return;
	}

	assert(goto_.cluster != nullptr);
	ForwardHttpRequest(*goto_.cluster, request, cancel_ptr);
}

void
LbHttpConnection::ForwardHttpRequest(LbCluster &cluster,
				     IncomingHttpRequest &request,
				     CancellablePointer &cancel_ptr)
{
	::ForwardHttpRequest(*this, request, cluster, cancel_ptr);
}

void
LbHttpConnection::HttpConnectionError(std::exception_ptr e) noexcept
{
	logger(HttpServerLogLevel(e), e);

	assert(http != nullptr);
	http = nullptr;

	Destroy();
}

void
LbHttpConnection::HttpConnectionClosed() noexcept
{
	assert(http != nullptr);
	http = nullptr;

	Destroy();
}

std::string
LbHttpConnection::MakeLoggerDomain() const noexcept
{
	return "listener='" + listener.name
		+ "' cluster='" + listener.destination.GetName()
		+ "' client='" + client_address
		+ "'";
}
