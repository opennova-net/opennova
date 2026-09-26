#include <runtime/inmatch/napi_np_protocol.h>

#include <runtime/inmatch/server_initial_state.h>    // Server_SendInitialGameStateToPlayer (the §5.2a burst)
#include <runtime/inmatch/server_message_dispatch.h> // dispatch_session_replies (the reactive §5.1 replies)
#include <runtime/inmatch/server_spawn.h>            // Server_ProcessPendingPlayerSpawns (World-driven spawn)

#include <net/npwire/ingame_encode.h>    // encode_player_sync_removal (the disconnect 0x46 removal)
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h> // make_protocol_message (frame the burst messages)
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/npwire/session_ping.h> // the shared 0x45/0x85 body codec

#include <runtime/inmatch/session_transport.h> // ISessionTransport::host_send (loopback burst delivery)

#include <base/io/strutil.h> // iequals (Napi_StrCaseEqual)

#include <runtime/world/ai.h>
#include <runtime/world/angle.h> // spawn_angle_bam
#include <runtime/world/entity.h>
#include <runtime/world/geom.h> // to_fixed
#include <runtime/world/spawn_select.h> // SpawnWaveList::remove_player (the disconnect leg)
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <utility>

namespace opennova::inmatch {

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

struct ParsedClientJoinRequest {
	bool spectator = false;
	std::string spectator_password;
	std::string join_password;
};

ParsedClientJoinRequest parse_client_join_request(const ClientAuth &auth) {
	ParsedClientJoinRequest parsed;
	for (const auto &blob : auth.cu) {
		uint8_t cu_type = 0;
		std::string cu_name;
		std::string cu_value;
		if (!parse_client_cu_chunk(
					blob.data(), blob.size(), cu_type, cu_name, cu_value) ||
		    cu_type != 2) {
			continue;
		}
		// The connection tag-list loader is ordered and uses atol semantics;
		// therefore the last duplicate wins for both JSR and JSPP. JSR is
		// stored through a uint8 truncation before the nonzero test. Tag
		// names compare case-insensitively.
		// [orig: NapiNetConfig_LoadFromConnTags @0x4c7260 — JSR ->
		// (unsigned __int8)atol @0x4c7607, JSPP -> NapiNetConfig_SetJspp;
		// Napi_StrCaseEqual @0x616e70]
		if (strutil::iequals(cu_name, "JSR")) {
			parsed.spectator = static_cast<uint8_t>(
					std::strtol(cu_value.c_str(), nullptr, 10)) != 0;
		} else if (strutil::iequals(cu_name, "JSP")) {
			// [orig: NapiNetConfig_SetJsp @0x4C26BE, Napi_CopyString(..., 64)]
			parsed.join_password = cu_value.substr(0, 63);
		} else if (strutil::iequals(cu_name, "JSPP")) {
			parsed.spectator_password = std::move(cu_value);
		}
	}
	return parsed;
}

ClientGameEnvironment parse_client_game_environment(const ClientAuth &auth) {
	ClientGameEnvironment parsed;
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
		// later duplicate overwrites an earlier value; the tags compare case-insensitively
		// [orig: Napi_StrCaseEqual @0x616e70].
		const long value = std::strtol(cu_value.c_str(), nullptr, 10);
		if (strutil::iequals(cu_name, "BT")) {
			parsed.bt = value;
		} else if (strutil::iequals(cu_name, "VN")) {
			parsed.vn = value;
		} else if (strutil::iequals(cu_name, "BN")) {
			parsed.bn = value;
		} else if (strutil::iequals(cu_name, "DB")) {
			// [orig: NapiNetConfig_LoadFromConnTags @0x4C7260, the DB leg @0x4C733E]
			parsed.db = value;
		} else if (strutil::iequals(cu_name, "MBN")) {
			parsed.mbn = value;
		} else if (strutil::iequals(cu_name, "SOPD")) {
			parsed.sopd = value;
		}
	}
	return parsed;
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
	// Store the session period for bookkeeping but do NOT arm the countdown:
	// the witnessed apply point is the admission dictation (H:0x00 mask 8 /
	// CS field 3 [orig: NapiNPServer_UpdateHoldoffTicks @0x4c5f40]) with the
	// boundary reset @0x61e140 — pre-dictation sequenced sends (the first
	// 0x83 after 0x82) are not period-gated, and an armed-at-creation
	// countdown delayed them up to period-1 pumps versus the golden timing.
	node.s2c_send_holdoff_ticks = clamp_send_holdoff_ticks(
			ctx.config.effective_send_holdoff_ticks());
	// The cs_dir template copy: the reap window and the outbound pool bound this node lives
	// under (120000 / 1200 unless the host's `_NSTMOUT.TXT` changed them).
	// [orig: CNapiNPConnection_Create @0x62acb0 copies proto+0xE44/+0xE80 (`rep movsd ecx=0Fh`
	//  @0x62ae7b/@0x62aeb8); NapiNPMessage_Create reads msg_out_max off the node @0x628048]
	node.timeouts = ctx.np_protocol.connection_template;
	node.seq = make_jo_game_session_sequencing(1, 0, node.timeouts.msg_out_max);
	ctx.np_protocol.connection_list.push_back(std::move(node));
	return ctx.np_protocol.connection_list.back();
}

