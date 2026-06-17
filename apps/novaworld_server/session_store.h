#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace opennova::server {

// Per-tag in-memory state for the legacy NW*.dll login + join relay dance.
// Mirrors onnet's `login_sessions` / `join_sessions` dicts in
// onnw/controllers/nova_world/{login,join}.py — same fields, same key
// scheme.

struct LoginSession {
	std::string session_tag;
	// Identity (DB-backed, assigned at POST /NWLogin.dll). Until the EPASK
	// decoder lands (Phase E.4) we round-robin the seeded `players` rows
	// instead of honouring retail's encrypted NAME/PASSWORD form fields.
	int64_t     user_id = 0;     // players.id
	std::string username;        // players.username  (e.g. "test")
	std::string pcid;            // players.pcid      (e.g. "00000002")
	std::string nwh;             // players.nwh       (e.g. "1")
	std::string nwhandle;        // players.nwhandle  (e.g. "TestPlayer")
	std::string exp_bits = "3";  // game access / expansion ownership bits
	// Form-supplied template names (echoed by the relay GET).
	std::string relay;           // e.g. "jop_2_relay.htm"
	std::string msgbase;         // e.g. "jop_2_msg.htm"
	std::string success;         // e.g. "jop_2_main.htm"
	std::string failure;         // e.g. "jop_2_main.htm"
	std::string pfid;            // e.g. "28"
	std::string nodb;
	std::string needtoagree;
	std::string enterkey;
};

struct JoinSession {
	std::string session_tag;
	std::string success;
	std::string failure;
	std::string relay;
	std::string msgbase;
	std::string nodb;
	std::string needexpkey;
	std::string pfid;
	std::string mode;
	std::string rid;             // RID of the host the client is joining
};

struct HostSession {
	std::string session_tag;
	std::string host_key;        // 48-char A-P alphabet (24 random bytes nibble-encoded)
	std::string success;         // e.g. jop_2_host2.htm
	std::string failure;
	std::string relay;
	std::string msgbase;
	std::string nodb;
	std::string needexpkey;
	std::string pfid;
};

// Thread-safe session container. The HTTP handlers run on Crow's worker
// threads, so all reads/writes synchronise via the internal mutex.
class SessionStore {
public:
	SessionStore() = default;

	// Generate a tag matching onnet's format:
	//   NWServer:<dll>:SESSIONTAG:<5-digit random>:<8 hex chars>
	// (per onnw/controllers/nova_world/base.py:_generate_session_tag).
	std::string generate_tag(const std::string &dll_name);

	void put_login(const std::string &tag, LoginSession session);
	std::optional<LoginSession> get_login(const std::string &tag) const;
	void erase_login(const std::string &tag);

	void put_join(const std::string &tag, JoinSession session);
	std::optional<JoinSession> get_join(const std::string &tag) const;
	void erase_join(const std::string &tag);

	void put_host(const std::string &tag, HostSession session);
	std::optional<HostSession> get_host(const std::string &tag) const;
	void erase_host(const std::string &tag);

	std::size_t login_count() const;
	std::size_t join_count() const;
	std::size_t host_count() const;

	// Drop entries older than `max_age_ms`. Phase I.8 — keeps the maps
	// bounded under server uptime + occasional bot probes that POST
	// /NWLogin.dll without ever following up. Returns the number of
	// entries dropped across all three maps. Cheap enough to call from a
	// once-per-N-requests path on the request thread.
	std::size_t evict_older_than(uint64_t max_age_ms);

private:
	mutable std::mutex mu_;
	struct LoginEntry { LoginSession s; uint64_t created_ms; };
	struct JoinEntry  { JoinSession  s; uint64_t created_ms; };
	struct HostEntry  { HostSession  s; uint64_t created_ms; };
	std::unordered_map<std::string, LoginEntry> login_;
	std::unordered_map<std::string, JoinEntry>  join_;
	std::unordered_map<std::string, HostEntry>  host_;
};

} // namespace opennova::server
