#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_dispatch_join.h>    // the admission FSM and the join cases
#include <runtime/inmatch/server_dispatch_respawn.h> // the respawn and deploy cases
#include <runtime/inmatch/server_visible_players.h>
#include <runtime/inmatch/server_entity_routes.h> // Server_HandleDoorRowRequest (C2S 0x1A)
#include <runtime/inmatch/server_kill_page.h>
#include <runtime/inmatch/server_emote.h>
#include <runtime/inmatch/server_radio_call.h>
#include <runtime/inmatch/server_loadout_grant.h> // the 0x2F grant family (GrantedWeaponLoadout, grant_weapon_loadout, ...)
#include <runtime/inmatch/server_fired_round.h>   // the C2S 0x06 fired round


#include <runtime/inmatch/integrity_sources.h>
#include <runtime/inmatch/end_round_protocol.h>
#include <runtime/inmatch/server_spawn.h> // Server_HandlePlayerJoinRequest (C2S 0x0B)
#include <runtime/inmatch/server_tick.h> // Server_StageHostDisconnect
#include <runtime/inmatch/server_chat.h>         // the C2S 0x0D fan-out
#include <runtime/inmatch/server_squad.h>        // the command map's squad legs
#include <runtime/inmatch/server_net_quality.h>  // the 0x2C return leg / 0x4C store
#include <runtime/inmatch/server_vehicle_spawn.h> // the 0x40 / 0x42 handlers
#include <runtime/inmatch/napi_np_server_ctx.h>  // ServerDispatchInputs::server_ctx
#include <runtime/inmatch/session_status.h>
#include <runtime/world/weapon_table_build.h> // loadout_entry_permitted (D-NET-141)

#include <runtime/replication/connection_fan.h>     // apply_connection_uplink — the in-order 0x0C read-apply
#include <runtime/replication/entity_wire_bridge.h> // build_full_entity_spawn — the 0x0F -> 0x18 repair record

#include <base/gameprofile/game_type.h>       // is_waypoint_family / is_stock_coop (§5.32, D-NET-203/205)
#include <net/npwire/wire_handle.h>     // is_batch_end_sentinel / kInvalid (the wire handle home)
#include <net/npwire/ingame_decode.h>   // decode_entity_packet_sub_header / decode_player_extended_uplink
#include <net/npwire/ingame_encode.h>   // encode_player_sync / encode_player_list (§5.1)
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/replication_model.h> // PlayerReplicationState (POD) — the reply builders' input

#include <runtime/world/entity.h> // world::Entity / EntityHandle: the bound entity read through owned_entity
#include <runtime/world/local_player.h>
#include <runtime/world/shell_sort_by_value.h> // CPairList_ShellSortByValue (the 0x16 rows)
#include <runtime/world/vehicle_attach.h> // entity_process_vehicle_attach / entity_detach_from_vehicle (0x26/0x27)
#include <runtime/world/world.h>  // world::World::registry (the authoritative roster, §6.9)
#include <runtime/audio/sound_profile.h> // compose_entity_sound_set — the 0x2E MEDIC_REQUEST composite
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/infantry.h>

#include <algorithm>
#include <utility>
#include <base/io/le.h>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::inmatch {

namespace {

bool client_delta_fits_retail_time_window(
		uint32_t client_delta, uint32_t host_delta) {
	// Retail truncates 1.03 * hostDelta before its unsigned comparison. The
	// exact rational form avoids platform floating-point variation while
	// preserving that truncation for integral millisecond inputs.
	const uint64_t allowed =
			(static_cast<uint64_t>(host_delta) * 103ull) / 100ull;
	return static_cast<uint64_t>(client_delta) <= allowed;
}

uint32_t read_u32_le_lenient(
		const std::vector<uint8_t> &payload, std::size_t offset) {
	uint32_t value = 0;
	for (std::size_t i = 0; i < 4 && offset + i < payload.size(); ++i)
		value |= static_cast<uint32_t>(payload[offset + i]) << (i * 8);
	return value;
}

bool stage_integrity_crc_punt(const ServerDispatchInputs &inputs, NapiNPConnection &conn, const char *reason) {
	// The _PUNT.TXT line first [orig: Server_WritePuntLog @0x50200e / @0x5021da].
	if (inputs.server_ctx != nullptr) server_logs_punt(inputs.server_ctx->logs, conn, reason);
	DisconnectEvent event;
	event.ds = 1;
	event.dc = 2;
	event.dpc = 46;
	event.ddstr = std::string("PUNT ") + reason;
	return Server_StageHostDisconnect(conn, event);
}

} // namespace

namespace {

bool is_default_ash_session_config(const GameConfig &cfg) {
	return cfg.mission_name == "AS - Dormant Volcano Isle" && cfg.mission_file == "ASH_I5A.BMS";
}

// tag=0x57 RTT pong. [orig: NapiNPServerMsg_HandlePingResponse @0x515070 -> NetPacket_WriteInt32AndByte_0
// @0x5070c0]: when a client's C2S 0x2C carries echo_flag != 0, the host bounces the timestamp back as
// S2C 0x57 = [u32 timestamp][u8 0]. (A 0x2C with echo_flag == 0 is the client's return leg — the host
// then computes RTT + runs the min/max-ping kick; no reply.)
std::vector<uint8_t> build_tag57_pong(uint32_t timestamp) {
	std::vector<uint8_t> b;
	io::append_u32_le(b, timestamp);
	b.push_back(0);
	return b;
}

std::vector<uint8_t> build_tag1a_tick(uint32_t now_tick) {
	return {
			static_cast<uint8_t>(now_tick & 0xFFu),
			static_cast<uint8_t>((now_tick >> 8) & 0xFFu),
			static_cast<uint8_t>((now_tick >> 16) & 0xFFu),
			static_cast<uint8_t>((now_tick >> 24) & 0xFFu),
	};
}

// ---------------------------------------------------------------------------
// §5.1 reply bodies — roster / loadout / spawn-confirm (relocated from the retired reply builders).
// ---------------------------------------------------------------------------

// tag=0x16 PLAYER-LIST. [orig: NapiNPClientMsg_PlayerList @0x42FAE0] Enumerates the LIVE player roster — the
// host loopback (slot 0) + each spawned joiner (slot 1+) — so a joining client sees ITS OWN slot and can
// bind its local player (golden: 0x16 grows 31 B [host only] -> 39 B [host + joiner] right before the
// joiner deploys). `roster` is the connection_list; with no in-game player the list carries zero rows,
// which empties every recipient's board [orig: the row loop over an empty pair list @0x50da54..0x50da62;
// the client's unconditional apply @0x42fb46]. The wire SERIALIZE lives in encode_player_list (npwire);
// this is just the inmatch-side roster walk (it reads NapiNPConnection) that builds the entry list.
std::vector<uint8_t> build_reply_tag_16(const GameConfig &config,
	                                    const std::vector<NapiNPConnection> &roster,
                                        world::World *world) {
	PlayerListFrame frame;
	world::MatchLiveScoreboard scoreboard;
	if (world != nullptr) {
		scoreboard = world->match.live_scoreboard(*world);
	} else {
		scoreboard.team_count = game_type::active_team_count(
				config.game_type, config.num_teams);
		scoreboard.team_mode = scoreboard.team_count != 0;
		scoreboard.timed_score_mode =
				config.game_type == game_type::kKingOfTheHill;
	}
	frame.flags = static_cast<uint8_t>(
			(scoreboard.team_mode ? 1u : 0u) |
			(scoreboard.timed_score_mode ? 2u : 0u));
	frame.team_count = scoreboard.team_count;
	// The {computed score, row} pair list retail sorts before serializing.
	// [orig: CPairList_AddEntry @0x50DA26]
	struct ScoredPlayerListEntry {
		int32_t score;
		PlayerListEntry row;
	};
	std::vector<ScoredPlayerListEntry> rows;
	for (const NapiNPConnection &c : roster) {
		if (c.phase < ConnectionPhase::PlayerAdded || !c.link.owned_entity.valid()) continue;
		// Rows carry slots in state 6 exactly with a bound entity: a loading joiner is no row
		// until its burst completes (the golden 31 B -> 39 B grow), a punted slot stays one
		// until its teardown, and the round end's 6 -> 7 store (Match's outcome latch) leaves
		// none; +100579 is never set [orig: Server_BuildAndBroadcastScoreboard @0x50D960 -
		// state 6 @0x50d9f6, +100579 @0x50da73; a punt writes no slot byte,
		// Server_LogCRCMismatchPunt @0x517ED0; Server_ProcessRoundEnd @0x51685E].
		if (!c.burst.spawned || (world != nullptr && world->match.outcome().ended)) continue;
		// The row's team is the slot's own byte (+0x1A0), never its entity's: a script team
		// write to the body leaves it (D-NET-651) [orig: Server_BuildAndBroadcastScoreboard
		// `mov dl, [esi+1A0h]` @0x50DAF2, stored @0x50DAFB; Entity_SetTeamByNetId @0x43C680].
		const uint8_t team = c.assigned_team_valid ? c.assigned_team : uint8_t{0};
		PlayerListEntry row;
		int32_t computed_score = 0;
		row.slot = c.reply.player_slot;
		row.team = team;
		row.spectator = c.link.spectator;
		// The status word goes out only to a recipient whose slot+96481 byte is
		// set, and nothing in JO writes that byte (the 2026-08-30 image-wide
		// sweep, net-re "The slot hide bytes"): every host-sent status word is 0,
		// whatever the row's slot+448 holds.
		// [orig: NetPacket_SerializeScoreboard0x16 @0x504bd4..0x504bde]
		row.status_flags = 0;
		if (world != nullptr) {
			if (const world::MatchPlayer *player =
					world->match.player(c.link.owned_entity)) {
				const int32_t mode_stat = world->match.primary_score(*player);
				const int32_t points =
						player->stats[world::MatchStats::kPoints];
				row.score1 = static_cast<uint16_t>(mode_stat);
				row.score2 = static_cast<uint16_t>(points);
				// The pair-list sort key is the SIGNED computed score, taken before the u16 wire
				// fold: team modes rank by the accumulated points, others by the mode stat
				// (Match::computed_score) [orig: Player_ComputeScore @0x500A80, call @0x50DA10].
				computed_score = world->match.computed_score(*player, scoreboard.team_mode);
			}
		}
		rows.push_back({computed_score, row});
	}
	// Retail scans the slots in numeric order, pairs each row with
	// Player_ComputeScore's value, then shell-sorts the pair list by that value
	// before serializing — the wire row order IS the board order, and the
	// client never re-sorts (client_replica_scoreboard.cpp). Same Knuth-gap
	// descending shell sort as the end-round twin (match.cpp
	// sort_scoreboard_players).
	// [orig: Server_BuildAndBroadcastScoreboard @0x50D960 — Player_ComputeScore
	//  @0x50DA10, CPairList_AddEntry @0x50DA26, CPairList_ShellSortByValue
	//  @0x50DA42 / @0x526CF0]
	std::sort(rows.begin(), rows.end(),
	          [](const ScoredPlayerListEntry &a, const ScoredPlayerListEntry &b) {
	              return a.row.slot < b.row.slot;
	          });
	world::shell_sort_by_value(rows, [](const ScoredPlayerListEntry &e) { return e.score; });
	for (const ScoredPlayerListEntry &entry : rows)
		frame.players.push_back(entry.row);
	frame.teams.resize(size_t(frame.team_count) + 1);
	for (size_t team = 1; team < frame.teams.size(); ++team) {
		const world::MatchLiveTeamScore &source = scoreboard.teams[team];
		frame.teams[team].score1 = static_cast<uint16_t>(source.primary_score);
		frame.teams[team].score2 = static_cast<uint16_t>(source.points);
		frame.teams[team].koth_hold = source.alive_players;
		frame.teams[team].ctf_flag = source.authored_objectives;
	}
	// The in-game count is its own slot walk, not the row count: every active
	// slot (+4) whose +100579 byte is clear, the host's own slot (+5) only on a
	// session peer. +100579 is never set in JO, so a joiner counts from its
	// add and a punted slot until its teardown's row memset; a Serve Only
	// host's own row has no connection here, so the walk is the active test.
	// [orig: Server_BuildAndBroadcastScoreboard @0x50dd5b..0x50dda9 (+4
	//  @0x50dd82, +100579 @0x50dd8b, +5 @0x50dd90..0x50dd9a) -> packet dword
	//  1031, the trailer's first byte @0x504cda; Server_PlayerAdd stores
	//  +100579 = 0 @0x51CD5D; Server_HandlePlayerDisconnect's memset @0x51b87d]
	frame.in_game_count = static_cast<uint8_t>(std::min<size_t>(
			static_cast<size_t>(std::count_if(roster.begin(), roster.end(),
					[](const NapiNPConnection &c) { return player_slot_active(c); })),
			0xFFu));
	// The spectator count rides the row loop: each emitted row whose spectator
	// latch (slot+100567) is set, the same byte the row's bit 0 carries
	// beside the slot team (slot+416) << 1.
	// [orig: @0x50dace..0x50db0c; the row byte (latch & 1) + 2 * team @0x504c30]
	frame.spectator_count = static_cast<uint8_t>(std::min<size_t>(
			std::count_if(rows.begin(), rows.end(),
					[](const ScoredPlayerListEntry &entry) {
						return entry.row.spectator;
					}),
			0xFFu));
	return encode_player_list(frame);
}

} // namespace

