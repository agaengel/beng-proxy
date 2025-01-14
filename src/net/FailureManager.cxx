// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "FailureManager.hxx"
#include "FailureRef.hxx"
#include "net/AllocatedSocketAddress.hxx"
#include "net/SocketAddress.hxx"
#include "util/djbhash.h"
#include "util/LeakDetector.hxx"

#include <assert.h>

class FailureManager::Failure final
	: public IntrusiveHashSetHook<IntrusiveHookMode::NORMAL>,
	  LeakDetector,
	  public ReferencedFailureInfo {

	const AllocatedSocketAddress address;

public:
	explicit Failure(SocketAddress _address) noexcept
		:address(_address) {}

	SocketAddress GetAddress() const noexcept {
		return address;
	}

protected:
	void Destroy() override {
		delete this;
	}
};

inline size_t
FailureManager::Hash::operator()(const SocketAddress a) const noexcept
{
	assert(!a.IsNull());

	return djb_hash(a.GetAddress(), a.GetSize());
}

inline size_t
FailureManager::Hash::operator()(const Failure &f) const noexcept
{
	return this->operator()(f.GetAddress());
}

inline bool
FailureManager::Equal::operator()(const SocketAddress a,
				  const SocketAddress b) const noexcept
{
	return a == b;
}

inline bool
FailureManager::Equal::operator()(const SocketAddress a,
				  const Failure &b) const noexcept
{
	return a == b.GetAddress();
}

FailureManager::FailureManager() noexcept = default;

FailureManager::~FailureManager() noexcept
{
	failures.clear_and_dispose(Failure::UnrefDisposer());
}

ReferencedFailureInfo &
FailureManager::Make(SocketAddress address) noexcept
{
	assert(!address.IsNull());

	auto [position, inserted] = failures.insert_check(address);
	if (inserted) {
		Failure *failure = new Failure(address);
		failures.insert(position, *failure);
		return *failure;
	} else {
		return *position;
	}
}

SocketAddress
FailureManager::GetAddress(const FailureInfo &info) const noexcept
{
	const auto &f = (const Failure &)info;
	return f.GetAddress();
}

FailureStatus
FailureManager::Get(const Expiry now, SocketAddress address) const noexcept
{
	assert(!address.IsNull());

	auto i = failures.find(address);
	if (i == failures.end())
		return FailureStatus::OK;

	return i->GetStatus(now);
}

bool
FailureManager::Check(const Expiry now, SocketAddress address,
		      bool allow_fade) const noexcept
{
	assert(!address.IsNull());

	auto i = failures.find(address);
	if (i == failures.end())
		return true;

	return i->Check(now, allow_fade);
}
