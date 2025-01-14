// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "ApproveIstream.hxx"
#include "ForwardIstream.hxx"
#include "Bucket.hxx"
#include "UnusedPtr.hxx"
#include "New.hxx"
#include "event/DeferEvent.hxx"
#include "io/FileDescriptor.hxx"

class ApproveIstream final : public ForwardIstream {
	const SharedPoolPtr<ApproveIstreamControl> control;

	DeferEvent defer_read;

	off_t approved = 0;

public:
	ApproveIstream(struct pool &p, EventLoop &event_loop,
		       UnusedIstreamPtr _input) noexcept
		:ForwardIstream(p, std::move(_input)),
		 control(SharedPoolPtr<ApproveIstreamControl>::Make(p, *this)),
		 defer_read(event_loop, BIND_THIS_METHOD(DeferredRead)) {}

	~ApproveIstream() noexcept override {
		control->approve = nullptr;
	}

	auto GetControl() noexcept {
		return control;
	}

	void Approve(off_t nbytes) noexcept {
		if (approved <= 0)
			defer_read.Schedule();

		approved += nbytes;
	}

private:
	void DeferredRead() noexcept {
		ForwardIstream::_Read();
	}

protected:
	/* virtual methods from class Istream */

	off_t _Skip(off_t length) noexcept override {
		if (approved <= 0)
			return -1;

		if (length > approved)
			length = approved;

		return ForwardIstream::_Skip(length);
	}

	void _Read() noexcept override {
		if (approved > 0)
			ForwardIstream::_Read();
	}

	void _FillBucketList(IstreamBucketList &list) override {
		if (approved <= 0) {
			list.SetMore();
			return;
		}

		IstreamBucketList tmp;

		try {
			input.FillBucketList(tmp);
		} catch (...) {
			Destroy();
			throw;
		}

		list.SpliceBuffersFrom(std::move(tmp), approved);
	}

	int _AsFd() noexcept override {
		return -1;
	}

	/* virtual methods from class IstreamHandler */

	std::size_t OnData(std::span<const std::byte> src) noexcept override {
		if (approved <= 0)
			return 0;

		if ((off_t)src.size() > approved)
			src = src.first((std::size_t)approved);

		return ForwardIstream::OnData(src);
	}

	IstreamDirectResult OnDirect(FdType type, FileDescriptor fd,
				     off_t offset,
				     std::size_t max_length) noexcept override {
		if (approved <= 0)
			return IstreamDirectResult::BLOCKING;

		if ((off_t)max_length > approved)
			max_length = (std::size_t)approved;

		return ForwardIstream::OnDirect(type, fd, offset, max_length);
	}
};

void
ApproveIstreamControl::Approve(off_t nbytes) noexcept
{
	if (approve != nullptr)
		approve->Approve(nbytes);
}

std::pair<UnusedIstreamPtr, SharedPoolPtr<ApproveIstreamControl>>
NewApproveIstream(struct pool &pool, EventLoop &event_loop,
		  UnusedIstreamPtr input)
{
	auto *i = NewIstream<ApproveIstream>(pool, event_loop, std::move(input));
	return std::make_pair(UnusedIstreamPtr{i}, i->GetControl());
}