// ---------------------------------------------------------------------------
// Dispatch helpers.
// ---------------------------------------------------------------------------

// Build the per-reply PlayerReplicationState from the session config + the connection's binding (the
// retired GameSession::player_replication_state). [orig: the player slot the reply serializers read]
PlayerReplicationState make_rep_state(const GameConfig &cfg, const NapiNPConnection &conn,
                                      const world::World *world) {
	PlayerReplicationState ctx;
	ctx.player_name = cfg.player_name;
	ctx.player_slot = 0;
	ctx.entity_handle = 0;
	ctx.spawn_x = cfg.spawn_x;
	ctx.spawn_y = cfg.spawn_y;
	ctx.spawn_z = cfg.spawn_z;
	ctx.spawn_names = cfg.spawn_names;
	if (ctx.spawn_names.empty() && !is_default_ash_session_config(cfg)) {
		if (!cfg.mission_name.empty()) ctx.spawn_names.push_back(cfg.mission_name);
		else if (!cfg.mission_file.empty()) ctx.spawn_names.push_back(cfg.mission_file);
	}
	// D-NET-132 / §6.9: the wire handle is read THROUGH link.owned_entity (the single binding); the
	// field-0x0004 team is the slot's own byte, 0 for an inactive slot (D-NET-651) [orig:
	// NetPacket_SerializePlayerSync0x46 - slot+4 @0x5060B9, `mov cl, [ebx+1A0h]` @0x5060D1].
	// player_slot / player_name stay on conn.reply (roster order + the echoed ClientAuth.NA). A
	// World-less bind (bind_session_reply_player) stamps owned_entity with the bare wire handle.
	if (conn.link.owned_entity.valid()) {
		ctx.team = player_slot_active(conn) && conn.assigned_team_valid ? conn.assigned_team : 0;
		ctx.player_name = conn.reply.player_name;
		ctx.player_slot = conn.reply.player_slot;
		ctx.entity_handle = conn.link.owned_entity.packed;
		ctx.quality = conn.reply.client_quality;
		ctx.account_pcid = conn.account.pcid;         // field 0x0010 [orig: player+0x250 @0x506070]
		ctx.account_squad_id = conn.account.squad_id; // field 0x0800 [orig: player+0x270 @0x506257]
		ctx.squad_leader = conn.squad_leader; // +100576 (field 0x0040)
		ctx.fireteam = conn.fireteam;         // +100577 (field 0x0080)
		// Field 0x1000: an active slot (+4) whose spectator latch (+100567) is
		// set; the +100579 test is dead (never set in JO), so a loading or punted
		// spectator sends 1 [orig: NetPacket_SerializePlayerSync0x46 @0x506199..0x5061AF
		//  (+100579 @0x5061A8), the 1 @0x5061bc; the latch @0x51CD7D..0x51CD83].
		ctx.spectator_in_game = conn.link.spectator && player_slot_active(conn) ? 1 : 0;
		if (world != nullptr) {
			if (const world::Entity *e = world->registry.get(conn.link.owned_entity)) {
				// Field 0x0008 is present only when the requester asks for it.
				// Its value is nonzero only for a dead player whose automatic
				// preference or explicit request exposes the revive window.
				// [orig: NetPacket_SerializePlayerSync0x46 @0x505E80]
				if ((e->flags & world::kEntityFlagDead) != 0u &&
						(conn.link.auto_medic_enabled ||
						 conn.link.medic_request_active)) {
					ctx.downed_state = static_cast<uint8_t>(
							(conn.link.downed_revive_seconds & 0x7Fu) |
							(conn.link.medic_request_active ? 0x80u : 0u));
				}
			}
		}
	}
	// The recipient's deploy-map owned-zone mask (0x0F variant-0 u32) from the live chain: the
	// walk runs unconditionally, so a chain-less world sends its 0.
	// [orig: ZoneSlotChain_GetOwnedZoneMask @0x4a2620 per recipient team @0x4ff9a3; net-re §5.61]
	if (world != nullptr)
		ctx.uniform_team_mask =
				world->zones.owned_zone_mask(ctx.team);
	return ctx;
}

namespace {

// S2C 0x75 is the requesting player's live slot state, not a fixed
// "spectator flags" fixture. Byte 0 is the spectator/death-screen bit and byte
// 1 is playerSlot+416 (team), the slot's own byte whatever its body's team
// (D-NET-651), written straight into the retail client's g_LocalTeam loadout
// selector. [orig: NetPacket_SerializeHostEntityState (ex sub_510890) @0x510890
// - `mov dl, [eax+1A0h]` @0x5108D0; NapiNPClientMsg_SetSpectatorMode @0x4259E0]
std::vector<uint8_t> build_tag75_player_state(const NapiNPConnection &conn) {
	const uint8_t team = conn.assigned_team_valid ? conn.assigned_team : 0;
	return {static_cast<uint8_t>(conn.link.spectator ? 0x01u : 0x00u), team};
}

// Build a 0x46 player-sync for a SPECIFIC roster slot (the one the client's C2S 0x22 requests). Finds
// the connection bound to that slot and serializes its real entity/team/name; falls back to `rep` (the
// requesting connection's own binding) when no other player owns the slot. [orig: NapiNPServerMsg_0x022
// @0x514C90 serializes the requested playerSlot from dword_A87048]
PlayerReplicationState rep_for_slot(const GameConfig &config,
                                    const std::vector<NapiNPConnection> &roster, uint8_t slot,
                                    const PlayerReplicationState &fallback, const world::World *world) {
	for (const NapiNPConnection &c : roster) {
		if (c.phase < ConnectionPhase::PlayerAdded || !c.link.owned_entity.valid()) continue;
		if (c.reply.player_slot == slot) return make_rep_state(config, c, world);
	}
	return fallback;
}

// tag=0x0C C2S player-input uplink — cache the joiner's pre-spawn pose. [orig: NapiNPServerMsg_0x00C
// @0x501c30 stores the uplink into the player slot.]
void cache_client_pose(const std::vector<uint8_t> &payload, SessionReplyState &st) {
	EntityPacketSubHeader hdr;
	size_t header_consumed = 0;
	if (decode_entity_packet_sub_header(payload.data(), payload.size(), hdr, header_consumed) &&
	    hdr.sub_op == ENTITY_SUB_OP_EXTENDED) {
		PlayerExtendedUplink uplink;
		size_t body_consumed = 0;
		if (decode_player_extended_uplink(payload.data() + header_consumed,
		                                  payload.size() - header_consumed, uplink, body_consumed) &&
		    header_consumed + body_consumed == payload.size()) {
			PreSpawnJoinerPose &pose = st.pre_spawn_pose;
			pose.entity_handle = hdr.handle;
			pose.item_type_id = hdr.item_type_id;
			pose.carrier_handle = uplink.carrier_handle;
			pose.pos_x = static_cast<uint32_t>(uplink.pos_x);
			pose.pos_y = static_cast<uint32_t>(uplink.pos_y);
			pose.pos_z = static_cast<uint32_t>(uplink.pos_z);
			pose.heading = uplink.heading;
			pose.pitch = uplink.pitch;
			pose.valid = true;
		}
	}
}

} // namespace

