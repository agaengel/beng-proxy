// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#ifndef BENG_PROXY_DIRECT_RESOURCE_LOADER_HXX
#define BENG_PROXY_DIRECT_RESOURCE_LOADER_HXX

#include "ResourceLoader.hxx"
#include "http/AnyClient.hxx"

class EventLoop;
class SpawnService;
class WasStock;
class MultiWasStock;
class RemoteWasStock;
class WasMetricsHandler;
class StockMap;
class LhttpStock;
class FcgiStock;
class NfsCache;
class TcpBalancer;
namespace Uring { class Queue; }
class FilteredSocketBalancer;
class SslClientFactory;
namespace NgHttp2 { class Stock; }
struct XForwardedForConfig;

/**
 * A #ResourceLoader implementation which integrates all client-side
 * protocols implemented by beng-proxy.
 */
class DirectResourceLoader final : public ResourceLoader {
	EventLoop &event_loop;
#ifdef HAVE_URING
	Uring::Queue *const uring;
#endif
	TcpBalancer *tcp_balancer;
	AnyHttpClient any_http_client;
	SpawnService &spawn_service;
	LhttpStock *lhttp_stock;
	FcgiStock *fcgi_stock;
#ifdef HAVE_LIBWAS
	WasStock *was_stock;
	MultiWasStock *const multi_was_stock;
	RemoteWasStock *const remote_was_stock;
	WasMetricsHandler *const metrics_handler;
#endif
#ifdef HAVE_LIBNFS
	NfsCache *nfs_cache;
#endif
	StockMap *delegate_stock;

	const XForwardedForConfig &xff;

public:
	DirectResourceLoader(EventLoop &_event_loop,
#ifdef HAVE_URING
			     Uring::Queue *_uring,
#endif
			     TcpBalancer *_tcp_balancer,
			     FilteredSocketBalancer &_fs_balancer,
#ifdef HAVE_NGHTTP2
			     NgHttp2::Stock &_nghttp2_stock,
#endif
			     SpawnService &_spawn_service,
			     LhttpStock *_lhttp_stock,
			     FcgiStock *_fcgi_stock,
#ifdef HAVE_LIBWAS
			     WasStock *_was_stock,
			     MultiWasStock *_multi_was_stock,
			     RemoteWasStock *_remote_was_stock,
			     WasMetricsHandler *_metrics_handler,
#endif
			     StockMap *_delegate_stock,
#ifdef HAVE_LIBNFS
			     NfsCache *_nfs_cache,
#endif
			     SslClientFactory *_ssl_client_factory,
			     const XForwardedForConfig &_xff) noexcept
		:event_loop(_event_loop),
#ifdef HAVE_URING
		 uring(_uring),
#endif
		 tcp_balancer(_tcp_balancer),
		 any_http_client(_fs_balancer,
#ifdef HAVE_NGHTTP2
				 _nghttp2_stock,
#endif
				 _ssl_client_factory),
		 spawn_service(_spawn_service),
		 lhttp_stock(_lhttp_stock),
		 fcgi_stock(_fcgi_stock),
#ifdef HAVE_LIBWAS
		 was_stock(_was_stock),
		 multi_was_stock(_multi_was_stock),
		 remote_was_stock(_remote_was_stock),
		 metrics_handler(_metrics_handler),
#endif
#ifdef HAVE_LIBNFS
		 nfs_cache(_nfs_cache),
#endif
		 delegate_stock(_delegate_stock),
		 xff(_xff)
	{
	}

	/* virtual methods from class ResourceLoader */
	void SendRequest(struct pool &pool,
			 const StopwatchPtr &parent_stopwatch,
			 const ResourceRequestParams &params,
			 HttpMethod method,
			 const ResourceAddress &address,
			 HttpStatus status, StringMap &&headers,
			 UnusedIstreamPtr body, const char *body_etag,
			 HttpResponseHandler &handler,
			 CancellablePointer &cancel_ptr) noexcept override;
};

#endif
