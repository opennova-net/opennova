#include "npruntime/napi_np_protocol.h"

#include "npruntime/server_initial_state.h" // Server_SendInitialGameStateToPlayer (the §5.2a burst)
#include "npruntime/server_spawn.h"         // Server_ProcessPendingPlayerSpawns (World-driven spawn)

#include <novaworld/game_server_runtime.h>
#include <novaworld/nw_session_framing.h>
#include <novaworld/protocol_message.h> // make_protocol_message (frame the burst messages)
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>
#include <novaworld/session_protocol.h>

#include <netsim/session_transport.h> // ISessionTransport::host_send (loopback burst delivery)

#include <world/entity.h>
#include <world/geom.h> // to_fixed
#include <world/world.h>

#include <cstdio>
#include <utility>

namespace opennova::np {

// Out-of-line ctor/dtor/move: GameServerRuntime is incomplete in the ctx header (forward-declared
// to keep game_session.h out of it), so the unique_ptr<GameServerRuntime> ctor/dtor must be
// emitted here, where the type is complete.
NapiNPServerCtx::NapiNPServerCtx() = default;
NapiNPServerCtx::~NapiNPServerCtx() = default;
NapiNPServerCtx::NapiNPServerCtx(NapiNPServerCtx &&) noexcept = default;
NapiNPServerCtx &NapiNPServerCtx::operator=(NapiNPServerCtx &&) noexcept = default;

namespace {

// Stable "a.b.c.d:port" label — the GameServerRuntime sessions_ key. PeerAddr.ip is LE octet
// packing (a | b<<8 | c<<16 | d<<24); print low->high so the label reads a.b.c.d (matches
// nw_udp_listener's client_label). [orig: HostSessionAccept::peer_session_id]
std::string peer_session_id(const PeerAddr &peer) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
	              peer.ip & 0xffu, (peer.ip >> 8) & 0xffu,
	              (peer.ip >> 16) & 0xffu, (peer.ip >> 24) & 0xffu, peer.port);
	return buf;
}

NapiNPConnection *find_connection(NapiNPServerCtx &ctx, const PeerAddr &peer) {
	for (auto &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) return &c;
	}
	return nullptr;
}

// Linear scan + create-on-miss over connection_list — faithful to the original intrusive-list walk
// (NapiNPServer_SendFiltered @0x4C87E0 -> SendToConn per node). connection_list stays authoritative
// (P4 reconciles it vs the NetSystem table). A fresh node is a server-side connection (type 1).
NapiNPConnection &find_or_create_connection(NapiNPServerCtx &ctx, const PeerAddr &peer) {
	if (NapiNPConnection *existing = find_connection(ctx, peer)) return *existing;
	NapiNPConnection node;
	node.peer = peer;
	node.type = 1; // server-side: the host's view of a client
	ctx.np_protocol.connection_list.push_back(std::move(node));
	return ctx.np_protocol.connection_list.back();
}

// Drop the node keyed by `peer` from connection_list, if present (the goodbye-erase + the
// HandleClientJoin "destroy a stale node before recreating" path [orig: NapiNPConnection_Destroy
// @0x62a4b0]).
void erase_connection(NapiNPServerCtx &ctx, const PeerAddr &peer) {
	auto &list = ctx.np_protocol.connection_list;
	for (auto it = list.begin(); it != list.end(); ++it) {
		if (it->peer == peer) {
			list.erase(it);
			return;
		}
	}
}

// Build + frame a 0x82 ServerAuth for `conn` from its CURRENT keys (server_sk / server_scrk /
// connection_id). Used for a fresh join AND to re-send on a retransmitted 0x42 (the original
// re-sends the cached packet via NapiNPConnection_SendSessionInit @0x620ef0 rather than re-minting).
std::vector<uint8_t> make_server_auth_datagram(const NapiNPServerCtx &ctx, const ClientAuth &auth,
                                               const PeerAddr &peer, const NapiNPConnection &conn) {
	const std::string nwuid =
			ctx.server_key_mint.forced ? ctx.server_key_mint.nwuid : make_dev_nwuid();
	ServerAuth reply = build_server_auth(auth, peer.ip, peer.port, conn.server_sk, conn.server_scrk,
	                                     ctx.server_key_mint.novaworld_name,
	                                     ctx.server_key_mint.novaworld_web_url, nwuid);
	// [orig: 0x82 MI TLV = conn->connection_id @ NapiNPConnection_SendSessionInit 0x620ef0] — the
	// host-assigned dcb the joiner stores as its own ConnectionId and echoes in its 0x48 client-ack.
	reply.mi = conn.connection_id;
	return nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(reply));
}

