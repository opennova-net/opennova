#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <npwire/peer_addr.h> // opennova::PeerAddr / PeerAddrHash (moved out of this header)

namespace opennova {

// Lifecycle phases observed by the manager. The wire-level handshake lives
// in libs/napi (NWSession) — the Registry only mirrors lifecycle so the
// app layer (Drogon HTTP, Godot HUD, …) can subscribe.
enum class ConnectionState {
	Handshaking, // got HELLO, no JOIN yet
	Active,      // JOIN accepted, SESSION traffic flowing
	Closing,     // GOODBYE received OR explicit logout requested
};

// One tracked connection. The id is the session_id from the Layer-3
// ProtocolPacketHeader (witnessed at offset +1 of opcode 0x43 / 0x83
// packets — see libs/npwire/include/npwire/protocol_message.h).
struct Connection {
	// Server-local registry identity. It starts as ClientHello.CI, but the
	// registry may synthesize it when another endpoint already owns that CI.
	uint32_t id = 0;
	// The unmodified ClientHello.CI. Keeping it separate lets an exact later
	// Hello be recognized as a retransmit even when `id` was synthesized.
	uint32_t reported_id = 0;
	PeerAddr addr;
	ConnectionState state = ConnectionState::Handshaking;
	uint64_t created_ms = 0;
	uint64_t last_seen_ms = 0;
	std::string pn;          // negotiated Protocol Name from CLIENT_HELLO ("NOVAWORLDUDP", …)
	// Per-session crypto keys for the SESSION (0x43/0x83) inner-NWU cipher.
	// Inbound SESSION packets (C→S) are encrypted with `client_scrk`;
	// outbound SESSION packets (S→C) are encrypted with `server_scrk`.
	// Both come from the AUTH handshake (ClientAuth.scrk → client_scrk;
	// our locally-generated server scrk → server_scrk, also echoed in
	// ServerAuth.scrk so the client can decrypt our replies).
	//
	// LIFETIME (do not "dedupe" this against the lobby listener's copy):
	// these keys are WIRE-level and are reset whenever a ClientHello
	// re-inserts the address as Handshaking. The lobby listener keeps its own
	// durable copy in LobbyConnState (apps/novaworld_server/nw_udp_listener.h)
	// precisely so a repeated ClientAuth can replay the SAME cached ServerAuth
	// after such a reset — it restores these fields via notify_active_addr.
	// The two copies are a deliberate lifetime split, not duplication; merging
	// the stores would break the retransmit invariant (D-NET-104: an exact
	// 0x42 repeat re-sends the cached 0x82 and never re-mints keys).
	std::string client_scrk;
	std::string server_scrk;
	std::string identity;    // player handle once auth completes (empty during handshake)
};

// Thread-safe in-memory directory of connected peers. Owned by the
// ConnectionManager (this class has no timer of its own; the manager
// drives expiration). Designed to be touched from multiple listener
// threads (gate UDP, NW UDP, HTTP) so all mutating ops take a lock.
class ConnectionRegistry {
public:
	ConnectionRegistry() = default;

	// Insert a freshly accepted connection. If the same addr already has
	// an entry it's evicted first (replaces). If `conn.id` is already in
	// use by a *different* addr (G.7: two retail processes both send
	// ci=0x00000001), a unique synthetic id is assigned to the new entry
	// — caller should re-read the inserted connection via find_by_addr to
	// pick up the new id.
	void add(Connection conn);

	// Refresh `last_seen_ms` for an existing connection. No-op if missing.
	void touch(uint32_t id, uint64_t now_ms);

	// Refresh `last_seen_ms` for a connection found by remote addr (used by
	// pre-JOIN traffic where the session id isn't stable yet).
	void touch_addr(const PeerAddr &addr, uint64_t now_ms);

	// Remove a connection. Returns the removed entry if it existed.
	std::optional<Connection> drop(uint32_t id);

	// Address-keyed variant — preferred when the caller is reacting to an
	// inbound UDP datagram. Avoids the CI-collision pitfall (G.7).
	std::optional<Connection> drop_by_addr(const PeerAddr &addr);

	// Promote a Handshaking connection to Active once JOIN succeeds.
	// No-op if missing or already Active. Returns true on transition.
	// Stores both SCRKs (client- and server-generated) needed for SESSION
	// inner-NWU decrypt/encrypt respectively.
	bool mark_active(uint32_t id, std::string identity,
	                 std::string client_scrk, std::string server_scrk);

	// Address-keyed variant — preferred when the inbound packet identifies
	// the peer via PeerAddr rather than a server-assigned id.
	bool mark_active_by_addr(const PeerAddr &addr, std::string identity,
	                         std::string client_scrk, std::string server_scrk);

	std::optional<Connection> find(uint32_t id) const;
	std::optional<Connection> find_by_addr(const PeerAddr &addr) const;
	std::vector<Connection> snapshot() const;
	std::size_t size() const;

	// Return ids of connections whose last_seen_ms + timeout_ms < now_ms
	// (strict — a peer at exactly its deadline still gets one more tick).
	// Does NOT remove them — the manager is responsible for callback ordering.
	std::vector<uint32_t> iter_expired(uint64_t now_ms, uint64_t timeout_ms) const;

private:
	mutable std::mutex mu_;
	std::unordered_map<uint32_t, Connection> by_id_;
	std::unordered_map<PeerAddr, uint32_t, PeerAddrHash> by_addr_;
	uint32_t next_synth_id_ = 0x80000000u; // synthesized when CI collides (G.7)
};

} // namespace opennova
