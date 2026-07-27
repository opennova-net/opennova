#pragma once

#include "npruntime/napi_np_server_ctx.h"

#include <npwire/peer_addr.h> // opennova::PeerAddr
#include <npwire/protocol_message.h>     // opennova::ProtocolMessage
// np::GameConfig (the consolidated server-state config) arrives via napi_np_server_ctx.h (ADR 0013).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// P2 — Per-connection handshake. The proven server legs of novaworld::HostSessionAccept, promoted
// onto npruntime free functions over NapiNPServerCtx / NapiNPProtocol.connection_list (the legs
// the original walks as NapiNPProtocol methods over its NapiListHead of NapiNPConnection nodes).
// Socket-free and Godot-agnostic: the owner (apps/nw_server or the Godot binding) does socket I/O
// and pumps raw datagrams through here.
//
//   0x41 ClientHello -> 0x81 ServerHello   [orig: NapiNPProtocol_HandleClientHello @0x6213b0]
//   0x42 ClientAuth  -> 0x82 ServerAuth     [orig: NapiNPProtocol_HandleClientJoin  @0x62b750]
//   0x43 SESSION     -> 0x83 SESSION        [orig: NapiNPProtocol_HandleSessionPacket @0x626A00]
//   0x46 ClientGoodbye                      [orig: Nwu_HandleClientGoodbye @0x624250]
//
// The per-connection SCRK/seq/ack + handshake latches live ON NapiNPConnection (folded from the
// old PeerState). The 0x43 reactive §5.1 replies are produced by the gameplay-message dispatcher
// (server_message_dispatch.h, dispatch_session_replies) over ctx.config + the node's reply
// state (P8 — the retired ctx.game_runtime / GameServerRuntime); the one-shot world-stream/spawn burst
// is Server_SendInitialGameStateToPlayer over conn.burst. The F3 dcb-timing fix and the D.0 name-match
// are ported VERBATIM. Wire bytes stay byte-exact.
//
// [orig: NapiNPProtocol_HandleSessionPacket @0x626A00; NapiNPConnection_ParseMessages @0x625BC0;
//  accept loop apps/novaworld_server/nw_udp_listener.cpp run_loop]
namespace opennova::np {

// Joiner pose recovered when a peer reaches Spawned. Position is i32 16.16 world: the joiner's own
// decoded C2S 0x0C uplink when one has arrived, else the host-advertised spawn from the session
// config (pos_valid distinguishes). (Relocated from novaworld::HostJoinerPose, unchanged.)
struct HostJoinerPose {
	bool     pos_valid = false;     // false => from config spawn, not a joiner uplink
	uint16_t entity_handle = 0;     // joiner's claimed pool<<12|slot (informational)
	uint16_t item_type_id = 0x14B9; // player infantry template type
	int32_t  pos_x = 0, pos_y = 0, pos_z = 0;
	int16_t  heading = 0;
	int16_t  pitch = 0;
	uint8_t  team = 1;
};

// One event surfaced by handle_server_datagram / tick_connections. Owner reacts:
// PeerEnteredWorldStreaming -> admit the joiner EARLY (spawn its pool-0 entity + stream its own
// dcb-bearing 0x0C) so the record is present in the client's load pump before its Player_InitPlayer
// runs; PeerSpawned -> bind the connection so the per-frame S2C 0x0A starts; PeerC2SInMatch ->
// route each 0x0C into the live sim; PeerGoodbye -> drop the connection.
//
// PeerEnteredWorldStreaming is the F3 dcb-timing fix: the retail client's load-time
// Player_InitPlayer @0x4e15f0 -> Player_FindLocalPlayerEntity @0x4e0090 scans pool 0 for its own
// entity (Flags&0x100 && entity+0x78 == its ConnectionId); that entity is created by the S2C 0x0C
// organic-spawn, which therefore must arrive DURING world streaming, before the game-start bundle.
// Emitting it reactively on PeerSpawned (after the bundle) is too late -> "Could not find player
// dcb" fatal @0x4dff60. (Relocated from novaworld::HostAcceptEvent, unchanged.)
struct HostAcceptEvent {
	enum class Kind {
		PeerHandshakeAdvanced,
		PeerEnteredWorldStreaming,
		PeerSpawned,
		PeerC2SInMatch,
		PeerGoodbye
	};
	Kind kind = Kind::PeerHandshakeAdvanced;
	PeerAddr peer;
	HostJoinerPose pose;                       // valid for PeerEnteredWorldStreaming / PeerSpawned
	uint32_t self_id = 0;                      // valid for PeerEnteredWorldStreaming / PeerSpawned:
	                                           // the joiner's own ConnectionId (NapiNPConnection's
	                                           // connection_id / unk_18 = its dcb), learned from its
	                                           // in-match 0x48 client-ack. The host MUST stamp this
	                                           // into the joiner's 0x0C entity_flags (entity+0x78) or
	                                           // the client's Player_FindLocalPlayerEntity self-scan
	                                           // fails ("Could not find player dcb"). Witnessed:
	                                           // working retail join ack==eFlags==3; a guess does not.
	std::string peer_name;                     // valid for PeerEnteredWorldStreaming / PeerSpawned:
	                                           // the joiner's game ClientAuth.NA callsign, streamed
	                                           // back as the S2C 0x0C organic entity_name so the
	                                           // joiner name-matches.
	std::vector<ProtocolMessage> in_match_c2s; // valid when kind == PeerC2SInMatch
};

struct HandleResult {
	std::vector<std::vector<uint8_t>> outbound; // fully-framed datagrams to send to `peer`
	// When the host owner asks to defer an in-match reply, these ordinary ProtocolMessages are
	// framed at the end of the same owner tick beside the per-frame transport output. This is the
	// retail send-boundary batching seam (for example S2C 0x57 + 0x0A).
	std::vector<ProtocolMessage>       deferred_session_replies;
	std::vector<HostAcceptEvent>      events;
};

struct TickOut {
	PeerAddr peer;
	std::vector<std::vector<uint8_t>> outbound;
	std::vector<HostAcceptEvent> events;
};

// Construct + configure + start the ctx's SESSION-leg spawn-gate runtime (the old
// HostSessionAccept(config) ctor + start()), and drop the server-side (type-1) remote-joiner
// connections — PRESERVING the host's own type-2 loopback (the faithful translation of
// HostSessionAccept::configure's peers_.clear(), which only held remote joiners).
//
// Canonical listen-host bring-up (P0 -> P1 -> P2): set_connection_mode -> set_transport_mode ->
// create_session(config, local_client) [P1: seeds ctx.config incl. the §5.1 reply slice,
// host_running=1, registers the loopback] -> configure_session_runtime() [P2: drops the type-1
// remote-joiner nodes, keeps the loopback]. The live handshake legs reject until host_running == 1,
// so this bring-up must run before any datagram.
void configure_session_runtime(NapiNPServerCtx &ctx);

// Decode + dispatch one raw inbound datagram from `peer` (the bytes off the socket, envelope+NWU
// still on). `now_tick` feeds the game-session tag clock. Returns the outbound datagrams to ship
// back + events to react to. [orig: NapiNPConnection_ParseMessages @0x625BC0 / the accept switch]
HandleResult handle_server_datagram(NapiNPServerCtx &ctx, const PeerAddr &peer,
                                    const uint8_t *raw, std::size_t len, uint32_t now_tick = 0,
                                    bool defer_in_match_replies = false);

// Finalize one host socket receive batch. Future packets latch a missing-sequence check while
// handle_server_datagram drains; only here, after later datagrams could have closed the gap, does
// the host emit one 0x84 per connection whose ordered queue is still nonempty.
// [orig: recv latch in HandleSessionPacket @0x626A00; SendMissingSeqList @0x623560 after PumpRecv]
std::vector<TickOut> flush_server_missing_requests(NapiNPServerCtx &ctx);

// Drive the periodic emitter for every connection NOT yet Spawned (so entity_batch_count climbs and
// the spawn gate opens) and frame each session's replies. PeerEnteredWorldStreaming surfaces here
// the tick a peer's batches start (F3). Spawned peers are skipped — Server_TickUpdate owns their 0x0A.
std::vector<TickOut> tick_connections(NapiNPServerCtx &ctx, int elapsed_ms, uint32_t now_tick);

// Wrap one in-match S2C inner message (e.g. the per-frame 0x0A body) into a fully-framed 0x83 SESSION
// datagram for `peer`, using that connection's live SCRK + seq (advances its outbound seq). False
// if the peer is unknown / pre-auth.
bool frame_in_match_s2c(NapiNPServerCtx &ctx, const PeerAddr &peer, uint8_t inner_tag,
                        const std::vector<uint8_t> &inner_body, std::vector<uint8_t> &out_datagram);

// Multi-message form used by the host owner to preserve one retail send boundary. The caller is
// responsible for MTU-aware grouping; this advances the connection's sequence exactly once.
bool frame_in_match_s2c_batch(NapiNPServerCtx &ctx, const PeerAddr &peer,
                              const std::vector<ProtocolMessage> &messages,
                              std::vector<uint8_t> &out_datagram);

// P5 — the production PeerC2SInMatch consumer. Route each decoded in-match C2S 0x0C carried by a
// PeerC2SInMatch `event` onto its owning connection's transport (ISessionTransport::deliver_c2s), so
// the NEXT Server_TickUpdate's drain_connection_c2s read-applies it. This is the owner-boundary form
// of the manual push tests/npruntime/joiner_connection_test does inline. Deliberately SEPARATE from
// handle_client_session (which only SURFACES the event): the single C2S drain/apply is
// Server_TickUpdate (D-NET-125), so the consumer stages into that drain rather than applying inline
// (which would be a double-apply trap). A null/unbound owning transport or a non-0x0C inner message
// is skipped. Returns the number of 0x0C uplinks staged. [D-NET-126; orig: NapiNPServerMsg_0x00C
// @0x501c30 -> dispatch_entity_packet_callback @0x4D6A80; docs/net/novaworld-net-re.md §5.44]
std::size_t apply_in_match_c2s(NapiNPServerCtx &ctx, const HostAcceptEvent &event);

// True once `peer` has completed handshake + spawn (its connection is live).
bool connection_spawned(const NapiNPServerCtx &ctx, const PeerAddr &peer);

bool bind_connection_player(NapiNPServerCtx &ctx, const PeerAddr &peer, uint8_t player_slot,
                            uint16_t entity_handle);

// Owner-initiated eviction of `peer`'s connection node — the recv-timeout / dead-endpoint path where
// no 0x46 ClientGoodbye ever arrives (a crashed or half-open peer would otherwise leak its
// connection_list node and keep getting per-frame 0x0A framed to a dead address). Mirrors
// handle_client_goodbye's complete player/entity/roster teardown without emitting
// an event — the owner already knows it is dropping. The owner is responsible for releasing its own
// (non-owning) transport for that peer. Returns true if a node was dropped. [orig: the timeout sweep
// under NapiNPProtocol_DrainTimers feeds Nwu_HandleDisconnect @0x624250 the same teardown.]
bool drop_connection(NapiNPServerCtx &ctx, const PeerAddr &peer);

std::size_t connection_count(const NapiNPServerCtx &ctx);

} // namespace opennova::np