HostJoinerPose pose_from_session(NapiNPServerCtx &ctx, const GameSessionState &gss) {
	// [orig: HostSessionAccept::pose_from_session]
	HostJoinerPose p;
	if (gss.client_pos_valid) {
		// The joiner already sent a C2S 0x0C — spawn it where it reported.
		p.pos_valid = true;
		p.entity_handle = gss.client_entity_handle;
		p.item_type_id = gss.client_item_type_id != 0 ? gss.client_item_type_id : 0x14B9u;
		p.pos_x = static_cast<int32_t>(gss.client_pos_x);
		p.pos_y = static_cast<int32_t>(gss.client_pos_y);
		p.pos_z = static_cast<int32_t>(gss.client_pos_z);
		p.heading = gss.client_heading;
		p.pitch = gss.client_pitch;
	} else {
		// No uplink yet — fall back to the host-advertised spawn (the same position the
		// game-start bundle's 0x0F told the joiner).
		const GameSessionConfig &cfg = ctx.game_runtime->config().session;
		p.pos_valid = false;
		p.item_type_id = 0x14B9u;
		p.pos_x = static_cast<int32_t>(cfg.spawn_x);
		p.pos_y = static_cast<int32_t>(cfg.spawn_y);
		p.pos_z = static_cast<int32_t>(cfg.spawn_z);
	}
	p.team = 1; // co-op default; the MP game-type/team matrix is a later pass.
	return p;
}

std::vector<uint8_t> frame_session_replies(NapiNPConnection &conn,
                                           const std::vector<ProtocolMessage> &replies) {
	// [orig: SESSION reply build apps/novaworld_server/nw_udp_listener.cpp:606-627]
	ProtocolPacketHeader rhdr;
	rhdr.session_id = conn.client_ck;            // retail's local_key == ClientAuth.ck
	rhdr.seq_num = conn.next_outbound_seq++;
	rhdr.ack_count = conn.last_inbound_seq;
	rhdr.connection_flags = 0;

	std::vector<uint8_t> body_out;
	if (!encode_protocol_packet_plaintext(rhdr, replies, conn.server_scrk, body_out)) {
		return {};
	}
	return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body_out));
}

// ---------------------------------------------------------------------------
// P3 — the World-driven spawn-gate. conn.burst is the SINGLE authority for a connection's spawn
// progress; the F3 / PeerSpawned latches read it. It is driven by the World burst machine when
// ctx.world is wired, or MIRRORED from the still-extant game_runtime GameSessionState on the P2
// unit-test path (ctx.world == nullptr) so the latches stay value/tick-identical (keep-green lever).
// ---------------------------------------------------------------------------

// The joiner/host pose surfaced with the F3 / PeerSpawned events. Prefer the live World entity the
// spawn pipeline bound (World path); fall back to the game_runtime session pose (P2 path).
HostJoinerPose pose_for_conn(NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	if (ctx.world != nullptr && conn.link.owned_entity.valid()) {
		if (const world::Entity *e = ctx.world->registry.get(conn.link.owned_entity)) {
			HostJoinerPose p;
			p.pos_valid = true;
			p.entity_handle = e->handle.packed;
			p.item_type_id = static_cast<uint16_t>(e->item_id);
			p.pos_x = world::to_fixed(e->position.x);
			p.pos_y = world::to_fixed(e->position.y);
			p.pos_z = world::to_fixed(e->position.z);
			// Match the wire heading convention the P2 pose / the 0x0C body use: the high 16 bits of
			// the engine-frame BAM = (90 - mission_yaw) deg (D-NET-86), NOT raw mission degrees.
			constexpr int64_t kBamPerDegree = 11930464; // 2^32 / 360
			p.heading = static_cast<int16_t>(
					(static_cast<int64_t>(90 - e->yaw) * kBamPerDegree) >> 16);
			// Look-pitch: DEFERRED on the World path [D-NET-117]. The session pitch this mirrors
			// (pose_from_session's gss.client_pitch) is the HIGH 16 bits of the BAM32 look-pitch
			// (entity_wire_bridge stores ae.pitch >> 16). world::Entity::pitch is NOT that value — it is
			// unset for a net-snapped remote peer (apply_player_intent writes AiEntity::pitch, not the
			// world entity) and the LOW 16 bits for the local player (infantry.cpp narrows look_pitch) —
			// so sourcing it here would report wrong-units pitch. Leave it 0 (the HostJoinerPose default)
			// until the AiEntity look-pitch is threaded in; pitch is ~0 at spawn and never reaches the
			// wire (the 0x0C/0x0A pitch comes from the AiEntity, not this in-process pose event).
			p.team = e->team;
			return p;
		}
	}
	const GameSessionState *gss =
			ctx.game_runtime ? ctx.game_runtime->session_state(conn.session_id) : nullptr;
	if (gss != nullptr) return pose_from_session(ctx, *gss);
	return {};
}

