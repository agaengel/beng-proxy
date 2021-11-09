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

#pragma once

#include "http/Status.h"

#include <array>
#include <cstdint>
#include <cstddef>

constexpr std::array valid_http_status_array{
	http_status_t{},

	HTTP_STATUS_CONTINUE,
	HTTP_STATUS_SWITCHING_PROTOCOLS,
	HTTP_STATUS_OK,
	HTTP_STATUS_CREATED,
	HTTP_STATUS_ACCEPTED,
	HTTP_STATUS_NON_AUTHORITATIVE_INFORMATION,
	HTTP_STATUS_NO_CONTENT,
	HTTP_STATUS_RESET_CONTENT,
	HTTP_STATUS_PARTIAL_CONTENT,
	HTTP_STATUS_MULTI_STATUS,
	HTTP_STATUS_MULTIPLE_CHOICES,
	HTTP_STATUS_MOVED_PERMANENTLY,
	HTTP_STATUS_FOUND,
	HTTP_STATUS_SEE_OTHER,
	HTTP_STATUS_NOT_MODIFIED,
	HTTP_STATUS_USE_PROXY,
	HTTP_STATUS_TEMPORARY_REDIRECT,
	HTTP_STATUS_BAD_REQUEST,
	HTTP_STATUS_UNAUTHORIZED,
	HTTP_STATUS_PAYMENT_REQUIRED,
	HTTP_STATUS_FORBIDDEN,
	HTTP_STATUS_NOT_FOUND,
	HTTP_STATUS_METHOD_NOT_ALLOWED,
	HTTP_STATUS_NOT_ACCEPTABLE,
	HTTP_STATUS_PROXY_AUTHENTICATION_REQUIRED,
	HTTP_STATUS_REQUEST_TIMEOUT,
	HTTP_STATUS_CONFLICT,
	HTTP_STATUS_GONE,
	HTTP_STATUS_LENGTH_REQUIRED,
	HTTP_STATUS_PRECONDITION_FAILED,
	HTTP_STATUS_REQUEST_ENTITY_TOO_LARGE,
	HTTP_STATUS_REQUEST_URI_TOO_LONG,
	HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE,
	HTTP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE,
	HTTP_STATUS_EXPECTATION_FAILED,
	HTTP_STATUS_I_M_A_TEAPOT,
	HTTP_STATUS_UNPROCESSABLE_ENTITY,
	HTTP_STATUS_LOCKED,
	HTTP_STATUS_FAILED_DEPENDENCY,
	HTTP_STATUS_UPGRADE_REQUIRED,
	HTTP_STATUS_PRECONDITION_REQUIRED,
	HTTP_STATUS_TOO_MANY_REQUESTS,
	HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE,
	HTTP_STATUS_UNAVAILABLE_FOR_LEGAL_REASONS,
	HTTP_STATUS_INTERNAL_SERVER_ERROR,
	HTTP_STATUS_NOT_IMPLEMENTED,
	HTTP_STATUS_BAD_GATEWAY,
	HTTP_STATUS_SERVICE_UNAVAILABLE,
	HTTP_STATUS_GATEWAY_TIMEOUT,
	HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED,
	HTTP_STATUS_INSUFFICIENT_STORAGE,
	HTTP_STATUS_NETWORK_AUTHENTICATION_REQUIRED,
};

constexpr auto
GenerateHttpStatusToIndex() noexcept
{
	std::array<uint_least8_t, 550> result{};

	std::size_t i = 0;
	for (const auto &status : valid_http_status_array) {
		result[status] = i++;
	}

	return result;
}

constexpr auto http_status_to_index = GenerateHttpStatusToIndex();

constexpr std::size_t
HttpStatusToIndex(http_status_t status) noexcept
{
	std::size_t i = status;
	return i < http_status_to_index.size()
		? http_status_to_index[i]
		: 0U;
}

constexpr http_status_t
IndexToHttpStatus(std::size_t i) noexcept
{
	return valid_http_status_array[i];
}