// The S2C 0x86 SERVER_GOODBYE burst for `conn`: up to cs_dir0.recv_max_per_tick (4, clamped
// 0..32) identical datagrams carrying [le32 client CK][the connection's latched disconnect
// record, zeros when none is latched], NWU-encrypted like every session opcode. Empty for a
// node that never completed the 0x42 (no CK), a client-side node, or a host that is no longer
// running — SendDisconnectPacket's own gates. The unwitnessed proto+0x1F4 no-op gate (zero at
// NapiNPProtocol_Create @0x625840; its setter was not located) is not modeled.
// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253C0 — count clamp @0x6253ef..0x625403,
//  send loop @0x625406..0x625424; CNapiNPConnection_SendDisconnectPacket @0x61F2A0 — conn_flag0
//  gate @0x61f30b, `!is_server || host_running` @0x61f329, opcode 0x86 @0x61f367, the peer key
//  (CK) @0x61f3af, the record TLVs @0x61f3d0..0x61f4aa; recv_max_per_tick = 4 @0x4cab60]
std::vector<std::vector<uint8_t>> host_goodbye_burst(const NapiNPServerCtx &ctx,
		const NapiNPConnection &conn) {
	// SendDisconnectPacket's gates: an ACTIVE server-side node (conn_flag0, i.e. the 0x42 was
	// accepted and the CK is known) on a host that is still running; a client-side node would
	// select 0x46 instead and is never torn down through this host path.
	// [orig: @0x61f30b conn_flag0; @0x61f329 `!is_server || host_running`; @0x61f367 opcode 0x86]
	if (conn.type != NapiNPConnection::kTypeServerSide || conn.phase < ConnectionPhase::Joined ||
	    conn.client_ck == 0 || ctx.np_protocol.host_running == 0) {
		return {};
	}
	const DisconnectEvent record =
			conn.disconnect_event_valid ? conn.disconnect_event : DisconnectEvent{};
	std::vector<uint8_t> datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_GOODBYE, server_goodbye_to_bytes(conn.client_ck, record));
	// `n = clamp(recv_max_per_tick, 0, 32)` sends while each SendTo succeeds
	// [orig: TeardownActiveConnection @0x6253ef..0x625424].
	return std::vector<std::vector<uint8_t>>(disconnect_burst_count(), std::move(datagram));
}

// The one player/session teardown path shared by keyed goodbye, receive timeout, pending
// disconnect, StopServer, owner eviction, and same-address replacement. The original's Destroy runs
// TeardownActiveConnection: the disconnect-packet burst to the departing peer FIRST, then the
// removal callback (Server_HandlePlayerDisconnect) before the node is cleared; a bare list erase
// leaks the entity and roster identity. `goodbye_out` receives the burst (null = no wire output:
// the owner's dead-endpoint eviction and the D-NET-171 silent admission legs).
// [orig: CNapiNPConnection_Destroy @0x62A4B0 -> CNapiNPConnection_TeardownActiveConnection
//  @0x6253C0 (the burst @0x6253ef..0x625424 precedes the callback @0x625426..0x625438) ->
//  Server_HandlePlayerDisconnect @0x51B5C0]
bool teardown_connection(NapiNPServerCtx &ctx, const PeerAddr &peer,
		std::vector<std::vector<uint8_t>> *goodbye_out) {
	auto &list = ctx.np_protocol.connection_list;
	auto it = list.end();
	for (auto candidate = list.begin(); candidate != list.end(); ++candidate) {
		if (candidate->peer == peer) {
			it = candidate;
			break;
		}
	}
	if (it == list.end()) return false;

	if (goodbye_out != nullptr) {
		std::vector<std::vector<uint8_t>> burst = host_goodbye_burst(ctx, *it);
		goodbye_out->insert(goodbye_out->end(),
				std::make_move_iterator(burst.begin()),
				std::make_move_iterator(burst.end()));
	}

	const bool had_player = it->type == NapiNPConnection::kTypeServerSide &&
			(it->link.owned_entity.valid() || it->phase >= ConnectionPhase::PlayerAdded);
	const world::EntityHandle owned_entity = it->link.owned_entity;
	const uint8_t player_slot = it->reply.player_slot;
	if (had_player) {
		const bool freed_pool0_entity = ctx.world != nullptr && owned_entity.valid() &&
				owned_entity.pool() == 0;
		const uint16_t freed_pool0_slot =
				freed_pool0_entity ? static_cast<uint16_t>(owned_entity.slot()) : uint16_t{0};
		if (ctx.world != nullptr && owned_entity.valid()) {
			ctx.world->match.remove_player(*ctx.world, owned_entity);
			// The leaver drops out of every spawn-wave row it was queued in, before the
			// entity is removed: a stale row entry would restart that row's countdown
			// on release and, because pool-0 slots are reused, force-deploy the slot's
			// next occupant at the leaver's zone. The had_player gate above excludes
			// spectator-only connections exactly as retail's slot+5 arm returns early.
			// [orig: Server_HandlePlayerDisconnect @0x51B5C0 -> SpawnWaveList_RemovePlayer
			//  @0x52A410, the call @0x51b809 (before Server_RemoveEntityAndNotify @0x51b82e)]
			ctx.world->zones.spawn_waves.remove_player(owned_entity);
			ctx.world->vehicles.detach(owned_entity);
			// The leaver's row goes through Server_RemoveEntityAndNotify, whose
			// Player arm removes the placed devices the leaver owns, each through
			// the notifying removal, before the row itself.
			// [orig: Server_HandlePlayerDisconnect @0x51B5C0 — the
			//  Server_RemoveEntityAndNotify call @0x51B82E; Server_RemoveEntityAndNotify
			//  @0x50A270 — the Player test @0x50A2B1, the sweep call @0x50A2BB]
			if (const world::Entity *leaver = ctx.world->registry.get(owned_entity);
					leaver != nullptr &&
					((leaver->flags | leaver->engine_flags) & world::kEntityFlagPlayer) != 0)
				ctx.world->commands.remove_placed_devices_by_owner(owned_entity);
			// The player's brain (player_spawn attaches one) is freed with the row so
			// the next pool-0 spawn into this slot starts brainless.
			ctx.world->ai.release(owned_entity);
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
		if (connection.type == NapiNPConnection::kTypeClientSide || connection.phase >= ConnectionPhase::Joined) ++occupied;
	}
	return occupied;
}

// The witnessed 0x42-time rejection is a CR=0 0x82 carrying ONLY the identity
// echo plus the JFC failure family, the JFP sub-reason, and an optional JFS
// string — no MI/SK/CS/SCRK/NA. The validate-callback family is JFC=14 with
// JFP from the capacity callback (2 locked / 3 banned / 4 full / 5 full with
// positive spectator-only slots). [orig: NapiNPProtocol_SendJoinRejection
// @0x620cd0 (ex "SendDrawOverlay"), called from NapiNPProtocol_HandleClientJoin
// @0x62b750; CNapiNetwork_ValidateJoinRequest @0x4c61b0 writes +1520=14 /
// +1524=reason]
void reject_client_join(const ClientAuth &auth, const PeerAddr &peer,
		uint32_t failure_family, uint32_t failure_reason, HandleResult &out) {
	(void)peer;
	ServerAuth rejection;
	rejection.ci = auth.ci;
	rejection.ck = auth.ck;
	rejection.cr = 0;
	rejection.jfc = failure_family;
	rejection.jfp = failure_reason;
	out.outbound.push_back(nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH,
			server_auth_rejection_to_bytes(rejection)));
}

