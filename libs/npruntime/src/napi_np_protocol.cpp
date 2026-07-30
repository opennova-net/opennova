#include "npruntime/napi_np_protocol.h"

#include "npruntime/server_initial_state.h"    // Server_SendInitialGameStateToPlayer (the §5.2a burst)
#include "npruntime/server_message_dispatch.h" // dispatch_session_replies (the reactive §5.1 replies)
#include "npruntime/server_spawn.h"            // Server_ProcessPendingPlayerSpawns (World-driven spawn)

#include <npwire/ingame_encode.h>    // encode_player_sync_removal (the disconnect 0x46 removal)
#include <npwire/ingame_message_id.h>
#include <npwire/nw_session_framing.h>
#include <npwire/protocol_message.h> // make_protocol_message (frame the burst messages)
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <netsim/session_transport.h> // ISessionTransport::host_send (loopback burst delivery)

#include <world/ai.h>
#include <world/entity.h>
#include <world/geom.h> // to_fixed
#include <world/vehicle_attach.h>
#include <world/world.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <utility>

namespace opennova::np {

namespace {

// Retail's initialized JOINTOPERATIONS connection-template active-send interval:
// cs_dir0/cs_dir1.active_send_interval_ms = 10000 (idle 30000). If reliable records remain
// unacknowledged and no other packet was built for longer than this interval,
// BuildOutgoingPackets mints a fresh header-only sequence. The induced gap asks the peer's
// ordinary 0x44/0x84 machinery to reconstruct a lost semantic packet. (1000 ms is the
// NOVAWORLDUDP service template's value @0x4d3e60, not the game session's.)
// [orig: CNapiNetwork_Init @0x4ca4a0 stores @0x4caac5/@0x4cab98 -> read by
// CNapiNPConnection_PumpSendIntervals @0x628FD0 -> BuildOutgoingPackets @0x628430]
constexpr uint32_t kActiveSendIntervalMilliseconds = 10000;

// ASCII case-insensitive tag-name compare [orig: Napi_StrCaseEqual @0x616e70 — the
// NapiNetConfig_LoadFromConnTags match].
bool str_case_equal(const std::string &a, const char *b) {
	size_t i = 0;
	for (; i < a.size() && b[i] != '\0'; ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) !=
		    std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	}
	return i == a.size() && b[i] == '\0';
}

struct ParsedClientGameEnvironment {
	long bt = 0;
	long vn = 0;
	long bn = 0;
	long mbn = 0;
	long sopd = 0;
};

ParsedClientGameEnvironment parse_client_game_environment(const ClientAuth &auth) {
	ParsedClientGameEnvironment parsed;
	for (const auto &blob : auth.cu) {
		uint8_t cu_type = 0;
		std::string cu_name;
		std::string cu_value;
		if (!parse_client_cu_chunk(
					blob.data(), blob.size(), cu_type, cu_name, cu_value) ||
		    cu_type != 2) {
			continue;
		}

		// NapiNetConfig_LoadFromConnTags applies the tag list in order with atol semantics, so a
		// later duplicate overwrites an earlier value.
		const long value = std::strtol(cu_value.c_str(), nullptr, 10);
		if (str_case_equal(cu_name, "BT")) {
			parsed.bt = value;
		} else if (str_case_equal(cu_name, "VN")) {
			parsed.vn = value;
		} else if (str_case_equal(cu_name, "BN")) {
			parsed.bn = value;
		} else if (str_case_equal(cu_name, "MBN")) {
			parsed.mbn = value;
		} else if (str_case_equal(cu_name, "SOPD")) {
			parsed.sopd = value;
		}
	}
	return parsed;
}

bool validates_jointoperations_game_environment(const ParsedClientGameEnvironment &parsed) {
	// These are literals embedded in the 1.7.5.7 server, not host-selected configuration. Missing
	// type-2 tags retain NapiNetConfig's zero initialization and therefore fail the nonzero checks.
	// BT is an account state: only 1 and 2 are the witnessed ban rejects. VERSIONSTRING, DB,
	// COUNTRYCODE and TZB are stored/display-only and deliberately do not participate in this gate.
	// [orig: NapiNetConfig_LoadFromConnTags @0x4c7260 ->
	// Server_ValidatePlayerJoinRequest @0x512100, DC=2/3/4/6/7/8]
	return parsed.bn == 1 &&
	       parsed.vn == 2 &&
	       parsed.mbn == 20042002 &&
	       parsed.sopd == 180 &&
	       parsed.bt != 1 &&
	       parsed.bt != 2;
}

// Stable "a.b.c.d:port" label — the connection's session_id (NapiNPConnection.session_id). PeerAddr.ip
// is LE octet packing (a | b<<8 | c<<16 | d<<24); print low->high so the label reads a.b.c.d (matches
// nw_udp_listener's client_label). [orig: HostSessionAccept::peer_session_id]
std::string peer_session_id(const PeerAddr &peer) {
	return peer_addr_to_string(peer);
}

NapiNPConnection *find_connection(NapiNPServerCtx &ctx, const PeerAddr &peer) {
	for (auto &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) return &c;
	}
	return nullptr;
}

// Linear scan + create-on-miss over connection_list — faithful to the original intrusive-list walk
// (NapiNPServer_SendFiltered @0x4C87E0 -> SendToConn per node). connection_list stays authoritative
// (the single-owner table; there is no parallel connection table). A fresh node is a server-side connection (type 1).
NapiNPConnection &find_or_create_connection(NapiNPServerCtx &ctx, const PeerAddr &peer) {
	if (NapiNPConnection *existing = find_connection(ctx, peer)) return *existing;
	NapiNPConnection node;
	node.peer = peer;
	node.type = 1; // server-side: the host's view of a client
	ctx.np_protocol.connection_list.push_back(std::move(node));
	return ctx.np_protocol.connection_list.back();
}