bool is_medic_recipient(const NapiNPConnection &candidate,
		const world::World &world, int32_t team_filter) {
	// The mask-0x580 group: bit 0x80 admits player-slot state 6/7 (deployed /
	// round ended — the in-match state; the ordinary death path never rewrites
	// slot+0x20), bit 0x100 compares the slot's own team byte (zero-extended)
	// with the filter dword, bit 0x400 the Medic class attribute of the slot's
	// entity. No leg reads entity health or the Flags dead bit, so a dead or
	// respawn-pending same-team Medic still receives the record.
	// [orig: NapiNPServer_SendFiltered @0x4C87E0 — roster gate @0x4C889F,
	//  0x80 @0x4C8948..0x4C8953, 0x100 `movzx eax, [esi+1A0h]` @0x4C896A..0x4C8977,
	//  0x400 @0x4C8990..0x4C89A8]
	if (!is_in_match(candidate) || candidate.link.transport == nullptr ||
			!candidate.link.owned_entity.valid() ||
			static_cast<int32_t>(candidate.assigned_team) != team_filter)
		return false;
	const world::Entity *medic = world.registry.get(candidate.link.owned_entity);
	return medic != nullptr &&
			world.tables.class_has_attribute(medic->player_class,
					world::MissionTables::kCharAttrMedic);
}

