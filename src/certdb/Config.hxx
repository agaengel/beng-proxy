// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include <map>
#include <string>
#include <array>

class LineParser;

struct CertDatabaseConfig {
	std::string connect;
	std::string schema;

	typedef std::array<unsigned char, 256/8> AES256;

	std::map<std::string, AES256> wrap_keys;

	std::string default_wrap_key;

	/**
	 * Throws on error.
	 *
	 * @return false if the word was not recognized
	 */
	bool ParseLine(const char *word, LineParser &line);

	/**
	 * Throws on error.
	 */
	void Check();
};

CertDatabaseConfig
LoadStandaloneCertDatabaseConfig(const char *path);