// The one player/session teardown path shared by keyed goodbye, receive timeout, owner eviction, and
// same-address replacement. The original runs Server_HandlePlayerDisconnect before destroying the
// NapiNPConnection node; a bare list erase leaks the entity and roster identity.
// [orig: Server_HandlePlayerDisconnect @0x51B5C0 -> NapiNPConnection_Destroy @0x62A4B0]
bool teardown_connection(NapiNPServerCtx &ctx, const PeerAddr &peer) {
	auto &list = ctx.np_protocol.connection_list;
	auto it = list.end();
	for (auto candidate = list.begin(); candidate != list.end(); ++candidate) {
		if (candidate->peer == peer) {
			it = candidate;
			break;
		}
	}
	if (it == list.end()) return false;

	const bool had_player = it->type == 1 &&
			(it->link.owned_entity.valid() || it->phase >= ConnectionPhase::PlayerAdded);
	const world::EntityHandle owned_entity = it->link.owned_entity;
	const uint8_t player_slot = it->reply.player_slot;
	if (had_player) {
		const bool freed_pool0_entity = ctx.world != nullptr && owned_entity.valid() &&
				owned_entity.pool() == 0;
		const uint16_t freed_pool0_slot =
				freed_pool0_entity ? static_cast<uint16_t>(owned_entity.slot()) : uint16_t{0};
		if (ctx.world != nullptr && owned_entity.valid()) {
			world::entity_detach_from_vehicle(*ctx.world, owned_entity);
			ctx.world->registry.despawn(owned_entity);
		}
		// The witnessed leave broadcast is S2C 0x46 bit15, which clears the peer's
		// ROSTER BOOKKEEPING ONLY — PlayerSlot_ClearAndUnlink @0x434730 never
		// destroys the entity (@0x431411..0x43144c; the entity-field wipes @0x431437
		// are dead code there because entitySlotPtr is null). Retail relies on the
		// client asking for a sweep (C2S 0x32 -> S2C 0x5D) to retire the entity, so
		// on retail a leaver's body lingers until the next sweep.
		// NOT WITNESSED (D-NET-176): pushing that same sweep at teardown instead of
		// waiting to be asked. The probe3 capture shows 0x5D coinciding with
		// disconnects, which is consistent with a sweep, but the only witnessed
		// TRIGGER is the 0x32 request — this is a trigger relocation, and the BYTES
		// are the witnessed builder's (@0x5160f0) exactly.
		DestroyEntityList leave_sweep;
		if (freed_pool0_entity) leave_sweep.pool0_indices.push_back(freed_pool0_slot);
		for (NapiNPConnection &other : list) {
			if (&other == &*it || !is_in_match(other) || other.link.transport == nullptr)
				continue;
			other.link.transport->host_send(
					0x46, encode_player_sync_removal(player_slot, /*with_ack=*/false));
			if (freed_pool0_entity)
				other.link.transport->host_send(
						0x5D, encode_destroy_entity_list(leave_sweep));
		}
		++ctx.np_protocol.roster_generation;
	}

	list.erase(it);
	return true;
}

// The single current-player count used by both 0x81 NP advertisement and the
// 0x42 capacity gate: the host's type-2 loopback plus admitted remote peers.
// Stateless 0x41 probes never enter the connection list.
uint32_t occupied_player_count(const NapiNPServerCtx &ctx) {
	uint32_t occupied = 0;
	for (const NapiNPConnection &connection : ctx.np_protocol.connection_list) {
		if (connection.type == 2 || connection.phase >= ConnectionPhase::Joined) ++occupied;
	}
	return occupied;
}

// Build + frame a 0x82 ServerAuth for `conn` from its CURRENT keys (server_sk / server_scrk /
// connection_id). Used for a fresh join AND to re-send on a retransmitted 0x42 (the original
// re-sends the cached packet via NapiNPConnection_SendSessionInit @0x620ef0 rather than re-minting).
std::vector<uint8_t> make_server_auth_datagram(const NapiNPServerCtx &ctx, const ClientAuth &auth,
                                               const PeerAddr &peer, const NapiNPConnection &conn) {
	const std::string nwuid =
			ctx.server_key_mint.forced ? ctx.server_key_mint.nwuid : make_dev_nwuid();
	// [D-NET Wave 3] Only a NovaWorld-routed host appends the NovaworldName/url/NWUID CU block (sourced
	// from the type-3 msg_out queue @0x620ef0); a LAN/SP host queues none and emits no CU.
	const bool include_cu = ctx.transport_mode == NetworkType::NovaWorld;
	ServerAuth reply = build_server_auth(auth, peer.ip, peer.port, conn.server_sk, conn.server_scrk,
	                                     ctx.server_key_mint.novaworld_name,
	                                     ctx.server_key_mint.novaworld_web_url, nwuid, include_cu);
	// [orig: 0x82 MI TLV = conn->connection_id @ NapiNPConnection_SendSessionInit 0x620ef0] — the
	// host-assigned dcb the joiner stores as its own ConnectionId and echoes in its 0x48 client-ack.
	reply.mi = conn.connection_id;
	return nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(reply));
}

HostJoinerPose pose_from_session(NapiNPServerCtx &ctx, const SessionReplyState &reply) {
	// [orig: HostSessionAccept::pose_from_session]
	HostJoinerPose p;
	const PreSpawnJoinerPose &pose = reply.pre_spawn_pose;
	if (pose.valid) {
		// The joiner already sent a C2S 0x0C — spawn it where it reported.
		p.pos_valid = true;
		p.entity_handle = pose.entity_handle;
		p.item_type_id = pose.item_type_id != 0 ? pose.item_type_id : 0x14B9u;
		p.pos_x = static_cast<int32_t>(pose.pos_x);
		p.pos_y = static_cast<int32_t>(pose.pos_y);
		p.pos_z = static_cast<int32_t>(pose.pos_z);
		p.heading = pose.heading;
		p.pitch = pose.pitch;
	} else {
		// No uplink yet — fall back to the host-advertised spawn from the session config.
		const GameConfig &cfg = ctx.config;
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
	// [orig: SESSION reply build apps/novaworld_server/nw_udp_listener.cpp:606-627] Host S2C direction:
	// encrypt with our server_scrk, stamp session_id = the ClientAuth.ck (retail's local_key). Shared
	// seq/ack framing via frame_session_packet (ADR 0013).
	std::vector<uint8_t> body_out;
	if (!frame_session_packet(conn.seq, SessionCrypto{conn.server_scrk, {}, conn.client_ck}, replies,
	                          body_out)) {
		return {};
	}
	conn.active_send_elapsed_ms = 0;
	return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body_out));
}

