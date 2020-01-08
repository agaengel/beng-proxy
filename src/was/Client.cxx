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

#include "Client.hxx"
#include "Error.hxx"
#include "Control.hxx"
#include "Output.hxx"
#include "Input.hxx"
#include "Lease.hxx"
#include "HttpResponseHandler.hxx"
#include "istream/istream_null.hxx"
#include "istream/UnusedPtr.hxx"
#include "strmap.hxx"
#include "pool/pool.hxx"
#include "pool/LeakDetector.hxx"
#include "stopwatch.hxx"
#include "io/FileDescriptor.hxx"
#include "http/HeaderName.hxx"
#include "util/Cast.hxx"
#include "util/ConstBuffer.hxx"
#include "util/Cancellable.hxx"
#include "util/DestructObserver.hxx"
#include "util/Exception.hxx"
#include "util/StringFormat.hxx"
#include "util/StringView.hxx"
#include "util/ScopeExit.hxx"

#include <was/protocol.h>

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

class WasClient final
	: WasControlHandler, WasOutputHandler, WasInputHandler,
	  DestructAnchor, PoolLeakDetector,
	  Cancellable
{
	struct pool &pool;
	struct pool &caller_pool;

	const StopwatchPtr stopwatch;

	WasLease &lease;

	WasControl control;

	HttpResponseHandler &handler;

	struct Request {
		WasOutput *body;

		explicit Request(WasOutput *_body):body(_body) {}

		void ClearBody() {
			if (body != nullptr)
				was_output_free_p(&body);
		}
	} request;

	struct Response {
		http_status_t status = HTTP_STATUS_OK;

		/**
		 * Response headers being assembled.
		 */
		StringMap headers;

		WasInput *body;

		bool receiving_metadata = true;

		/**
		 * If set, then the invocation of the response handler is
		 * postponed, until the remaining control packets have been
		 * evaluated.
		 */
		bool pending = false;

		/**
		 * Did the #WasInput release its pipe yet?  If this happens
		 * before the response is pending, then the response body must
		 * be empty.
		 */
		bool released = false;

		explicit Response(WasInput *_body) noexcept
			:body(_body) {}

		/**
		 * Are we currently receiving response metadata (such as
		 * headers)?
		 */
		bool IsReceivingMetadata() const {
			return receiving_metadata && !pending;
		}

		/**
		 * Has the response been submitted to the response handler?
		 */
		bool WasSubmitted() const {
			return !receiving_metadata;
		}
	} response;

	/**
	 * This is set to true while the final STOP is being sent to avoid
	 * recursive errors.
	 */
	bool ignore_control_errors = false;

public:
	WasClient(struct pool &_pool, struct pool &_caller_pool,
		  EventLoop &event_loop,
		  StopwatchPtr &&_stopwatch,
		  SocketDescriptor control_fd,
		  FileDescriptor input_fd, FileDescriptor output_fd,
		  WasLease &_lease,
		  http_method_t method, UnusedIstreamPtr body,
		  HttpResponseHandler &_handler,
		  CancellablePointer &cancel_ptr);

	void SendRequest(http_method_t method, const char *uri,
			 const char *script_name, const char *path_info,
			 const char *query_string,
			 const StringMap &headers,
			 ConstBuffer<const char *> params) noexcept;

private:
	void Destroy() noexcept {
		DeleteFromPool(pool, this);
	}

	template<typename B>
	void DestroyInvokeResponse(http_status_t status, StringMap headers,
				   B &&body) noexcept {
		auto &_handler = handler;
		Destroy();
		_handler.InvokeResponse(status, std::move(headers),
					std::forward<B>(body));
	}

	void DestroyInvokeError(std::exception_ptr ep) noexcept {
		auto &_handler = handler;
		Destroy();
		_handler.InvokeError(ep);
	}

	/**
	 * Cancel the request body by sending #WAS_COMMAND_PREMATURE to
	 * the WAS child process.
	 *
	 * @return false on error (OnWasControlError() has been called).
	 */
	bool CancelRequestBody() {
		if (request.body == nullptr)
			return true;

		uint64_t sent = was_output_free_p(&request.body);
		return control.SendUint64(WAS_COMMAND_PREMATURE, sent);
	}

	/**
	 * Release the control channel and invoke WasLease::ReleaseWas().
	 * If the control channel is clean (i.e. buffers are empty), it
	 * will attempt to reuse the WAS child process.
	 *
	 * Prior to calling this method, the #WasInput and the #WasOutput
	 * must be released already.
	 */
	void ReleaseControl() {
		assert(request.body == nullptr);
		assert(response.body == nullptr || response.released);

		if (!control.IsDefined())
			/* already released */
			return;

		bool reuse = control.empty();
		control.ReleaseSocket();

		lease.ReleaseWas(reuse);
	}

	/**
	 * @return false on error (OnWasControlError() has been called).
	 */
	bool ReleaseControlStop(uint64_t received) {
		assert(response.body == nullptr);

		if (!control.IsDefined())
			/* already released */
			return true;

		request.ClearBody();

		/* if an error occurs while sending STOP, don't pass it to our
		   handler - he's not interested anymore */
		ignore_control_errors = true;

		if (!control.SendEmpty(WAS_COMMAND_STOP))
			return false;

		control.ReleaseSocket();

		lease.ReleaseWasStop(received);

		return true;
	}

	/**
	 * Destroys the objects WasControl, WasInput, WasOutput and
	 * releases the socket lease.  Assumes the response body has not
	 * been enabled.
	 */
	void ClearUnused() {
		request.ClearBody();

		if (response.body != nullptr)
			was_input_free_unused_p(&response.body);

		if (control.IsDefined())
			control.ReleaseSocket();

		lease.ReleaseWas(false);
	}

	/**
	 * Abort receiving the response status/headers from the WAS server.
	 */
	void AbortResponseHeaders(std::exception_ptr ep) {
		assert(response.IsReceivingMetadata());

		ClearUnused();

		DestroyInvokeError(ep);
	}

	/**
	 * Abort receiving the response body from the WAS server.
	 */
	void AbortResponseBody(std::exception_ptr ep) {
		assert(response.WasSubmitted());

		request.ClearBody();

		auto *response_body = std::exchange(response.body, nullptr);
		if (response_body != nullptr)
			/* cancel the SocketEvent before releasing the WAS
			   process lease */
			was_input_disable(*response_body);

		if (control.IsDefined())
			control.ReleaseSocket();

		lease.ReleaseWas(false);

		Destroy();

		if (response_body != nullptr)
			was_input_free(response_body, ep);
	}

	/**
	 * Call this when end of the response body has been seen.  It will
	 * take care of releasing the #WasClient.
	 */
	void ResponseEof() {
		assert(response.WasSubmitted());
		assert(response.body == nullptr);

		if (!CancelRequestBody())
			return;

		ReleaseControl();
		Destroy();
	}

	/**
	 * Abort a pending response (BODY has been received, but the response
	 * handler has not yet been invoked).
	 */
	void AbortPending(std::exception_ptr ep) {
		assert(!response.IsReceivingMetadata() &&
		       !response.WasSubmitted());

		ClearUnused();

		DestroyInvokeError(ep);
	}

	/**
	 * Abort receiving the response status/headers from the WAS server.
	 */
	void AbortResponse(std::exception_ptr ep) {
		if (response.IsReceivingMetadata())
			AbortResponseHeaders(ep);
		else if (response.WasSubmitted())
			AbortResponseBody(ep);
		else
			AbortPending(ep);
	}

	/**
	 * Submit the pending response to our handler.
	 *
	 * @return false if our #WasControl instance has been disposed
	 */
	bool SubmitPendingResponse();

	/* virtual methods from class Cancellable */
	void Cancel() noexcept override {
		/* Cancellable::Cancel() can only be used before the
		   response was delivered to our callback */
		assert(!response.WasSubmitted());

		stopwatch.RecordEvent("cancel");

		/* if an error occurs while sending PREMATURE, don't pass it
		   to our handler - he's not interested anymore */
		ignore_control_errors = true;

		if (!CancelRequestBody())
			return;

		if (response.body != nullptr)
			was_input_free_unused_p(&response.body);

		if (!ReleaseControlStop(0))
			return;

		Destroy();
	}

	/* virtual methods from class WasControlHandler */
	bool OnWasControlPacket(enum was_command cmd,
				ConstBuffer<void> payload) noexcept override;
	bool OnWasControlDrained() noexcept override;

	void OnWasControlDone() noexcept override {
		assert(request.body == nullptr);
		assert(response.body == nullptr);
		assert(!control.IsDefined());
	}

	void OnWasControlError(std::exception_ptr ep) noexcept override {
		assert(!control.IsDefined());

		if (ignore_control_errors) {
			ClearUnused();
			Destroy();
			return;
		}

		stopwatch.RecordEvent("control_error");

		AbortResponse(NestException(ep,
					    std::runtime_error("Error on WAS control channel")));
	}

	/* virtual methods from class WasOutputHandler */
	bool WasOutputLength(uint64_t length) noexcept override;
	bool WasOutputPremature(uint64_t length,
				std::exception_ptr ep) noexcept override;
	void WasOutputEof() noexcept override;
	void WasOutputError(std::exception_ptr ep) noexcept override;

	/* virtual methods from class WasInputHandler */
	void WasInputClose(uint64_t received) noexcept override;
	bool WasInputRelease() noexcept override;
	void WasInputEof() noexcept override;
	void WasInputError() noexcept override;
};

