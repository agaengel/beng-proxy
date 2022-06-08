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

#include "PCookieString.hxx"
#include "CookieString.hxx"
#include "Tokenizer.hxx"
#include "PTokenizer.hxx"
#include "util/StringView.hxx"
#include "AllocatorPtr.hxx"

static StringView
cookie_next_value(AllocatorPtr alloc, StringView &input) noexcept
{
	if (!input.empty() && input.front() == '"')
		return http_next_quoted_string(alloc, input);
	else
		return cookie_next_unquoted_value(input);
}

static StringView
cookie_next_rfc_ignorant_value(AllocatorPtr alloc, StringView &input) noexcept
{
	if (!input.empty() && input.front() == '"')
		return http_next_quoted_string(alloc, input);
	else
		return cookie_next_rfc_ignorant_value(input);
}

std::pair<StringView, StringView>
cookie_next_name_value(AllocatorPtr alloc, StringView &input,
		       bool rfc_ignorant) noexcept
{
	const auto name = http_next_token(input);
	if (name.empty())
		return {name, nullptr};

	input.StripLeft();
	if (!input.empty() && input.front() == '=') {
		input.pop_front();
		input.StripLeft();

		const auto value = rfc_ignorant
			? cookie_next_rfc_ignorant_value(alloc, input)
			: cookie_next_value(alloc, input);
		return {name, value};
	} else
		return {name, nullptr};
}