// Build + frame a 0x82 ServerAuth for `conn` from its CURRENT keys (server_sk / server_scrk /
// connection_id). Used for a fresh join AND to re-send on a retransmitted 0x42 (the original
// re-sends the cached packet via CNapiNPConnection_SendSessionInit @0x620ef0 rather than re-minting).
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
	// [orig: 0x82 MI TLV = conn->connection_id @ CNapiNPConnection_SendSessionInit 0x620ef0] — the
	// host-assigned dcb the joiner stores as its own ConnectionId and echoes in its 0x48 client-ack.
	reply.mi = conn.connection_id;
	// A GAME host advertises the JOINTOPERATIONS session template (120 s reap,
	// 30 s idle keepalive, 10 s active probe, 512/256 pools, 1200 msg cap), not
	// the NOVAWORLDUDP service block build_server_auth defaults to for the
	// service, with CS field 13 from the host's configured `mpmaxpacketsize`
	// [orig: CNapiNetwork_Init @0x4ca4a0 (field 13 @0x4CAA53) ->
	// CNapiNPConnection_Create @0x62acb0 -> SendSessionInit @0x620ef0].
	reply.client_cs = jointoperations_cs_fields(ctx.config.max_packet_size);
	reply.server_cs = jointoperations_cs_fields(ctx.config.max_packet_size);
	// SendSessionInit emits the host's LIVE cs_dir blocks verbatim, so a `_NSTMOUT.TXT`
	// override of timeout_ms (field 0) / msg_out_max (field 11) reaches the joiner here —
	// its HandleServerJoinResponse overlays these onto its own template. -1 rides as
	// 0xFFFFFFFF. [orig: CNapiNPConnection_SendSessionInit @0x620ef0; CNapiNetwork_Init
	//  stores @0x4caa81/@0x4cab20/@0x4cab54/@0x4cabf0]
	const SessionTimeoutConfig &tmpl = ctx.np_protocol.connection_template;
	for (std::vector<CsField> *block : {&reply.client_cs, &reply.server_cs}) {
		for (CsField &field : *block) {
			if (field.field_index == 0)
				field.value = static_cast<uint32_t>(tmpl.timeout_ms);
			else if (field.field_index == 11)
				field.value = static_cast<uint32_t>(tmpl.msg_out_max);
		}
	}
	return nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(reply));
}

HostJoinerPose pose_from_session(NapiNPServerCtx &ctx, const SessionReplyState &reply) {
	// [orig: HostSessionAccept::pose_from_session]
	HostJoinerPose p;
	const PreSpawnJoinerPose &pose = reply.pre_spawn_pose;
	if (pose.valid) {
		// The joiner already sent a C2S 0x0C — spawn it where it reported.
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
			p.entity_handle = e->handle.packed;
			p.item_type_id = static_cast<uint16_t>(e->item_id);
			p.pos_x = world::to_fixed(e->position.x);
			p.pos_y = world::to_fixed(e->position.y);
			p.pos_z = world::to_fixed(e->position.z);
			// Match the wire heading convention the P2 pose / the 0x0C body use: the high 16 bits of
			// the engine-frame BAM = (90 - mission_yaw) deg (D-NET-86), NOT raw mission degrees. The
			// spawned player's heading is its start marker's placement angle, so the high half is the
			// spawn angle's, the one the 0x0F world-state load sends (world::spawn_angle_bam).
			p.heading = static_cast<int16_t>(world::spawn_angle_bam(90 - e->yaw) >> 16);
			// The pose event mirrors the same signed BAM32 high word as the C2S 0x0C path. The
			// authoritative look pitch lives on AiEntity, not world::Entity; retain the zero default
			// when a non-AI entity is bound. [orig: pose_from_session gss.client_pitch;
			// entity_wire_bridge ae.pitch >> 16]
			if (const world::AiEntity *ae =
					ctx.world->ai.for_handle(conn.link.owned_entity)) {
				p.pitch = static_cast<int16_t>(ae->pitch >> 16);
			}
			p.team = e->team;
			return p;
		}
	}
	return pose_from_session(ctx, conn.reply);
}