bool
WasClient::SubmitPendingResponse()
{
	assert(response.pending);
	assert(!response.WasSubmitted());

	stopwatch.RecordEvent("headers");

	response.pending = false;

	response.receiving_metadata = false;

	if (response.released) {
		was_input_free_unused_p(&response.body);
		ReleaseControl();

		DestroyInvokeResponse(response.status, std::move(response.headers),
				      istream_null_new(caller_pool));
		return false;
	} else {
		const DestructObserver destructed(*this);
		handler.InvokeResponse(response.status, std::move(response.headers),
				       was_input_enable(*response.body));
		return !destructed && control.IsDefined();
	}
}

/*
 * WasControlHandler
 */

static constexpr bool
IsValidHeaderValueChar(char ch) noexcept
{
	return ch != '\0' && ch != '\n' && ch != '\r';
}

gcc_pure
static bool
IsValidHeaderValue(StringView value) noexcept
{
	for (char ch : value)
		if (!IsValidHeaderValueChar(ch))
			return false;

	return true;
}

static void
ParseHeaderPacket(struct pool &pool, StringMap &headers,
		  StringView payload)
{
	const auto pair = payload.Split('=');
	const StringView name = pair.first;
	const StringView value = pair.second;

	if (value.IsNull() || !http_header_name_valid(name) ||
	    !IsValidHeaderValue(value))
		throw WasProtocolError("Malformed WAS HEADER packet");

	headers.Add(pool,
		    p_strdup_lower(pool, name),
		    p_strdup(pool, value));
}

