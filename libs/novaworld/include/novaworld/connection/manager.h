#pragma once

#include <novaworld/connection/registry.h>

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

// Owns a ConnectionRegistry and delivers lifecycle events to the app
// layer. Designed for a single tick thread to drive `tick(now_ms)` while
// other threads call into the mutating verbs (added/touch/etc.).
//
// Default heartbeat timeout — 120s. Retail stops sending UDP
// heartbeats while it's idle in the lobby browser (polling /jop_2.gsb
// over HTTP instead). 15s was too aggressive for that — the
// connection got evicted, and when retail resumed UDP after the user
// clicked Host, the SESSION packets bounced as "before AUTH" forever.
// Per-connection RandomizeTimeout window is still
// (CNapiNetwork_RandomizeTimeout @ 0x4a6d50 jodemo / 0x4c4d80 retail → [1000, 9999]ms) but the
// SERVER's grace can be much wider since we only enforce eventual
// cleanup. Override via HEARTBEAT_TIMEOUT_MS env var if needed.
class ConnectionManager {
public:
	using AddedHandler = std::function<void(const Connection &)>;
	using LostHandler  = std::function<void(const Connection &, DropReason)>;

	explicit ConnectionManager(uint64_t heartbeat_timeout_ms = 120000)
		: heartbeat_timeout_ms_(heartbeat_timeout_ms) {}

	ConnectionRegistry &registry() { return registry_; }
	const ConnectionRegistry &registry() const { return registry_; }

	void on_added(AddedHandler h) { added_handler_ = std::move(h); }
	void on_lost(LostHandler h) { lost_handler_ = std::move(h); }

	uint64_t heartbeat_timeout_ms() const { return heartbeat_timeout_ms_; }
	void set_heartbeat_timeout_ms(uint64_t ms) { heartbeat_timeout_ms_ = ms; }

	// Verbs the listener threads call when a HELLO/JOIN/SESSION/GOODBYE
	// packet has been recognised. These wrap the registry and fire the
	// appropriate lifecycle callback.
	void notify_handshake(Connection conn);
	void notify_active(uint32_t id, std::string identity,
	                   std::string client_scrk, std::string server_scrk);
	// Address-keyed variant — preferred for AUTH dispatch since two retail
	// processes both ship ci=0x00000001 (G.7).
	void notify_active_addr(const PeerAddr &addr, std::string identity,
	                        std::string client_scrk, std::string server_scrk);
	void notify_seen(uint32_t id, uint64_t now_ms);
	void notify_seen_addr(const PeerAddr &addr, uint64_t now_ms);
	void notify_logout(uint32_t id);
	// Address-keyed variant — preferred for GOODBYE dispatch (CI collisions).
	void notify_logout_addr(const PeerAddr &addr);

	// Tick from the server's main loop. Drops every connection whose
	// last_seen_ms exceeded the heartbeat timeout, fires `on_lost(.., Timeout)`
	// for each. Returns the number of connections dropped this tick.
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
