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

#include "XForwardedFor.hxx"
#include "util/StringView.hxx"

/**
 * Extract the right-most item of a comma-separated list, such as an
 * X-Forwarded-For header value.  Returns the remaining string and the
 * right-most item as a std::pair.
 */
[[gnu::pure]]
static std::pair<StringView, StringView>
LastListItem(StringView list) noexcept
{
	const char *comma = (const char *)memrchr(list.data, ',', list.size);
	if (comma == nullptr) {
		list.Strip();
		if (list.empty())
			return std::make_pair(nullptr, nullptr);

		return std::make_pair("", list);
	}

	StringView value = list.substr(comma + 1);
	value.Strip();

	list.size = comma - list.data;

	return std::make_pair(list, value);
}

std::string_view
XForwardedForConfig::GetRealRemoteHost(const char *xff) const noexcept
{
	StringView list{xff};
	std::string_view result{};

	while (true) {
		auto l = LastListItem(list);
		if (l.second.empty())
			/* list finished; return the last good address (even if
			   it's a trusted proxy) */
			return result;

		result = l.second;
		if (trust.find(std::string{result}) == trust.end())
			/* this address is not a trusted proxy; return it */
			return result;

		list = l.first;
	}
}