bool
WasClient::OnWasControlPacket(enum was_command cmd, ConstBuffer<void> payload) noexcept
{
	switch (cmd) {
		const uint32_t *status32_r;
		const uint16_t *status16_r;
		http_status_t status;
		const uint64_t *length_p;

	case WAS_COMMAND_NOP:
		break;

	case WAS_COMMAND_REQUEST:
	case WAS_COMMAND_URI:
	case WAS_COMMAND_METHOD:
	case WAS_COMMAND_SCRIPT_NAME:
	case WAS_COMMAND_PATH_INFO:
	case WAS_COMMAND_QUERY_STRING:
	case WAS_COMMAND_PARAMETER:
		stopwatch.RecordEvent("control_error");
		AbortResponse(std::make_exception_ptr(WasProtocolError(StringFormat<64>("Unexpected WAS packet %d", cmd))));
		return false;

	case WAS_COMMAND_HEADER:
		if (!response.IsReceivingMetadata()) {
			stopwatch.RecordEvent("control_error");
			AbortResponse(std::make_exception_ptr(WasProtocolError("response header was too late")));
			return false;
		}

		try {
			ParseHeaderPacket(pool, response.headers,
					  StringView(payload));
		} catch (...) {
			stopwatch.RecordEvent("control_error");
			AbortResponseHeaders(std::current_exception());
			return false;
		}

		break;

	case WAS_COMMAND_STATUS:
		if (!response.IsReceivingMetadata()) {
			stopwatch.RecordEvent("control_error");
			/* note: using AbortResponse() instead of
			   AbortResponseBody() because the response may be still
			   "pending" */
			AbortResponse(std::make_exception_ptr(WasProtocolError("STATUS after body start")));
			return false;
		}

		status32_r = (const uint32_t *)payload.data;
		status16_r = (const uint16_t *)payload.data;

		if (payload.size == sizeof(*status32_r))
			status = (http_status_t)*status32_r;
		else if (payload.size == sizeof(*status16_r))
			status = (http_status_t)*status16_r;
		else {
			stopwatch.RecordEvent("control_error");
			AbortResponseHeaders(std::make_exception_ptr(WasProtocolError("malformed STATUS")));
			return false;
		}

		if (!http_status_is_valid(status)) {
			stopwatch.RecordEvent("control_error");
			AbortResponseHeaders(std::make_exception_ptr(WasProtocolError("malformed STATUS")));
			return false;
		}

		response.status = status;

		if (http_status_is_empty(response.status) &&
		    response.body != nullptr)
			/* no response body possible with this status; release the
			   object */
			was_input_free_unused_p(&response.body);

		break;

	case WAS_COMMAND_NO_DATA:
		if (!response.IsReceivingMetadata()) {
			stopwatch.RecordEvent("control_error");
			AbortResponseBody(std::make_exception_ptr(WasProtocolError("NO_DATA after body start")));
			return false;
		}

		response.receiving_metadata = false;

		if (response.body != nullptr)
			was_input_free_unused_p(&response.body);

		if (!CancelRequestBody())
			return false;

		ReleaseControl();

		DestroyInvokeResponse(response.status, std::move(response.headers),
				      UnusedIstreamPtr());
		return false;

	case WAS_COMMAND_DATA:
		if (!response.IsReceivingMetadata()) {
			stopwatch.RecordEvent("control_error");
			AbortResponseBody(std::make_exception_ptr(WasProtocolError("DATA after body start")));
			return false;
		}

		if (response.body == nullptr) {
			stopwatch.RecordEvent("control_error");
			AbortResponseHeaders(std::make_exception_ptr(WasProtocolError("no response body allowed")));
			return false;
		}

		response.pending = true;
		break;

	case WAS_COMMAND_LENGTH:
		if (response.IsReceivingMetadata()) {
			stopwatch.RecordEvent("control_error");
			AbortResponseHeaders(std::make_exception_ptr(WasProtocolError("LENGTH before DATA")));
			return false;
		}

		if (response.body == nullptr) {
			stopwatch.RecordEvent("control_error");
			AbortResponseBody(std::make_exception_ptr(WasProtocolError("LENGTH after NO_DATA")));
			return false;
		}

		length_p = (const uint64_t *)payload.data;
		if (payload.size != sizeof(*length_p)) {
			stopwatch.RecordEvent("control_error");
			AbortResponseBody(std::make_exception_ptr(WasProtocolError("malformed LENGTH packet")));
			return false;
		}

		if (!was_input_set_length(response.body, *length_p))
			return false;

		if (!control.IsDefined()) {
			/* through WasInputRelease(), the above
			   was_input_set_length() call may have disposed the
			   WasControl instance; this condition needs to be
			   reported to our caller */

			if (response.pending)
				/* since OnWasControlDrained() isn't going to be
				   called (because we cancelled that), we need to do
				   this check manually */
				SubmitPendingResponse();

			return false;
		}

		break;

	case WAS_COMMAND_STOP:
		return CancelRequestBody();

	case WAS_COMMAND_PREMATURE:
		if (response.IsReceivingMetadata()) {
			stopwatch.RecordEvent("control_error");
			AbortResponseHeaders(std::make_exception_ptr(WasProtocolError("PREMATURE before DATA")));
			return false;
		}

		length_p = (const uint64_t *)payload.data;
		if (payload.size != sizeof(*length_p)) {
			stopwatch.RecordEvent("control_error");
			AbortResponseBody(std::make_exception_ptr(WasProtocolError("malformed PREMATURE packet")));
			return false;
		}

		if (response.body == nullptr)
			break;

		if (response.pending) {
			/* we can't let was_input report the error to its handler,
			   because it cannot possibly have a handler yet; thus
			   catch it and report it to the #HttpResponseHandler */
			try {
				AtScopeExit(this) { response.body = nullptr; };
				was_input_premature_throw(response.body, *length_p);
			} catch (...) {
				AbortPending(std::current_exception());
			}
			return false;
		} else {
			if (!was_input_premature(response.body, *length_p)) {
				WasInputError();
				return false;
			}
		}

		response.body = nullptr;
		ResponseEof();
		return false;
	}

	return true;
}