std::vector<ProtocolMessage> dispatch_session_replies(const GameConfig &config,
                                                      NapiNPConnection &conn,
                                                      const std::vector<ProtocolMessage> &messages,
                                                      uint32_t now_tick,
                                                      std::vector<NapiNPConnection> &roster,
                                                      world::World *world,
                                                      const ServerDispatchInputs &inputs) {
	std::vector<ProtocolMessage> replies;
	// A staged high-table disconnect is terminal even though the owner retains
	// the node briefly to flush that reliable record. Do not let a retransmitted
	// session packet queue ordinary traffic during that flush window.
	if (conn.host_disconnect_sent) return replies;
	SessionReplyState &st = conn.reply;
	const PlayerReplicationState rep = make_rep_state(config, conn, world);
	const IntegritySources &integrity = config.integrity_sources;

	// The loadout gate (burst phase 7→8) is opened ONLY by the C2S 0x2F handler (case 0x2F below).
	// A prior auto-advance ("any C2S at phase 8") fired on stale handshake messages before the
	// client had processed 0x1A, defeating the pacing. The loopback (type-2) bypasses the gate.

	if (conn.admission_stage != GameAdmissionStage::Complete)
		return dispatch_admission(config, conn, messages, now_tick, roster, world, inputs);

	for (const ProtocolMessage &msg : messages) {
		// Protocol-control + settings-update frames are not gameplay messages (the original filters
		// these before the gameplay dispatch). [orig: dispatch_in_match_session_messages gate]
		if (msg.flags.settings_update || msg.full_tag >= PROTOCOL_FULL_TAG_HIGH_BASE) continue;

		switch (msg.tag) {
			case c2s::JOIN: // JOIN ack [orig: NapiNPServer_HandlePlayerJoinMessage @0x512AA0]
				dispatch_join(conn, msg, replies);
				break;
			case c2s::AUTO_MEDIC_PREFERENCE: {
				// One inverse checkbox dword copied to playerSlot+372: zero is
				// automatic requests enabled, any nonzero value is manual. A body
				// shorter than the dword stores 0 (automatic) rather than leaving
				// the slot untouched; trailing bytes are ignored.
				// [orig: NapiNPServerMsg_AutoMedicPreference @0x501BE0 (short
				// body -> 0 @0x501C16, dword copy @0x501C1F);
				// OPTIONS_AUTOMEDIC @0x5549E7/@0x554E40]
				AutoMedicPreference preference;
				size_t consumed = 0;
				conn.link.auto_medic_enabled = !decode_auto_medic_preference(
						msg.payload.data(), msg.payload.size(),
						preference, consumed) || preference.enabled();
				break;
			}
			case c2s::MEDIC_REQUEST: { // [orig: Server_BroadcastMedicRequest @0x515390]
				std::vector<ProtocolMessage> own = Server_HandleMedicRequest(
						inputs.medic_request_format, conn, roster, world);
				for (ProtocolMessage &m : own) replies.push_back(std::move(m));
				break;
			}
			case c2s::EMOTE_REQUEST: { // [orig: NapiNPServerMsg_HandleEmoteRequest @0x501E00]
				if (world == nullptr) break;
				EmoteRequest request;
				decode_emote_request(msg.payload.data(), msg.payload.size(), request);
				for (ProtocolMessage &m : Server_HandleEmoteRequest(conn, request, roster, *world))
					replies.push_back(std::move(m));
				break;
			}
			case c2s::DOOR_SLOT_REQUEST: // [orig: NapiNPServerMsg_HandleDoorSlotRequest @0x514B20]
				if (world == nullptr || inputs.server_ctx == nullptr || !inputs.server_ctx->is_authority) break;
				for (ProtocolMessage &m : Server_HandleDoorRowRequest(conn, msg.payload, *world))
					replies.push_back(std::move(m));
				break;
			case c2s::TEAM_CHANGE_REQUEST: // [orig: NapiNPServerMsg_0x04D_ChangeTeam @0x518F70]
				dispatch_team_change_request(conn, now_tick, world, inputs);
				break;
			case c2s::DEATH_TIMEOUT_RESET: // [orig: NapiNPServerMsg_DeathTimeoutReset @0x510540]
				dispatch_death_timeout_reset(conn, world, inputs);
				break;
			case c2s::RADIO_CALL_REQUEST: { // [orig: NapiNPServerMsg_HandleRadioCall @0x514330]
				if (world == nullptr) break;
				RadioCallRequest request;
				decode_radio_call_request(msg.payload.data(), msg.payload.size(), request);
				for (ProtocolMessage &m : Server_HandleRadioCall(inputs.server_ctx, conn, request,
						roster, *world))
					replies.push_back(std::move(m));
				break;
			}
			case c2s::CHARATTR_CRC_REPLY:
				// The host discards the returned checksum and only clears this
				// player's silence counter. [orig: NapiNPServerMsg_AnimChecksumRequest
				// @0x501D40, discard @0x501D71, reset @0x501D79]
				st.charattr_unanswered_count = 0;
				break;
			case c2s::TIME_SYNC_REPLY:
				// Any dispatched 0x08 clears the silence counter. Retail performs
				// that store after reading the two dwords but before testing the
				// tracking latch, sequence, or timing; malformed and stale replies
				// therefore count as contact even though they cannot advance the
				// sequence cycle. A matching first sample installs the client
				// baseline/previous stamp. A later one closes the round only if both
				// its round and baseline deltas fit within 103% of host time, yielding
				// S2C 0x43 values 1,1,2,2...
				// [orig: NapiNPServerMsg_ValidateTimeSync @0x502210, reset @0x502273]
				st.time_sync_unanswered_count = 0;
				if (msg.payload.size() == 8 &&
						io::read_u32_le(msg.payload.data()) == st.time_sync_sequence &&
						st.time_sync_round_host_ms != 0) {
					const uint32_t client_ms =
							io::read_u32_le(msg.payload.data() + 4);
					if (st.time_sync_client_baseline_ms == 0)
						st.time_sync_client_baseline_ms = client_ms;
					if (st.time_sync_previous_client_ms == 0) {
						st.time_sync_previous_client_ms = client_ms;
					} else {
						st.time_sync_latest_client_ms = client_ms;
						st.time_sync_current_host_ms =
								io::host_milliseconds_for_logic_tick(now_tick);
						const bool round_ok = client_delta_fits_retail_time_window(
								client_ms - st.time_sync_previous_client_ms,
								st.time_sync_current_host_ms -
										st.time_sync_round_host_ms);
						const bool baseline_ok = client_delta_fits_retail_time_window(
								client_ms - st.time_sync_client_baseline_ms,
								st.time_sync_current_host_ms -
										st.time_sync_host_baseline_ms);
						if (round_ok && baseline_ok) {
							st.time_sync_previous_client_ms = 0;
							st.time_sync_latest_client_ms = 0;
							st.time_sync_round_host_ms = 0;
							st.time_sync_current_host_ms = 0;
						}
					}
				}
				break;
			case c2s::ENTITY_CHECKSUM_REPLY: {
				// C2S 0x20 is [u8 id][u32 checksum]. Retail's packet reads
				// are lenient (missing bytes are zero) and ignore everything
				// after byte 4. Id 0xFF selects the host's own whole weapon.def
				// table CRC; any other id one row's (0 for a row with no name).
				// The sources are the host's own loaded tables, computed as
				// retail computes them; a source they hold no value for (an
				// uncomputed table, a named row) is silence, never an invented
				// CRC-zero source.
				// [orig: Server_ValidateWeaponCRC @0x501F70]
				const uint8_t id = msg.payload.empty() ? 0 : msg.payload[0];
				const uint32_t received = read_u32_le_lenient(msg.payload, 1);
				uint32_t source = 0;
				if (id == 0xFF) {
					if (!integrity.weapon_table_known) break;
					source = integrity.weapon_table_crc;
				} else if (!integrity.weapon_row_crc(id, source)) {
					break;
				}
				if (!weapon_integrity_reply_matches(
							id, source, st.integrity_weapon_crc_salt, received) &&
						stage_integrity_crc_punt(inputs, conn, "WCRC"))
					return {};
				break;
			}
			case c2s::CHECKSUM_REPLY: {
				// C2S 0x21 shares the lenient five-byte prefix, but casts its
				// index to signed char before the live AmmoDef-capacity gate.
				// The retail client commonly appends the echoed four-byte request
				// key; the host consumes only index+CRC and ignores that tail.
				// [orig: NapiNPServerMsg_HandleAntiCheatCRCCheck (0x21) @0x502050]
				const uint8_t raw_index =
						msg.payload.empty() ? 0 : msg.payload[0];
				uint32_t source = 0;
				if (integrity.ammo_row(raw_index, source) !=
						IntegritySources::AmmoRow::Crc)
					break;
				const uint32_t received = read_u32_le_lenient(msg.payload, 1);
				if ((source ^ st.integrity_ammo_crc_salt) != received &&
						stage_integrity_crc_punt(inputs, conn, "ACRC"))
					return {};
				break;
			}
			case c2s::JOIN_FORM_POST: // handshake push -> S2C 0x02 GLB_JOIN push [orig: NapiNPServerMsg_HandleJoinRequest @0x512ED0]
				dispatch_join_form_post(now_tick, replies);
				break;
			case c2s::JOIN_PADDING_ECHO: // duplicate GLB_JOIN reply after admission
				dispatch_join_padding_echo(config, conn, now_tick, roster, world, inputs, replies);
				break;
			case c2s::PLAYER_SYNC_REQUEST: { // player-sync request [u8 slot][u16 fieldFlags] -> S2C 0x46 player-sync.
				// [orig: NapiNPServerMsg_0x022 @0x514C90; §5.33] The client requests a SPECIFIC slot
				// (body byte 0) + a fieldFlags word (bytes 1-2); an added requester asking for an
				// in-table slot gets the 0x46 for THAT slot, so the joiner binds it. The ACK-WALK is
				// CLIENT-driven: the server ECHOES bit 0x4000 from the request into the reply
				// (NetPacket_SerializePlayerSync0x46 @0x505f07 `adjusted |= 0x4000`), and the
				// CLIENT, on seeing the echoed ack, re-requests slot+1 with fieldFlags 0x5CF7 while
				// slot < its max-player count g_MaxPlayerSlots (NapiNPClientMsg_PlayerSync @0x43175a),
				// so its final request is slot == the 0x04 capacity byte, which the capacity gate
				// below leaves unanswered — the server never terminates the walk. An in-table
				// inactive slot / NULL entity answers the 0x8000 removal record (@0x505ecb..0x505ee0).
				// Without the walk the joiner only ever binds its OWN slot, leaving other players (the
				// host's serve-and-play player) unbound (residual 0x0F flood grill 2026-07-01).
				// The requester must be an added player: retail reads the connection's session
				// player (conn+0x160) and its server player slot (+0xC0) and sends nothing when
				// either is null [orig: @0x514ca1..0x514cb8]; our analogue of the bound slot is the
				// PlayerAdded phase (the 0x0F query's gate below).
				if (conn.phase < ConnectionPhase::PlayerAdded) break;
				// Lenient reads: a missing slot byte reads 0, and the flags word reads only when
				// both of its bytes follow the slot byte, else 0 [orig: @0x514cca..0x514ce7].
				const uint8_t req_slot = msg.payload.empty() ? 0 : msg.payload[0];
				const uint16_t req_flags =
						msg.payload.size() >= 3
								? static_cast<uint16_t>(msg.payload[1] | (msg.payload[2] << 8))
								: 0;
				// A slot at or past the slot table sends nothing at all, removal record included
				// [orig: `cmp eax, g_PlayerSlotCapacity` @0x514cf1, `jge` @0x514cf7]. The retail
				// walk's inclusive final request (slot == the 0x04 capacity byte) lands here.
				if (req_slot >= config.total_player_slot_capacity()) break;
				bool slot_has_player = false;
				for (const NapiNPConnection &c : roster) {
					if (c.phase >= ConnectionPhase::PlayerAdded && c.link.owned_entity.valid() &&
					    c.reply.player_slot == req_slot) {
						slot_has_player = true;
						break;
					}
				}
				// An active slot answers the request mask minus bit 0x8000, the ack bit riding
				// along; there is no default field set, so a zero or ack-only mask answers the
				// header and the pool index alone [orig: NetPacket_SerializePlayerSync0x46
				// @0x505E80 - `fieldFlags & 0x7FFF` @0x505efb, the ack OR @0x505f07]. The retail
				// client stops the walk itself at its max-player count g_MaxPlayerSlots (fed by
				// our 0x04 slot-config byte 18).
				const bool echo_ack = (req_flags & kPlayerSyncAck) != 0;
				const uint16_t reply_flags = static_cast<uint16_t>(req_flags & ~kPlayerSyncRemoval);
				if (slot_has_player) {
					const PlayerReplicationState prs = rep_for_slot(config, roster, req_slot, rep, world);
					replies.push_back(
							make_protocol_message(s2c::PLAYER_SYNC, encode_player_sync(prs, reply_flags)));
				} else {
					// An in-table empty or disconnected slot: the 0x8000 removal record (|0x4000
					// when the request asked for the ack) [orig: @0x505ecb..0x505ee0, @0x505f37].
					replies.push_back(
							make_protocol_message(s2c::PLAYER_SYNC, encode_player_sync_removal(req_slot, echo_ack)));
				}
				break;
			}
			case c2s::PING: // re-broadcast entity-state request -> 0x75 [orig: handler @0x510ED0]
				replies.push_back(make_protocol_message(
						s2c::SPECTATOR_FLAGS,
						build_tag75_player_state(conn)));
				break;
			case c2s::FILE_CHUNK_REQUEST: // server-info request -> 0x60 chunk [orig: NapiNPServerMsg_HandleReplayRequest (0x33) @0x515230]
				dispatch_file_chunk_request(config, msg, inputs, replies);
				break;
			case c2s::MISSION_CHUNK_REQUEST: // mission-file request -> 0x64 chunk only [orig: NapiNPServerMsg_0x037_SendCircularBuffer @0x5152E0]
				dispatch_mission_chunk_request(config, conn, msg, now_tick, roster, world, inputs, replies);
				break;
			case c2s::LOADOUT_SUBMIT: { // loadout request -> 0x5A [orig: NapiNPServerMsg_HandlePlayerLoadout (0x2F) @0x515790]
				// Golden (f317-318): C 0x2F -> S 0x5A + game-start bundle. The 0x5A populates
				// the joiner's weapon slots BEFORE the first 0x0A; without it every 0x0A triggers
				// the weapon-slot mismatch -> C2S 0x0F flood. Also unlatches the burst's phase 8
				// gate so the game-start bundle emits on the next tick. The reply derives from
				// THIS request + the armory table when the host fed one (see
				// grant_weapon_loadout; D-NET-141).
				LoadoutSubmit req;
				const world::WeaponTable *armory =
						(world != nullptr && !world->tables.weapons.empty()) ? &world->tables.weapons : nullptr;
				world::Entity *loadout_player = (world != nullptr && conn.link.owned_entity.valid())
						? world->registry.get(conn.link.owned_entity)
						: nullptr;
				// A slot with no entity, or a spectator's, is answered with nothing, ahead of
				// the armory window, the parse and every list send (D-NET-678).
				// [orig: @0x5157E1..0x5157E5 (slot[0]), @0x5157EB..0x5157F2 (+0x188D7)]
				if (loadout_player == nullptr || conn.link.spectator) break;
				// The armory-window gate runs BEFORE the body is parsed: a joined (state-10)
				// slot whose pre-round latch is clear, whose entity is neither DEAD (0x2)
				// nor inside an ARMORY zone (0x400000), and no pre-round timer, is answered
				// with its CURRENT slot list and nothing is read or written. A dead player
				// re-arms from the death screen; an alive one only from an armory volume;
				// the pre-round window and the first post-join/deploy submit (the latch)
				// bypass it. [orig: NapiNPServerMsg_HandlePlayerLoadout @0x51581c..0x51583e
				//  -> Server_SendWeaponSlotListToPlayer]
				if (is_in_match(conn) && !conn.link.preround_loadout_latch &&
				    ((loadout_player->flags | loadout_player->engine_flags) &
				     (world::kEntityFlagDead | world::kEntityFlagArmoryZone)) == 0u &&
				    world->preround_delay_seconds == 0) {
					replies.push_back(make_protocol_message(0x5A, current_weapon_slot_list(conn, world)));
					break;
				}
				// The decoder owns the framing contract: header, whole 4-byte entries, and one
				// terminal 0xFF. Trailing bytes after the terminator are accepted exactly as
				// retail accepts them (no end check after the 0xFF exit @0x515a99); only an
				// unterminated list is ignored — the crash-safe stand-in for retail's
				// zero-fill infinite loop — without opening the phase-8 gate or disturbing
				// the last valid grant.
				if (!decode_loadout_submit(msg.payload.data(), msg.payload.size(), req)) break;
				// The envelope is validated BEFORE anything is applied: a bad team or a
				// nonzero out-of-range class aborts with a re-send of the player's CURRENT
				// slot list and NO state write — no class stamp, no damage-class table, no
				// phase-8 gate, no retained body [orig: @0x5158a9 / @0x515fa5, both
				// `return Server_SendWeaponSlotListToPlayer(player)`].
				if (!loadout_envelope_accepted(req, config.game_type)) {
					replies.push_back(make_protocol_message(0x5A, current_weapon_slot_list(conn, world)));
					break;
				}
				// The armory-reuse cooldown: a nonzero soldier type is accepted only while
				// playerSlot+356 has expired or the pre-round timer runs; otherwise the same
				// current-list re-send. Class 0 skips this test. The per-tick decrement is
				// tick_respawn_holds. [orig: @0x5158d0 -> @0x515fa5]
				if (req.player_class != 0 && conn.link.armory_reuse_seconds > 0 &&
				    world->preround_delay_seconds == 0) {
					replies.push_back(make_protocol_message(0x5A, current_weapon_slot_list(conn, world)));
					break;
				}
				const GrantedWeaponLoadout grant =
						grant_weapon_loadout(req, config.class_allow_mask, armory, conn,
								world::entity_ammo_seat(*world, *loadout_player),
								world->rules.sniper_zoom_fill);
				// The accepted entries become the slot's loadout buffer, which every
				// deploy rebuilds the kit from (D-NET-378) [orig: @0x515CBD..0x515D83].
				st.loadout_buffer = req;
				st.loadout_buffer.player_class = grant.reply.avatar_class;
				st.loadout_buffer_set = true;
				// The accepted class is the slot's class and its PlayerType, at its store and its
				// list send [orig: the slot store @0x515AA8; @0x515AE2; the
				// Server_SendWeaponSlotListToPlayer call @0x515F95].
				conn.slot_class = grant.reply.avatar_class;
				conn.player_list_type = grant.reply.avatar_class;
				write_ammo_damage_classes(conn, grant);
				apply_granted_loadout(conn, grant, armory != nullptr);
				if (armory != nullptr) {
					// The accept points the entity's EquippedSlot at the submitted slot and
					// clears it when that slot holds no def or a NoSelect one (flags2 bit 0).
					// [orig: NapiNPServerMsg_HandlePlayerLoadout - the store @0x515F52..0x515F62,
					//  the slot / def tests @0x515F73..0x515F7C, `and ecx, 1` @0x515F81 whose
					//  zero skips the clear @0x515F88, the clear @0x515F8A]
					conn.equipped_slot = {};
					const auto submitted = req.weapon_slot_index <= 0xFFFFu
							? conn.weapon_slots.find(static_cast<uint16_t>(req.weapon_slot_index))
							: conn.weapon_slots.end();
					const world::WeaponTableEntry *submitted_def = submitted != conn.weapon_slots.end()
							? armory->by_index(submitted->second.adm_index)
							: nullptr;
					if (submitted_def != nullptr &&
							(submitted_def->flags2 & world::weapon_flag2::kNoSelect) == 0) {
						conn.equipped_slot.kind = NapiNPConnection::EquippedSlotRef::kPersonal;
						conn.equipped_slot.combo = submitted->first;
					}
				}
				replies.push_back(make_protocol_message(s2c::WEAPON_LOADOUT, st.last_loadout_reply));
				// Accepted soldier type -> entity+660 playerClass [orig: @0x515ab0] — feeds the
				// §5.10 field-17 class nibble and the 0x0C/0x18 spawn records for this player.
				loadout_player->player_class = grant.reply.avatar_class;
				loadout_player->carry_flags =
						(loadout_player->carry_flags & ~0x18u) | grant.carry_flags;
				// The impact damage's copy of the slot's damage-class bytes (D-NET-508).
				loadout_player->ammo_damage_class.assign(conn.ammo_damage_classes.begin(),
						conn.ammo_damage_classes.end());
				// The accept re-arms the armory cooldown unless the pre-round latch is
				// set, then clears the latch, class 0 included. [orig: @0x515b96..0x515bb3,
				//  `if (!(byte & 2)) slot[89] = armoryReuseTime_A38; byte &= 0xFD`]
				if (!conn.link.preround_loadout_latch)
					conn.link.armory_reuse_seconds = static_cast<int32_t>(config.armory_reuse_time);
				conn.link.preround_loadout_latch = false;
				st.loadout_synced = true;
				conn.burst.loadout_received = true;
				// Every accepted loadout re-stamps every player's live kit weight
				// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515f9d ->
				//  Server_RecalculateAllPlayerScores @0x5014E0].
				Server_RecalculateAllPlayerKitWeights(roster, *world);
				break;
			}
			case c2s::SPAWN_MENU_REQUEST: // spawn-menu request [orig: NapiNPServerMsg_HandlePlayerSpawnRequest @0x513260]
				// THE world-stream unlock (D-NET-150): the handler sets game state 9, advances
				// the session's sync state to 4 and RESTARTS the world-stream phase machine —
				// the §5.2a world stream never starts until the client asks for the spawn menu.
				// (Golden retail-ashi5a: S 0x11 f=201572 -> C 0x0A f=201573 -> first S 0x10
				// f=201713.) Streaming without waiting raced a COLD retail client's mission
				// build and its 0x0C self-bind hit a half-built character registry (the DBuggy
				// blob shadow + missing FP arms). Pre-spawn only in the reimpl: retail re-runs
				// the full world stream on EVERY spawn-menu visit (state 5 -> 4 + phase reset);
				// our tick loop skips spawned connections, so the mid-game re-stream stays an
				// open divergence (see the D-NET-150 record).
				if (!conn.burst.spawned && conn.burst.sync_state != 0) {
					conn.burst.game_state = 9;              // [orig: @0x513295]
					if (conn.burst.sync_state != 4)
						conn.burst.sync_state = 4;          // [orig: @0x5132a0/@0x5132b1]
					conn.burst.world_stream_phase = InitialStateBurst::kStreamInit; // [orig: @0x5132f6]
				}
				// Golden (f161-162): C 0x0A (empty) -> S 0x19 (4 B tick) ONLY. The prior
				// emit_roster sent 0x46/0x16/0x19/0x1A — the unsolicited 0x1A re-sets
				// g_ClientDeployHold (the deploy gate) via NapiClient_WaitForGameStart @0x42cc10,
				// re-blocking deploy after 0x0F cleared it. The 0x16 pushes are proactive
				// (post-handshake + periodic during world-stream), not reactive to 0x0A.
				// The body is the host's GetTickCount, the 0x28 window's floor.
				// [orig: NetPacket_WriteTimestampB @0x5046f0 -> S2C 0x19 @0x5132f1]
				replies.push_back(make_protocol_message(s2c::SPAWN_ACK_TIMESTAMP,
						build_tag1a_tick(io::host_milliseconds_for_logic_tick(
								world != nullptr ? world->logic_tick : now_tick))));
				break;
			case c2s::TEAM_SPAWN_ACK: { // team/spawn ack [u16 team_change_index] — NO reply on a plain join.
				// [orig: NapiNPServerMsg_0x029 @0x514F10] replies S2C 0x51 ONLY when the index
				// resolves to an entity in g_TeamChangeEntityList @0xC947C8 (gated
				// !g_NetSpawnSuspended && !g_SpawnSuccessGate), and that reply is a REAL
				// NetPacket_WriteEntityPacket @0x506BB0 record. The golden retail-ashi5a session's
				// deploy-time C 0x29 draws NO 0x51 anywhere. The client FIELD-PARSES 0x51 —
				// NapiNPClientMsg_HandlePlayerSpawn @0x431BB0 stamps team (+354) and NetId
				// (@0x431cad) and REBINDS CharacterEntity (@0x431cf3) — so the former
				// unconditional zero-id 0x51 "spawn-confirm" re-bound the joiner's own player
				// to a vehicle archetype: the retail-join DBuggy1 shadow (D-NET-148). The
				// gates: the authority (@0x514F11), the round-over latch (@0x514F2B; its
				// reachable analog is the Match outcome, g_NetSpawnSuspended is unmodeled)
				// and the sender's player slot (@0x514F38..0x514F53; the 0xA0 mask also
				// needs it in the match). A short body reads index 0 (@0x514F5F..0x514F6A).
				if (inputs.server_ctx == nullptr || world == nullptr ||
						inputs.server_ctx->is_authority == 0 || world->match.outcome().ended ||
						!is_in_match(conn))
					break;
				const uint16_t index = msg.payload.size() >= 2
						? static_cast<uint16_t>(msg.payload[0] | (msg.payload[1] << 8))
						: uint16_t{0};
				const std::vector<world::EntityHandle> &changed =
						inputs.server_ctx->team_change_entities;
				// CBufferList_GetAtIndex @0x514F7C and the null entry @0x514F81..0x514F8B.
				if (index >= changed.size()) break;
				const world::Entity *entity = world->registry.get(changed[index]);
				if (entity == nullptr) break;
				TeamAssign assign;
				assign.entity_handle = entity->handle.packed;
				assign.team = entity->team;
				if ((entity->flags & world::kEntityFlagPlayer) != 0) {
					assign.net_id = entity->minimap_net_id;
					assign.anim_slot = entity->anim_slot;
				}
				// Class 1 to the sender only (mask 0xA0) @0x514FAA..0x514FCB.
				replies.push_back(make_protocol_message(s2c::TEAM_CHANGE_CONFIRM,
						encode_team_change_confirm(index, assign)));
				break;
			}
			case c2s::RESPAWN_REQUEST: // RESPAWN/DEPLOY request [i16 spawnHandle] — the deploy-map pick.
				dispatch_respawn_request(config, conn, msg, world, inputs, replies);
				break;
			case c2s::ENTITY_UPLINK:
				// C2S player-input uplink. Pre-spawn it only caches the joiner's pose for
				// the spawn; once the connection owns a player it is read-applied HERE, in
				// wire order with the other gameplay messages of this datagram — retail's
				// handler resolves the owner ctx and invokes the entity's read-apply
				// callback inside the dispatch walk, so a `[0x0C pose][0x26 attach]`
				// datagram attaches against the new pose, never a tick-old one.
				// [orig: NapiNPServerMsg_0x00C @0x501C30 -> NetPacket_DispatchEntityPacketCallback
				//  @0x4D6A80 (mode-4 read-apply, owner gate @0x4D6B08)]
				cache_client_pose(msg.payload, st);
				if (world != nullptr && conn.burst.spawned)
					replication::apply_connection_uplink(*world, conn.link, msg.payload);
				break;
			case c2s::MOUNTED_WEAPON_SLOT_SELECT: {
				// Action 6 on a designated-G attached EWeap selects which live
				// MountSlot the player borrows. The authored G bit is only the
				// capability gate; MountSlot+0x5e bit 8 is mutable route state.
				// [orig: NapiNPServerMsg_HandleWeaponToggle @0x511a70]
				if (world == nullptr || !conn.burst.spawned ||
						!conn.link.owned_entity.valid())
					break;
				MountedWeaponSlotSelection selection;
				size_t consumed = 0;
				if (!decode_mounted_weapon_slot_selection(
							msg.payload.data(), msg.payload.size(),
							selection, consumed))
					break;
				world::Entity *player =
						world->registry.get(conn.link.owned_entity);
				if (player == nullptr || !player->use_gun_slot_swapped ||
						!player->mount_target.valid())
					break;
				world::Entity *mount = world->registry.get(player->mount_target);
				if (mount == nullptr || !mount->has_item_def ||
						mount->item_type == 1u ||
						(mount->item_attrib & world::kItemAttribEweap) == 0u ||
						(mount->emplacement_attachment_flags & 0x02u) == 0u ||
						!world->vehicles.prepare_weapon_slot(*mount))
					break;

				if (!selection.use_parent_slot) {
					mount->primary_weapon_slot.redirect_to_parent_slot = false;
					player->equipped_adm_index = mount->primary_weapon_slot_adm;
					break;
				}

				// Retail follows groundEntity (+0x28), then requires a type-1
				// EWeap ItemDef before setting the route bit. The generation
				// sidecar rejects a recycled packed-handle lifetime.
				if (!mount->ground_target.valid() ||
						mount->emplacement_parent != mount->ground_target ||
						mount->emplacement_parent_spawn_id == 0)
					break;
				world::Entity *parent = world->registry.get(mount->ground_target);
				if (parent == nullptr ||
						parent->registry_spawn_id !=
								mount->emplacement_parent_spawn_id ||
						!parent->has_item_def || parent->item_type != 1u ||
						(parent->item_attrib & world::kItemAttribEweap) == 0u ||
						!world->vehicles.prepare_weapon_slot(*parent))
					break;
				mount->primary_weapon_slot.redirect_to_parent_slot = true;
				player->equipped_adm_index = parent->primary_weapon_slot_adm;
				break;
			}
			case c2s::STANCE_CHANGE: { // STANCE CHANGE [i16 stanceCode] — the crouch/prone replication leg.
				// [orig: NapiNPServerMsg_HandleStanceChange @0x501C60 — authority-gated; the
				// SENDER connection's player entity (conn+352 -> +192 -> entity, the same
				// chain as the vehicle attach); code 169 -> MoveOrder = (MoveOrder & ~0x300)
				// | 0x200 (crouch), 170 -> | 0x100 (prone), 172 -> & ~0x300 (stand). The
				// codes are the stance-TRANSITION anim-state ids. Feeds the body-anim
				// selection's stance bases (11/19) and the recipient's own 0x0A tail echo.]
				if (world == nullptr || !conn.burst.spawned || !conn.link.owned_entity.valid())
					break;
				world::Entity *pe = world->registry.get(conn.link.owned_entity);
				if (pe == nullptr) break;
				const int16_t code = msg.payload.size() >= 2
						? static_cast<int16_t>(msg.payload[0] | (msg.payload[1] << 8))
						: 0; // short body reads 0 [orig: @0x501ca8]
				if (code == 169) pe->net_stance_bits = 2;      // crouch (0x200) [orig: @0x501d01]
				else if (code == 170) pe->net_stance_bits = 1; // prone  (0x100) [orig: @0x501ce7]
				else if (code == 172) pe->net_stance_bits = 0; // stand          [orig: @0x501cc9]
				// The authority's own player latches the bits it just wrote
				// [orig: `cmp g_LocalPlayerEntity` @0x501d0d, @0x501d1b / @0x501d2d].
				if (pe->handle == world->cached.local_player && world->local_player_state != nullptr)
					world->local_player_state->latch_stance(pe->net_stance_bits);
				break;
			}
			case c2s::VEHICLE_ATTACH_REQUEST: { // VEHICLE ATTACH [u16 senderHandle][u16 vehicleHandle][u8 bone][u8 pad]
				// [orig: NapiNPServerMsg_HandleVehicleAttach @0x502390 — authority-gated;
				// word0 is OVERWRITTEN with the sender's authoritative handle (@0x502415,
				// anti-spoof — the client value is never read); then Entity_ProcessVehicleAttach
				// @0x435AA0 validates + attaches. NO reply message — the 0x0A compact record's
				// mounted branch (carrier + bone byte0) is the confirmation for everyone
				// including the requester.]
				if (world == nullptr || !conn.burst.spawned || !conn.link.owned_entity.valid())
					break;
				if (msg.payload.size() < 5) break;
				const uint16_t veh =
						static_cast<uint16_t>(msg.payload[2] | (msg.payload[3] << 8));
				const uint8_t bone = msg.payload[4];
				world->vehicles.process_attach(conn.link.owned_entity,
				                                     world::EntityHandle{veh}, bone);
				break;
			}
			case c2s::VEHICLE_DETACH_REQUEST: { // VEHICLE DETACH [u16 selfHandle][u16 vehicleHandle][u16 junk]
				// [orig: NapiNPServerMsg_HandleVehicleDetach @0x4FC980 -> the @0x435D00 tail:
				// sender conn must have a player block+cell; the wire word0 is TRUSTED in
				// retail (no anti-spoof, no range guard) — we clamp the detach subject to the
				// sender's own entity, a tracked hardening divergence (D-NET-157); then
				// Entity_DetachFromVehicle(e, *(e+0x16C)) @0x435d40. The use-key dismount
				// SENDS ONLY and waits for the 0x0A echo [orig: @0x4369c7].]
				if (world == nullptr || !conn.burst.spawned || !conn.link.owned_entity.valid())
					break;
				world->vehicles.detach(conn.link.owned_entity);
				break;
			}
			case c2s::END_ROUND_STATS_REQUEST: {
				// Retail serves the already-frozen board stream (stru_C947D8) to
				// this requester only; the stream is built once by the round-end
				// producer and this handler never rebuilds it. The request is
				// exactly one u16 offset and every response carries at most 200
				// stream bytes. [orig: NapiNPServerMsg_HandleReplayDataRequest @0x514FE0 ->
				// NetPacket_WriteReplayStreamChunk @0x506F60; producer
				// Server_BuildEndOfRoundScoreboard @0x508F30 (the call @0x516590
				// in Server_ProcessRoundEnd @0x5164F0)]
				if (inputs.round_end_board_stream == nullptr ||
						inputs.round_end_board_stream->empty())
					break;
				EndRoundStatsRequest request;
				if (!decode_end_round_stats_request(
						msg.payload.data(), msg.payload.size(), request)) break;
				std::vector<uint8_t> chunk = encode_end_round_stats_chunk(
						*inputs.round_end_board_stream, request.offset);
				// NetPacket_WriteReplayStreamChunk returns zero for an offset
				// beyond the stream and its caller sends only for len > 0.
				// [orig: NapiNPServerMsg_HandleReplayDataRequest @0x514FE0]
				if (!chunk.empty())
					replies.push_back(make_protocol_message(
							s2c::END_ROUND_STATS, std::move(chunk)));
				break;
			}
			case c2s::RTT_CONSUMED: { // RTT probe [orig: NapiNPServerMsg_HandlePingResponse @0x515070]
				// [u32 timestamp][u8 echo_flag]. echo_flag != 0 -> bounce S2C 0x57 [u32 ts][u8 0]; the
				// echo_flag == 0 return leg measures `now - ts` into the slot's ring and
				// runs the min/max-ping strike policy (no reply). Both legs read the
				// timestamp with a short read of 0; the flag byte's absence selects the
				// return leg [orig: @0x5150BE..0x5150CF].
				const uint32_t ts = msg.payload.size() >= 4
						? (static_cast<uint32_t>(msg.payload[0]) |
						   (static_cast<uint32_t>(msg.payload[1]) << 8) |
						   (static_cast<uint32_t>(msg.payload[2]) << 16) |
						   (static_cast<uint32_t>(msg.payload[3]) << 24))
						: 0u;
				if (msg.payload.size() >= 5 && msg.payload[4] != 0) {
					ProtocolMessage pong = make_protocol_message(
							s2c::RTT_ECHO, build_tag57_pong(ts));
					pong.reliable = false; // SendFiltered @0x51510C userParam=1
					replies.push_back(std::move(pong));
				} else if (conn.link.owned_entity.valid()) {
					Server_RecordPingSample(config, conn, ts,
							io::host_milliseconds_for_logic_tick(now_tick),
							inputs.server_ctx != nullptr && inputs.server_ctx->is_in_session != 0);
				}
				break;
			}
			case c2s::CLIENT_QUALITY: { // [orig: NapiNPServerMsg_0x04C @0x5111B0]
				// [u8 level] (a short read is 0), clamped to 4, stored on the slot,
				// dirtied on change for the 1 Hz 0x46 resend.
				if (!conn.link.owned_entity.valid()) break;
				Server_StoreClientQuality(conn, msg.payload.empty() ? uint8_t{0} : msg.payload[0]);
				break;
			}
			case c2s::CHAT_MESSAGE: { // [orig: NapiNPServer_HandleChatMessage @0x513760]
				if (world == nullptr || inputs.server_ctx == nullptr) break;
				ChatUplink uplink;
				// The channel byte and text both short-read (0 / ""), then the
				// text is stripped and fanned per channel.
				if (!msg.payload.empty()) uplink.channel = msg.payload[0];
				if (msg.payload.size() > 1) {
					const auto begin = msg.payload.begin() + 1;
					const auto nul = std::find(begin, msg.payload.end(), uint8_t{0});
					uplink.text.assign(begin, nul);
				}
				std::vector<ProtocolMessage> chat = Server_HandleChatMessage(
						*inputs.server_ctx, conn, uplink,
						io::host_milliseconds_for_logic_tick(now_tick), *world);
				for (ProtocolMessage &m : chat) replies.push_back(std::move(m));
				break;
			}
			case c2s::ADMIN_NETLOG_COMMAND: // [orig: NapiNPServerMsg_HandleConsoleLine @0x5199D0]
				if (inputs.server_ctx != nullptr) // a peer's console line (server_console.h)
					Server_HandleConsoleCommandMessage(*inputs.server_ctx, conn, msg.payload,
							io::host_milliseconds_for_logic_tick(now_tick), replies);
				break;
			case c2s::LOADED_MODEL_PAGE_REPLY: { // [orig: NapiNPServerMsg_0x03D @0x500EC0]
				// Authority-gated; the body is never read: the handler stamps the
				// host frame clock onto the player slot (+97560 = dword_A87060).
				if (!conn.link.owned_entity.valid()) break;
				st.loaded_model_reply_frame = now_tick;
				break;
			}
			case c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST: { // [orig: NapiNPServerMsg_SendWeaponSlotStates @0x510930]
				// No fields read; the reply is the requester's team availability
				// (mask 32, reliable).
				if (world == nullptr || inputs.server_ctx == nullptr ||
						!conn.link.owned_entity.valid())
					break;
				const world::Entity *requester = world->registry.get(conn.link.owned_entity);
				if (requester == nullptr) break;
				replies.push_back(make_protocol_message(
						s2c::VEHICLE_SPAWN_AVAILABILITY,
						Server_BuildVehicleSpawnAvailability(*inputs.server_ctx, *world,
								requester->team)));
				break;
			}
			case c2s::VEHICLE_SPAWN_REQUEST: { // [orig: NapiNPServerMsg_HandleVehicleSpawnRequest @0x51C4C0]
				if (world == nullptr || inputs.server_ctx == nullptr) break;
				VehicleSpawnRequest request;
				size_t consumed = 0;
				(void)decode_vehicle_spawn_request(msg.payload.data(), msg.payload.size(),
						request, consumed); // short fields read 0, as retail
				(void)Server_HandleVehicleSpawnRequest(*inputs.server_ctx, conn, request, *world);
				break;
			}
			case c2s::SPECTATOR_RESPAWN: // [orig: Server_ProcessClientRequestSpectatorRespawn @0x51C840]
				dispatch_spectator_respawn(config, conn, msg, inputs);
				break;
			case c2s::MISSION_FILE_STATUS: // mission-file status report [orig: NapiNPServerMsg_PlayerJoinRequest @0x51AB10]
				Server_HandlePlayerJoinRequest(conn, world);
				break;
			case c2s::FIRED_ROUND: // client fired round -> ammo authority + the S2C 0x0A tag-2 round echo
				Server_HandleClientFiredRound(config, conn, msg.payload.data(), msg.payload.size(), world);
				break;
			case c2s::WEAPON_RELOAD_REQUEST: { // reload request -> S2C 0x49 BROADCAST [orig: NapiNPServerMsg_HandleReloadRequest
				// @0x514DF0 — validates the [u16 entityHandle][u16 weaponSlotCombo] body, then relays it
				// verbatim as S2C 0x49 via two NapiNPServer_SendFiltered @0x4C87E0 sends that together
				// reach ALL in-match connections INCLUDING the requester. The client's 0x49 apply is the
				// ONLY place its clip refills. The 0x80 phase bit is transient; without the echo, the
				// empty clip returns to idle and auto-reload requests again (§5.58, D-NET-142). Staged on
				// each recipient's transport; the per-connection flush frames it with that connection's
				// own sequencing. Personal weapons use the per-owner combo map; an addressed EWeap
				// follows the shared live child/groundEntity MountSlot route.]
				WeaponReload req;
				size_t consumed = 0;
				if (!decode_weapon_reload(msg.payload.data(), msg.payload.size(), req, consumed))
					break;
				// Retail resolves both the requesting player and the payload-addressed
				// entity before relaying. The two need not be the same: a mounted player
				// may address a vehicle weapon, so this is deliberately not an anti-spoof
				// equality check. [orig: requester player slot/dead mark @0x514e02..1f;
				// packed pool/slot + live item-def checks @0x514e5c..91]
				if (world == nullptr || !is_in_match(conn) ||
				    !conn.link.owned_entity.valid())
					break;
				const world::Entity *requester =
						world->registry.get(conn.link.owned_entity);
				if (requester == nullptr || requester->health <= 0) break;
				if (wire_handle::is_batch_end_sentinel(req.entity_handle))
					break;
				const world::EntityHandle addressed{req.entity_handle};
				if (static_cast<size_t>(addressed.slot()) >=
				            world->registry.pool_capacity(addressed.pool()) ||
				    world->registry.get(addressed) == nullptr)
					break;
				world::Entity *addressed_entity = world->registry.get(addressed);
				const std::vector<uint8_t> body = encode_weapon_reload(req); // rebuilt, never raw (ADR 0003)
				for (NapiNPConnection &c : roster) {
					if (!is_in_match(c) || c.link.transport == nullptr) continue;
					c.link.transport->host_send(
							s2c::WEAPON_RELOAD, body, /*reliable=*/true, 0, false,
							&c == &conn ? 0u : 1u);
				}
				// The 3P RELOAD POSE for a remote requester. Retail's host, unlike a pure
				// client, does run the refill on its own copy of the peer, and that stamps
				// the reload window the weapon-channel hold selector reads — which is why a
				// peer's reload clip is visible when we HOST and never when we merely join.
				// Stamped above the two bails below because retail has neither.
				// [orig: NapiNPServerMsg_HandleReloadRequest @0x514df0 — the requester
				//  != local-player gate @0x514efa, WeaponSlot_ReloadAmmo @0x514f03, the
				//  window stamp entity+0x372 = 80 @0x54173c; consumer @0x4b5e5e..0x4b5e6f]
				if (conn.link.mode != replication::TransportMode::Loopback) {
					if (world::AiEntity *peer = world->ai.for_handle(
					            world::EntityHandle{req.entity_handle}))
						peer->inf.reload_anim_ticks = 80;
				}
				// Host-side clip refill for a REMOTE requester [orig: WeaponSlot_ReloadAmmo
				// @0x541720, called @0x514F03 after the relay iff requester != local player:
				// the refund @0x5417E7, the draw clamped by the pool @0x541811, the store
				// @0x541842 / @0x541850]. The wire
				// combo (the second u16) keys the same slot the 0x06 pipeline decrements
				// [orig: slotIndex = HIWORD @0x514f03; slot = playerSlot+464+100*combo @0x54176d].
				if (!world->tables.weapons.empty() &&
				    conn.link.mode != replication::TransportMode::Loopback) {
					// EWeap reload ignores the wire combo and follows the same live
					// child/groundEntity route as fire and phase-8 serialization.
					// [orig: WeaponSlot_ReloadAmmo @0x541720 -> @0x5460e0]
					if (addressed_entity != nullptr &&
							addressed_entity->has_item_def &&
							(addressed_entity->item_attrib &
									world::kItemAttribEweap) != 0u &&
							world->vehicles.prepare_weapon_slot(*addressed_entity)) {
						// [orig: WeaponSlot_ReloadAmmo's EWeap legs]
						world->vehicles.reload_weapon_slot(*addressed_entity);
						break;
					}
					NapiNPConnection *addressed_owner = nullptr;
					for (NapiNPConnection &candidate : roster) {
						if (candidate.link.owned_entity.valid() &&
						    candidate.link.owned_entity.packed == req.entity_handle) {
							addressed_owner = &candidate;
							break;
						}
					}
					if (addressed_owner == nullptr) break;
					auto slot_it = addressed_owner->weapon_slots.find(req.reload_param);
					if (slot_it != addressed_owner->weapon_slots.end()) {
						const world::WeaponTableEntry *adm =
								world->tables.weapons.by_index(slot_it->second.adm_index);
						// The pool half of the refill: the remaining clip is refunded into
						// the ammo class pool (capped), then a full clip is drawn back out
						// clamped by what that pool affords, so pool + clip is conserved
						// across reloads — the total the kit-weight recompute sums. A slot
						// with no pool units per round keeps the plain refill.
						// [orig: WeaponSlot_ReloadAmmo @0x541720 — refund @0x5417a2,
						//  the clamped draw and slot+16 store @0x541811..0x541850]
						if (adm != nullptr && adm->clipsize != -1) {
							WeaponSlotState &slot = slot_it->second;
							const bool shared = adm->ammo_bucket != 0;
                            const uint32_t bucket = static_cast<uint32_t>(adm->ammo_bucket);
                            auto &shared_clips = addressed_owner->reply.shared_clips;
                            const bool bucket_valid = bucket < shared_clips.size() &&
                                    bucket < world->tables.weapons.ammo_class_names.size();
                            const int32_t loaded = shared ? (bucket_valid ? shared_clips[bucket] : 0) : slot.clip;
                            const int class_id = adm->ammo_class_id;
							if (adm->ammo_class_count != 0 && class_id >= 0 &&
							    class_id < static_cast<int>(addressed_owner->reply.ammo_pools.size())) {
								int32_t &pool = addressed_owner->reply.ammo_pools[
										static_cast<size_t>(class_id)];
								const int32_t units = adm->ammo_class_count;
								const int32_t cap = class_id < static_cast<int>(
										world->tables.weapons.ammo_class_caps.size())
										? world->tables.weapons.ammo_class_caps[
												  static_cast<size_t>(class_id)]
										: 0;
								if (loaded != 0) {
                                    pool += loaded * units;
									if (pool > cap) pool = cap;
								}
								int32_t draw = static_cast<int32_t>(adm->clipsize) * units;
								if (draw > pool) draw = pool;
								pool -= draw;
								if (!shared) slot.clip = static_cast<int16_t>(draw / units);
                                else if (bucket_valid) shared_clips[bucket] = draw / units;
							} else {
								slot.clip = adm->clipsize; // [orig: slot+16 @0x541850]
							}
						}
					}
				}
				break;
			}
			case c2s::ENTITY_INFO_QUERY: { // entity-info query [u16 handle] -> S2C 0x18 FULL-ENTITY-SPAWN (the self-heal).
				// [orig: NapiNPServerMsg_HandlePlayerInfoRequest @0x514180 — validates pool <= 1 &&
				// slot < capacity, serializes the REQUESTED entity via NetPacket_SerializeObjectToBuffer
				// @0x504d10, replies msg 0x18 to the requester only (send_mask 0x20).] The client sends
				// 0x0F when its per-frame 0x0A cross-check finds a stale/mismatched entity
				// (NapiNPClientMsg_0x00A tail @0x4307c4); NapiNPClientMsg_FullEntitySpawn @0x433780
				// then DESTROYS + fully REBUILDS the entity from the record — itemDef, models,
				// playerClass, minimap slot, ADM/anim registration. A healthy join never exercises it
				// (the retail↔retail golden has zero 0x0F), but a joiner whose round-load re-resolve
				// broke an entity queries EVERY 0x0A frame — leaving it unanswered strands the broken
				// entity forever (the retail-join DBuggy-host-player + ~1000/session C 0x0F flood).
				// An in-capacity EMPTY slot still replies: retail serializes the empty pool slot
				// (itemDef null -> type 0), which the client answers by clearing its stale entity.
				// The requester must already be an added player: retail reads the
				// connection's session player (conn+0x160) and its server player
				// slot (+0xC0, bound by Server_PlayerAdd @0x51CD51) and drops the
				// query when either is null [orig: @0x514191..0x5141A8]; our
				// analogue of the bound slot is the PlayerAdded phase.
				if (conn.phase < ConnectionPhase::PlayerAdded) break;
				const uint16_t handle =
						msg.payload.size() >= 2
								? static_cast<uint16_t>(msg.payload[0] | (msg.payload[1] << 8))
								: 0;
				const int pool = handle >> 12;
				if (world != nullptr && pool <= 1) {
					const world::EntityHandle h{handle};
					if (static_cast<size_t>(h.slot()) < world->registry.pool_capacity(pool)) {
						FullEntitySpawnRecord frec;
						if (const world::Entity *e = world->registry.get(h)) {
							frec = replication::build_full_entity_spawn(*e);
						} else {
							frec.slot_id = handle; // empty slot: type-0 record clears the client's entity
						}
						replies.push_back(make_protocol_message(s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(frec)));
					}
				}
				break;
			}
			case c2s::BURST_MEMBER_2D: { // SESSION-STATUS request -> requester-only S2C 0x58.
				// The request has no fields. Retail rebuilds its live 0x158-byte report,
				// serializes it, then uses send_mask 0x20 with the requester's player slot.
				// A 0x2D only reaches the host in a session (is_in_session for key 8).
				// [orig: NapiNPServerMsg_0x02D @0x502430 -> Server_BuildAndSerializeSessionStatus @0x4FC990 ->
				// Server_BuildStatusReport @0x530A60]
				replies.push_back(make_protocol_message(
						s2c::SESSION_STATUS,
						serialize_session_status(
								config, inputs.session_uptime_ms, true, world)));
				break;
			}
			case c2s::EMPTY_SLOT_SWEEP_REQUEST: { // EMPTY-SLOT SWEEP REQUEST -> S2C 0x5D to the REQUESTER ONLY.
				// The client queues this inside its S2C 0x0F world-state-load reply burst
				// (@0x42e647). The host walks pool 0 and answers with the index of every
				// EMPTY entry (`entry_dword[7] == 0`); the client destroys whatever it
				// still holds in those slots (NapiNPClientMsg_DestroyEntityList
				// @0x429730). The handler reads NO fields from the request. It answers
				// on the authority only, is SKIPPED while g_NetSpawnSuspended or
				// g_SpawnSuccessGate (round over) is set — our reachable analog of the
				// latter is world->match.outcome().ended — and answers only a requester
				// whose session player (conn+0x160) and its server slot (+0xC0) are
				// bound, which our PlayerAdded phase stands for (the 0x22 and 0x0F
				// gates' analogue). The walk covers pool 0's used
				// count, its allocation high-water mark (D-NET-417); its empty test is the
				// ItemTypeIndex (+0x1C) alone, with no def test: a free row, or a live one
				// of a type items.def lacks (the lookup's miss answers row 0).
				// [orig: NapiNPServerMsg_SendEmptySlots @0x51a600 — the authority
				//  @0x51A600..0x51A607, g_NetSpawnSuspended @0x51A609..0x51A610
				//  (unmodeled, D-NET-116), the round-over gate @0x51A612..0x51A619, the
				//  session player @0x51A61F..0x51A627 and its slot @0x51A62A..0x51A632;
				//  body builder Server_CollectEmptyPool0Slots @0x5160f0, the used read
				//  @0x5160FF, the +0x1C test @0x516123]
				if (inputs.server_ctx == nullptr || !inputs.server_ctx->is_authority ||
				    conn.phase < ConnectionPhase::PlayerAdded)
					break;
				std::size_t consumed = 0;
				if (world == nullptr ||
				    !decode_empty_slots_request(
						msg.payload.data(), msg.payload.size(), consumed) ||
				    world->match.outcome().ended) {
					break;
				}
				const std::size_t used = world->registry.pool_used(0);
				DestroyEntityList sweep;
				for (std::size_t slot = 0; slot < used; ++slot) {
					const world::Entity *e =
							world->registry.get(world::EntityHandle{static_cast<uint16_t>(slot)});
					if (e == nullptr || e->item_type_index == 0)
						sweep.pool0_indices.push_back(static_cast<uint16_t>(slot));
				}
				replies.push_back(make_protocol_message(
						0x5D, encode_destroy_entity_list(sweep)));
				break;
			}
			case c2s::VISIBLE_PLAYERS_REQUEST: { // -> S2C 0x4C, the requester only
				// The requester's visible-players snapshot, the only writer of
				// its client's player-slot pointer table (docs/interface/hud-re.md
				// "The MP legs"). [orig: NapiNPServerMsg_0x023_WeaponOverlayBroadcast
				//  @0x514D50 (authority, the requester's slot, send_mask 32) ->
				//  NetPacket_SerializeVisiblePlayersSnapshot @0x506320]
				std::size_t consumed = 0;
				if (!decode_burst_visible_request(
						msg.payload.data(), msg.payload.size(), consumed))
					break;
				const bool peer = inputs.server_ctx != nullptr
						? inputs.server_ctx->is_mp_session_peer != 0
						: (world != nullptr && world->rules.mp_session_peer);
				replies.push_back(make_protocol_message(s2c::VISIBLE_PLAYERS,
						build_visible_players_snapshot(conn, roster, world,
								config.game_type, peer)));
				break;
			}
			case c2s::LOADOUT_REQUEST: // the join-window kill page -> S2C 0x4E (server_kill_page.h)
				for (ProtocolMessage &m : Server_HandleKillPageRequest(
						inputs.server_ctx, conn, msg.payload, world))
					replies.push_back(std::move(m));
				break;
			default:
				// The command map's squad / waypoint / punt legs (server_squad.h); 0x09
				// checksum / 0x48 + per-frame client updates: consumed (no reactive reply).
				if (inputs.server_ctx != nullptr && world != nullptr)
					Server_HandleSquadMessage(*inputs.server_ctx, conn, msg.tag, msg.payload, *world);
				break;
		}
	}
	return replies;
}

bool bind_session_reply_player(NapiNPConnection &conn, std::string player_name, uint8_t player_slot,
                               uint16_t entity_handle) {
	// D-NET-132: the bare wire handle IS the binding — stamp it onto owned_entity (the single source the
	// reply builders read the handle/team through). player_name/slot stay on conn.reply.
	conn.link.owned_entity.packed = entity_handle;
	conn.link.owned_entity_spawn_id = 0; // no World exists to stamp the allocation yet
	// [orig: CAdminServer_HandleStatus @0x402E30 supplies the bare handle; the
	// live Server_BuildPlayerInfoAndAdd binding follows when the World exists]
	conn.reply.player_name = std::move(player_name);
	conn.reply.player_slot = player_slot;
	conn.reply.player_slot_reserved = false;
	return true;
}

ProtocolMessage build_player_list_message(const GameConfig &config,
                                          const std::vector<NapiNPConnection> &roster,
                                          world::World *world) {
	ProtocolMessage message = make_protocol_message(
			s2c::PLAYER_LIST, build_reply_tag_16(config, roster, world));
	message.reliable = false; // Server_BuildAndBroadcastScoreboard @0x50DE00 userParam=1
	return message;
}

} // namespace opennova::inmatch