// Ship one burst step's messages for `conn`: a remote (type 1) gets one framed 0x83 SESSION datagram
// (SCRK + seq); the host's own loopback (type 2, no SCRK) gets each [tag][body] pushed straight into
// its in-process transport — the §5.2a step-4 socketless S2C delivery (what NetSystem::emit_s2c does
// for the loopback's per-frame 0x0A). [orig: NapiNPServer_SendToConn @0x4c4f20 / mode-1 in-process]
void ship_burst_messages(NapiNPConnection &conn, std::vector<InitialStateMessage> &msgs,
                         std::vector<std::vector<uint8_t>> &outbound) {
	if (msgs.empty()) return;
	if (conn.type == 2) {
		if (conn.link.transport != nullptr)
			for (InitialStateMessage &m : msgs) conn.link.transport->host_send(m.tag, std::move(m.body));
		return;
	}
	// Frame EACH drained burst message as its OWN 0x83 SESSION datagram — do NOT coalesce the whole
	// burst into one packet. The original emits a separate NapiNPServer_SendFiltered per tag; coalescing
	// the 616 B 0x0B BMS header plus the (un-paged) 0x0C pool-0 batch into one datagram would exceed the
	// UDP MTU and IP-fragment. (The 0x0C batch is still un-paged here; a large mission needs MTU paging
	// of build_pool0_organic_batch like the Godot listen host's kRecPerMsg — tracked follow-up.)
	for (InitialStateMessage &m : msgs) {
		std::vector<ProtocolMessage> reply{make_protocol_message(m.tag, std::move(m.body))};
		std::vector<uint8_t> dg = frame_session_replies(conn, reply);
		if (!dg.empty()) outbound.push_back(std::move(dg));
	}
}

// The F3 (PeerEnteredWorldStreaming) + PeerSpawned edge-latched events, sourced from conn.burst. The
// predicates are VERBATIM from the P2 legs — only the value source moved from the GameSessionState to
// conn.burst. F3 is checked first so the owner admits early + streams the joiner's own dcb-bearing
// 0x0C during load, ahead of the game-start bundle (else Player_InitPlayer can't find its dcb).
//
// NOTE (D-NET-114, host self-stream): the host's OWN type-2 loopback runs the §5.2a burst too — this
// is FAITHFUL (the original's Server_OnPlayerJoin @0x51a680 runs for the host's own join, not just
// remote joiners), so we do not suppress it here. But its PeerSpawned carries self_id == the host dcb
// (kHostPlayerDcb): the event CONSUMER (the binding / NetSystem) must recognize self_id == its own
// connection id and NOT admit a duplicate "ghost" host avatar — the host's local player is already
// World::cached.local_player. (Self-filter belongs at the consumer, where the local connection id is
// known; surfacing it here keeps the libs layer a faithful, consumer-agnostic event source.)
void surface_burst_events(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                          std::vector<HostAcceptEvent> &events) {
	// F3 fires once the world-stream produced batches and the connection's id is known. Do NOT gate on
	// !conn.burst.spawned: the World-path one-shot burst (D-NET-114) drains the whole §5.2a track in a
	// single call, so entity_batch_count and spawned latch true together. The !world_stream_announced
	// flag alone guarantees F3 fires exactly once, and because this block precedes the PeerSpawned block
	// below, F3 is still surfaced AHEAD of PeerSpawned (the dcb-timing contract) even when both land on
	// the same tick. (On the P2 path entity_batch_count climbs before spawned, so F3 already fired and
	// world_stream_announced is latched — removing the !spawned guard is a no-op there.)
	if (conn.burst.entity_batch_count > 0 && conn.self_id_seen && !conn.world_stream_announced) {
		conn.world_stream_announced = true;
		HostAcceptEvent ev;
		ev.kind = HostAcceptEvent::Kind::PeerEnteredWorldStreaming;
		ev.peer = conn.peer;
		ev.pose = pose_for_conn(ctx, conn);
		ev.self_id = conn.connection_id;
		ev.peer_name = conn.player_name;
		events.push_back(std::move(ev));
	}
	if (conn.burst.spawned && !conn.spawned_announced) {
		conn.spawned_announced = true;
		conn.phase = ConnectionPhase::Spawned;
		HostAcceptEvent ev;
		ev.kind = HostAcceptEvent::Kind::PeerSpawned;
		ev.peer = conn.peer;
		ev.pose = pose_for_conn(ctx, conn);
		ev.self_id = conn.connection_id;
		ev.peer_name = conn.player_name; // for the joiner-side name-match (D.0)
		events.push_back(std::move(ev));
	}
}