bool
WasClient::OnWasControlDrained() noexcept
{
	if (response.pending)
		return SubmitPendingResponse();
	else
		return true;
}

/*
 * Output handler
 */

bool
WasClient::WasOutputLength(uint64_t length) noexcept
{
	assert(control.IsDefined());
	assert(request.body != nullptr);

	return control.SendUint64(WAS_COMMAND_LENGTH, length);
}

bool
WasClient::WasOutputPremature(uint64_t length, std::exception_ptr ep) noexcept
{
	assert(control.IsDefined());
	assert(request.body != nullptr);

	stopwatch.RecordEvent("request_error");

	request.body = nullptr;

	/* XXX send PREMATURE, recover */
	(void)length;

	AbortResponse(ep);
	return false;
}

void
WasClient::WasOutputEof() noexcept
{
	assert(request.body != nullptr);

	stopwatch.RecordEvent("request_end");

	request.body = nullptr;
}

void
WasClient::WasOutputError(std::exception_ptr ep) noexcept
{
	assert(request.body != nullptr);

	stopwatch.RecordEvent("send_error");

	request.body = nullptr;

	AbortResponse(ep);
}

/*
 * Input handler
 */

void
WasClient::WasInputClose(uint64_t received) noexcept
{
	assert(response.WasSubmitted());
	assert(response.body != nullptr);

	stopwatch.RecordEvent("close");

	response.body = nullptr;

	/* if an error occurs while sending PREMATURE, don't pass it
	   to our handler - he's not interested anymore */
	ignore_control_errors = true;

	if (!CancelRequestBody() ||
	    !ReleaseControlStop(received))
		return;

	Destroy();
}

