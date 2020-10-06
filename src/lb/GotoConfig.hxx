/*
 * Copyright 2007-2019 CM4all GmbH
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

#include "ClusterConfig.hxx"
#include "SimpleHttpResponse.hxx"
#include "pcre/Regex.hxx"
#include "util/StringLess.hxx"

#include "util/Compiler.h"

#include <filesystem>
#include <string>
#include <list>
#include <map>

struct LbAttributeReference {
	enum class Type {
		METHOD,
		URI,
		HEADER,
	} type;

	std::string name;

	LbAttributeReference(Type _type) noexcept
		:type(_type) {}

	template<typename N>
	LbAttributeReference(Type _type, N &&_name) noexcept
		:type(_type), name(std::forward<N>(_name)) {}

	template<typename R>
	gcc_pure
	const char *GetRequestAttribute(const R &request) const noexcept {
		switch (type) {
		case Type::METHOD:
			return http_method_to_string(request.method);

		case Type::URI:
			return request.uri;

		case Type::HEADER:
			return request.headers.Get(name.c_str());
		}

		assert(false);
		gcc_unreachable();
	}

};

struct LbBranchConfig;
struct LbLuaHandlerConfig;
struct LbTranslationHandlerConfig;

struct LbGotoConfig {
	const LbClusterConfig *cluster = nullptr;
	const LbBranchConfig *branch = nullptr;
	const LbLuaHandlerConfig *lua = nullptr;
	const LbTranslationHandlerConfig *translation = nullptr;
	LbSimpleHttpResponse response;

	LbGotoConfig() = default;

	explicit LbGotoConfig(LbClusterConfig *_cluster) noexcept
		:cluster(_cluster) {}

	explicit LbGotoConfig(LbBranchConfig *_branch) noexcept
		:branch(_branch) {}

	explicit LbGotoConfig(LbLuaHandlerConfig *_lua) noexcept
		:lua(_lua) {}

	explicit LbGotoConfig(LbTranslationHandlerConfig *_translation) noexcept
		:translation(_translation) {}

	explicit LbGotoConfig(http_status_t _status) noexcept
		:response(_status) {}

	bool IsDefined() const noexcept {
		return cluster != nullptr || branch != nullptr ||
			lua != nullptr ||
			translation != nullptr ||
			response.IsDefined();
	}

	gcc_pure
	LbProtocol GetProtocol() const noexcept;

	gcc_pure
	const char *GetName() const noexcept;

#ifdef HAVE_AVAHI
	bool HasZeroConf() const noexcept;
#endif
};

struct LbConditionConfig {
	LbAttributeReference attribute_reference;

	enum class Operator {
		EQUALS,
		REGEX,
	};

	Operator op;

	bool negate;

	std::string string;
	UniqueRegex regex;

	LbConditionConfig(LbAttributeReference &&a, bool _negate,
			  const char *_string) noexcept
		:attribute_reference(std::move(a)), op(Operator::EQUALS),
		 negate(_negate), string(_string) {}

	LbConditionConfig(LbAttributeReference &&a, bool _negate,
			  UniqueRegex &&_regex) noexcept
		:attribute_reference(std::move(a)), op(Operator::REGEX),
		 negate(_negate), regex(std::move(_regex)) {}

	LbConditionConfig(LbConditionConfig &&other) = default;

	LbConditionConfig(const LbConditionConfig &) = delete;
	LbConditionConfig &operator=(const LbConditionConfig &) = delete;

	gcc_pure
	bool Match(const char *value) const noexcept {
		switch (op) {
		case Operator::EQUALS:
			return (string == value) ^ negate;

		case Operator::REGEX:
			return regex.Match(value) ^ negate;
		}

		gcc_unreachable();
	}

	template<typename R>
	gcc_pure
	bool MatchRequest(const R &request) const noexcept {
		const char *value = attribute_reference.GetRequestAttribute(request);
		if (value == nullptr)
			value = "";

		return Match(value);
	}
};

struct LbGotoIfConfig {
	LbConditionConfig condition;

	LbGotoConfig destination;

	LbGotoIfConfig(LbConditionConfig &&c, LbGotoConfig d) noexcept
		:condition(std::move(c)), destination(d) {}

#ifdef HAVE_AVAHI
	bool HasZeroConf() const {
		return destination.HasZeroConf();
	}
#endif
};

/**
 * An object that distributes connections or requests to the "real"
 * cluster.
 */
struct LbBranchConfig {
	std::string name;

	LbGotoConfig fallback;

	std::list<LbGotoIfConfig> conditions;

	explicit LbBranchConfig(const char *_name) noexcept
		:name(_name) {}

	LbBranchConfig(LbBranchConfig &&) = default;

	LbBranchConfig(const LbBranchConfig &) = delete;
	LbBranchConfig &operator=(const LbBranchConfig &) = delete;

	bool HasFallback() const noexcept {
		return fallback.IsDefined();
	}

	LbProtocol GetProtocol() const noexcept {
		return fallback.GetProtocol();
	}

#ifdef HAVE_AVAHI
	bool HasZeroConf() const noexcept;
#endif
};

/**
 * An HTTP request handler implemented in Lua.
 */
struct LbLuaHandlerConfig {
	std::string name;

	std::filesystem::path path;
	std::string function;

	explicit LbLuaHandlerConfig(const char *_name) noexcept
		:name(_name) {}

	LbLuaHandlerConfig(LbLuaHandlerConfig &&) = default;

	LbLuaHandlerConfig(const LbLuaHandlerConfig &) = delete;
	LbLuaHandlerConfig &operator=(const LbLuaHandlerConfig &) = delete;
};

struct LbTranslationHandlerConfig {
	std::string name;

	AllocatedSocketAddress address;

	std::map<const char *, LbGotoConfig, StringLess> destinations;

	explicit LbTranslationHandlerConfig(const char *_name) noexcept
		:name(_name) {}
};