std::vector<uint8_t> frame_retained_session_reply(
		NapiNPConnection &conn, uint32_t sequence) {
	const auto retained = conn.seq.retained_outbound.find(sequence);
	if (retained == conn.seq.retained_outbound.end() || retained->second.empty()) return {};

	std::vector<uint8_t> body_out;
	if (!frame_session_packet_for_sequence(
			conn.seq, SessionCrypto{conn.server_scrk, {}, conn.client_ck},
			sequence, body_out)) {
		return {};
	}
	return nw_encode_outbound(
			SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body_out));
}

// Retail follows ServerAuth with one sequenced packet that installs the 1300-byte packet ceiling
// in both control-setting directions. The joiner ACKs this as C2S sequence 1 before sending JOIN.
// [wire: host_and_join_lan frames 6-8; flags 0xA0 = settings update + u8 length]
std::vector<ProtocolMessage> make_game_session_initial_settings() {
	return {
			make_protocol_message(
					0x00, {0x00, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00},
					0xA0),
			make_protocol_message(
					0x00, {0x01, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00},
					0xA0),
	};
}

// ---------------------------------------------------------------------------
// P3/P8 — the World-driven spawn-gate. conn.burst is the SINGLE authority for a connection's spawn
// progress; the F3 / PeerSpawned latches read it. It is driven by the World burst machine
// (Server_SendInitialGameStateToPlayer) when ctx.world is wired. Without a World (a session-responder
// host) the burst never advances and the connection stays pre-spawn — the reactive §5.1 replies still
// flow, but no PeerSpawned is surfaced (faithful: the original needs the server-side sim to spawn).
// ---------------------------------------------------------------------------

// The joiner/host pose surfaced with the F3 / PeerSpawned events. Prefer the live World entity the
// spawn pipeline bound (World path); fall back to the joiner's cached C2S 0x0C pose / advertised spawn.
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
			// The pose event mirrors the same signed BAM32 high word as the C2S 0x0C path. The
			// authoritative look pitch lives on AiEntity, not world::Entity; retain the zero default
			// when a non-AI entity is bound. [orig: pose_from_session gss.client_pitch;
			// entity_wire_bridge ae.pitch >> 16]
			if (ctx.world->ai != nullptr) {
				if (const world::AiEntity *ae =
						ctx.world->ai->for_handle(conn.link.owned_entity)) {
					p.pitch = static_cast<int16_t>(ae->pitch >> 16);
				}
			}
			p.team = e->team;
			return p;
		}
	}
	return pose_from_session(ctx, conn.reply);
}

// Ship one burst step's messages for `conn`: a remote (type 1) gets one framed 0x83 SESSION datagram
// (SCRK + seq); the host's own loopback (type 2, no SCRK) gets each [tag][body] pushed straight into
// its in-process transport — the §5.2a step-4 socketless S2C delivery (what emit_connection_s2c does
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
	// the 616 B 0x0B BMS header plus the 0x0C pool-0 batch into one datagram would exceed the UDP MTU and
	// IP-fragment. Each message is already MTU-sized upstream: the world-stream pools (0x10/0x0D/0x0C/0x20)
	// are byte-budget paged with their witnessed per-stream policies by the shared chunker
	// (np::slice_batch_pages via emit_paged_pool, ADR 0013), so this loop frames one page per datagram.
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
// (kHostPlayerDcb): the event CONSUMER (the binding / ClientRuntime) must recognize self_id == its own
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

