// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "FacadeIstream.hxx"

class ForwardIstream : public FacadeIstream {
protected:
	template<typename I>
	ForwardIstream(struct pool &_pool, I &&_input)
		:FacadeIstream(_pool, std::forward<I>(_input)) {}

	explicit ForwardIstream(struct pool &_pool)
		:FacadeIstream(_pool) {}

public:
	/* virtual methods from class Istream */

	void _SetDirect(FdTypeMask mask) noexcept override {
		input.SetDirect(mask);
	}

	off_t _GetAvailable(bool partial) noexcept override {
		return input.GetAvailable(partial);
	}

	off_t _Skip(off_t length) noexcept override;

	void _Read() noexcept override {
		input.Read();
	}

	std::size_t _ConsumeBucketList(std::size_t nbytes) noexcept override {
		return Consumed(input.ConsumeBucketList(nbytes));
	}

	void _ConsumeDirect(std::size_t nbytes) noexcept override {
		input.ConsumeDirect(nbytes);
	}

	int _AsFd() noexcept override;

	/* virtual methods from class IstreamHandler */

	bool OnIstreamReady() noexcept override {
		return InvokeReady();
	}

	std::size_t OnData(std::span<const std::byte> src) noexcept override;

	IstreamDirectResult OnDirect(FdType type, FileDescriptor fd,
				     off_t offset,
				     std::size_t max_length) noexcept override;

	void OnEof() noexcept override;
	void OnError(std::exception_ptr ep) noexcept override;
};
