#pragma once

#include <cstdint>

#include <world/entity.h> // EntityHandle

#include "netsim/session_transport.h" // ISessionTransport

namespace opennova::netsim {

// The witnessed transport mode a connection carries [orig: CGameSession_SetConnectionMode
// @0x4c49f0 -> g_napi_np_ctx.transport_mode +0x50; CNapiNetwork_SetTransportMode @0x4c8750
// opens a socket only for 2/3/4 — mode 1 is in-process]. A connection's mode IS the
// original's mode, not an invented type (ADR 0011).
enum class TransportMode : uint8_t {
	Loopback = 1,   // socketless: the host's own local client (LoopbackChannel)
	Client = 2,     // a remote LAN/MP peer joining a host (socket)
	HostClient = 3, // host + client (the listen server itself opens a socket)
};

// One client connection on the authoritative host — the reimpl of a NapiNPConnection node on
// the server's connection list [orig: the list NapiNPServer_SendFiltered @0x4C87E0 walks, one
// SendToConn @0x4c4f20 per node]. The transport is NON-OWNING: the Godot binding (or a test
// harness) owns the LoopbackChannel / UdpSessionTransport; NetSystem only fans the per-frame
// world reference over it and drains its C2S queue. Keeping the table inside NetSystem mirrors
// the original — NetSystem IS our NapiNPServer (it already owns the per-frame world-state
// build and the receive drain).
struct Connection {
	// The byte transport for this peer (host's own client = a LoopbackChannel; a remote peer =
	// a UdpSessionTransport). Never null for a registered connection.
	ISessionTransport *transport = nullptr;

	// The witnessed transport mode (above). The host's own client is Loopback.
	TransportMode mode = TransportMode::Loopback;

	// The pool-0 player entity this connection drives — the SUBJECT its S2C 0x0A frame is
	// anchored to, and the owner the host verifies a C2S 0x0C uplink against [orig:
	// dispatch_entity_packet_callback @0x4D6A80 `entity == *owner_ctx`]. The host spawns it for
	// a joiner (NetSystem::admit_peer). An INVALID handle = pre-spawn (still handshaking): emit
	// falls back to the default anchor. The host's own loopback connection leaves this invalid
	// and rides the default anchor (= the local player; compute_net_anchor), preserving SP.
	world::EntityHandle owned_entity{};

	// Per-connection visibility/filter descriptor [orig: g_napi_np_ctx send_mask +0x1198].
	// PRESENT but UNREAD this increment — the 2-peer co-op MVP broadcasts the whole world to
	// every connection (faithful to a filter==1 send [orig: @0x510ED0]). The per-connection
	// SendFiltered cull is a deferred optimization; the field is here so it slots in without an
	// API change.
	uint32_t send_mask = 0;
};

} // namespace opennova::netsim
