// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

/*
 * Session management.
 */

#include "Lease.hxx"
#include "Session.hxx"
#include "Manager.hxx"

SessionLease::SessionLease(SessionManager &_manager, SessionId id) noexcept
	:SessionLease(_manager.Find(id)) {}

void
SessionLease::Put(SessionManager &manager, Session &session) noexcept
{
	manager.Put(session);
}

RealmSessionLease::RealmSessionLease(SessionLease &&src, const char *realm) noexcept
	:session(src.session != nullptr
		 ? src.session->GetRealm(realm)
		 : nullptr),
	 manager(src.manager)
{
	if (session != nullptr)
		src.session = nullptr;
}

RealmSessionLease::RealmSessionLease(SessionManager &_manager,
				     SessionId id, const char *realm) noexcept
	:manager(&_manager)
{
	SessionLease parent(_manager, id);
	if (!parent)
		return;

	session = parent.session->GetRealm(realm);
	if (session != nullptr)
		parent.session = nullptr;
}

void
RealmSessionLease::Put(SessionManager &manager, RealmSession &session) noexcept
{
	manager.Put(session.parent);
}
