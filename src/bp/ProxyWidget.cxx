/*
 * Copyright 2007-2021 CM4all GmbH
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

#include "ProxyWidget.hxx"
#include "Request.hxx"
#include "Instance.hxx"
#include "CsrfProtection.hxx"
#include "Global.hxx"
#include "widget/Widget.hxx"
#include "widget/Ref.hxx"
#include "widget/Class.hxx"
#include "widget/Context.hxx"
#include "widget/LookupHandler.hxx"
#include "widget/Resolver.hxx"
#include "widget/Frame.hxx"
#include "ForwardHeaders.hxx"
#include "http/IncomingRequest.hxx"
#include "http/Headers.hxx"
#include "HttpResponseHandler.hxx"
#include "WidgetLookupProcessor.hxx"
#include "istream/AutoPipeIstream.hxx"
#include "pool/pool.hxx"
#include "io/Logger.hxx"

struct ProxyWidget final : PoolLeakDetector, WidgetLookupHandler, HttpResponseHandler, Cancellable {
	Request &request;

	/**
	 * The view name of the top widget.
	 */
	const char *const view_name;

	/**
	 * The widget currently being processed.
	 */
	Widget *widget;

	/**
	 * A reference to the widget that should be proxied.
	 */
	const WidgetRef *ref;

	const SharedPoolPtr<WidgetContext> ctx;

	CancellablePointer cancel_ptr;

	ProxyWidget(Request &_request, Widget &_widget,
		    const WidgetRef *_ref,
		    SharedPoolPtr<WidgetContext> &&_ctx) noexcept
		:PoolLeakDetector(_request.pool),
		 request(_request),
		 view_name(request.args.Remove("view")),
		 widget(&_widget), ref(_ref), ctx(std::move(_ctx)) {
	}

	void Start(UnusedIstreamPtr body, unsigned options,
		   CancellablePointer &caller_cancel_ptr) noexcept;

	void Destroy() noexcept {
		DeleteFromPool(request.pool, this);
	}

	void Continue();

	void ResolverCallback() noexcept;

	/* virtual methods from class Cancellable */
	void Cancel() noexcept override;

	/* virtual methods from class WidgetLookupHandler */
	void WidgetFound(Widget &widget) noexcept override;
	void WidgetNotFound() noexcept override;
	void WidgetLookupError(std::exception_ptr ep) noexcept override;

	/* virtual methods from class HttpResponseHandler */
	void OnHttpResponse(http_status_t status, StringMap &&headers,
			    UnusedIstreamPtr body) noexcept override;
	void OnHttpError(std::exception_ptr ep) noexcept override;
};

/*
 * http_response_handler
 *
 */

void
ProxyWidget::OnHttpResponse(http_status_t status, StringMap &&_headers,
			    UnusedIstreamPtr body) noexcept
{
	assert(widget->cls != nullptr);

	/* XXX shall the address view or the transformation view be used
	   to control response header forwarding? */
	const WidgetView *view = widget->GetTransformationView();
	assert(view != nullptr);

	auto headers = request.ForwardResponseHeaders(status, _headers,
						      nullptr, nullptr,
						      view->response_header_forward);

	HttpHeaders headers2(std::move(headers));

	if (request.request.method == HTTP_METHOD_HEAD)
		/* pass Content-Length, even though there is no response body
		   (RFC 2616 14.13) */
		headers2.CopyToBuffer(_headers, "content-length");

	if (body)
		body = NewAutoPipeIstream(&request.pool, std::move(body),
					  global_pipe_stock);

	/* disable the following transformations, because they are meant
	   for the template, not for this widget */
	request.CancelTransformations();

	auto &_request = request;
	Destroy();
	_request.DispatchResponse(status, std::move(headers2),
				  std::move(body));
}

void
ProxyWidget::OnHttpError(std::exception_ptr ep) noexcept
{
	widget->DiscardForFocused();

	auto &_request = request;
	Destroy();
	_request.LogDispatchError(ep);
}

/**
 * Is the client allow to select the specified view?
 */
gcc_pure
static bool
widget_view_allowed(Widget &widget, const WidgetView &view)
{
	assert(view.name != nullptr);

	if (widget.from_template.view_name != nullptr &&
	    strcmp(view.name, widget.from_template.view_name) == 0)
		/* always allow when it's the same view that was specified in
		   the template */
		return true;

	/* views with an address must not be selected by the client */
	if (!view.inherited) {
		widget.logger(2,  "view '", view.name,
			      "' is forbidden because it has an address");
		return false;
	}

	/* if the default view is a container, we must await the widget's
	   response to see if we allow the new view; if the response is
	   processable, it may potentially contain widget elements with
	   parameters that must not be exposed to the client */
	if (widget.IsContainerByDefault())
		/* schedule a check in widget_update_view() */
		widget.from_request.unauthorized_view = true;

	return true;
}

