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

#include "bp/CsrfToken.hxx"
#include "bp/session/Id.hxx"
#include "bp/session/Prng.hxx"

#include <gtest/gtest.h>

TEST(CsrfProtectionTest, Time)
{
	const auto now = std::chrono::system_clock::now();
	const auto a = CsrfHash::ImportTime(now);

	EXPECT_EQ(a, CsrfHash::ImportTime(CsrfHash::ExportTime(a)));
}

TEST(CsrfProtectionTest, FormatAndParse)
{
	SessionPrng prng;

	SessionId salt;
	salt.Generate(prng);
	EXPECT_TRUE(salt.IsDefined());

	CsrfToken a;
	a.Generate(std::chrono::system_clock::now(), salt);

	char s[CsrfToken::STRING_LENGTH + 1];
	a.Format(s);

	CsrfToken b;
	ASSERT_TRUE(b.Parse(s));
	EXPECT_EQ(CsrfHash::ImportTime(b.time), CsrfHash::ImportTime(a.time));
	EXPECT_EQ(b.hash, a.hash);

	char t[CsrfToken::STRING_LENGTH + 1];
	b.Format(t);

	EXPECT_STREQ(s, t);
}
