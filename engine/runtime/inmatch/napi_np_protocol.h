#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h>

#include <net/npwire/peer_addr.h> // opennova::PeerAddr
#include <net/npwire/protocol_message.h>     // opennova::ProtocolMessage
// inmatch::GameConfig (the consolidated server-state config) arrives via napi_np_server_ctx.h (ADR 0013).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// P2 — Per-connection handshake. The proven server legs of novaworld::HostSessionAccept, promoted
// onto inmatch free functions over NapiNPServerCtx / NapiNPProtocol.connection_list (the legs
// the original walks as NapiNPProtocol methods over its NapiListHead of NapiNPConnection nodes).
// Socket-free and Godot-agnostic: the owner (the Godot binding) does socket I/O
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
// is Server_SendInitialGameStateToPlayer over conn.burst. The F3 dcb-timing fix and the D.0 owner-ID match
// are ported VERBATIM. Wire bytes stay byte-exact.
//
// [orig: NapiNPProtocol_HandleSessionPacket @0x626A00; CNapiNPConnection_ParseMessages @0x625BC0;
//  accept loop apps/novaworld_server/nw_udp_listener.cpp run_loop]
namespace opennova::inmatch {

// Joiner pose recovered when a peer reaches Spawned. Position is i32 16.16 world: the joiner's own
// decoded C2S 0x0C uplink when one has arrived, else the host-advertised spawn from the session
// config.
struct HostJoinerPose {
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
	                                           // back as the S2C 0x0C organic entity_name (the
	                                           // displayed callsign only; the joiner self-identifies
	                                           // by the record's owner_connection_id == ServerAuth.MI,
	                                           // never by this name).
	std::vector<ProtocolMessage> in_match_c2s; // valid when kind == PeerC2SInMatch
};

struct HandleResult {
	std::vector<std::vector<uint8_t>> outbound; // fully-framed datagrams to send to `peer`
	// Datagrams retail writes from INSIDE the server receive pump, which every Server_TickUpdate
	// runs ungated: the 0x83 packets a client 0x44 asks for, each rebuilt from its retained
	// records with the current ACK. They never wait for the connection's S2C send boundary; the
	// owner ships them at their datagram's receive position.
	// [orig: Server_TickUpdate -> CNapiNetwork_PumpServerProtocolRecv @0x51d895 (flags 0x19:
	//  0x8 = NapiNPProtocol_PumpRecvQueues @0x6266a0, queue_mask & 1 dispatch @0x6266ca..0x6267ad);
	//  NapiNP_HandleResendList @0x623800 -> CNapiNPConnection_SendSessionPacket @0x6239b6 ->
	//  CNapiNPManager_SendTo @0x61f039]
	std::vector<std::vector<uint8_t>> immediate_outbound;
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

// Decode + dispatch one raw inbound datagram from `peer` (the bytes off the socket, envelope+NWU
// still on). `now_tick` feeds the game-session tag clock. Returns the outbound datagrams to ship
// back + events to react to. [orig: CNapiNPConnection_ParseMessages @0x625BC0 / the accept switch]
HandleResult handle_server_datagram(NapiNPServerCtx &ctx, const PeerAddr &peer,
                                    const uint8_t *raw, std::size_t len, uint32_t now_tick = 0,
                                    bool defer_in_match_replies = false);

// Finalize one host socket receive batch. Future packets latch a missing-sequence check while
// handle_server_datagram drains; only here, after later datagrams could have closed the gap, does
// the host emit one 0x84 per connection whose ordered queue is still nonempty. This is the
// receive pump's own tail, every server tick: the owner sends the result at once, never behind
// the connection's S2C send boundary.
// [orig: recv latch in HandleSessionPacket @0x626c3a; the PumpRecvQueues per-connection tail
//  @0x6269bb..0x6269d6 -> SendMissingSeqList(conn, 0) @0x6269ce -> CNapiNPManager_SendTo
//  @0x62376d, run from PumpServerProtocolRecv @0x51d895 ahead of PumpFlags' 0x10 decrement]
std::vector<TickOut> flush_server_missing_requests(NapiNPServerCtx &ctx);

// Drive the periodic emitter for every connection NOT yet Spawned (so entity_batch_count climbs and
// the spawn gate opens) and frame each session's replies. PeerEnteredWorldStreaming surfaces here
// the tick a peer's batches start (F3). Spawned peers are skipped — Server_TickUpdate owns their 0x0A.
std::vector<TickOut> tick_connections(
		NapiNPServerCtx &ctx, int elapsed_ms, uint32_t now_tick,
		bool respect_s2c_send_boundary = false);

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
// of the manual push the since-removed joiner_connection_test did inline. Deliberately SEPARATE from
// handle_client_session (which only SURFACES the event): the single C2S drain/apply is
// Server_TickUpdate (D-NET-125), so the consumer stages into that drain rather than applying inline
// (which would be a double-apply trap). A null/unbound owning transport or a non-0x0C inner message
// is skipped. Returns the number of 0x0C uplinks staged. [D-NET-126; orig: NapiNPServerMsg_0x00C
// @0x501c30 -> NetPacket_DispatchEntityPacketCallback @0x4D6A80; docs/net/novaworld-net-re.md §5.44]
std::size_t apply_in_match_c2s(NapiNPServerCtx &ctx, const HostAcceptEvent &event);

// Test seams: the handshake/spawn tests read and pin a node through these three; no production
// caller uses them. connection_spawned is true once `peer` has completed handshake + spawn (its
// connection is live); bind_connection_player binds a slot + entity onto an authenticated node;
// connection_count is the node count.
bool connection_spawned(const NapiNPServerCtx &ctx, const PeerAddr &peer);

bool bind_connection_player(NapiNPServerCtx &ctx, const PeerAddr &peer, uint8_t player_slot,
                            uint16_t entity_handle);

// The host-initiated destroy of `peer`'s node: the S2C 0x86 SERVER_GOODBYE burst is appended to
// `goodbye_out` (when non-null) BEFORE the player/entity/roster teardown and the node erase —
// retail sends the burst, then fires the removal callback, then clears keys and queues.
// Producers: the receive reap (SERTMOUT), the pending-disconnect pump, StopServer, same-endpoint
// replacement, a received 0x46 (its record echoed back). A null `goodbye_out` is the owner's
// dead-endpoint eviction (the transport already knows the address is gone): the same complete
// teardown with no wire output. The owner is responsible for releasing its own (non-owning)
// transport for that peer and for shipping the datagrams. Returns true if a node was dropped.
// [orig: CNapiNPConnection_Destroy @0x62A4B0 -> TeardownActiveConnection @0x6253C0 (burst
//  @0x6253ef, removal callback @0x625426, keys/queues cleared @0x625535..0x625574) ->
//  Server_HandlePlayerDisconnect @0x51B5C0]
bool destroy_connection(NapiNPServerCtx &ctx, const PeerAddr &peer,
		std::vector<std::vector<uint8_t>> *goodbye_out);

std::size_t connection_count(const NapiNPServerCtx &ctx);

} // namespace opennova::inmatch
