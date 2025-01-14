// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "FourIstream.hxx"
#include "ForwardIstream.hxx"
#include "UnusedPtr.hxx"
#include "New.hxx"
#include "Bucket.hxx"
#include "io/FileDescriptor.hxx"

#include <algorithm>

class FourIstream final : public ForwardIstream {
public:
	FourIstream(struct pool &p, UnusedIstreamPtr _input)
		:ForwardIstream(p, std::move(_input)) {}

	/* virtual methods from class Istream */

	off_t _GetAvailable([[maybe_unused]] bool partial) noexcept override {
		return -1;
	}

	off_t _Skip([[maybe_unused]] off_t length) noexcept override {
		return -1;
	}

	void _FillBucketList(IstreamBucketList &list) override {
		IstreamBucketList tmp;

		try {
			input.FillBucketList(tmp);
		} catch (...) {
			Destroy();
			throw;
		}

		list.SpliceBuffersFrom(std::move(tmp), 4);
	}

	int _AsFd() noexcept override {
		return -1;
	}

	/* virtual methods from class IstreamHandler */

	std::size_t OnData(std::span<const std::byte> src) noexcept override {
		if (src.size() > 4)
			src = src.first(4);

		return ForwardIstream::OnData(src);
	}

	IstreamDirectResult OnDirect(FdType type, FileDescriptor fd,
				     off_t offset,
				     std::size_t max_length) noexcept override {
		return ForwardIstream::OnDirect(type, fd, offset,
						std::min(max_length, std::size_t{4}));
	}
};

UnusedIstreamPtr
istream_four_new(struct pool *pool, UnusedIstreamPtr input) noexcept
{
	return NewIstreamPtr<FourIstream>(*pool, std::move(input));
}