bool
WasClient::WasInputRelease() noexcept
{
	assert(response.body != nullptr);
	assert(!response.released);

	stopwatch.RecordEvent("eof");

	response.released = true;

	if (!CancelRequestBody())
		return false;

	ReleaseControl();
	return true;
}

void
WasClient::WasInputEof() noexcept
{
	assert(response.WasSubmitted());
	assert(response.body != nullptr);
	assert(response.released);

	response.body = nullptr;

	ResponseEof();
}

void
WasClient::WasInputError() noexcept
{
	assert(response.WasSubmitted());
	assert(response.body != nullptr);

	stopwatch.RecordEvent("error");

	response.body = nullptr;

	if (control.IsDefined())
		control.ReleaseSocket();

	lease.ReleaseWas(false);

	Destroy();
}

/*
 * constructor
 *
 */

inline
WasClient::WasClient(struct pool &_pool, struct pool &_caller_pool,
		     EventLoop &event_loop,
		     StopwatchPtr &&_stopwatch,
		     SocketDescriptor control_fd,
		     FileDescriptor input_fd, FileDescriptor output_fd,
		     WasLease &_lease,
		     http_method_t method, UnusedIstreamPtr body,
		     HttpResponseHandler &_handler,
		     CancellablePointer &cancel_ptr)
	:PoolLeakDetector(_pool),
	 pool(_pool), caller_pool(_caller_pool),
	 stopwatch(std::move(_stopwatch)),
	 lease(_lease),
	 control(event_loop, control_fd, *this),
	 handler(_handler),
	 request(body
		 ? was_output_new(pool, event_loop, output_fd,
				  std::move(body), *this)
		 : nullptr),
	 response(http_method_is_empty(method)
		  ? nullptr
		  : was_input_new(pool, event_loop, input_fd, *this))
{
	cancel_ptr = *this;
}