// 0x41 ClientHello -> 0x81 ServerHello. [orig: NapiNPProtocol_HandleClientHello @0x6213b0]
void handle_client_hello(NapiNPServerCtx &ctx, const PeerAddr &peer,
                         const std::vector<uint8_t> &body, HandleResult &out) {
	ClientHello hello;
	if (!parse_client_hello(body.data(), body.size(), hello)) return;
	if (!matches_jointoperations_identity(hello)) {
		return; // not the retail JO game identity (no node created)
	}
	// The host must be up before it admits a join — Hello rejects while host_running == 0 (and only
	// an authority accepts joins). This is P1's bring-up gate: create_session -> start_server sets
	// is_in_session/is_authority/host_running. [orig: CNapiNetwork_ValidateJoinRequest @0x4c61b0;
	// host_running gate @+0x538]
	if (!ctx.is_authority || ctx.np_protocol.host_running == 0) {
		return; // host not started — no ServerHello, no node created
	}
	// client_ip_net: the builders take the IP as the four payload octets in LE packing (so retail's
	// positional TLV reader prints a.b.c.d) — pass peer.ip verbatim, matching nw_udp_listener.
	ServerHello reply = build_server_hello(hello, peer.ip, peer.port);
	// A LAN 0x41 is a stateless enumerate/handshake probe. Populate the retail
	// game-server fields from live host state without registering the source as
	// a peer; only a validated 0x42 creates the connection node.
	reply.sn = ctx.config.server_name;
	reply.p1 = ctx.config.game_type;
	// P2 and SUS1 are live host configuration/session values in retail. This runtime does not yet
	// model either producer, so let the faithful encoder omit them instead of replaying the
	// ServerHello struct's capture-oriented sample defaults as if they belonged to every host.
	reply.p2 = 0;
	reply.np = occupied_player_count(ctx);
	reply.mp = ctx.np_protocol.max_players;
	reply.sus1.clear();
	reply.sus2 = ctx.config.expansion;
	// R1: advertise our real host key (seed-injected via SessionStartup) rather than
	// build_server_hello's placeholder default, when one is set. The retail 0x81 carries host_key.
	if (ctx.np_protocol.host_key != 0) reply.hk = ctx.np_protocol.host_key;
	out.outbound.push_back(
			nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(reply)));
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
	// NapiNPProtocol_HandleClientJoin @0x62b750 — NVS/PN/PG/PV1/PV2 + non-empty NA + HK]
	if (!ctx.is_authority || ctx.np_protocol.host_running == 0) return; // host not started
	if (!matches_jointoperations_identity(auth) || auth.na.empty()) return; // not a retail JO game join
	// HK echo: the joiner must echo the host key it learned in ServerHello.hk. Checked only when the
	// host has a key set (a deterministic 0 seed means "unchecked", matching P1's pass-in startup).
	if (ctx.np_protocol.host_key != 0 && auth.hk != ctx.np_protocol.host_key) return; // wrong host key
	const ParsedClientGameEnvironment game_environment =
			parse_client_game_environment(auth);
	if (!validates_jointoperations_game_environment(game_environment)) {
		// Retail would send its draw-overlay disconnect class/reason. That packet is not modeled;
		// fail closed before replacement teardown, capacity accounting, or node allocation.
		return;
	}

	// [orig: NapiNPProtocol_HandleClientJoin @0x62b750] The stateless 0x41 leaves
	// no node, so a first 0x42 creates one. Only retransmit/address-reuse paths
	// find an existing admitted connection here.
	if (NapiNPConnection *existing = find_connection(ctx, peer);
	    existing != nullptr && existing->phase >= ConnectionPhase::Joined) {
		// Retransmitted 0x42 from the SAME client (matching CI + CK) on an already-joined connection:
		// re-send the cached ServerAuth, do NOT re-mint. Re-minting would rotate the server SCRK/SK
		// the joiner already latched from the first 0x82, so every later S2C 0x83 would fail to
		// decrypt and the join would silently stall. [orig: conn_state == 1 &&
		// session_keys.client_id == CI && session_keys.remote_key == CK ->
		// NapiNPConnection_SendSessionInit @0x620ef0 (re-emits the same 0x82), return 1]
		if (existing->client_ci == auth.ci && existing->client_ck == auth.ck) {
			existing->receive_inactive_ms = 0;
			out.outbound.push_back(make_server_auth_datagram(ctx, auth, peer, *existing));
			// A retry normally means the original ServerAuth/settings pair was lost. Reconstruct the
			// retained first session packet under sequence 1 rather than minting a new sequence.
			if (existing->peer_acked_seq < 1) {
				std::vector<uint8_t> settings =
						frame_retained_session_reply(*existing, 1);
				if (!settings.empty()) out.outbound.push_back(std::move(settings));
			}
			return;
		}
		// A different client (CI/CK) reusing an already-joined addr: drop the stale node and recreate
		// fresh below. [orig: NapiNPConnection_Destroy then NapiNPConnection_Create]
		if (teardown_connection(ctx, peer)) {
			// The socket owner must release the old UdpSessionTransport/announce latch before the
			// replacement reaches world streaming. The new node is created below, so this event is
			// owner cleanup only (it must not erase by address again).
			HostAcceptEvent ev;
			ev.kind = HostAcceptEvent::Kind::PeerGoodbye;
			ev.peer = peer;
			out.events.push_back(std::move(ev));
		}
	}

	// [orig: CNapiNetwork_ValidateJoinRequest @0x4c61b0, registered as the join-validate callback by
	// CNapiGameSession_CreateSession @0x4c97c0 and invoked at the 0x42 join]. Reject when the session
	// is already full: the witnessed gate rejects on current_player_count >= max_players (CNapiNetwork
	// +0xF28; spectator slots add in when enabled). The player count is the host's own type-2 loopback
	// (when present) plus already-admitted (>= Joined) joiners — matching networkCtx[11], which counts
	// added players and the host. Retail replies
	// with a draw-overlay reject (state 14, reason 4 "server full"); we model the reject as a silent
	// drop + no node (consistent with the other 0x42 reject legs) — the overlay-reject packet is not
	// modeled yet (tracked: D-NET overlay-reject).
	const uint32_t occupied = occupied_player_count(ctx);
	if (occupied >= ctx.np_protocol.max_players) {
		return; // server full; the stateless 0x41 left no node to clean up
	}

	NapiNPConnection &conn = find_or_create_connection(ctx, peer);
	if (conn.session_id.empty()) conn.session_id = peer_session_id(peer);
	// The joiner's display name: the GAME join's NA TLV is the player CALLSIGN — the retail
	// client puts its company string in CO ("NovaLogic Inc, Calabasas CA U.S.A.") and the
	// callsign in NA, and the golden host's 0x0C record name equals NA ("FooPlayer"). Only
	// the witnessed NOVAWORLD-connect gate tags ("jop:cus2" retail / "jopd:cus4" demo — a
	// "jop"-prefixed tag) fall back to CO / the Hello name: a callsign is free text and may
	// itself contain ':' — treating any colon as a gate tag stalled that joiner's 0x0C
	// name-match (the host would name it the company string, never equal to its NA).
	// [wire: retail-ashi5a f=199140 / retail_join_v18 f=47676; net-re §5.0b]
	const bool na_is_gate_tag = auth.na.size() >= 4 &&
			(std::tolower(static_cast<unsigned char>(auth.na[0])) == 'j') &&
			(std::tolower(static_cast<unsigned char>(auth.na[1])) == 'o') &&
			(std::tolower(static_cast<unsigned char>(auth.na[2])) == 'p') &&
			auth.na.find(':') != std::string::npos;
	if (!auth.na.empty() && !na_is_gate_tag) {
		conn.player_name = auth.na;
	} else if (conn.player_name.empty() && !auth.co.empty()) {
		conn.player_name = auth.co;
	}
	// auth.scrk decrypts inbound SESSION; our server_scrk encrypts outbound SESSION and is echoed
	// in ServerAuth so the client can read our replies.
	conn.client_scrk = auth.scrk;
	conn.client_ck = auth.ck;
	conn.client_ci = auth.ci;
	conn.receive_inactive_ms = 0;
	// The 0x42's CU chunks carry the joiner's character/profile vars — the per-side character
	// selection Server_PlayerAdd folds into the player record (CharacterJoinVars). Values are
	// decimal strings converted with atol semantics: CI0/CI1 keep the low u16, TR clamps to
	// {0, 1, 0xFF}, the rest keep the low u8. Only type-2 chunks are tag-list vars. [orig:
	// NapiNPProtocol_HandleClientJoin @0x62b750 CU loop (type gate @node+20 == 2) ->
	// NapiNetConfig_LoadFromConnTags @0x4c7260 (Napi_StrCaseEqual match, atol values); D-NET-146]
	conn.char_vars = CharacterJoinVars{}; // a recreated node starts tag-absent (zero-init)
	for (const auto &blob : auth.cu) {
		uint8_t cu_type = 0;
		std::string cu_name, cu_value;
		if (!parse_client_cu_chunk(blob.data(), blob.size(), cu_type, cu_name, cu_value)) continue;
		if (cu_type != 2) continue;
		const long v = std::strtol(cu_value.c_str(), nullptr, 10); // retail atol
		if (str_case_equal(cu_name, "CI0")) {
			conn.char_vars.char_id[0] = static_cast<uint16_t>(v);
		} else if (str_case_equal(cu_name, "CI1")) {
			conn.char_vars.char_id[1] = static_cast<uint16_t>(v);
		} else if (str_case_equal(cu_name, "TR")) {
			// [@0x4c752f] tr != -1 && (u8)tr >= 2 -> -1: only 0 (side A) and 1 (side B) pass.
			uint8_t tr = static_cast<uint8_t>(v);
			if (tr != 0xFF && tr >= 2) tr = 0xFF;
			conn.char_vars.team_request = tr;
		} else if (str_case_equal(cu_name, "CTA")) {
			conn.char_vars.char_class[0] = static_cast<uint8_t>(v);
		} else if (str_case_equal(cu_name, "CTB")) {
			conn.char_vars.char_class[1] = static_cast<uint8_t>(v);
		} else if (str_case_equal(cu_name, "VCA")) {
			conn.char_vars.avatar[0] = static_cast<uint8_t>(v);
		} else if (str_case_equal(cu_name, "VCB")) {
			conn.char_vars.avatar[1] = static_cast<uint8_t>(v);
		}
		// The environment tags were parsed and validated before allocation. Stored/display-only
		// fields (VERSIONSTRING/DB/COUNTRYCODE/TZB...) have no retained runtime consumer yet.
	}
	// [orig: NapiNPConnection_Create @0x62acb0 — conn.connection_id (the dcb) = ++protocol[947],
	// wrapping 0 -> 1]. On a LAN listen host the host ASSIGNS the dcb (it does not learn it from the
	// client) and advertises it in ServerAuth.MI. It becomes spawn-eligible only after the final
	// admission 0x02; the bundled client later echoes it in 0x48. On NovaWorld the gate-assigned id
	// arrives via that 0x48 and overrides this host provisional value.
	// TODO(P6): gate this on the LAN network type when NovaWorld transport lands — on NovaWorld the
	// dcb is gate-assigned, not host-assigned.
	conn.connection_id = ctx.np_protocol.next_connection_id++;
	if (ctx.np_protocol.next_connection_id == 0) ctx.np_protocol.next_connection_id = 1; // wrap 0 -> 1
	conn.self_id_seen = false;
	conn.admission_stage = GameAdmissionStage::AwaitJoinRequest;
	conn.admission_padding_x = 0;
	conn.admission_padding_y = 0;
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
	std::vector<uint8_t> initial_settings =
			frame_session_replies(conn, make_game_session_initial_settings());
	if (!initial_settings.empty()) out.outbound.push_back(std::move(initial_settings));

	HostAcceptEvent ev;
	ev.kind = HostAcceptEvent::Kind::PeerHandshakeAdvanced;
	ev.peer = peer;
	out.events.push_back(std::move(ev));
}

