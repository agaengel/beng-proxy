/*
 * Copyright 2007-2020 CM4all GmbH
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

#include "http/Logger.hxx"
#include "pool/Holder.hxx"

#include <boost/intrusive/list.hpp>

#include <chrono>

#include <stdint.h>

struct BpInstance;

/**
 * Attributes which are specific to the current request.  They are
 * only valid while a request is being handled (i.e. during the
 * lifetime of the #IncomingHttpRequest instance).  Strings are
 * allocated from the request pool.
 */
struct BpRequestLogger final : IncomingHttpRequestLogger {
	BpInstance &instance;

	/**
	 * The time stamp at the start of the request.  Used to calculate
	 * the request duration.
	 */
	const std::chrono::steady_clock::time_point start_time;

	/**
	 * The name of the site being accessed by the current HTTP
	 * request (from #TRANSLATE_SITE).  It is a hack to allow the
	 * "log" callback to see this information.
	 */
	const char *site_name = nullptr;

	explicit BpRequestLogger(BpInstance &_instance) noexcept;

	std::chrono::steady_clock::duration GetDuration(std::chrono::steady_clock::time_point now) const noexcept {
		return now - start_time;
	}

	/* virtual methods from class IncomingHttpRequestLogger */
	void LogHttpRequest(IncomingHttpRequest &request,
			    http_status_t status, off_t length,
			    uint64_t bytes_received,
			    uint64_t bytes_sent) noexcept override;
};
