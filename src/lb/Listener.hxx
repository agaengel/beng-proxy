// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "Goto.hxx"
#include "Protocol.hxx"
#include "stats/HttpStats.hxx"
#include "io/Logger.hxx"
#include "fs/Listener.hxx"
#include "net/StaticSocketAddress.hxx"

#include <memory>

struct HttpStats;
struct LbListenerConfig;
struct LbInstance;
class LbGotoMap;
class ClientAccountingMap;

/**
 * Listener on a TCP port.
 */
class LbListener final : FilteredSocketListenerHandler {
	LbInstance &instance;

	const LbListenerConfig &config;

	HttpStats http_stats;

	FilteredSocketListener listener;

	LbGoto destination;

	const Logger logger;

	const LbProtocol protocol;

	std::unique_ptr<ClientAccountingMap> client_accounting;

public:
	LbListener(LbInstance &_instance,
		   const LbListenerConfig &_config);

	~LbListener() noexcept;

	auto &GetEventLoop() const noexcept {
		return listener.GetEventLoop();
	}

	auto GetLocalAddress() const noexcept {
		return listener.GetLocalAddress();
	}

	LbProtocol GetProtocol() const noexcept {
		return protocol;
	}

	const auto &GetConfig() const noexcept {
		return config;
	}

	HttpStats &GetHttpStats() noexcept {
		return http_stats;
	}

	const HttpStats *GetHttpStats() const noexcept {
		return protocol == LbProtocol::HTTP ? &http_stats : nullptr;
	}

	void Scan(LbGotoMap &goto_map);

private:
	/* virtual methods from class FilteredSocketListenerHandler */
	UniqueSocketDescriptor OnFilteredSocketAccept(UniqueSocketDescriptor s,
						      SocketAddress address) override;

	void OnFilteredSocketConnect(PoolPtr pool,
				     UniquePoolPtr<FilteredSocket> socket,
				     SocketAddress address,
				     const SslFilter *ssl_filter) noexcept override;
	void OnFilteredSocketError(std::exception_ptr e) noexcept override;

};
