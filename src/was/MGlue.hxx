/*
 * Copyright 2007-2022 CM4all GmbH
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

#pragma once

#include <cstdint>
#include <span>

enum class HttpMethod : uint_least8_t;
struct pool;
class StopwatchPtr;
class SocketAddress;
class UnusedIstreamPtr;
class MultiWasStock;
class RemoteWasStock;
class StringMap;
class HttpResponseHandler;
class CancellablePointer;
struct ChildOptions;

/**
 * High level Multi-WAS client.
 *
 * @param args command-line arguments
 */
void
SendMultiWasRequest(struct pool &pool, MultiWasStock &was_stock,
		    const StopwatchPtr &parent_stopwatch,
		    const char *site_name,
		    const ChildOptions &options,
		    const char *action,
		    const char *path,
		    std::span<const char *const> args,
		    unsigned parallelism,
		    const char *remote_host,
		    HttpMethod method, const char *uri,
		    const char *script_name, const char *path_info,
		    const char *query_string,
		    StringMap &&headers, UnusedIstreamPtr body,
		    std::span<const char *const> params,
		    unsigned concurrency,
		    HttpResponseHandler &handler,
		    CancellablePointer &cancel_ptr) noexcept;

/**
 * High level Remote-WAS client.
 *
 * @param args command-line arguments
 */
void
SendRemoteWasRequest(struct pool &pool, RemoteWasStock &was_stock,
		     const StopwatchPtr &parent_stopwatch,
		     SocketAddress address,
		     unsigned parallelism,
		     const char *remote_host,
		     HttpMethod method, const char *uri,
		     const char *script_name, const char *path_info,
		     const char *query_string,
		     StringMap &&headers, UnusedIstreamPtr body,
		     std::span<const char *const> params,
		     unsigned concurrency,
		     HttpResponseHandler &handler,
		     CancellablePointer &cancel_ptr) noexcept;
