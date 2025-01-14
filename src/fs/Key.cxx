// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "Key.hxx"
#include "fs/Factory.hxx"
#include "net/SocketAddress.hxx"
#include "net/ToString.hxx"
#include "util/StringBuilder.hxx"

#include <assert.h>
#include <string.h>

static void
AppendSocketAddress(StringBuilder &b, SocketAddress address)
{
	assert(!address.IsNull());

	auto w = b.Write();
	if (ToString(w.data(), w.size(), address))
		b.Extend(strlen(w.data()));
}

static void
MakeKey(StringBuilder &b, SocketAddress bind_address,
	SocketAddress address) noexcept
{
	if (!bind_address.IsNull()) {
		AppendSocketAddress(b, bind_address);
		b.Append('>');
	}

	AppendSocketAddress(b, address);
}

void
MakeFilteredSocketStockKey(StringBuilder &b, const char *name,
			   SocketAddress bind_address, SocketAddress address,
			   const SocketFilterFactory *filter_factory)
{
	if (name != nullptr)
		b.Append(name);
	else
		MakeKey(b, bind_address, address);

	if (filter_factory != nullptr) {
		b.Append('|');

		const char *id = filter_factory->GetFilterId();
		if (id != nullptr)
			b.Append(id);
	}
}