// Mirror the game_runtime spawn-gate state onto conn.burst (P2 path only — no World wired). The World
// path drives conn.burst directly via Server_SendInitialGameStateToPlayer.
void mirror_game_runtime_burst(NapiNPServerCtx &ctx, NapiNPConnection &conn) {
	if (ctx.world != nullptr || !ctx.game_runtime) return;
	if (const GameSessionState *gss = ctx.game_runtime->session_state(conn.session_id)) {
		conn.burst.entity_batch_count = static_cast<uint32_t>(gss->entity_batch_count);
		conn.burst.spawned = gss->spawned;
	}
}

// 0x41 ClientHello -> 0x81 ServerHello. [orig: NapiNPProtocol_HandleClientHello @0x6213b0]
void handle_client_hello(NapiNPServerCtx &ctx, const PeerAddr &peer,
                         const std::vector<uint8_t> &body, HandleResult &out) {
	ClientHello hello;
	if (!parse_client_hello(body.data(), body.size(), hello)) return;
	if (classify_session_protocol(hello.pn) != SessionProtocolKind::JointOperations) {
		return; // not an in-match game join — the owner routes lobby PNs elsewhere (no node created)
	}
	// The host must be up before it admits a join — Hello rejects while host_running == 0 (and only
	// an authority accepts joins). This is P1's bring-up gate: create_session -> start_server sets
	// is_in_session/is_authority/host_running. [orig: CNapiNetwork_ValidateJoinRequest @0x4c61b0;
	// host_running gate @+0x538]
	if (!ctx.is_authority || ctx.np_protocol.host_running == 0) {
		return; // host not started — no ServerHello, no node created
	}
	NapiNPConnection &conn = find_or_create_connection(ctx, peer);
	conn.pn = hello.pn;
	conn.player_name = hello.co; // the joiner's player name (CO is free/unvalidated); streamed back
	                             // in the organic-spawn 0x0C name-match
	conn.session_id = peer_session_id(peer);
	conn.phase = ConnectionPhase::HelloReceived;
	// client_ip_net: the builders take the IP as the four payload octets in LE packing (so retail's
	// positional TLV reader prints a.b.c.d) — pass peer.ip verbatim, matching nw_udp_listener.
	ServerHello reply = build_server_hello(hello, peer.ip, peer.port);
	// R1: advertise our real host key (seed-injected via SessionStartup) rather than
	// build_server_hello's placeholder default, when one is set. The retail 0x81 carries host_key.
	if (ctx.np_protocol.host_key != 0) reply.hk = ctx.np_protocol.host_key;
	out.outbound.push_back(
			nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(reply)));
	HostAcceptEvent ev;
	ev.kind = HostAcceptEvent::Kind::PeerHandshakeAdvanced;
	ev.peer = peer;
	out.events.push_back(std::move(ev));
}

