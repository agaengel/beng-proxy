// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "PrometheusExporter.hxx"
#include "PrometheusExporterConfig.hxx"
#include "Instance.hxx"
#include "Listener.hxx"
#include "Config.hxx"
#include "prometheus/Stats.hxx"
#include "prometheus/HttpStats.hxx"
#include "net/control/Protocol.hxx"
#include "http/Address.hxx"
#include "http/Headers.hxx"
#include "http/IncomingRequest.hxx"
#include "http/Method.hxx"
#include "http/ResponseHandler.hxx"
#include "http/GlueClient.hxx"
#include "util/Cancellable.hxx"
#include "util/MimeType.hxx"
#include "istream/ConcatIstream.hxx"
#include "istream/DelayedIstream.hxx"
#include "istream/UnusedHoldPtr.hxx"
#include "istream/istream_catch.hxx"
#include "memory/istream_gb.hxx"
#include "memory/GrowingBuffer.hxx"
#include "stopwatch.hxx"

class LbPrometheusExporter::AppendRequest final
	: public HttpResponseHandler, Cancellable
{
	DelayedIstreamControl &control;

	const SocketAddress socket_address;

	HttpAddress address{false, "dummy:80", "/"};

	CancellablePointer cancel_ptr;

public:
	AppendRequest(SocketAddress _address,
		      DelayedIstreamControl &_control) noexcept
		:control(_control), socket_address(_address)
	{
		control.cancel_ptr = *this;

		address.addresses = AddressList{
			ShallowCopy{},
			StickyMode::NONE,
			std::span{&socket_address, 1},
		};
	}

	void Start(struct pool &pool, LbInstance &instance) noexcept;

	void Destroy() noexcept {
		this->~AppendRequest();
	}

	void DestroyError(std::exception_ptr error) noexcept {
		auto &_control = control;
		Destroy();
		_control.SetError(std::move(error));
	}

	/* virtual methods from class HttpResponseHandler */
	void OnHttpResponse(HttpStatus status, StringMap &&headers,
			    UnusedIstreamPtr body) noexcept override;

	void OnHttpError(std::exception_ptr error) noexcept override {
		DestroyError(std::move(error));
	}

private:
	/* virtual methods from class Cancellable */
	void Cancel() noexcept override {
		cancel_ptr.Cancel();
		Destroy();
	}
};

inline void
LbPrometheusExporter::AppendRequest::Start(struct pool &pool,
					   LbInstance &_instance) noexcept
{
	http_request(pool, _instance.event_loop,
		     *_instance.fs_balancer, {}, {},
		     nullptr,
		     HttpMethod::GET, address, {}, nullptr,
		     *this, cancel_ptr);
}

void
LbPrometheusExporter::AppendRequest::OnHttpResponse(HttpStatus status,
						    StringMap &&headers,
						    UnusedIstreamPtr body) noexcept
try {
	if (!http_status_is_success(status))
		throw std::runtime_error("HTTP request not sucessful");

	const char *content_type = headers.Get("content-type");
	if (content_type == nullptr ||
	    GetMimeTypeBase(content_type) != "text/plain")
		throw std::runtime_error("Not text/plain");

	control.Set(std::move(body));
} catch (...) {
	DestroyError(std::current_exception());
}

static std::exception_ptr
CatchCallback(std::exception_ptr, void *) noexcept
{
	// TODO log?
	return {};
}

static void
WriteStats(GrowingBuffer &buffer, const LbInstance &instance) noexcept
{
	const char *process = "lb";

	Prometheus::Write(buffer, process, instance.GetStats());

	for (const auto &listener : instance.listeners)
		if (const auto *stats = listener.GetHttpStats())
			Prometheus::Write(buffer, process,
					  listener.GetConfig().name.c_str(),
					  *stats);
}

void
LbPrometheusExporter::HandleHttpRequest(IncomingHttpRequest &request,
					const StopwatchPtr &,
					CancellablePointer &) noexcept
{
	auto &pool = request.pool;

	GrowingBuffer buffer;

	if (instance != nullptr)
		WriteStats(buffer, *instance);

	HttpHeaders headers;
	headers.Write("content-type", "text/plain;version=0.0.4");

	auto body = NewConcatIstream(pool,
				     istream_gb_new(pool, std::move(buffer)));

	for (const auto &i : config.load_from_local) {
		// TODO check instance!=nullptr
		auto delayed = istream_delayed_new(pool, instance->event_loop);
		UnusedHoldIstreamPtr hold(pool, std::move(delayed.first));

		auto *ar = NewFromPool<AppendRequest>(pool, i, delayed.second);
		ar->Start(pool, *instance);

		AppendConcatIstream(body,
				    istream_catch_new(pool,
						      std::move(hold),
						      CatchCallback, nullptr));
	}

	request.SendResponse(HttpStatus::OK, std::move(headers),
			     std::move(body));
}
