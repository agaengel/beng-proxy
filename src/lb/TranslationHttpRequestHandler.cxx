// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "HttpConnection.hxx"
#include "RLogger.hxx"
#include "ListenerConfig.hxx"
#include "TranslationHandler.hxx"
#include "http/Status.hxx"
#include "http/IncomingRequest.hxx"
#include "translation/Handler.hxx"
#include "translation/Response.hxx"
#include "pool/pool.hxx"
#include "uri/RedirectHttps.hxx"
#include "istream/UnusedHoldPtr.hxx"
#include "util/Cancellable.hxx"
#include "util/LeakDetector.hxx"
#include "AllocatorPtr.hxx"
#include "stopwatch.hxx"

/*
 * TranslateHandler
 *
 */

struct LbHttpRequest final : private Cancellable, TranslateHandler, private LeakDetector {
	struct pool &pool;
	LbHttpConnection &connection;
	LbTranslationHandler &handler;
	IncomingHttpRequest &request;

	/**
	 * This object temporarily holds the request body
	 */
	UnusedHoldIstreamPtr request_body;

	CancellablePointer &caller_cancel_ptr;
	CancellablePointer translate_cancel_ptr;

	LbHttpRequest(LbHttpConnection &_connection,
		      LbTranslationHandler &_handler,
		      IncomingHttpRequest &_request,
		      CancellablePointer &_cancel_ptr)
		:pool(_request.pool), connection(_connection), handler(_handler),
		 request(_request),
		 request_body(request.pool, std::move(request.body)),
		 caller_cancel_ptr(_cancel_ptr) {
		caller_cancel_ptr = *this;
	}

	void Destroy() noexcept {
		DeleteFromPool(pool, this);
	}

private:
	/* virtual methods from class Cancellable */
	void Cancel() noexcept override {
		CancellablePointer cancel_ptr(std::move(translate_cancel_ptr));
		Destroy();
		cancel_ptr.Cancel();
	}

	/* virtual methods from TranslateHandler */
	void OnTranslateResponse(UniquePoolPtr<TranslateResponse> response) noexcept override;
	void OnTranslateError(std::exception_ptr error) noexcept override;
};

void
LbHttpRequest::OnTranslateResponse(UniquePoolPtr<TranslateResponse> _response) noexcept
{
	const auto &response = *_response;
	auto &_request = request;
	auto &c = connection;
	auto &rl = *(LbRequestLogger *)request.logger;

	if (response.site != nullptr)
		rl.site_name = p_strdup(request.pool, response.site);

	if (response.https_only != 0 && !c.IsEncrypted()) {
		Destroy();

		const char *host = rl.host;
		if (host == nullptr) {
			_request.SendMessage(HttpStatus::BAD_REQUEST, "No Host header");
			return;
		}

		HttpStatus status = response.status;
		if (status == HttpStatus{})
			status = HttpStatus::MOVED_PERMANENTLY;

		const char *msg = response.message;
		if (msg == nullptr)
			msg = "This page requires \"https\"";

		_request.SendRedirect(status,
				      MakeHttpsRedirect(AllocatorPtr{_request.pool},
							host,
							response.https_only,
							_request.uri),
				      msg);
	} else if (response.status != HttpStatus{} ||
		   response.redirect != nullptr ||
		   response.message != nullptr) {
		Destroy();

		auto status = response.status;
		if (status == HttpStatus{})
			status = HttpStatus::SEE_OTHER;

		const char *body = response.message;
		if (body == nullptr)
			body = http_status_to_string(status);

		_request.SendSimpleResponse(status, response.redirect, body);
	} else if (response.pool != nullptr) {
		auto *destination = handler.FindDestination(response.pool);
		if (destination == nullptr) {
			Destroy();

			c.LogSendError(_request,
				       std::make_exception_ptr(std::runtime_error("No such pool")),
				       1);
			return;
		}

		if (response.canonical_host != nullptr)
			rl.canonical_host = response.canonical_host;

		request.body = std::move(request_body);

		auto &_caller_cancel_ptr = caller_cancel_ptr;
		Destroy();

		c.HandleHttpRequest(*destination, _request, {}, _caller_cancel_ptr);
	} else {
		Destroy();

		c.LogSendError(_request,
			       std::make_exception_ptr(std::runtime_error("Invalid translation server response")),
			       1);
	}
}

void
LbHttpRequest::OnTranslateError(std::exception_ptr ep) noexcept
{
	auto &_request = request;
	auto &_connection = connection;

	Destroy();

	_connection.LogSendError(_request, ep, 1);
}

/*
 * constructor
 *
 */

void
LbHttpConnection::AskTranslationServer(LbTranslationHandler &handler,
				       IncomingHttpRequest &request,
				       CancellablePointer &cancel_ptr) noexcept
{
	auto *r = NewFromPool<LbHttpRequest>(request.pool, *this, handler, request,
					     cancel_ptr);

	handler.Pick(request.pool, request,
		     listener_config.tag.empty() ? nullptr : listener_config.tag.c_str(),
		     *r, r->translate_cancel_ptr);
}