// 0x42 ClientAuth -> 0x82 ServerAuth (per-session SCRK established).
// [orig: NapiNPProtocol_HandleClientJoin @0x62b750]
void handle_client_join(NapiNPServerCtx &ctx, const PeerAddr &peer,
                        const std::vector<uint8_t> &body, HandleResult &out) {
	ClientAuth auth;
	if (!parse_client_auth(body.data(), body.size(), auth)) return;
	// Validate the join before admitting it (no ServerAuth, no node, on failure). The original
	// re-runs the SAME identity gate as Hello on the 0x42 and additionally checks the HK echo
	// against the host key, dropping the join (return 0) otherwise. [orig:
	// NapiNPProtocol_HandleClientJoin @0x62b750 — NVS/PN/PG/PV1 + HK == host_key]
	if (!ctx.is_authority || ctx.np_protocol.host_running == 0) return; // host not started
	if (classify_session_protocol(auth.pn) != SessionProtocolKind::JointOperations) return; // not JO
	// HK echo: the joiner must echo the host key it learned in ServerHello.hk. Checked only when the
	// host has a key set (a deterministic 0 seed means "unchecked", matching P1's pass-in startup).
	if (ctx.np_protocol.host_key != 0 && auth.hk != ctx.np_protocol.host_key) return; // wrong host key

	// [orig: NapiNPProtocol_HandleClientJoin @0x62b750] FindConnection, then branch on what it found.
	// (Unlike retail — where the 0x41 leaves no persisted connection so the 0x42 always Creates —
	// our handle_client_hello persists a HelloReceived node carrying the joiner's name; the normal
	// first 0x42 therefore ADVANCES that node in place below, and only a genuine retransmit or a
	// different client reusing the addr take the re-send / destroy paths.)
	if (NapiNPConnection *existing = find_connection(ctx, peer);
	    existing != nullptr && existing->phase >= ConnectionPhase::Joined) {
		// Retransmitted 0x42 from the SAME client (matching CI + CK) on an already-joined connection:
		// re-send the cached ServerAuth, do NOT re-mint. Re-minting would rotate the server SCRK/SK
		// the joiner already latched from the first 0x82, so every later S2C 0x83 would fail to
		// decrypt and the join would silently stall. [orig: conn_state == 1 &&
		// session_keys.client_id == CI && session_keys.remote_key == CK ->
		// NapiNPConnection_SendSessionInit @0x620ef0 (re-emits the same 0x82), return 1]
		if (existing->client_ci == auth.ci && existing->client_ck == auth.ck) {
			out.outbound.push_back(make_server_auth_datagram(ctx, auth, peer, *existing));
			return;
		}
		// A different client (CI/CK) reusing an already-joined addr: drop the stale node and recreate
		// fresh below. [orig: NapiNPConnection_Destroy then NapiNPConnection_Create]
		erase_connection(ctx, peer);
	}

	// [orig: CNapiNetwork_ValidateJoinRequest @0x4c61b0, registered as the join-validate callback by
	// CNapiGameSession_CreateSession @0x4c97c0 and invoked at the 0x42 join]. Reject when the session
	// is already full: the witnessed gate rejects on current_player_count >= max_players (CNapiNetwork
	// +0xF28; spectator slots add in when enabled). The player count is the host's own type-2 loopback
	// (when present) plus already-admitted (>= Joined) joiners — matching networkCtx[11], which counts
	// added players and the host but not this still-joining peer's pre-join Hello node. Retail replies
	// with a draw-overlay reject (state 14, reason 4 "server full"); we model the reject as a silent
	// drop + no node (consistent with the other 0x42 reject legs) — the overlay-reject packet is not
	// modeled yet (tracked: D-NET overlay-reject).
	std::size_t occupied = 0;
	for (const NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) continue; // this joiner does not count against itself
		if (c.type == 2 || c.phase >= ConnectionPhase::Joined) ++occupied;
	}
	if (occupied >= ctx.np_protocol.max_players) {
		erase_connection(ctx, peer); // drop this peer's pre-join Hello node — the join is rejected
		return;                      // server full
	}

	NapiNPConnection &conn = find_or_create_connection(ctx, peer);
	if (conn.session_id.empty()) conn.session_id = peer_session_id(peer);
	// The 0x42 also carries the joiner's name (CO) — keep the Hello node's name when present, else
	// adopt the auth's (covers a fresh recreate where no Hello node survived).
	if (conn.player_name.empty() && !auth.co.empty()) conn.player_name = auth.co;
	// auth.scrk decrypts inbound SESSION; our server_scrk encrypts outbound SESSION and is echoed
	// in ServerAuth so the client can read our replies.
	conn.client_scrk = auth.scrk;
	conn.client_ck = auth.ck;
	conn.client_ci = auth.ci;
	// [orig: NapiNPConnection_Create @0x62acb0 — conn.connection_id (the dcb) = ++protocol[947],
	// wrapping 0 -> 1]. On a LAN listen host the host ASSIGNS the dcb (it does not learn it from the
	// client), so latch self_id_seen now: the F3 streaming-entered gate no longer waits for the
	// joiner's 0x48 client-ack (the bundled np::JoinerConnection still sends one, and on NovaWorld
	// the gate-assigned id arrives via that 0x48 and overrides this in handle_client_session).
	// TODO(P6): gate this on the LAN network type when NovaWorld transport lands — on NovaWorld the
	// dcb is gate-assigned, not host-assigned.
	conn.connection_id = ctx.np_protocol.next_connection_id++;
	if (ctx.np_protocol.next_connection_id == 0) ctx.np_protocol.next_connection_id = 1; // wrap 0 -> 1
	conn.self_id_seen = true;
	// R2: mint the server SCRK / SK randomly (retail) unless a deterministic source is forced
	// (golden byte-parity). [orig: make_dev_scrk / make_random_session_u32]
	if (ctx.server_key_mint.forced) {
		conn.server_scrk = ctx.server_key_mint.server_scrk;
		conn.server_sk = ctx.server_key_mint.server_sk;
	} else {
		conn.server_scrk = make_dev_scrk();
		conn.server_sk = make_random_session_u32();
	}
	conn.phase = ConnectionPhase::Joined;
	out.outbound.push_back(make_server_auth_datagram(ctx, auth, peer, conn));
	HostAcceptEvent ev;
	ev.kind = HostAcceptEvent::Kind::PeerHandshakeAdvanced;
	ev.peer = peer;
	out.events.push_back(std::move(ev));
}

