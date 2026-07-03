#pragma once

#include <array>
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
// harness) owns the LoopbackChannel / UdpSessionTransport; the per-connection drain/fan
// primitives (connection_fan.h) only fan the per-frame world reference over it and drain its
// C2S queue. The connection TABLE is owned by the host driver — npruntime's Server_TickUpdate
// over NapiNPProtocol.connection_list (each node embeds a Connection `link`).
struct Connection {
	// The byte transport for this peer (host's own client = a LoopbackChannel; a remote peer =
	// a UdpSessionTransport). Never null for a registered connection.
	ISessionTransport *transport = nullptr;

	// The witnessed transport mode (above). The host's own client is Loopback.
	TransportMode mode = TransportMode::Loopback;

	// The pool-0 player entity this connection drives — the SUBJECT its S2C 0x0A frame is
	// anchored to, and the owner the host verifies a C2S 0x0C uplink against [orig:
	// dispatch_entity_packet_callback @0x4D6A80 `entity == *owner_ctx`]. The host spawns it for
	// a joiner (spawn_remote_player, bound by the host driver). An INVALID handle = pre-spawn
	// (still handshaking): emit
	// falls back to the default anchor. The host's own loopback connection leaves this invalid
	// and rides the default anchor (= the local player; compute_net_anchor), preserving SP.
	world::EntityHandle owned_entity{};

	// Per-connection visibility/filter descriptor [orig: g_napi_np_ctx send_mask +0x1198].
	// PRESENT but UNREAD this increment — the 2-peer co-op MVP broadcasts the whole world to
	// every connection (faithful to a filter==1 send [orig: @0x510ED0]). The per-connection
	// SendFiltered cull is a deferred optimization; the field is here so it slots in without an
	// API change.
	uint32_t send_mask = 0;

	// Per-connection S2C 0x0A sub-block phase — the reimpl of the original's per-player-slot send
	// counter [orig: playerSlot+100566, ++ before every 0x0A in Server_SendEntityStateToPlayer
	// @0x517be8]. NetPacket_WritePlayerState @0x4ff6b0 writes it as the frame's `flags2` byte and
	// `phase & 3` selects the header sub-block (0 weapon / 1 server-status / 2 env / 3 gametype),
	// while `phase & 0xF == 8` gates the passenger block. emit_connection_s2c advances it per send.
	uint8_t s2c_phase = 0;

	// Per-connection entity AGE bytes for the 0x0A priority loop — the reimpl of the original's
	// per-player-slot age arrays: pool-0 ages at slot+89978 (256 B), pool-1 at slot+90234 (+256)
	// [orig: Server_BuildEntityPriorityList @ 0x50e590 saturating `paddusb +1` sweep @0x50e60f].
	// Index = (pool==1 ? 256 : 0) + (slot & 0xFF). Aged +1 (saturating) every emit, reset to 0
	// when the entity's record is serialized [orig: @0x50f168]; age raises the priority key and
	// age >= 50 force-admits past the 1124-tile distance gate, so budget-starved entities climb
	// until they win a slot — the original's round-robin is EMERGENT from aging (no resume cursor).
	std::array<uint8_t, 512> s2c_entity_age{};

	// Per-connection round-event watermark: the newest world.rounds sequence already swept
	// into this connection's 0x0A tag-2 stream [orig: playerSlot+97544, stamped = stat_id after
	// each Server_BuildRoundEventListForPlayer @0x4ffee0 sweep; its non-zero gate skips the walk
	// until the player is armed]. Armed on the first in-match emit at the CURRENT ring sequence,
	// so a joiner never receives the pre-join round backlog. (D-NET-152)
	uint32_t round_watermark = 0;
	bool round_watermark_armed = false;

	// RESPAWN-PENDING / undeployed — the reimpl of the player slot's stateByte bit4
	// (slot+89912 & 0x10). Set at join iff the mission offers deploy-selectable spawn zones
	// [orig: Server_OnPlayerJoin @0x51a6f2 `|= 0x10 iff SpawnZoneList_GetCount() > 0`];
	// cleared by a successful 0x0E deploy [orig: Server_ProcessPlayerDeath @0x517791
	// `and 0xEF`]. While set: the connection's 0x0A header flags1 carries bit1 EVERY frame
	// (the client's deploy screen is held open by it — one flags1 bit1=0 frame closes it
	// [orig: NetPacket_WritePlayerState @0x4ff7bd; client g_deploy_screen_active = (flags1 & 2) != 0
	// @0x42ff82]), the player entity carries the hidden bit0, and the 0x0E handler accepts
	// a deploy from an alive-but-undeployed player (the dead-or-pending gate @0x519cc7).
	// Death does NOT set it — the death screen is client-local (D-NET-156).
	bool respawn_pending = false;
};

} // namespace opennova::netsim