// 0x43 SESSION -> 0x83 SESSION (produces the reactive §5.1 replies; surfaces F3 + spawn + in-match
// C2S off conn.burst). [orig: NapiNPProtocol_HandleSessionPacket @0x626A00]
void handle_client_session(NapiNPServerCtx &ctx, const PeerAddr &peer,
                           const std::vector<uint8_t> &body, uint32_t now_tick, HandleResult &out,
                           bool defer_in_match_replies) {
	NapiNPConnection *connp = find_connection(ctx, peer);
	if (connp == nullptr || connp->client_scrk.empty()) {
		return; // SESSION before AUTH completed — drop
	}
	NapiNPConnection &conn = *connp;

	// Host recv: decrypt with the joiner's client_scrk; deframe latches conn.seq.last_inbound_seq.
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> messages;
	SessionDeframeAdmission admission;
	if (!deframe_session_packet(
			conn.seq, SessionCrypto{{}, conn.client_scrk, 0, conn.server_sk}, body.data(),
	                            body.size(), hdr, messages, &admission)) {
		return;
	}
	// A quiet deframe drop may still return true for a stale packet addressed to another
	// receiver-local SK. It is not activity on this connection. Once the local key matches,
	// deframe has also authenticated/decrypted the packet; valid duplicates/future packets may
	// refresh the receive clock even when they do not advance semantic admission.
	if (hdr.session_id == conn.server_sk) conn.receive_inactive_ms = 0;

	// The header's ack_count is the peer's "last of YOUR seqs I received" — the confirm side of the
	// initial-state backlog throttle (retail clients carry it on every 0x43, including game-message-
	// less keepalive datagrams; the golden world-stream gap has no C2S game messages yet the stream
	// advances). High-water only: a reordered older ack must not un-confirm. [orig: header layout
	// @0x61edd0 field +8; consumed by the conn+0x768 outstanding gate @0x51bf1b/0x51bc04]
	if (admission.admitted && admission.max_ack_count > conn.peer_acked_seq)
		conn.peer_acked_seq = admission.max_ack_count;
	if (admission.admitted)
		acknowledge_session_packets(conn.seq, admission.max_ack_count);

	// Learn the joiner's own ConnectionId (NapiNPConnection.unk_18 = its dcb, our connection_id)
	// from its in-match 0x48 client-ack (a 4-byte LE u32). This is the value the client's
	// Player_FindLocalPlayerEntity @0x4e0090 compares entity+0x78 against, so the host MUST stamp it
	// into the joiner's 0x0C entity_flags — a guessed sequential id does NOT match (witnessed:
	// capture2.pcapng ack=0x113F vs our guessed 3 -> crash; the working retail join had
	// ack==eFlags==3). The ack arrives during the early handshake, before world streaming, so it's
	// known by the time we stream the 0x0C.
	for (const ProtocolMessage &m : messages) {
		if (conn.admission_stage == GameAdmissionStage::Complete &&
		    m.tag == c2s::CLIENT_ACK && m.payload.size() >= 4) {
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

	// Produce the reactive §5.1 replies (handshake / server-info / mission-metadata / loadout /
	// spawn-confirm) for this datagram's gameplay messages, and cache the joiner's pre-spawn 0x0C pose
	// into conn.reply. The one-shot world-stream/spawn burst is owned by
	// Server_SendInitialGameStateToPlayer (tick_connections), NOT produced here. [orig: the 0x43 SESSION
	// dispatch routes each gameplay message to its NapiNPServerMsg_0x0NN reply handler]
	std::vector<ProtocolMessage> replies =
			dispatch_session_replies(ctx.config, conn, messages, now_tick,
			                         ctx.np_protocol.connection_list, ctx.world,
			                         ctx.np_protocol.session_seed_id);
	if (conn.admission_stage == GameAdmissionStage::Rejected) {
		// The retail reject overlay is not modeled. Still release the pending node immediately:
		// an out-of-order or malformed admission must not retain capacity or become an entity.
		if (teardown_connection(ctx, peer)) {
			HostAcceptEvent ev;
			ev.kind = HostAcceptEvent::Kind::PeerGoodbye;
			ev.peer = peer;
			out.events.push_back(std::move(ev));
		}
		return;
	}
	if (!replies.empty()) {
		if (defer_in_match_replies && conn.burst.spawned) {
			out.deferred_session_replies.insert(
					out.deferred_session_replies.end(),
					std::make_move_iterator(replies.begin()),
					std::make_move_iterator(replies.end()));
		} else {
			std::vector<uint8_t> dg = frame_session_replies(conn, replies);
			if (!dg.empty()) out.outbound.push_back(std::move(dg));
		}
	}

	// conn.burst is the single spawn-gate authority (driven by the World burst in tick_connections).
	// Surface the (verbatim-predicate) F3 / PeerSpawned edge-latched events — whichever of the datagram
	// / tick path observes the burst change first wins (one-shot via the *_announced latches).
	surface_burst_events(ctx, conn, out.events);

	// Once a connection exists, surface the joiner's in-match C2S 0x0C uplinks for Server_TickUpdate to
	// read-apply. Pre-spawn 0x0C only updates the cached pose (handled above) — no connection to
	// route it to yet.
	if (conn.spawned_announced) {
		std::vector<ProtocolMessage> c2s;
		for (const ProtocolMessage &m : messages) {
			if (m.tag == c2s::ENTITY_UPLINK) c2s.push_back(m);
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

// 0x44 ClientResendList -> reconstructed 0x83 packets. The request body names this host
// connection's local SK, followed by requested sequence dwords. Retail retains message records,
// not encrypted datagrams, so each old sequence is reframed with the current inbound ACK.
// [orig: NapiNP_HandleResendList @0x623800; SendSessionPacket @0x61EDD0]
void handle_client_resend_list(NapiNPServerCtx &ctx, const PeerAddr &peer,
		const std::vector<uint8_t> &body, HandleResult &out) {
	NapiNPConnection *conn = find_connection(ctx, peer);
	if (conn == nullptr || conn->server_scrk.empty()) return;

	std::vector<uint32_t> requested;
	if (!decode_session_resend_list(
			body.data(), body.size(), conn->server_sk, requested)) {
		return;
	}
	conn->receive_inactive_ms = 0;
	for (uint32_t requested_sequence : requested) {
		const uint32_t sequence = requested_sequence == 0
				? conn->seq.next_outbound_seq
				: requested_sequence;
		std::vector<uint8_t> session_body;
		if (!frame_session_packet_for_sequence(
				conn->seq,
				SessionCrypto{conn->server_scrk, {}, conn->client_ck},
				sequence, session_body)) {
			continue;
		}
		out.outbound.push_back(nw_encode_outbound(
				SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(session_body)));
	}
}

// 0x46 ClientGoodbye. [orig: Nwu_HandleClientGoodbye @0x624250]
void handle_client_goodbye(NapiNPServerCtx &ctx, const PeerAddr &peer,
		const std::vector<uint8_t> &body, HandleResult &out) {
	// The player teardown, BEFORE the node erase [orig: Server_HandlePlayerDisconnect @0x51B5C0]:
	// despawn the owned world entity — a leaked body keeps streaming forever and re-enters every
	// future joiner's 0x0C batch (the retail-join v23 ghost players, D-NET-149) — and broadcast
	// the roster slot's 0x46 REMOVAL to the remaining in-match peers (@0x51b8ad re-serializes
	// fieldFlags 0x1CF7 over the memset player slot -> the 0x8000 removal record
	// @0x505ecb..0x505ee0; send_mask 128 @0x51b8bc; the client's apply is
	// PlayerSlot_ClearAndUnlink @0x431420). Deferred, tracked in D-NET-149: the 0x32
	// minimap-slot + 0x6A squad broadcasts and the team spawn-token return
	// (@0x51b661..0x51b67a). The 120-second receive-timeout sweep uses this same teardown path.
	NapiNPConnection *conn = find_connection(ctx, peer);
	if (conn == nullptr || conn->type != 1 || body.size() < 4) return;
	const uint32_t receiver_local_key =
			static_cast<uint32_t>(body[0]) |
			(static_cast<uint32_t>(body[1]) << 8) |
			(static_cast<uint32_t>(body[2]) << 16) |
			(static_cast<uint32_t>(body[3]) << 24);
	// A delayed goodbye from the prior occupant of this endpoint must not destroy its replacement.
	// [orig: CNapiNPConnection_SendDisconnectPacket @0x61F2A0 / Nwu_HandleClientGoodbye @0x624250]
	if (receiver_local_key != conn->server_sk) return;
	if (!teardown_connection(ctx, peer)) return;
	HostAcceptEvent ev;
	ev.kind = HostAcceptEvent::Kind::PeerGoodbye;
	ev.peer = peer;
	out.events.push_back(std::move(ev));
}

} // namespace

void configure_session_runtime(NapiNPServerCtx &ctx) {
	// The reply/runtime config (§5.1 mission/player/spawn) now lives in ctx.config, seeded by
	// create_session with the host's full GameConfig — so this step no longer copies a config; it only
	// performs the peer-list reset below.
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
                                    const uint8_t *raw, std::size_t len, uint32_t now_tick,
                                    bool defer_in_match_replies) {
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
		handle_client_session(ctx, peer, body, now_tick, out, defer_in_match_replies);
		break;
	case SESSION_OPCODE_CLIENT_RESEND_LIST:
		handle_client_resend_list(ctx, peer, body, out);
		break;
	case SESSION_OPCODE_CLIENT_GOODBYE:
		handle_client_goodbye(ctx, peer, body, out);
		break;
	default:
		break;
	}
	return out;
}

std::vector<TickOut> flush_server_missing_requests(NapiNPServerCtx &ctx) {
	std::vector<TickOut> out;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type != 1 || !conn.seq.missing_request_pending) continue;
		conn.seq.missing_request_pending = false;
		if (conn.seq.queued_inbound.empty()) continue;

		const std::vector<uint32_t> missing =
				build_session_missing_sequence_list(conn.seq, false);
		std::vector<uint8_t> missing_body;
		if (!encode_session_resend_list(conn.client_ck, missing, missing_body)) continue;

		TickOut item;
		item.peer = conn.peer;
		item.outbound.push_back(nw_encode_outbound(
				SESSION_OPCODE_SERVER_RESEND_LIST, std::move(missing_body)));
		out.push_back(std::move(item));
	}
	return out;
}

std::vector<TickOut> tick_connections(NapiNPServerCtx &ctx, int elapsed_ms, uint32_t now_tick) {
	std::vector<TickOut> out;
	// Pump the JO receive timeout before any spawn/burst work. Collect keys first because the complete
	// teardown erases vector nodes and may broadcast roster removal through surviving transports.
	std::vector<PeerAddr> timed_out;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type != 1 || conn.phase < ConnectionPhase::Joined) continue;
		if (elapsed_ms > 0) {
			const uint64_t total =
					static_cast<uint64_t>(conn.receive_inactive_ms) +
					static_cast<uint32_t>(elapsed_ms);
			conn.receive_inactive_ms = static_cast<uint32_t>(
					total < std::numeric_limits<uint32_t>::max()
							? total
							: std::numeric_limits<uint32_t>::max());
		}
		if (conn.receive_inactive_ms > JO_GAME_SESSION_TIMEOUT_MS)
			timed_out.push_back(conn.peer);
	}
	for (const PeerAddr &peer : timed_out) {
		if (!teardown_connection(ctx, peer)) continue;
		TickOut timeout;
		timeout.peer = peer;
		HostAcceptEvent event;
		event.kind = HostAcceptEvent::Kind::PeerGoodbye;
		event.peer = peer;
		timeout.events.push_back(std::move(event));
		out.push_back(std::move(timeout));
	}

	auto append_active_probe = [](NapiNPConnection &conn, TickOut &to) {
		if (conn.type != 1 || conn.server_scrk.empty() ||
		    conn.seq.retained_outbound_message_count == 0 ||
		    conn.active_send_elapsed_ms <= kActiveSendIntervalMilliseconds) {
			return;
		}
		std::vector<uint8_t> datagram = frame_session_replies(conn, {});
		if (!datagram.empty()) to.outbound.push_back(std::move(datagram));
	};

	// P3 World-driven path: spawn any accepted-but-unspawned players once per tick (idempotent), before
	// walking the connections to advance their bursts.
	if (ctx.world != nullptr) Server_ProcessPendingPlayerSpawns(ctx, *ctx.world);

	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		// Accumulate the active-send interval before application production, but defer minting its
		// header probe until afterward. Any semantic packet framed below resets the timer and takes
		// this sequence slot, matching BuildOutgoingPackets rather than inserting an empty packet
		// immediately before queued data.
		if (conn.type == 1 && !conn.server_scrk.empty()) {
			if (conn.seq.retained_outbound_message_count == 0) {
				conn.active_send_elapsed_ms = 0;
			} else if (elapsed_ms > 0) {
				const uint64_t total =
						static_cast<uint64_t>(conn.active_send_elapsed_ms) +
						static_cast<uint32_t>(elapsed_ms);
				conn.active_send_elapsed_ms = static_cast<uint32_t>(
						total < std::numeric_limits<uint32_t>::max()
								? total
								: std::numeric_limits<uint32_t>::max());
			}
		}
		if (conn.burst.spawned) {
			// Spawned peers: Server_TickUpdate owns their per-frame 0x0A — but the roster
			// version check must keep running here so EXISTING clients learn about LATER
			// joins/leaves (D-NET-155). The first cut only evaluated it below this skip,
			// i.e. once, on each connection's own burst-completion tick — the v30 wire
			// showed the first joiner never received the grown 47-B 0x16 when the second
			// spawned (its HUD count stayed at 2).
			TickOut to;
			to.peer = conn.peer;
			if (conn.type == 1 &&
			    conn.reply.roster_seen_gen != ctx.np_protocol.roster_generation) {
				std::vector<ProtocolMessage> roster_reply{build_player_list_message(
						ctx.config, ctx.np_protocol.connection_list, ctx.world)};
				std::vector<uint8_t> dg = frame_session_replies(conn, roster_reply);
				if (!dg.empty()) to.outbound.push_back(std::move(dg));
				conn.reply.roster_seen_gen = ctx.np_protocol.roster_generation;
			}
			append_active_probe(conn, to);
			if (!to.outbound.empty()) out.push_back(std::move(to));
			continue;
		}

		TickOut to;
		to.peer = conn.peer;

		if (ctx.world == nullptr) {
			// No World means no semantic burst, but reliable settings/handshake records still need
			// the transport-level probe that induces the peer's missing-sequence request.
			append_active_probe(conn, to);
			if (!to.outbound.empty()) out.push_back(std::move(to));
			continue;
		}
		// Advance this connection's §5.2a burst one step and frame/ship the bodies (built from real
		// World/bms state). conn.burst is authoritative for the spawn-gate latches.
		// Golden ordering: the post-handshake burst (case 0x02 → 0x16 player-list) PRECEDES the
		// §5.2a world-stream. Without this gate, the burst fires in the same pump as the C2S 0x42
		// join — before the post-handshake round-trip (C2S 0x01→S2C 0x02→C2S 0x02→burst) has
		// completed, so the retail client receives the world-stream before it has reached join-FSM
		// state 6 verification. roster_pushed is set by dispatch case 0x02; the host's own loopback
		// (type 2) bypasses the gate. Remote peers have no timeout shortcut: the bundled client now
		// drives this captured exchange, and bypassing it marks malformed/out-of-order joins as players.
		const bool roster_ready =
				conn.type != 1 ||
				(conn.admission_stage == GameAdmissionStage::Complete &&
				 conn.reply.roster_pushed);
		if (conn.phase >= ConnectionPhase::PlayerAdded && roster_ready) {
			InitialStateStep step = Server_SendInitialGameStateToPlayer(ctx, conn, now_tick);
			ship_burst_messages(conn, step.messages, to.outbound);
		}

		// Roster versioning (D-NET-155): the first time we observe ANY connection spawned,
		// the roster grew — bump the generation so EVERY in-match client refreshes its 0x16
		// player list (the HUD player count follows it). The old one-shot-per-connection
		// re-push only reached the JOINING client, so existing clients' lists went stale
		// when a later joiner arrived (v29: HUD stuck at 2 with 3 players in).
		if (conn.burst.spawned && !conn.reply.roster_counted) {
			conn.reply.roster_counted = true;
			++ctx.np_protocol.roster_generation;
			// The join-time 0x46 push (fieldFlags 0x1CF7) to every EXISTING in-match client,
			// so its next 0x16's new row is ACCEPTED instead of dropped + 0x22-retried — the
			// unknown-slot churn behind the stale HUD count (D-NET-158). [orig:
			// Server_PlayerAdd @0x51D296]
			broadcast_player_sync_on_join(ctx.config, ctx.np_protocol.connection_list, conn,
			                              ctx.world);
		}
		// (Re)push the list to a spawned joiner whenever its seen generation is stale.
		// Covers the joiner's OWN spawn — the client needs its own slot to bind its local
		// player and deploy (golden: 0x16 31→39 just before the first C2S 0x0C) — and every
		// later roster change (join/leave). One framed push per generation per connection.
		if (conn.type == 1 && conn.burst.spawned &&
		    conn.reply.roster_seen_gen != ctx.np_protocol.roster_generation) {
			std::vector<ProtocolMessage> roster_reply{
					build_player_list_message(ctx.config, ctx.np_protocol.connection_list, ctx.world)};
			std::vector<uint8_t> dg = frame_session_replies(conn, roster_reply);
			if (!dg.empty()) to.outbound.push_back(std::move(dg));
			conn.reply.roster_seen_gen = ctx.np_protocol.roster_generation;
		}

		// Surface the F3 / PeerSpawned events from conn.burst (verbatim predicates). Same latch as the
		// datagram-driven handle_client_session path — whichever observes the burst change first wins.
		surface_burst_events(ctx, conn, to.events);

		append_active_probe(conn, to);
		if (to.outbound.empty() && to.events.empty()) continue;
		out.push_back(std::move(to));
	}
	return out;
}