// 0x43 SESSION -> 0x83 SESSION (drives the GameSession to Spawned; surfaces F3 + spawn + in-match
// C2S). [orig: NapiNPProtocol_HandleSessionPacket @0x626A00]
void handle_client_session(NapiNPServerCtx &ctx, const PeerAddr &peer,
                           const std::vector<uint8_t> &body, uint32_t now_tick, HandleResult &out) {
	NapiNPConnection *connp = find_connection(ctx, peer);
	if (connp == nullptr || connp->client_scrk.empty()) {
		return; // SESSION before AUTH completed — drop
	}
	NapiNPConnection &conn = *connp;

	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> messages;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), conn.client_scrk,
	                                       hdr, messages)) {
		return;
	}
	conn.last_inbound_seq = hdr.seq_num;

	// Learn the joiner's own ConnectionId (NapiNPConnection.unk_18 = its dcb, our connection_id)
	// from its in-match 0x48 client-ack (a 4-byte LE u32). This is the value the client's
	// Player_FindLocalPlayerEntity @0x4e0090 compares entity+0x78 against, so the host MUST stamp it
	// into the joiner's 0x0C entity_flags — a guessed sequential id does NOT match (witnessed:
	// capture2.pcapng ack=0x113F vs our guessed 3 -> crash; the working retail join had
	// ack==eFlags==3). The ack arrives during the early handshake, before world streaming, so it's
	// known by the time we stream the 0x0C.
	for (const ProtocolMessage &m : messages) {
		if (m.tag == 0x48 && m.payload.size() >= 4) {
			conn.connection_id = static_cast<uint32_t>(m.payload[0]) |
					(static_cast<uint32_t>(m.payload[1]) << 8) |
					(static_cast<uint32_t>(m.payload[2]) << 16) |
					(static_cast<uint32_t>(m.payload[3]) << 24);
			conn.self_id_seen = true;
			// On LAN the ack echoes the host-assigned dcb (no-op); on NovaWorld it carries the
			// gate-assigned id. If the player was already spawned (World path) with the prior id,
			// re-stamp entity+0x78 so the joiner still self-matches. [orig: the dcb the client's
			// Player_FindLocalPlayerEntity @0x4e0090 matches is whatever it adopted as its own]
			if (ctx.world != nullptr && conn.link.owned_entity.valid()) {
				if (world::Entity *e = ctx.world->registry.get(conn.link.owned_entity))
					e->owner_connection_id = conn.connection_id;
			}
		}
	}

	// Drive the reactive handshake/spawn state machine (also caches the joiner's 0x0C pose into
	// client_*); BYPASS GameSession::tick here — the periodic emitter runs via tick_connections
	// (pre-Spawned) / NetSystem. game_runtime is retained for the §5.1 handshake-reply bodies; guard
	// it so a World-only deployment (no game_runtime wired) never null-derefs.
	if (ctx.game_runtime) {
		SessionProtocolDispatchResult disp = dispatch_in_match_session_messages(
				SessionProtocolKind::JointOperations, *ctx.game_runtime, conn.session_id,
				messages, now_tick);
		if (!disp.replies.empty()) {
			std::vector<uint8_t> dg = frame_session_replies(conn, disp.replies);
			if (!dg.empty()) out.outbound.push_back(std::move(dg));
		}
	}

	// P3: conn.burst is the single spawn-gate authority. On the P2 path (no World) mirror it from the
	// GameSessionState the dispatch above advanced, so the F3 / PeerSpawned latches are value-identical
	// to the pre-P3 gss reads; on the World path tick_connections drives conn.burst. Then surface the
	// (verbatim-predicate) edge-latched events. Dual-path: whichever of the datagram / tick path
	// observes the burst change first wins (one-shot via the *_announced latches).
	mirror_game_runtime_burst(ctx, conn);
	surface_burst_events(ctx, conn, out.events);

	// Once a connection exists, surface the joiner's in-match C2S 0x0C uplinks for NetSystem to
	// read-apply. Pre-spawn 0x0C only updates the cached pose (handled above) — no connection to
	// route it to yet.
	if (conn.spawned_announced) {
		std::vector<ProtocolMessage> c2s;
		for (const ProtocolMessage &m : messages) {
			if (m.tag == 0x0C) c2s.push_back(m);
		}
		if (!c2s.empty()) {
			HostAcceptEvent ev;
			ev.kind = HostAcceptEvent::Kind::PeerC2SInMatch;
			ev.peer = peer;
			ev.in_match_c2s = std::move(c2s);
			out.events.push_back(std::move(ev));
		}
	}
}

