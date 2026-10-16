#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace opennova::novaworld_server {

class RedisPool;

// Per-tag state for the legacy NW*.dll login + join relay dance.
// Follows onnet's `login_sessions` / `join_sessions` dicts in
// onnw/controllers/nova_world/{login,join}.py — same key scheme, holding the
// fields a later leg of the dance reads back.

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
	// Form-supplied template names read back by the relay GET (the relay
	// template itself renders the POST response only and is not stored).
	std::string msgbase;         // e.g. "jop_2_msg.htm"
	std::string success;         // e.g. "jop_2_main.htm"
	std::string failure;         // e.g. "jop_2_main.htm"
};

struct JoinSession {
	std::string session_tag;
	std::string success;
	std::string failure;
	std::string msgbase;
	std::string needexpkey;
	std::string pfid;
	std::string rid;             // RID of the host the client is joining
};

struct HostSession {
	std::string session_tag;
	std::string host_key;        // 48-char A-P alphabet (24 random bytes nibble-encoded)
	std::string success;         // e.g. jop_2_host2.htm
	std::string pfid;
};

// The sessions, in Redis (redis_client.h), so a dance's legs may land on
// different web replicas: one key per tag (<prefix>login:<tag>, join:, host:),
// written with a TTL of kTtl, so a session a client never followed up (a bot's
// POST /NWLogin.dll) is gone 5 minutes after it was written. Every call is one
// command; a Redis failure throws RedisError (the request fails).
class SessionStore {
public:
	// How long a session lives after it was written.
	static constexpr std::chrono::minutes kTtl{ 5 };

	explicit SessionStore(RedisPool &redis) : redis_(redis) {}

	// Generate a tag matching onnet's format:
	//   NWServer:<dll>:SESSIONTAG:<5-digit random>:<8 hex chars>
	// (per onnw/controllers/nova_world/base.py:_generate_session_tag).
	static std::string generate_tag(const std::string &dll_name);

	void put_login(const std::string &tag, const LoginSession &session);
	std::optional<LoginSession> get_login(const std::string &tag);
	void erase_login(const std::string &tag);

	void put_join(const std::string &tag, const JoinSession &session);
	std::optional<JoinSession> get_join(const std::string &tag);

	void put_host(const std::string &tag, const HostSession &session);
	std::optional<HostSession> get_host(const std::string &tag);
	// The tag's session, removed in the same step (GETDEL): /NWHost.dll's
	// second leg serves a HOSTKEY once, across every replica, so a tag seen in
	// a log or a jar serves nothing. Never sent twice (RedisResend::Never): a
	// connection found dropped fails the call, as a second GETDEL after one the
	// server ran would answer no session.
	std::optional<HostSession> take_host(const std::string &tag);

private:
	RedisPool &redis_;
};

} // namespace opennova::novaworld_server
