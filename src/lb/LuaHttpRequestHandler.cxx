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

#include "HttpConnection.hxx"
#include "LuaHandler.hxx"
#include "ListenerConfig.hxx"
#include "http_server/http_server.hxx"
#include "http_server/Request.hxx"
#include "http_response.hxx"
#include "http_headers.hxx"

class LbLuaResponseHandler final : public HttpResponseHandler {
    LbHttpConnection &connection;

    HttpServerRequest &request;

    bool finished = false;

public:
    LbLuaResponseHandler(LbHttpConnection &_connection,
                         HttpServerRequest &_request)
        :connection(_connection), request(_request) {}

    bool IsFinished() const {
        return finished;
    }

    /* virtual methods from class HttpResponseHandler */
    void OnHttpResponse(http_status_t status, StringMap &&headers,
                        Istream *body) noexcept override;
    void OnHttpError(std::exception_ptr ep) noexcept override;
};

void
LbLuaResponseHandler::OnHttpResponse(http_status_t status,
                                     StringMap &&_headers,
                                     Istream *response_body) noexcept
{
    finished = true;

    HttpHeaders headers(std::move(_headers));

    if (request.method == HTTP_METHOD_HEAD)
        /* pass Content-Length, even though there is no response body
           (RFC 2616 14.13) */
        headers.MoveToBuffer("content-length");

    http_server_response(&request, status, std::move(headers),
                         UnusedIstreamPtr(response_body));
}

void
LbLuaResponseHandler::OnHttpError(std::exception_ptr ep) noexcept
{
    finished = true;

    connection.LogSendError(request, ep);
}

void
LbHttpConnection::InvokeLua(LbLuaHandler &handler,
                            HttpServerRequest &request,
                            CancellablePointer &cancel_ptr)
{
    LbLuaResponseHandler response_handler(*this, request);
    const LbGoto *g;

    try {
        g = handler.HandleRequest(request, response_handler);
    } catch (...) {
        if (response_handler.IsFinished())
            logger(1, "Lua error: ", std::current_exception());
        else
            response_handler.InvokeError(std::current_exception());
        return;
    }

    if (response_handler.IsFinished())
        return;

    if (g == nullptr) {
        request.CheckCloseUnusedBody();
        http_server_send_message(&request, HTTP_STATUS_BAD_GATEWAY,
                                 "No response from Lua handler");
        return;
    }

    HandleHttpRequest(*g, request, cancel_ptr);
    return;
}