// 0x46 ClientGoodbye. [orig: Nwu_HandleClientGoodbye @0x624250]
void handle_client_goodbye(NapiNPServerCtx &ctx, const PeerAddr &peer, HandleResult &out) {
	if (ctx.game_runtime) ctx.game_runtime->reset_session(peer_session_id(peer));
	erase_connection(ctx, peer);
	HostAcceptEvent ev;
	ev.kind = HostAcceptEvent::Kind::PeerGoodbye;
	ev.peer = peer;
	out.events.push_back(std::move(ev));
}

} // namespace

void configure_session_runtime(NapiNPServerCtx &ctx, GameServerRuntimeConfig config) {
	if (!ctx.game_runtime) {
		ctx.game_runtime = std::make_unique<GameServerRuntime>(std::move(config));
	} else {
		ctx.game_runtime->configure(std::move(config));
	}
	ctx.game_runtime->start();
	// [orig: HostSessionAccept::configure clears peers_] — peers_ held ONLY remote joiners (the
	// host's own client lived elsewhere), so the faithful translation drops the server-side (type-1)
	// remote-joiner nodes and PRESERVES the host's own type-2 loopback client that P1's
	// create_session registered (else the canonical bring-up create_session(local_client) ->
	// configure_session_runtime would silently delete it).
	auto &list = ctx.np_protocol.connection_list;
	for (auto it = list.begin(); it != list.end();) {
		if (it->type == 1) it = list.erase(it);
		else ++it;
	}
}

HandleResult handle_server_datagram(NapiNPServerCtx &ctx, const PeerAddr &peer,
                                    const uint8_t *raw, std::size_t len, uint32_t now_tick) {
	HandleResult out;

	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(raw, len, opcode, body)) {
		return out; // bad envelope — drop quietly
	}

	switch (opcode) {
	case SESSION_OPCODE_CLIENT_HELLO:
		handle_client_hello(ctx, peer, body, out);
		break;
	case SESSION_OPCODE_CLIENT_AUTH:
		handle_client_join(ctx, peer, body, out);
		break;
	case SESSION_OPCODE_PROTOCOL_MESSAGE:
		handle_client_session(ctx, peer, body, now_tick, out);
		break;
	case SESSION_OPCODE_CLIENT_GOODBYE:
		handle_client_goodbye(ctx, peer, out);
		break;
	default:
		break;
	}
	return out;
}

