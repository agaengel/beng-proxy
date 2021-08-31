/*
 * Copyright 2007-2021 CM4all GmbH
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

#include "SConnection.hxx"
#include "stock/Stock.hxx"

#include <cassert>

WasStockConnection::WasStockConnection(CreateStockItem c) noexcept
	:StockItem(c),
	 logger(GetStockName()),
	 connection(c.stock.GetEventLoop(), *this) {}

void
WasStockConnection::Stop(uint64_t _received) noexcept
{
	assert(!is_idle);

	connection.Stop(_received);
}

bool
WasStockConnection::Borrow() noexcept
{
	return connection.Borrow();
}

bool
WasStockConnection::Release() noexcept
{
	connection.Release();
	unclean = connection.IsStopping();
	return true;
}

void
WasStockConnection::OnWasIdleConnectionClean() noexcept
{
	ClearUncleanFlag();
}

void
WasStockConnection::OnWasIdleConnectionError(std::exception_ptr e) noexcept
{
	logger(2, e);
	InvokeIdleDisconnect();
}