// Queue one burst step's semantic messages for `conn`. A production remote and
// the host's own loopback both retain [tag][body] records in their transport;
// HostSession frames/batches the remote queue on its send boundary, while the
// loopback stays socketless. Protocol-only fixtures without an owner retain the
// legacy preframed fallback below. [orig: NapiNPServer_SendToConn @0x4c4f20 /
// mode-1 in-process]
void ship_burst_messages(NapiNPConnection &conn, std::vector<InitialStateMessage> &msgs,
                         std::vector<std::vector<uint8_t>> &outbound) {
	if (msgs.empty()) return;
	// Keep production initial-state records semantic until HostSession's send
	// boundary. They can then batch behind already-queued reactive replies
	// (golden: 0x46 then 0x2C); the packet builder still enforces its byte cap.
	if (conn.link.transport != nullptr) {
		for (InitialStateMessage &m : msgs)
			conn.link.transport->host_send(
					m.tag, std::move(m.body), m.reliable);
		return;
	}
	// Protocol-only tests may drive tick_connections without a HostOwner. Keep
	// their remote framed fallback; a loopback without a transport has no sink.
	if (conn.type == NapiNPConnection::kTypeClientSide) return;
	// Frame EACH drained burst message as its OWN 0x83 SESSION datagram — do NOT coalesce the whole
	// burst into one packet. The original emits a separate NapiNPServer_SendFiltered per tag; coalescing
	// the 616 B 0x0B BMS header plus the 0x0C pool-0 batch into one datagram would exceed the UDP MTU and
	// IP-fragment. Each message is already MTU-sized upstream: the world-stream pools (0x10/0x0D/0x0C/0x20)
	// are byte-budget paged with their witnessed per-stream policies by the shared chunker
	// (inmatch::slice_batch_pages via emit_paged_pool, ADR 0013), so this loop frames one page per datagram.
	for (InitialStateMessage &m : msgs) {
		ProtocolMessage message = make_protocol_message(
				m.tag, std::move(m.body));
		message.reliable = m.reliable;
		std::vector<ProtocolMessage> reply{std::move(message)};
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
	// A nonzero PM (a peer host's own announce) bypasses the NVS/PN/PG/PV1
	// identity walk; only a PM-less probe is held to the JO identity.
	// [orig: NapiNPProtocol_HandleClientHello @0x6213B0 — `jnz loc_6218B6`
	//  @0x6217C2 skips the validation, send @0x6218FC]
	if (!client_hello_admits(hello)) {
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
	// [orig: NapiNPProtocol_SendServerInfoPacket @0x6204b0, SF]
	reply.sf = ctx.config.server_password.empty() ? 0u : 1u;
	reply.p1 = ctx.config.game_type;
	// Retail advertises the same session BuildFlags value here and in the
	// trailing dword of S2C 0x08. create_session snapshots that live semantic
	// value after installing GameConfig; never substitute a captured constant.
	reply.p2 = ctx.np_protocol.build_flags;
	reply.np = occupied_player_count(ctx);
	reply.mp = ctx.np_protocol.max_players;
	// SUS1 is the protocol's server_user_string1: the GSID a NovaWorld
	// ServerHostResult handed this host, copied into the protocol block and
	// written whenever non-empty. A pure LAN host never ran that leg, so fresh
	// retail LAN captures omit the field; it is never session_seed_id.
	// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4D5C0F
	//  -> server_user_string1; NapiNPProtocol_SendServerInfoPacket @0x6204B0
	//  SUS1 write @0x620AF2 gated on the first byte]
	reply.sus1 = ctx.novaworld_gsid;
	reply.sus2 = ctx.config.expansion;
	// R1: advertise our real host key (seed-injected via SessionStartup) rather than
	// build_server_hello's placeholder default, when one is set. The retail 0x81 carries host_key.
	reply.hk = ctx.np_protocol.host_key;
	out.outbound.push_back(
			nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(reply)));
}

// 0x42 ClientAuth -> 0x82 ServerAuth (per-session SCRK established).
// [orig: NapiNPProtocol_HandleClientJoin @0x62b750]
void handle_client_join(NapiNPServerCtx &ctx, const PeerAddr &peer,
                        const std::vector<uint8_t> &body, HandleResult &out) {
	ClientAuth auth;
	if (!parse_client_auth(body.data(), body.size(), auth)) return;
	if (!ctx.is_authority || ctx.np_protocol.host_running == 0) return;
	// CU bounds are checked during the retail TLV walk, before its envelope.
	// [orig: NapiNPProtocol_HandleClientJoin @0x62b750]
	for (size_t i = 0; i < auth.cu.size(); ++i) {
		if (auth.cu[i].size() >= 2048 || i >= 64) {
			reject_client_join(auth, peer, auth.cu[i].size() >= 2048 ? 9u : 10u, 0, out);
			return;
		}
	}
	// Only the NVS/PN/PG/PV1 envelope is silent. The subsequent server-side
	// gates answer a CR=0 ServerAuth, in this order, before retry/replacement.
	// [orig: NapiNPProtocol_HandleClientJoin @0x62b750, gates @0x62bdf2]
	if (!matches_jointoperations_identity(auth)) return;
	// The PW and PV2 compares are case-insensitive C-string compares
	// [orig: Napi_StrCaseEqual @0x616e70].
	uint32_t reject = 0;
	if (auth.hk != ctx.np_protocol.host_key) reject = 3;
	else if (!ctx.config.server_password.empty() &&
	         !strutil::iequals(auth.pw, std::string_view(ctx.config.server_password.c_str()))) reject = 4;
	else if (!strutil::iequals(auth.pv2, "16")) reject = 7;
	else if (auth.na.empty()) reject = 5;
	else if (ctx.np_protocol.reject_new_connections) reject = 6;
	if (reject != 0) {
		reject_client_join(auth, peer, reject, 0, out);
		return;
	}
	const ClientGameEnvironment game_environment =
			parse_client_game_environment(auth);
	const ParsedClientJoinRequest join_role = parse_client_join_request(auth);

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
		// CNapiNPConnection_SendSessionInit @0x620ef0 (re-emits the same 0x82), return 1]
		// The retransmit leg touches no receive clock: only an in-order session packet or
		// a ping stamps conn+0x5E8 [orig: HandleClientJoin @0x62bee6..0x62bef1].
		if (existing->client_ci == auth.ci && existing->client_ck == auth.ck) {
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
		// A different client (CI/CK) reusing an already-joined addr: destroy the stale node and
		// recreate fresh below. The old occupant's 0x86 burst (its latched record, normally
		// none => zero TLVs) goes to the same endpoint keyed by the OLD CK; the replacement's
		// own key check drops it. [orig: HandleClientJoin @0x62befb CNapiNPConnection_Destroy
		//  then CNapiNPConnection_Create]
		if (teardown_connection(ctx, peer, &out.outbound)) {
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
	// is already full. The witnessed gate is ONE shared count — players AND
	// spectators against max_players plus positive spectator-only slots (a -1
	// setting shares the ordinary cap; ordinary players may occupy spectator
	// headroom — retail has no separate ordinary cap, here or at the game-layer
	// join gate @0x512aa0). The count is the host's own type-2 loopback (when
	// present) plus already-admitted (>= Joined) joiners — matching
	// networkCtx[11]. The reject is the CR=0 0x82 with the validate-callback
	// family JFC=14 and reason JFP=4, or JFP=5 when spectator-only slots exist
	// (@0x4c624c). Spectator-specific validation (codes 14/15/16) is NOT this
	// leg: it runs at the game-layer 0x00 join message and punts through the
	// connection-description record (server_message_dispatch.cpp).
	if (ctx.is_in_session && ctx.join_locked) {
		reject_client_join(auth, peer, 14, 2, out);
		return;
	}
	// The ban compares the datagram's UDP source (conn+0x30, stored from the
	// packet address at create), never the client-reported SIP TLV (conn+0x38 /
	// session_keys+0x158, stored @0x62bf91/@0x62bf85 and read by nothing here).
	// g_banned_id_list packs a.b.c.d as a | b<<8 | c<<16 | d<<24 -- PeerAddr::ip.
	// [orig: CNapiNetwork_ValidateJoinRequest @0x4c6203..0x4c6217 reads conn+0x30 =
	//  the datagram source stored @0x62bf28; BanList_ParseIPEntry @0x4fd5c9]
	if (ctx.is_in_session && std::find(ctx.banned_join_addresses.begin(),
	            ctx.banned_join_addresses.end(), peer.ip) != ctx.banned_join_addresses.end()) {
		reject_client_join(auth, peer, 14, 3, out);
		return;
	}
	const uint32_t occupied = occupied_player_count(ctx);
	if (occupied >= ctx.config.total_player_slot_capacity()) {
		reject_client_join(auth, peer, 14,
				ctx.config.spectator_slots > 0 ? 5u : 4u, out);
		return;
	}

	NapiNPConnection &conn = find_or_create_connection(ctx, peer);
	// Retail stores the ClientAuth CU tags on the connection and only latches
	// the live spectator flag at the game-layer join message; keep the same
	// two-step shape so the roster/0x16/0x75 state cannot flip before the
	// witnessed latch point. [orig: NapiNetConfig_LoadFromConnTags @0x4c7260;
	// the entry+55 latch @0x512aa0]
	conn.join_environment = game_environment;
	conn.join_spectator_request = join_role.spectator ? 1 : 0;
	conn.join_spectator_password = join_role.spectator_password;
	conn.join_password = join_role.join_password;
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
	conn.c2s_reassembly = {};
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
		if (strutil::iequals(cu_name, "CI0")) {
			conn.char_vars.char_id[0] = static_cast<uint16_t>(v);
		} else if (strutil::iequals(cu_name, "CI1")) {
			conn.char_vars.char_id[1] = static_cast<uint16_t>(v);
		} else if (strutil::iequals(cu_name, "TR")) {
			// [@0x4c752f] tr != -1 && (u8)tr >= 2 -> -1: only 0 (side A) and 1 (side B) pass.
			uint8_t tr = static_cast<uint8_t>(v);
			if (tr != 0xFF && tr >= 2) tr = 0xFF;
			conn.char_vars.team_request = tr;
		} else if (strutil::iequals(cu_name, "CTA")) {
			conn.char_vars.char_class[0] = static_cast<uint8_t>(v);
		} else if (strutil::iequals(cu_name, "CTB")) {
			conn.char_vars.char_class[1] = static_cast<uint8_t>(v);
		} else if (strutil::iequals(cu_name, "VCA")) {
			conn.char_vars.avatar[0] = static_cast<uint8_t>(v);
		} else if (strutil::iequals(cu_name, "VCB")) {
			conn.char_vars.avatar[1] = static_cast<uint8_t>(v);
		}
		// The environment tags (DB included) were parsed and validated before allocation.
		// Stored/display-only fields (VERSIONSTRING/COUNTRYCODE/TZB...) have no retained
		// runtime consumer yet.
	}
	// [orig: CNapiNPConnection_Create @0x62acb0 — conn.connection_id (the dcb) = ++protocol[947],
	// wrapping 0 -> 1]. On a LAN listen host the host ASSIGNS the dcb (it does not learn it from the
	// client) and advertises it in ServerAuth.MI. It becomes spawn-eligible only after the final
	// admission 0x02; the bundled client later echoes it in 0x48. On NovaWorld the gate-assigned id
	// arrives via that 0x48 and overrides this host provisional value -- retail's Create assigns
	// the provisional `++protocol[947]` on BOTH network types (no transport-mode gate @0x62acb0),
	// so the provisional-then-override shape is the port, not a placeholder.
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
	// The validation-phase deadline base for CheckPlayerTimeouts (netPlayer+0xA4
	// is stamped with GetTickCount when the node enters the validating state).
	conn.join_validated_host_ms = ctx.np_protocol.host_run_duration_ms;
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
	// The reap clock is stamped ONLY when the packet is delivered in order: retail's
	// HandleSessionPacket reaches ParseMessages (which writes conn+0x5E8 before its message loop,
	// so a zero-message keepalive counts) solely for seq == recv_ack_seq+1 or the in-order drain
	// that closes a gap; a stale packet for another receiver-local SK, seq 0, a duplicate and a
	// future packet all return without touching it.
	// [orig: HandleSessionPacket @0x626A00 — in-order leg @0x626beb..0x626bfc -> ParseMessages
	//  @0x625BC0 stamp @0x625d54; seq 0 @0x626bcc / duplicate @0x626c03 / future @0x626c0c..0x626c3a
	//  stamp nothing]
	if (admission.admitted) conn.receive_inactive_ms = 0;

	// The header's ack_count is the peer's "last of YOUR seqs I received" — the confirm side of the
	// initial-state backlog throttle (retail clients carry it on every 0x43, including game-message-
	// less keepalive datagrams; the golden world-stream gap has no C2S game messages yet the stream
	// advances). High-water only: a reordered older ack must not un-confirm. [orig: header layout
	// @0x61edd0 field +8; consumed by the conn+0x768 outstanding gate @0x51bf1b/0x51bc04]
	if (admission.admitted && admission.max_ack_count > conn.peer_acked_seq)
		conn.peer_acked_seq = admission.max_ack_count;
	if (admission.admitted)
		acknowledge_session_packets(conn.seq, admission.max_ack_count);

	// Deframing admits physical records in contiguous sequence order, including
	// packets released while closing a gap. Fold this connection's witnessed
	// FIRST/MID/FINAL stream before every metadata and gameplay consumer below;
	// a partial record is not itself a semantic C2S message.
	std::vector<ProtocolMessage> semantic_messages;
	semantic_messages.reserve(messages.size());
	for (const SessionDeframeAdmission::Packet &packet : admission.packets) {
		for (const ProtocolMessage &physical : packet.messages) {
			std::vector<uint8_t> payload;
			bool was_fragmented = false;
			if (!reassemble_protocol_payload(
						conn.c2s_reassembly, physical, payload, &was_fragmented)) {
				continue;
			}
			if (!was_fragmented) {
				semantic_messages.push_back(physical);
				continue;
			}
			// Fragment and physical-length bits are transport metadata. Preserve
			// the dispatch table and skip mode, then describe the completed body.
			constexpr uint8_t kSemanticFlagMask =
					PROTOCOL_MSG_FLAG_SETTINGS_UPDATE |
					PROTOCOL_MSG_FLAG_SKIP1 | PROTOCOL_MSG_FLAG_SKIP2 | 0x01u;
			uint8_t semantic_flags = static_cast<uint8_t>(
					physical.flags.raw & kSemanticFlagMask);
			if (!payload.empty()) {
				semantic_flags = static_cast<uint8_t>(semantic_flags |
						(payload.size() > 0xFFu ? PROTOCOL_MSG_FLAG_LEN16
						                          : PROTOCOL_MSG_FLAG_LEN8));
			}
			ProtocolMessage semantic = make_protocol_message(
					physical.tag, std::move(payload), semantic_flags);
			semantic.skip_bytes = physical.skip_bytes;
			semantic_messages.push_back(std::move(semantic));
		}
	}
	messages = std::move(semantic_messages);

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
	ServerDispatchInputs dispatch_inputs;
	dispatch_inputs.session_uptime_ms = ctx.np_protocol.host_run_duration_ms;
	dispatch_inputs.mission_metadata_blob = &ctx.mission_metadata_blob;
	dispatch_inputs.server_info_transfer_id = ctx.server_info_transfer_id;
	dispatch_inputs.mission_metadata_transfer_id = ctx.mission_metadata_transfer_id;
	dispatch_inputs.round_end_board_stream = &ctx.round_end_board_stream;
	dispatch_inputs.medic_request_format = &ctx.server_text.medic_request_format;
	dispatch_inputs.server_ctx = &ctx;
	std::vector<ProtocolMessage> replies =
			dispatch_session_replies(ctx.config, conn, messages, now_tick,
			                         ctx.np_protocol.connection_list, ctx.world,
			                         dispatch_inputs);
	if (conn.admission_stage == GameAdmissionStage::Rejected) {
		// A game-layer join-gate failure that STAGED retail's description punt
		// (the spectator DPC 14/15/16 legs) must retain the node: the owner
		// flushes the reliable record on its next boundary and the punted
		// joiner answers with the CLIENT_GOODBYE burst that performs the real
		// teardown. [orig: the @0x512aa0 failure return keeps the connection;
		// teardown is the client's goodbye or the receive reap]
		if (conn.host_disconnect_sent) return;
		// The remaining silent legs are not modeled (D-NET-171). Release the
		// pending node immediately: an out-of-order or malformed admission
		// must not retain capacity or become an entity. No wire output: retail
		// keeps this node and sends nothing on these legs.
		if (teardown_connection(ctx, peer, nullptr)) {
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

	// With a World bound, every in-match C2S 0x0C was read-applied in wire order inside
	// dispatch_session_replies above (retail's NapiNPServerMsg_0x00C applies during the
	// dispatch walk). Only the World-less unit path still surfaces the uplinks for the
	// transport FIFO that Server_TickUpdate drains; surfacing them here as well with a
	// World would apply each pose twice. Pre-spawn 0x0C only updates the cached pose.
	if (conn.spawned_announced && ctx.world == nullptr) {
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
	// A resend list is not receive activity: the handler reads GetTickCount and discards it,
	// leaving the reap clock alone [orig: NapiNP_HandleResendList @0x623800 @0x62395f].
	// The backoff callback (cb_server_6 = sub_4C62A0 -> entity+89876 = 1, the
	// next 0x0A budget halving) is latched only by a NONZERO requested dword; a
	// zero-only "send next" list or a key-only body arms nothing.
	// [orig: NapiNP_HandleResendList @0x6239aa sets the latch on the nonzero
	//  path only; @0x6239ef gates the callback on it; @0x623974 returns before
	//  the loop on a key-only body]
	if (std::any_of(requested.begin(), requested.end(),
			[](uint32_t sequence) { return sequence != 0; })) {
		conn->link.nak_backoff_pending = true;
	}
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

// 0x45 ClientPing -> 0x85 ServerPing (WR set) or an RTT sample (WR clear). The
// body is the receiver-local key dword then WR/MS TLVs (net/npwire/session_ping.h);
// a mismatched key is dropped before anything else. The ping refreshes the reap
// clock like an in-order session packet. [orig: Nwu_HandlePing @0x623A70 —
// active/drop gates @0x623B55, key compare @0x623BC2, WR @0x623BFF, MS @0x623C1A,
// activity stamp @0x623C56, SendPing(conn, 0, MS) @0x623C6F, rtt store @0x623C9B]
void handle_client_ping(NapiNPServerCtx &ctx, const PeerAddr &peer,
		const std::vector<uint8_t> &body, HandleResult &out) {
	NapiNPConnection *conn = find_connection(ctx, peer);
	if (conn == nullptr || conn->type != NapiNPConnection::kTypeServerSide ||
			conn->phase < ConnectionPhase::Joined)
		return;
	SessionPingBody ping;
	if (!parse_session_ping_body(body.data(), body.size(), ping)) return;
	if (ping.receiver_local_key != conn->server_sk) return;
	conn->receive_inactive_ms = 0;
	if (ping.wants_reply) {
		out.outbound.push_back(nw_encode_outbound(SESSION_OPCODE_SERVER_PING,
				build_session_ping_body(conn->client_ck, /*wants_reply=*/false,
						ping.timestamp_ms)));
	} else {
		conn->session_ping_rtt_ms =
				ctx.np_protocol.host_run_duration_ms - ping.timestamp_ms;
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
	if (conn == nullptr || conn->type != NapiNPConnection::kTypeServerSide || body.size() < 4) return;
	const uint32_t receiver_local_key =
			static_cast<uint32_t>(body[0]) |
			(static_cast<uint32_t>(body[1]) << 8) |
			(static_cast<uint32_t>(body[2]) << 16) |
			(static_cast<uint32_t>(body[3]) << 24);
	// A delayed goodbye from the prior occupant of this endpoint must not destroy its replacement.
	// [orig: CNapiNPConnection_SendDisconnectPacket @0x61F2A0 / Nwu_HandleClientGoodbye @0x624250]
	if (receiver_local_key != conn->server_sk) return;
	// The client's record (DC/DP1/DP2/DSTR/DPC/DDSTR; DS read and discarded) is latched with the
	// PEER's role 2 if nothing is latched yet, then RequestDisconnect -> the destroy echoes it
	// back in the 0x86 burst. An empty TLV run (ReadTLV fails first) still latches zeros.
	// [orig: Nwu_HandleDisconnect @0x623CE0 — TLV walk @0x623eb2..0x623fbd, the type-1 record
	//  {[1]=2 @0x624005, DC/DP1/DP2 @0x62400d..0x624015, DSTR->128 @0x624019, DPC @0x624090,
	//  DDSTR->32 @0x624097}, latch @0x6240a8..0x6240ba, RequestDisconnect @0x6240c4]
	DisconnectEvent received;
	(void)parse_disconnect_event(body.data() + 4, body.size() - 4, received);
	latch_disconnect_event(*conn, make_disconnect_event(2, received.dc, received.dp1,
			received.dp2, received.dstr, received.dpc, received.ddstr));
	if (!teardown_connection(ctx, peer, &out.outbound)) return;
	HostAcceptEvent ev;
	ev.kind = HostAcceptEvent::Kind::PeerGoodbye;
	ev.peer = peer;
	out.events.push_back(std::move(ev));
}

} // namespace

HandleResult handle_server_datagram(NapiNPServerCtx &ctx, const PeerAddr &peer,
                                    const uint8_t *raw, std::size_t len, uint32_t now_tick,
                                    bool defer_in_match_replies) {
	HandleResult out;

	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(raw, len, opcode, body)) {
		return out; // bad envelope — drop quietly
	}

	// Once the host has staged its terminal connection-description record, the
	// resident node exists only long enough to receive the matching teardown
	// acknowledgement. Drop every other leg before it can refresh activity,
	// mutate sequencing/reassembly, surface gameplay, enqueue a deferred reply,
	// or replay retained packets. The 0x46 handler below still validates the
	// receiver-local key, so a delayed goodbye for an older endpoint occupant
	// cannot tear down this node.
	if (opcode != SESSION_OPCODE_CLIENT_GOODBYE) {
		const NapiNPConnection *conn = find_connection(ctx, peer);
		if (conn != nullptr && conn->host_disconnect_sent) return out;
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
	case SESSION_OPCODE_CLIENT_PING:
		handle_client_ping(ctx, peer, body, out);
		break;
	case SESSION_OPCODE_CLIENT_GOODBYE:
		handle_client_goodbye(ctx, peer, body, out);
		break;
	default:
		break;
	}
	return out;
}

std::vector<TickOut> flush_server_missing_requests(
		NapiNPServerCtx &ctx, bool respect_s2c_send_boundary) {
	std::vector<TickOut> out;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type != NapiNPConnection::kTypeServerSide || !conn.seq.missing_request_pending) continue;
		if (respect_s2c_send_boundary && !conn.s2c_send_boundary_open) continue;
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

std::vector<TickOut> tick_connections(
		NapiNPServerCtx &ctx, int elapsed_ms, uint32_t now_tick,
		bool respect_s2c_send_boundary) {
	std::vector<TickOut> out;
	// Pump the JO receive timeout before any spawn/burst work. Collect keys first because the complete
	// teardown erases vector nodes and may broadcast roster removal through surviving transports.
	// [orig: CNapiNPConnection_PumpStateMachine @0x6292E0 state 1 — `timeout_ms < 0 -> skip`
	//  @0x62934c, elapsed = now - conn+0x5E8 @0x62935c, `elapsed > timeout_ms` @0x629362, the
	//  record {role, DC 3, DP1 elapsed, DP2 timeout, "", 0, "NP.C:PT:SERTMOUT"} @0x6293b7..0x6293e6
	//  latched-if-invalid @0x6293f4..0x629406, RequestDisconnect @0x62940a -> pending_disconnect
	//  @0x61e107; NapiNPProtocol_Pump @0x62A650 then destroys every pending node after its
	//  per-connection pump @0x62a6fd..0x62a793, latching {1, 8, 0, 0, "", 0, "NP.C:PMP:PDESTME"}
	//  only when nothing else was @0x62a72f..0x62a788]
	std::vector<PeerAddr> destroy_pending;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type != NapiNPConnection::kTypeServerSide || conn.phase < ConnectionPhase::Joined) continue;
		if (elapsed_ms > 0) {
			const uint64_t total =
					static_cast<uint64_t>(conn.receive_inactive_ms) +
					static_cast<uint32_t>(elapsed_ms);
			conn.receive_inactive_ms = static_cast<uint32_t>(
					total < std::numeric_limits<uint32_t>::max()
							? total
							: std::numeric_limits<uint32_t>::max());
		}
		if (conn.timeouts.timeout_ms >= 0 &&
		    conn.receive_inactive_ms > static_cast<uint32_t>(conn.timeouts.timeout_ms)) {
			latch_disconnect_event(conn, make_disconnect_event(1, 3, conn.receive_inactive_ms,
					static_cast<uint32_t>(conn.timeouts.timeout_ms), "", 0,
					"NP.C:PT:SERTMOUT"));
			conn.pending_disconnect = true;
		}
		if (conn.pending_disconnect) {
			latch_disconnect_event(conn,
					make_disconnect_event(1, 8, 0, 0, "", 0, "NP.C:PMP:PDESTME"));
			destroy_pending.push_back(conn.peer);
		}
	}
	for (const PeerAddr &peer : destroy_pending) {
		TickOut destroyed;
		destroyed.peer = peer;
		if (!teardown_connection(ctx, peer, &destroyed.outbound)) continue;
		HostAcceptEvent event;
		event.kind = HostAcceptEvent::Kind::PeerGoodbye;
		event.peer = peer;
		destroyed.events.push_back(std::move(event));
		out.push_back(std::move(destroyed));
	}

	auto append_active_probe = [](NapiNPConnection &conn, TickOut &to) {
		if (conn.type != NapiNPConnection::kTypeServerSide || conn.server_scrk.empty() ||
		    conn.seq.retained_outbound_message_count == 0 ||
		    conn.active_send_elapsed_ms <= kActiveSendIntervalMilliseconds) {
			return;
		}
		std::vector<uint8_t> datagram = frame_session_replies(conn, {});
		if (!datagram.empty()) to.outbound.push_back(std::move(datagram));
	};

	// P3 World-driven path: the pending-player spawn pump runs on the shared periodic
	// second — the call sits inside Server_TickUpdate's g_periodic_second_timer block,
	// not on every tick — before walking the connections to advance their bursts. The
	// bring-up call for the host's own player (host_session.cpp) is direct.
	// [orig: Server_TickUpdate @0x51DBFD inside the reload-62 block @0x51DB93]
	if (ctx.world != nullptr && ctx.world->match.periodic_second())
		Server_ProcessPendingPlayerSpawns(ctx, *ctx.world);

	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		// Accumulate the active-send interval before application production, but defer minting its
		// header probe until afterward. Any semantic packet framed below resets the timer and takes
		// this sequence slot, matching BuildOutgoingPackets rather than inserting an empty packet
		// immediately before queued data.
		if (conn.type == NapiNPConnection::kTypeServerSide && !conn.server_scrk.empty()) {
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
		const bool send_boundary_closed =
				respect_s2c_send_boundary && conn.type == NapiNPConnection::kTypeServerSide &&
				!conn.s2c_send_boundary_open;
		if (conn.burst.spawned) {
			// Spawned peers: Server_TickUpdate owns their per-frame 0x0A — but the roster
			// version check must keep running here so EXISTING clients learn about LATER
			// joins/leaves (D-NET-155). The first cut only evaluated it below this skip,
			// i.e. once, on each connection's own burst-completion tick — the v30 wire
			// showed the first joiner never received the grown 47-B 0x16 when the second
			// spawned (its HUD count stayed at 2).
			if (send_boundary_closed) continue;
			TickOut to;
			to.peer = conn.peer;
			if (conn.type == NapiNPConnection::kTypeServerSide &&
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
		if (send_boundary_closed) continue;

		// A held pending player (capacity / balance-join) is re-sent the one-byte
		// S2C 0x03 restriction flag the spawn pump staged, at most once a second.
		// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4C8F9A..0x4C8FD1]
		if (conn.type == NapiNPConnection::kTypeServerSide &&
				conn.phase < ConnectionPhase::PlayerAdded &&
				conn.reply.admission_hold_nag_pending) {
			conn.reply.admission_hold_nag_pending = false;
			std::vector<uint8_t> nag = frame_session_replies(
					conn, {make_protocol_message(s2c::SYNC_TICK, {0x00})});
			if (!nag.empty()) to.outbound.push_back(std::move(nag));
		}

		// Retail's C2S 0x02 handler, pending-player spawn pump, and first
		// roster publish occupy distinct send boundaries. In particular 0x04
		// must arrive after the client has processed one intervening frame;
		// otherwise its join initialization resets the assigned team and it
		// submits the opposite faction's 0x2F kit. The production host calls
		// tick_connections in the same pump that handled C2S 0x02, so the
		// captured admission tick is an explicit lower bound as well as the
		// packet latches below. The pump itself runs on the periodic second
		// at the head of this call, so the 0x03 / settings / 0x05 / 0x04 /
		// 0x7B boundary ships in the same pump that added the player — the
		// retail in-pump order [orig: CNapiServer_ProcessPendingPlayerSpawns
		// @0x4C8FF6..0x4C9123]; the one residual is that retail writes its
		// 0x03 before Server_BuildPlayerInfoAndAdd and these bodies are built
		// after it, which the wire cannot distinguish.
		const bool staged_join_ready =
				conn.type == NapiNPConnection::kTypeServerSide &&
				conn.admission_stage == GameAdmissionStage::Complete &&
				conn.reply.admission_metadata_pushed &&
				ctx.world != nullptr &&
				conn.phase >= ConnectionPhase::PlayerAdded &&
				now_tick != conn.reply.admission_completed_tick;
		if (staged_join_ready && !conn.reply.spawn_metadata_pushed) {
			std::vector<ProtocolMessage> spawn_metadata =
					build_spawn_pump_metadata(
							ctx.config, conn,
							ctx.np_protocol.connection_list, ctx.world);
			std::vector<uint8_t> datagram =
					frame_session_replies(conn, spawn_metadata);
			if (!datagram.empty()) {
				to.outbound.push_back(std::move(datagram));
				conn.reply.spawn_metadata_pushed = true;
			}
			if (!to.outbound.empty()) out.push_back(std::move(to));
			continue;
		}
		if (ctx.world == nullptr) {
			// No World means no semantic burst, but reliable settings/handshake records still need
			// the transport-level probe that induces the peer's missing-sequence request.
			append_active_probe(conn, to);
			if (!to.outbound.empty()) out.push_back(std::move(to));
			continue;
		}
		// Advance this connection's §5.2a burst one step and frame/ship the bodies (built from real
		// World/bms state). conn.burst is authoritative for the spawn-gate latches.
		// Golden ordering: admission, spawn metadata, then the 0x16 player-list PRECEDE the
		// §5.2a world-stream. Without this gate, the burst fires in the same pump as the C2S 0x42
		// join — before the post-handshake round-trip (C2S 0x01→S2C 0x02→C2S 0x02→burst) has
		// completed, so the retail client receives the world-stream before it has reached join-FSM
		// state 6 verification. roster_pushed is set by the first C2S 0x37 reply; the host's own loopback
		// (type 2) bypasses the gate. Remote peers have no timeout shortcut: the bundled client now
		// drives this captured exchange, and bypassing it marks malformed/out-of-order joins as players.
		const bool roster_ready =
				conn.type != NapiNPConnection::kTypeServerSide ||
				(conn.admission_stage == GameAdmissionStage::Complete &&
				 conn.reply.roster_pushed &&
				 now_tick != conn.reply.roster_completed_tick);
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
		if (conn.type == NapiNPConnection::kTypeServerSide && conn.burst.spawned &&
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

bool destroy_connection(NapiNPServerCtx &ctx, const PeerAddr &peer,
		std::vector<std::vector<uint8_t>> *goodbye_out) {
	return teardown_connection(ctx, peer, goodbye_out);
}

std::size_t connection_count(const NapiNPServerCtx &ctx) {
	return ctx.np_protocol.connection_list.size();
}

} // namespace opennova::inmatch