std::vector<TickOut> tick_connections(NapiNPServerCtx &ctx, int elapsed_ms, uint32_t now_tick) {
	std::vector<TickOut> out;

	// P3 World-driven path: spawn any accepted-but-unspawned players once per tick (idempotent), before
	// walking the connections to advance their bursts.
	if (ctx.world != nullptr) Server_ProcessPendingPlayerSpawns(ctx, *ctx.world);

	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.burst.spawned) continue; // spawned peers: NetSystem owns their per-frame 0x0A

		TickOut to;
		to.peer = conn.peer;

		if (ctx.world != nullptr) {
			// Advance this connection's §5.2a burst one step and frame/ship the bodies (built from real
			// World/bms state). conn.burst is authoritative for the spawn-gate latches.
			if (conn.phase >= ConnectionPhase::PlayerAdded) {
				InitialStateStep step = Server_SendInitialGameStateToPlayer(ctx, conn, now_tick);
				ship_burst_messages(conn, step.messages, to.outbound);
			}
		} else if (ctx.game_runtime != nullptr) {
			// P2 path: the game_runtime periodic emitter climbs the GameSessionState; mirror it onto
			// conn.burst so the latches read the same values they did pre-P3.
			const GameSessionState *gss = ctx.game_runtime->session_state(conn.session_id);
			if (gss == nullptr) continue; // session not created yet (no 0x43 dispatched)
			if (gss->spawned) continue;   // NetSystem owns spawned peers' per-frame 0x0A
			GameServerDispatch disp = ctx.game_runtime->tick_session(conn.session_id, elapsed_ms, now_tick);
			if (!disp.replies.empty()) {
				std::vector<uint8_t> dg = frame_session_replies(conn, disp.replies);
				if (!dg.empty()) to.outbound.push_back(std::move(dg));
			}
			mirror_game_runtime_burst(ctx, conn); // re-read post-tick state onto conn.burst
		} else {
			continue;
		}

		// Surface the F3 / PeerSpawned events from conn.burst (verbatim predicates). Same latch as the
		// datagram-driven handle_client_session path — whichever observes the burst change first wins.
		surface_burst_events(ctx, conn, to.events);

		if (to.outbound.empty() && to.events.empty()) continue;
		out.push_back(std::move(to));
	}
	return out;
}

bool frame_in_match_s2c(NapiNPServerCtx &ctx, const PeerAddr &peer, uint8_t inner_tag,
                        const std::vector<uint8_t> &inner_body, std::vector<uint8_t> &out_datagram) {
	NapiNPConnection *conn = find_connection(ctx, peer);
	if (conn == nullptr || conn->server_scrk.empty()) {
		return false;
	}
	ProtocolMessage msg = make_protocol_message(inner_tag, inner_body);
	out_datagram = frame_session_replies(*conn, std::vector<ProtocolMessage>{std::move(msg)});
	return !out_datagram.empty();
}

std::size_t apply_in_match_c2s(NapiNPServerCtx &ctx, const HostAcceptEvent &event) {
	if (event.kind != HostAcceptEvent::Kind::PeerC2SInMatch) return 0;
	NapiNPConnection *conn = find_connection(ctx, event.peer);
	if (conn == nullptr || conn->link.transport == nullptr) return 0;
	std::size_t staged = 0;
	for (const ProtocolMessage &m : event.in_match_c2s) {
		if (m.tag != 0x0C) continue; // only the player-state uplink this increment (§5.10)
		conn->link.transport->deliver_c2s(0x0C, m.payload);
		++staged;
	}
	return staged;
}

bool connection_spawned(const NapiNPServerCtx &ctx, const PeerAddr &peer) {
	for (const NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) return c.spawned_announced;
	}
	return false;
}

bool bind_connection_player(NapiNPServerCtx &ctx, const PeerAddr &peer, uint8_t player_slot,
                            uint16_t entity_handle) {
	NapiNPConnection *conn = find_connection(ctx, peer);
	if (conn == nullptr || conn->session_id.empty() || !ctx.game_runtime) {
		return false;
	}
	const std::string player_name = !conn->player_name.empty()
			? conn->player_name
			: ctx.game_runtime->config().session.player_name;
	return ctx.game_runtime->bind_session_player(
			conn->session_id, player_name, player_slot, entity_handle);
}

bool drop_connection(NapiNPServerCtx &ctx, const PeerAddr &peer) {
	if (find_connection(ctx, peer) == nullptr) return false;
	// Same teardown as a 0x46 ClientGoodbye, minus the surfaced event: reset the game_runtime session
	// (if any) and erase the node. The owner releases its own non-owning transport for `peer`.
	if (ctx.game_runtime) ctx.game_runtime->reset_session(peer_session_id(peer));
	erase_connection(ctx, peer);
	return true;
}

std::size_t connection_count(const NapiNPServerCtx &ctx) {
	return ctx.np_protocol.connection_list.size();
}

} // namespace opennova::np
