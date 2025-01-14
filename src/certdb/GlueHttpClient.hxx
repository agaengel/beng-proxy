// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "lib/curl/Global.hxx"
#include "lib/curl/Headers.hxx"

#include <cstdint>
#include <span>
#include <string>

enum class HttpMethod : uint_least8_t;
enum class HttpStatus : uint_least16_t;
class EventLoop;

struct GlueHttpResponse {
	HttpStatus status;

	Curl::Headers headers;

	std::string body;

	GlueHttpResponse(HttpStatus _status,
			 Curl::Headers &&_headers,
			 std::string &&_body)
		:status(_status), headers(std::move(_headers)), body(_body) {}
};

class GlueHttpClient {
	CurlGlobal curl_global;

	bool verbose = false;

public:
	explicit GlueHttpClient(EventLoop &event_loop);
	~GlueHttpClient();

	GlueHttpClient(const GlueHttpClient &) = delete;
	GlueHttpClient &operator=(const GlueHttpClient &) = delete;

	void EnableVerbose() noexcept {
		verbose = true;
	}

	GlueHttpResponse Request(EventLoop &event_loop,
				 HttpMethod method, const char *uri,
				 std::span<const std::byte> body);
};
