// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "ExpansibleBuffer.hxx"
#include "pool/pool.hxx"
#include "util/Poison.hxx"

#include <assert.h>
#include <string.h>

ExpansibleBuffer::ExpansibleBuffer(struct pool &_pool,
				   size_t initial_size,
				   size_t _hard_limit) noexcept
	:pool(_pool),
	 buffer((char *)p_malloc(&pool, initial_size)),
	 hard_limit(_hard_limit),
	 max_size(initial_size)
{
	assert(initial_size > 0);
	assert(hard_limit >= initial_size);
}

void
ExpansibleBuffer::Clear() noexcept
{
	PoisonUndefined(buffer, max_size);

	size = 0;
}

bool
ExpansibleBuffer::Resize(size_t new_max_size) noexcept
{
	assert(new_max_size > max_size);

	if (new_max_size > hard_limit)
		return false;

	char *new_buffer = (char *)p_malloc(&pool, new_max_size);
	memcpy(new_buffer, buffer, size);

	p_free(&pool, buffer, max_size);

	buffer = new_buffer;
	max_size = new_max_size;
	return true;
}

void *
ExpansibleBuffer::Write(size_t length) noexcept
{
	size_t new_size = size + length;
	if (new_size > max_size &&
	    !Resize(((new_size - 1) | 0x3ff) + 1))
		return nullptr;

	char *dest = buffer + size;
	size = new_size;

	return dest;
}

bool
ExpansibleBuffer::Write(const void *p, size_t length) noexcept
{
	void *q = Write(length);
	if (q == nullptr)
		return false;

	memcpy(q, p, length);
	return true;
}

bool
ExpansibleBuffer::Write(const char *p) noexcept
{
	return Write(p, strlen(p));
}

bool
ExpansibleBuffer::Set(const void *p, size_t new_size) noexcept
{
	if (new_size > max_size && !Resize(((new_size - 1) | 0x3ff) + 1))
		return false;

	size = new_size;
	memcpy(buffer, p, new_size);
	return true;
}

bool
ExpansibleBuffer::Set(std::string_view p) noexcept
{
	return Set(p.data(), p.size());
}

std::span<const std::byte>
ExpansibleBuffer::Read() const noexcept
{
	return {(const std::byte *)buffer, size};
}

const char *
ExpansibleBuffer::ReadString() noexcept
{
	if (size == 0 || buffer[size - 1] != 0)
		/* append a null terminator */
		Write("\0", 1);

	/* the buffer is now a valid C string (assuming it doesn't contain
	   any nulls */
	return buffer;
}

std::string_view
ExpansibleBuffer::ReadStringView() const noexcept
{
	return { (const char *)buffer, size };
}

std::span<std::byte>
ExpansibleBuffer::Dup(struct pool &_pool) const noexcept
{
	return {
		(std::byte *)p_memdup(&_pool, buffer, size),
		size,
	};
}

char *
ExpansibleBuffer::StringDup(struct pool &_pool) const noexcept
{
	char *p = (char *)p_malloc(&_pool, size + 1);
	memcpy(p, buffer, size);
	p[size] = 0;
	return p;
}
