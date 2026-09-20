#pragma once

#include <net/novaworld/connection/registry.h>

#include <cstdint>
#include <functional>
#include <string>

namespace opennova {

// Reasons a connection can leave the registry. Hand-tracked so the app
// layer (HTTP /api/lobbies, Godot HUD) can format/log differently.
enum class DropReason {
	Logout,    // explicit GOODBYE / /api/logout / client politely left
	Timeout,   // last_seen_ms exceeded the heartbeat window
	Replaced,  // a new HELLO from the same addr/id evicted the prior entry
	Shutdown,  // server is stopping
};

const char *drop_reason_name(DropReason r);

// The receive-silence window the service advertises to every NOVAWORLDUDP
// peer: cs[0] (timeout_ms) of the service CS template the 0x82 SessionInit
// carries, 240000 ms [orig: CNapiGameSession_InitNPConnection @0x4d3e1f].
// The peer drops the connection only on silence strictly greater than that
// value [orig: CNapiNPConnection_PumpStateMachine @0x6292e0 compares
// GetTickCount deltas against cs_dir0.timeout_ms], and the service reaps on
// the same contract — one value, sourced from the template, never a second
// literal. (An earlier 120 s reap evicted idle browser clients two
// keepalives early; that comment cited CNapiNetwork_RandomizeTimeout
// @0x4c4d80 as its basis, but that routine draws the host's per-session
// AppId, not a timeout.)
uint64_t novaworldudp_session_timeout_ms();

// Owns a ConnectionRegistry and delivers lifecycle events to the app
// layer. Designed for a single tick thread to drive `tick(now_ms)` while
// other threads call into the mutating verbs (added/touch/etc.).
class ConnectionManager {
public:
	using AddedHandler = std::function<void(const Connection &)>;
	using LostHandler  = std::function<void(const Connection &, DropReason)>;

	explicit ConnectionManager(uint64_t heartbeat_timeout_ms = novaworldudp_session_timeout_ms())
		: heartbeat_timeout_ms_(heartbeat_timeout_ms) {}

	ConnectionRegistry &registry() { return registry_; }
	const ConnectionRegistry &registry() const { return registry_; }

	void on_added(AddedHandler h) { added_handler_ = std::move(h); }
	void on_lost(LostHandler h) { lost_handler_ = std::move(h); }

	uint64_t heartbeat_timeout_ms() const { return heartbeat_timeout_ms_; }

	// Verbs the listener threads call when a HELLO/JOIN/SESSION/GOODBYE
	// packet has been recognised. These wrap the registry and fire the
	// appropriate lifecycle callback.
	void notify_handshake(Connection conn);
	// The per-connection verbs are address-keyed: two retail processes both
	// ship ci=0x00000001 (G.7), so the reported id cannot pick the connection
	// for AUTH, keepalive or GOODBYE dispatch.
	void notify_active_addr(const PeerAddr &addr, std::string identity,
	                        std::string client_scrk, std::string server_scrk);
	void notify_seen_addr(const PeerAddr &addr, uint64_t now_ms);
	void notify_logout_addr(const PeerAddr &addr);

	// Tick from the server's main loop. Drops every connection whose
	// receive silence exceeds the timeout (strictly greater), fires
	// `on_lost(.., Timeout)` for each. Returns the number dropped this tick.
	std::size_t tick(uint64_t now_ms);

	// Called once at server shutdown — fires `on_lost(.., Shutdown)` for
	// every remaining connection then clears the registry.
	void shutdown();

private:
	ConnectionRegistry registry_;
	AddedHandler added_handler_;
	LostHandler lost_handler_;
	uint64_t heartbeat_timeout_ms_;
};

} // namespace opennova
