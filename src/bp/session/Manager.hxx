// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "Session.hxx"
#include "Prng.hxx"
#include "event/FarTimerEvent.hxx"
#include "util/IntrusiveHashSet.hxx"

#include <chrono>
#include <random>

class SessionId;
class SessionLease;
class RealmSessionLease;
class BufferedReader;

class SessionManager {
	/** clean up expired sessions every 60 seconds */
	static constexpr Event::Duration cleanup_interval = std::chrono::minutes(1);

	const unsigned cluster_size, cluster_node;

	/**
	 * The idle timeout of sessions [seconds].
	 */
	const std::chrono::seconds idle_timeout;

	SessionPrng prng;

	struct SessionHash {
		[[gnu::pure]]
		size_t operator()(const SessionId &id) const {
			return id.Hash();
		}

		[[gnu::pure]]
		size_t operator()(const Session &session) const {
			return session.id.Hash();
		}
	};

	struct SessionEqual {
		[[gnu::pure]]
		bool operator()(const Session &a, const Session &b) const {
			return a.id == b.id;
		}

		[[gnu::pure]]
		bool operator()(const SessionId &a, const Session &b) const {
			return a == b.id;
		}
	};

	struct SessionAttachHash {
		[[gnu::pure]]
		size_t operator()(std::span<const std::byte> attach) const noexcept;

		[[gnu::pure]]
		size_t operator()(const Session &session) const noexcept;
	};

	struct SessionAttachEqual {
		[[gnu::pure]]
		bool operator()(const Session &a, const Session &b) const noexcept {
			return b.IsAttach(a.attach);
		}

		[[gnu::pure]]
		bool operator()(std::span<const std::byte> a, const Session &b) const noexcept {
			return b.IsAttach(a);
		}
	};

	static constexpr unsigned N_BUCKETS = 65521;

	using Set = IntrusiveHashSet<Session, N_BUCKETS,
				     SessionHash, SessionEqual,
				     IntrusiveHashSetMemberHookTraits<&Session::set_hook>,
				     true>;

	Set sessions;

	using ByAttach = IntrusiveHashSet<Session, N_BUCKETS,
					  SessionAttachHash, SessionAttachEqual,
					  IntrusiveHashSetMemberHookTraits<&Session::by_attach_hook>,
					  true>;

	ByAttach sessions_by_attach;

	FarTimerEvent cleanup_timer;

public:
	SessionManager(EventLoop &event_loop, std::chrono::seconds idle_timeout,
		       unsigned _cluster_size, unsigned _cluster_node) noexcept;

	~SessionManager() noexcept;

	/**
	 * Re-add all libevent events after DisableEvents().
	 */
	void EnableEvents() noexcept {
		cleanup_timer.Schedule(cleanup_interval);
	}

	/**
	 * Removes all libevent events.  Call this before fork(), or
	 * before creating a new event base.  Don't forget to call
	 * EnableEvents() afterwards.
	 */
	void DisableEvents() noexcept {
		cleanup_timer.Cancel();
	}

	void AdjustNewSessionId(SessionId &id) const noexcept;

	/**
	 * Returns the number of sessions.
	 */
	[[gnu::pure]]
	unsigned Count() const noexcept {
		return sessions.size();
	}

	/**
	 * Invoke the callback for each session.
	 */
	void Visit(void (*callback)(const Session *session,
				    void *ctx), void *ctx);

	[[gnu::pure]]
	SessionLease Find(SessionId id) noexcept;

	/**
	 * Attach the given session to an existing session with the
	 * given #attach value.  If no such session exists already,
	 * only the #attach value of the given session is modified.
	 *
	 * If the given lease is #nullptr, a new session is created
	 * (or an existing one with the given #attach value is
	 * returned).
	 *
	 * @return a new lease for the attached session
	 */
	RealmSessionLease Attach(RealmSessionLease lease, const char *realm,
				 std::span<const std::byte> attach) noexcept;

	void Put(Session &session) noexcept;

	/**
	 * Add an initialized #Session object to the session manager.  It
	 * will be destroyed automatically when the session
	 * expires.  After returning from this function, the session is
	 * protected and the pointer must not be used, unless it is looked
	 * up (and thus locked).
	 */
	void Insert(Session &session) noexcept;

	void EraseAndDispose(SessionId id) noexcept;

	void DiscardRealmSession(SessionId id, const char *realm) noexcept;

	SessionLease CreateSession() noexcept;

	/**
	 * Forcefully deletes at least one session.
	 */
	bool Purge() noexcept;

	void Cleanup() noexcept;

	void DiscardAttachSession(std::span<const std::byte> attach) noexcept;

	bool Load(BufferedReader &r);

private:
	void SeedPrng();

	SessionId GenerateSessionId() noexcept;
	void EraseAndDispose(Session &session);
};