inline void
ProxyWidget::Start(UnusedIstreamPtr body, unsigned options,
		   CancellablePointer &caller_cancel_ptr) noexcept
{
	assert(body);
	assert(widget != nullptr);
	assert(ref != nullptr);

	caller_cancel_ptr = *this;

	processor_lookup_widget(request.pool, request.stopwatch,
				std::move(body),
				*widget, ref->id,
				std::move(ctx), options,
				*this, cancel_ptr);
}

void
ProxyWidget::Continue()
{
	assert(!widget->from_request.frame);

	if (!widget->HasDefaultView()) {
		widget->Cancel();
		auto &_request = request;
		Destroy();
		_request.DispatchError(HTTP_STATUS_NOT_FOUND, "No such view");
		return;
	}

	if (ref != nullptr) {
		frame_parent_widget(request.pool, *widget, ref->id, ctx,
				    request.stopwatch,
				    *this, cancel_ptr);
	} else {
		if (widget->cls->require_csrf_token &&
		    MethodNeedsCsrfProtection(widget->from_request.method)) {
			/* pool reference necessary because
			   Request::CheckCsrfToken() may destroy the
			   pool and leave us unable to call our
			   destructor */
			const ScopePoolRef _ref(request.pool);
			if (!request.CheckCsrfToken()) {
				Destroy();
				return;
			}
		}

		if (view_name != nullptr) {
			/* the client can select the view; he can never explicitly
			   select the default view */
			const WidgetView *view = widget->cls->FindViewByName(view_name);
			if (view == nullptr || view->name == nullptr) {
				widget->Cancel();
				auto &_request = request;
				Destroy();
				_request.DispatchError(HTTP_STATUS_NOT_FOUND,
						       "No such view");
				return;
			}

			if (!widget_view_allowed(*widget, *view)) {
				widget->Cancel();
				auto &_request = request;
				Destroy();
				_request.DispatchError(HTTP_STATUS_FORBIDDEN,
						       "Forbidden");
				return;
			}

			widget->from_request.view = view;
		}

		if (widget->cls->direct_addressing &&
		    !request.dissected_uri.path_info.empty())
			/* apply new-style path_info to frame top widget (direct
			   addressing) */
			widget->from_request.path_info =
				p_strndup(&request.pool,
					  request.dissected_uri.path_info.data + 1,
					  request.dissected_uri.path_info.size - 1);

		widget->from_request.frame = true;

		frame_top_widget(request.pool, *widget, ctx,
				 request.stopwatch,
				 *this,
				 cancel_ptr);
	}
}

void
ProxyWidget::ResolverCallback() noexcept
{
	if (widget->cls == nullptr) {
		widget->Cancel();

		char log_msg[256];
		snprintf(log_msg, sizeof(log_msg), "Failed to look up class for widget '%s'",
			 widget->GetLogName());

		auto &_request = request;
		Destroy();
		_request.LogDispatchError(HTTP_STATUS_BAD_GATEWAY,
					  "No such widget type",
					  log_msg);
		return;
	}

	Continue();
}

void
ProxyWidget::WidgetFound(Widget &_widget) noexcept
{
	assert(ref != nullptr);

	widget = &_widget;
	ref = ref->next;

	if (widget->cls == nullptr) {
		ResolveWidget(request.pool, *widget,
			      *request.instance.widget_registry,
			      BIND_THIS_METHOD(ResolverCallback),
			      cancel_ptr);
		return;
	}

	Continue();
}

void
ProxyWidget::WidgetNotFound() noexcept
{
	assert(ref != nullptr);

	widget->Cancel();

	char log_msg[256];
	snprintf(log_msg, sizeof(log_msg), "Widget '%s' not found in %s",
		 ref->id, widget->GetLogName());

	auto &_request = request;
	Destroy();
	_request.LogDispatchError(HTTP_STATUS_NOT_FOUND, "No such widget", log_msg);
}

void
ProxyWidget::WidgetLookupError(std::exception_ptr ep) noexcept
{
	widget->Cancel();
	auto &_request = request;
	Destroy();
	_request.LogDispatchError(ep);
}

/*
 * async operation
 *
 */

void
ProxyWidget::Cancel() noexcept
{
	/* make sure that all widget resources are freed when the request
	   is cancelled */
	widget->Cancel();

	cancel_ptr.Cancel();

	Destroy();
}

/*
 * constructor
 *
 */

void
proxy_widget(Request &request2,
	     UnusedIstreamPtr body,
	     Widget &widget, const WidgetRef *proxy_ref,
	     SharedPoolPtr<WidgetContext> ctx,
	     unsigned options)
{
	assert(!widget.from_request.frame);
	assert(proxy_ref != nullptr);
	assert(body);

	auto proxy = NewFromPool<ProxyWidget>(request2.pool, request2,
					      widget, proxy_ref,
					      std::move(ctx));
	proxy->Start(std::move(body), options,
		     request2.cancel_ptr);
}