bool frame_in_match_s2c(NapiNPServerCtx &ctx, const PeerAddr &peer, uint8_t inner_tag,
                        const std::vector<uint8_t> &inner_body, std::vector<uint8_t> &out_datagram) {
	ProtocolMessage msg = make_protocol_message(inner_tag, inner_body);
	return frame_in_match_s2c_batch(
			ctx, peer, std::vector<ProtocolMessage>{std::move(msg)}, out_datagram);
}

bool frame_in_match_s2c_batch(NapiNPServerCtx &ctx, const PeerAddr &peer,
		const std::vector<ProtocolMessage> &messages,
		std::vector<uint8_t> &out_datagram) {
	NapiNPConnection *conn = find_connection(ctx, peer);
	if (conn == nullptr || conn->server_scrk.empty()) {
		return false;
	}
	out_datagram = frame_session_replies(*conn, messages);
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
	if (conn == nullptr || conn->session_id.empty()) {
		return false;
	}
	const std::string player_name = !conn->player_name.empty()
			? conn->player_name
			: ctx.config.player_name;
	return bind_session_reply_player(*conn, player_name, player_slot, entity_handle);
}

bool drop_connection(NapiNPServerCtx &ctx, const PeerAddr &peer) {
	return teardown_connection(ctx, peer);
}

std::size_t connection_count(const NapiNPServerCtx &ctx) {
	return ctx.np_protocol.connection_list.size();
}

} // namespace opennova::np