static bool
SendRequest(WasControl &control,
	    http_method_t method, const char *uri,
	    const char *script_name, const char *path_info,
	    const char *query_string,
	    const StringMap &headers, WasOutput *request_body,
	    ConstBuffer<const char *> params)
{
	const uint32_t method32 = (uint32_t)method;

	return control.SendEmpty(WAS_COMMAND_REQUEST) &&
		(method == HTTP_METHOD_GET ||
		 control.Send(WAS_COMMAND_METHOD, &method32, sizeof(method32))) &&
		control.SendString(WAS_COMMAND_URI, uri) &&
		(script_name == nullptr ||
		 control.SendString(WAS_COMMAND_SCRIPT_NAME, script_name)) &&
		(path_info == nullptr ||
		 control.SendString(WAS_COMMAND_PATH_INFO, path_info)) &&
		(query_string == nullptr ||
		 control.SendString(WAS_COMMAND_QUERY_STRING, query_string)) &&
		control.SendStrmap(WAS_COMMAND_HEADER, headers) &&
		control.SendArray(WAS_COMMAND_PARAMETER, params) &&
		control.SendEmpty(request_body != nullptr
				  ? WAS_COMMAND_DATA
				  : WAS_COMMAND_NO_DATA) &&
		(request_body == nullptr || was_output_check_length(*request_body));
}

inline void
WasClient::SendRequest(http_method_t method, const char *uri,
		       const char *script_name, const char *path_info,
		       const char *query_string,
		       const StringMap &headers,
		       ConstBuffer<const char *> params) noexcept
{
	control.BulkOn();

	if (!::SendRequest(control,
			   method, uri, script_name, path_info,
			   query_string, headers, request.body,
			   params))
		return;

	control.BulkOff();
}

void
was_client_request(struct pool &caller_pool, EventLoop &event_loop,
		   StopwatchPtr stopwatch,
		   SocketDescriptor control_fd,
		   FileDescriptor input_fd, FileDescriptor output_fd,
		   WasLease &lease,
		   http_method_t method, const char *uri,
		   const char *script_name, const char *path_info,
		   const char *query_string,
		   const StringMap &headers, UnusedIstreamPtr body,
		   ConstBuffer<const char *> params,
		   HttpResponseHandler &handler,
		   CancellablePointer &cancel_ptr)
{
	assert(http_method_is_valid(method));
	assert(uri != nullptr);

	auto client = NewFromPool<WasClient>(caller_pool, caller_pool, caller_pool,
					     event_loop, std::move(stopwatch),
					     control_fd, input_fd, output_fd,
					     lease, method, std::move(body),
					     handler, cancel_ptr);
	client->SendRequest(method, uri, script_name, path_info,
			    query_string, headers,
			    params);
}
