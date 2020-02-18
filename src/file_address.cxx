/*
 * Copyright 2007-2020 CM4all GmbH
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

#include "file_address.hxx"
#include "delegate/Address.hxx"
#include "uri/Base.hxx"
#include "uri/Compare.hxx"
#include "util/StringView.hxx"
#include "puri_escape.hxx"
#include "pexpand.hxx"
#include "pool/pbuffer.hxx"
#include "AllocatorPtr.hxx"

#include <assert.h>
#include <string.h>

FileAddress::FileAddress(AllocatorPtr alloc, const FileAddress &src,
			 const char *_path) noexcept
	:path(_path),
	 deflated(alloc.CheckDup(src.deflated)),
	 gzipped(alloc.CheckDup(src.gzipped)),
	 content_type(alloc.CheckDup(src.content_type)),
	 content_type_lookup(alloc.Dup(src.content_type_lookup)),
	 document_root(alloc.CheckDup(src.document_root)),
	 delegate(src.delegate != nullptr
		  ? alloc.New<DelegateAddress>(alloc, *src.delegate)
		  : nullptr),
	 auto_gzipped(src.auto_gzipped),
	 expand_path(src.expand_path),
	 expand_document_root(src.expand_document_root)
{
}

FileAddress::FileAddress(AllocatorPtr alloc, const FileAddress &src) noexcept
	:FileAddress(alloc, src, alloc.Dup(src.path)) {}

void
FileAddress::Check() const
{
	if (delegate != nullptr)
		delegate->Check();
}

bool
FileAddress::IsValidBase() const noexcept
{
	return IsExpandable() || is_base(path);
}

FileAddress *
FileAddress::SaveBase(AllocatorPtr alloc, const char *suffix) const noexcept
{
	assert(suffix != nullptr);

	const char *end = UriFindUnescapedSuffix(path, suffix);
	if (end == nullptr)
		return nullptr;

	auto *dest = alloc.New<FileAddress>(alloc, *this,
					    alloc.DupZ({path, end}));

	/* BASE+DEFLATED is not supported */
	dest->deflated = nullptr;
	dest->gzipped = nullptr;

	return dest;
}

FileAddress *
FileAddress::LoadBase(AllocatorPtr alloc, const char *suffix) const noexcept
{
	assert(path != nullptr);
	assert(*path != 0);
	assert(path[strlen(path) - 1] == '/');
	assert(suffix != nullptr);

	char *new_path = uri_unescape_concat(alloc, path, suffix);
	if (new_path == nullptr)
		return nullptr;

	return alloc.New<FileAddress>(alloc, *this, new_path);
}

bool
FileAddress::IsExpandable() const noexcept
{
	return expand_path ||
		expand_document_root ||
		(delegate != nullptr && delegate->IsExpandable());
}

void
FileAddress::Expand(AllocatorPtr alloc, const MatchInfo &match_info)
{
	if (expand_path) {
		expand_path = false;
		path = expand_string_unescaped(alloc, path, match_info);
	}

	if (expand_document_root) {
		expand_document_root = false;
		document_root = expand_string_unescaped(alloc, document_root,
							match_info);
	}

	if (delegate != nullptr)
		delegate->Expand(alloc, match_info);
}
