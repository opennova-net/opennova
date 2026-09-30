#include <runtime/world/weapon_fire_gate.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_loadout_grant.h> // the 0x2F grant family (GrantedWeaponLoadout, grant_weapon_loadout, ...)


#include <runtime/inmatch/integrity_challenge_profile.h>
#include <runtime/inmatch/end_round_protocol.h>
#include <net/npwire/nw_session_framing.h> // make_random_session_u32 (the per-player tick seed)
#include <runtime/inmatch/server_spawn.h> // Server_ReservePlayerTeam (0x04/spawn identity)
#include <runtime/inmatch/server_tick.h> // Server_StageHostDisconnect
#include <runtime/inmatch/server_chat.h>         // the C2S 0x0D fan-out
#include <runtime/inmatch/server_net_quality.h>  // the 0x2C return leg / 0x4C store
#include <runtime/inmatch/server_vehicle_spawn.h> // the 0x40 / 0x42 handlers
#include <runtime/inmatch/napi_np_server_ctx.h>  // ServerDispatchInputs::server_ctx
#include <runtime/inmatch/session_status.h>
#include <runtime/world/weapon_table_build.h> // loadout_entry_permitted / resolve_loadout_ammo (D-NET-141)

#include <runtime/replication/connection_fan.h>     // apply_connection_uplink — the in-order 0x0C read-apply
#include <runtime/replication/entity_wire_bridge.h> // build_full_entity_spawn — the 0x0F -> 0x18 repair record

#include <base/gameprofile/game_type.h>       // is_waypoint_family / is_stock_coop (§5.32, D-NET-203/205)
#include <base/io/strutil.h>                  // iequals (the JOIN body's CD field)
#include <net/npwire/flat_tlv.h>        // the C2S 0x00 JOIN body walk (the CD identity blob)
#include <net/npwire/wire_handle.h>     // is_batch_end_sentinel / kInvalid (the wire handle home)
#include <net/npwire/ingame_decode.h>   // decode_entity_packet_sub_header / decode_player_extended_uplink
#include <net/npwire/ingame_encode.h>   // encode_player_sync / encode_player_list (§5.1)
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/session_vars.h>     // the 0x60 server-info stream
#include <net/npwire/replication_model.h> // PlayerReplicationState (POD) — the reply builders' input

#include <runtime/world/entity.h> // world::Entity / EntityHandle — team @entity+344 read through owned_entity
#include <runtime/world/local_player.h>
#include <runtime/world/entity_spawn.h>  // entity_reset_to_spawn_state — the deploy revive (§5.61)
#include <runtime/world/spawn_select.h>  // deploy target validation + one spawn-pose resolver
#include <runtime/world/vehicle_attach.h> // entity_process_vehicle_attach / entity_detach_from_vehicle (0x26/0x27)
#include <runtime/world/world.h>  // world::World::registry (the authoritative roster, §6.9)
#include <runtime/audio/sound_profile.h> // compose_entity_sound_set — the 0x2E MEDIC_REQUEST composite
#include <runtime/world/zone_chain.h>    // zone_chain_frontier_zone — the 0x1E ev-0x3A deploy hint

#include <runtime/world/vehicle_mount.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/geom.h>
#include <runtime/world/infantry.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <utility>
#include <base/io/fixed.h> // fp16_16_to_float
#include <base/io/le.h>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <optional>
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

bool stage_integrity_crc_punt(
		NapiNPConnection &conn, std::string_view detail) {
	DisconnectEvent event;
	event.ds = 1;
	event.dc = 2;
	event.dpc = 46;
	event.ddstr = std::string(detail);
	return Server_StageHostDisconnect(conn, event);
}

// A game-layer join-gate rejection: the empty-string description record with
// only the DPC reason set (DS=1 role, DC=2 class, DP1/DP2 0, DSTR/DDSTR "").
// [orig: Server_ValidatePlayerJoinRequest @0x512100 failures ride
// CNapiNPConnection_SendChatMessage @0x4c7ef0 with both strings g_EmptyStr ->
// NapiNPDataTransfer_SendDescription @0x628c80]
bool stage_join_gate_reject(NapiNPConnection &conn, uint32_t reason_dpc) {
	DisconnectEvent event;
	event.ds = 1;
	event.dc = 2;
	event.dpc = reason_dpc;
	return Server_StageHostDisconnect(conn, event);
}

// The game-layer spectator gate, run at the 0x00 join message AFTER the
// NP-level 0x42 admitted the connection. Order and codes are the witnessed
// legs: 14 spectating disabled, 15 positive slot count exceeded, 16 password
// mismatch (case-insensitive, like the side passwords). The candidate's flag
// is latched BEFORE the count, so the count includes it and rejects on
// strictly-greater — retail's exact shape. A -1 (shared-capacity) setting has
// no spectator-count leg; the shared 0x42 gate already bounded the total.
// [orig: the entry+55 latch from the stored JSR in
// NapiNPServer_HandlePlayerJoinMessage @0x512aa0; the disabled leg @0x512364
// (code 14), the `> g_GameConfigState.maxSpectators_26C` count @0x5123ca (code 15, type-1 rows
// with entry+55), the Napi_StrCaseEqual JSPP compare @0x512405..0x512425
// (code 16); Server_ValidatePlayerJoinRequest @0x512100]
uint32_t validate_spectator_join(const GameConfig &config,
		NapiNPConnection &conn, const std::vector<NapiNPConnection> &roster) {
	conn.link.spectator = conn.join_spectator_request != 0;
	if (!conn.link.spectator) return 0;
	if (config.spectator_slots == 0) return 14;
	if (config.spectator_slots > 0) {
		uint32_t spectators = 0;
		for (const NapiNPConnection &row : roster) {
			if (row.type == NapiNPConnection::kTypeServerSide &&
			    row.phase >= ConnectionPhase::Joined && row.link.spectator)
				++spectators;
		}
		if (spectators > static_cast<uint32_t>(config.spectator_slots))
			return 15;
	}
	// [orig: Napi_StrCaseEqual @0x616e70]
	if (!config.spectator_password.empty() &&
	    !strutil::iequals(
				conn.join_spectator_password, config.spectator_password))
		return 16;
	return 0;
}

// The side-password leg follows spectator admission. TR is already narrowed
// by the auth tag loader; -1 accepts either password (or an unlocked side).
// [orig: Server_ValidatePlayerJoinRequest @0x5124A2..0x5125D4; the password
//  compare Napi_StrCaseEqual @0x616e70]
uint32_t validate_side_password(const GameConfig &config, const NapiNPConnection &conn) {
	if ((config.mp_attributes & GameConfig::kMpAttribTeamChoose) == 0 ||
			conn.link.spectator) return 0;
	const auto matches = [&](const std::string &password) {
		return password.empty() || strutil::iequals(password, conn.join_password);
	};
	switch (conn.char_vars.team_request) {
		case 0xFF: return matches(config.side_a_password) || matches(config.side_b_password) ? 0 : 18;
		case 0: return matches(config.side_a_password) ? 0 : 19;
		case 1: return matches(config.side_b_password) ? 0 : 20;
		default: return 22;
	}
}

} // namespace

MissionMetadataBlob build_mission_metadata_blob(const GameConfig &config) {
	MissionMetadataBlob blob{};
	auto write_u32 = [&](std::size_t offset, uint32_t value) {
		opennova::io::write_u32_le(blob.data() + offset, value);
	};
	auto write_fixed_string = [&](std::size_t offset, const std::string &value) {
		const std::size_t count = std::min<std::size_t>(value.size(), 31);
		std::memcpy(blob.data() + offset, value.data(), count);
	};
	for (std::size_t i = 0; i < 32; ++i) {
		blob[i] = static_cast<uint8_t>(make_random_session_u32());
		blob[148 + i] = static_cast<uint8_t>(make_random_session_u32());
	}
	uint32_t mission_session_id = 0;
	while (mission_session_id == 0)
		mission_session_id = make_random_session_u32() & 0xFFFFu;
	write_u32(32, mission_session_id);
	// Offset 36 carries the session's max players verbatim — the only
	// capture-verified form (handshake goldens: le32 == max_players). An
	// earlier serve-mode decrement here was pure inference with no witness;
	// the dedicated-host form remains unwitnessed either way.
	write_u32(36, std::min<uint32_t>(config.max_players, 251u));
	write_u32(40, config.game_type);
	write_u32(44, config.mp_attributes);
	write_u32(48, 1u);
	write_fixed_string(52, config.server_name);
	write_fixed_string(84, config.mission_file);
	// The 00TRg LAN oracle carries the map filename in both mission slots.
	write_fixed_string(116, config.mission_file);
	return blob;
}

namespace {

// ---------------------------------------------------------------------------
// Byte helpers. The §5.1
// identity bodies (0x7A PCID, 0x7B session info) are now FAITHFUL ports of the witnessed serializers
// (NetPacket_WritePCID @0x5076e0, NapiNPMsg_0x7B_BuildPayload @0x507740; net-re §5.45, grilled
// 2026-06-27). The remaining reply bodies (0x46 NetPacket_SerializePlayerSync0x46 @0x505e80 —
// a flag-driven slot-state record, and 0x51 NetPacket_WriteEntityPacket @0x506bb0) carry approximations
// pending slot-state modeling; the witnessed field maps are landed in net-re §5.45 (D-NET-127).
// ---------------------------------------------------------------------------

void append_u16_le(std::vector<uint8_t> &out, uint16_t v) {
	opennova::io::append_u16_le(out, v);
}

void append_u32_le(std::vector<uint8_t> &out, uint32_t v) {
	opennova::io::append_u32_le(out, v);
}

void write_u32_le(std::vector<uint8_t> &out, size_t off, uint32_t v) {
	opennova::io::write_u32_le(out.data() + off, v);
}

uint32_t read_u32_le(const uint8_t *data) {
	return opennova::io::read_u32_le(data);
}

// Literal patch/account gates precede the expansion and spectator legs.
// Missing CU fields retain zero and fail here, after the NP handshake.
// [orig: Server_ValidatePlayerJoinRequest @0x512100]
uint32_t validate_join_environment(const ClientGameEnvironment &env) {
	if (env.bn != 1) return 2;
	if (env.vn != 2) return 3;
	if (env.mbn != 20042002) return 4;
	if (env.bt == 1) return 6;
	if (env.bt == 2) return 7;
	if (env.sopd != 180) return 8;
	return 0;
}

uint32_t validate_join_request(
		const GameConfig &config, const std::vector<uint8_t> &payload) {
	std::size_t pos = 0;
	std::string expansion;
	std::string version_crc;
	// The TLV walk accepts unknown fields, uses the last matching value, and
	// stops at its first short field or an empty name. String copies stop at
	// the first NUL; tag names compare through Napi_StrCaseEqual @0x616e70.
	// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512aa0]
	while (pos < payload.size()) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(payload.data(), payload.size(), pos, field);
		if (next == kFlatTlvEnd || field.name.empty()) break;
		pos = next;
		const std::string value(field.value,
				std::find(field.value, field.value + field.size, uint8_t{0}));
		if (strutil::iequals(field.name, "EXP")) expansion = value.substr(0, 31);
		else if (strutil::iequals(field.name, "VERSIONCRCSTRING")) version_crc = value.substr(0, 511);
	}
	// [orig: Server_ValidatePlayerJoinRequest @0x5122fc..0x512349]
	if (expansion != config.expansion) return 47;
	if (!config.expansion.empty() &&
	    static_cast<int32_t>(std::strtol(version_crc.c_str(), nullptr, 10)) !=
	            config.expansion_version_checksum) return 48;
	return 0;
}

bool validates_padding_echo(
		const NapiNPConnection &conn, const std::vector<uint8_t> &payload) {
	// The server challenge advertises paddingLen=256. Retail echoes exactly that many bytes,
	// preserving the first two position dwords and replacing the third with its net-frame counter.
	return payload.size() == 256 &&
	       read_u32_le(payload.data()) == conn.admission_padding_x &&
	       read_u32_le(payload.data() + 4) == conn.admission_padding_y;
}

void append_cstr(std::vector<uint8_t> &out, const std::string &s) {
	out.insert(out.end(), s.begin(), s.end());
	out.push_back(0);
}

bool is_default_ash_session_config(const GameConfig &cfg) {
	return cfg.mission_name == "AS - Dormant Volcano Isle" && cfg.mission_file == "ASH_I5A.BMS";
}

// ---------------------------------------------------------------------------
// §5.1 reply bodies — handshake + server-info + mission-metadata.
// ---------------------------------------------------------------------------

// tag=0x02 server push. [orig: NapiNPClientMsg_HandleJoinResponse @0x42E0F0 reads 12 B; D-NET-127 carried 512.]
std::vector<uint8_t> build_tag02_push(uint32_t now_tick) {
	std::vector<uint8_t> payload(512, 0);
	write_u32_le(payload, 0, now_tick);
	payload[4] = 0x02;
	payload[8] = 0x00;
	payload[9] = 0x01;
	return payload;
}

// tag=0x7A: the player's PCID string (NOT the player name). [orig: NetPacket_WritePCID @0x5076e0 —
// copies player+0x250]. Empty on a dev host -> a single NUL (golden frame 134 = len 1, body 00).
std::vector<uint8_t> build_tag7a_pcid(const GameConfig &cfg) {
	std::vector<uint8_t> payload;
	append_cstr(payload, cfg.pcid);
	return payload;
}

// tag=0x7B session/player info. [orig: NapiNPMsg_0x7B_BuildPayload @0x507740] field order:
// recipient player_name, PCID, server_name, advertised mission, map filename,
// gametype(u32), empty_str, expansion. Retail's field-4 source switches at
// @0x507822: waypoint-family modes use the active filename while other modes
// use the resolved MissionText title. The objective bit does not affect that
// selection. Field 5 is always g_MapFileName.
// (The PCID slot previously carried an invented "DEV-A02-0001" literal — D-NET-127; now sourced.)
std::vector<uint8_t> build_tag7b_session_summary(const GameConfig &cfg,
                                                 const NapiNPConnection &conn) {
	std::vector<uint8_t> payload;
	const std::string &advertised_mission =
			game_type::is_waypoint_family(cfg.game_type)
					? cfg.mission_file
					: cfg.mission_name;
	append_cstr(payload, conn.player_name.empty() ? conn.reply.player_name
	                                             : conn.player_name); // recipient player_data+128
	append_cstr(payload, cfg.pcid);        // [orig entity+592] PCID
	append_cstr(payload, cfg.server_name); // [orig g_ServerNameStr]
	append_cstr(payload, advertised_mission);
	append_cstr(payload, cfg.mission_file);// [orig g_MapFileName]
	append_u32_le(payload, cfg.game_type);  // [orig g_GameType]
	payload.push_back(0);                  // [orig g_EmptyStr] empty C string
	append_cstr(payload, cfg.expansion);   // [orig g_ExpansionName]
	return payload;
}

// The chunk header + at most 200 bytes of the stream from `offset`, shared by the
// 0x60 (server-info VarList) and 0x64 (mission block) transfers. Header dword 0 is
// the LIVE transfer identity, dword 1 the total length and dword 2 the clamped
// offset; an offset past the end yields an empty chunk. The client re-requests
// with [id][nextOffset] (C2S 0x33/0x37) and a mismatched id restarts at 0.
// [orig: NetPacket_WriteReplayDataBlock @0x5071F0 — copyLen clamp 200 @0x507255,
//  header @0x50727D..0x5072A0; NetPacket_WriteFixed180Block @0x507300 (180-byte
//  block, same 200 clamp); NapiNPServerMsg_HandleReplayRequest @0x515230 token
//  compare @0x51528B; NapiNPServerMsg_0x037_SendCircularBuffer @0x5152E0 @0x51533B]
constexpr uint32_t kTransferChunkBytes = 200;

std::vector<uint8_t> build_transfer_chunk(uint32_t transfer_id, const uint8_t *stream,
                                          uint32_t total, uint32_t offset) {
	const uint32_t clamped_offset = std::min(offset, total);
	const uint32_t copy_len = std::min(total - clamped_offset, kTransferChunkBytes);
	std::vector<uint8_t> reply(12, 0);
	write_u32_le(reply, 0, transfer_id);
	write_u32_le(reply, 4, total);
	write_u32_le(reply, 8, clamped_offset);
	reply.insert(reply.end(), stream + clamped_offset, stream + clamped_offset + copy_len);
	return reply;
}

// The [u32 id][u32 offset] body of a C2S 0x33/0x37 re-request. The retail
// handlers read both dwords and reset the offset to 0 when the token is not
// the live transfer id; a short body reads as {0, 0} (the first request is
// eight zero bytes). [orig: @0x515230 / @0x5152E0]
uint32_t chunk_request_offset(const std::vector<uint8_t> &payload, uint32_t live_transfer_id) {
	if (payload.size() < 8) return 0;
	const uint32_t token = static_cast<uint32_t>(payload[0]) |
			(static_cast<uint32_t>(payload[1]) << 8) |
			(static_cast<uint32_t>(payload[2]) << 16) |
			(static_cast<uint32_t>(payload[3]) << 24);
	const uint32_t offset = static_cast<uint32_t>(payload[4]) |
			(static_cast<uint32_t>(payload[5]) << 8) |
			(static_cast<uint32_t>(payload[6]) << 16) |
			(static_cast<uint32_t>(payload[7]) << 24);
	return token == live_transfer_id ? offset : 0u;
}

// tag=0x60 chunked server-info KV transfer: the authority's session-variable
// stream (host_session_vars). [orig: NapiNPClientMsg_HandleFileTransferChunk @0x432350;
// the stream Game_SerializeMissionInfoToDataStream @0x523620]
std::vector<uint8_t> build_tag60_server_info(const GameConfig &cfg, uint32_t transfer_id,
                                             uint32_t offset) {
	const std::vector<uint8_t> info_body = encode_session_vars(host_session_vars(cfg));
	return build_transfer_chunk(transfer_id, info_body.data(),
	                            static_cast<uint32_t>(info_body.size()), offset);
}

// tag=0x64 chunked mission-metadata transfer. [orig: NapiNPClientMsg_HandleMissionDataChunk (0x64) @0x432410]
std::vector<uint8_t> build_tag64_mission_metadata(
		const GameConfig &cfg, const MissionMetadataBlob *session_blob,
		uint32_t transfer_id, uint32_t offset) {
	MissionMetadataBlob fallback{};
	if (session_blob == nullptr) {
		fallback = build_mission_metadata_blob(cfg);
		session_blob = &fallback;
	}
	return build_transfer_chunk(transfer_id, session_blob->data(),
	                            static_cast<uint32_t>(session_blob->size()), offset);
}

// tag=0x57 RTT pong. [orig: NapiNPServerMsg_HandlePingResponse @0x515070 -> NetPacket_WriteInt32AndByte_0
// @0x5070c0]: when a client's C2S 0x2C carries echo_flag != 0, the host bounces the timestamp back as
// S2C 0x57 = [u32 timestamp][u8 0]. (A 0x2C with echo_flag == 0 is the client's return leg — the host
// then computes RTT + runs the min/max-ping kick; no reply.)
std::vector<uint8_t> build_tag57_pong(uint32_t timestamp) {
	std::vector<uint8_t> b;
	append_u32_le(b, timestamp);
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
		// Rows carry only IN-GAME players — a still-loading joiner (mid world-stream) is
		// excluded until its burst completes, matching the golden 31 B -> 39 B grow right
		// as the joiner enters the match [orig: the scoreboard rows exclude slots with the
		// not-yet-in-game byte slot+100579; Server_BuildAndBroadcastScoreboard @0x50D960].
		if (!is_in_match(c)) continue;
		// team @entity+344 read THROUGH owned_entity (D-NET-132); the World-less path defaults to 1.
		uint8_t team = 1;
		if (world != nullptr)
			if (const world::Entity *e = world->registry.get(c.link.owned_entity)) team = e->team;
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
				// The pair-list sort key is the SIGNED computed score, taken
				// before the u16 wire fold: team modes rank by the accumulated
				// points, others by the mode stat.
				// [orig: Player_ComputeScore @0x500A80, call @0x50DA10]
				computed_score = scoreboard.team_mode ? points : mode_stat;
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
	{
		const size_t count = rows.size();
		size_t gap = 1;
		while (gap <= count / 9)
			gap = 3 * gap + 1;
		for (; gap > 0; gap /= 3) {
			for (size_t i = gap; i < count; ++i) {
				ScoredPlayerListEntry insert = rows[i];
				size_t j = i;
				while (j >= gap && rows[j - gap].score < insert.score) {
					rows[j] = rows[j - gap];
					j -= gap;
				}
				rows[j] = insert;
			}
		}
	}
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
	// slot past its load (slot+100579 clear), the host's own slot (+5) counted
	// only by a session peer — a dedicated host carries no own connection
	// here, so the walk reduces to the in-match test.
	// [orig: Server_BuildAndBroadcastScoreboard @0x50dd5b..0x50dda9 ->
	//  packet dword 1031, serialized as the trailer's first byte @0x504cda]
	frame.in_game_count = static_cast<uint8_t>(std::min<size_t>(
			static_cast<size_t>(std::count_if(roster.begin(), roster.end(),
					[](const NapiNPConnection &c) { return is_in_match(c); })),
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

// tag=0x1E GAME-EVENT ev 0x3A (58) — the private deploy-screen frontier hint: "go capture zone N".
// attacker byte = the requester team's frontier zone number [orig: Server_ProcessPlayerDeath
// @0x517740 — GameEvent_BuildPayload(0x3A, ZoneSlotChain_FindFrontierZone(team), 0xFF, 0xFF, 0, 0)
// @0x517A1D, mask 0x20; §5.26 8-B body via GameEvent_BuildPayload @0x5054E0]. The golden retail
// frame-82540 body was {0x3a, 0x04, ...} — the retail host's live frontier at that moment; a
// World-less host keeps that golden byte so the burst pins hold.
std::vector<uint8_t> build_tag_1e_frontier_hint(uint8_t frontier_zone) {
	return {0x3a, frontier_zone, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00};
}
std::vector<uint8_t> build_tag_1e_game_event_post_spawn() {
	return build_tag_1e_frontier_hint(0x04); // the golden frame-82540 byte (World-less fallback)
}

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
	// D-NET-132 / §6.9: the roster identity is read THROUGH link.owned_entity (the single binding) — the
	// wire handle off owned_entity.packed and the team off the live registry Entity (team @entity+344).
	// player_slot / player_name stay on conn.reply (roster order + the echoed ClientAuth.NA). A
	// World-less bind (bind_session_reply_player) stamps owned_entity with the bare wire handle, so the
	// handle resolves here and the team keeps its default.
	if (conn.link.owned_entity.valid()) {
		ctx.player_name = conn.reply.player_name;
		ctx.player_slot = conn.reply.player_slot;
		ctx.entity_handle = conn.link.owned_entity.packed;
		ctx.quality = conn.reply.client_quality;
		if (world != nullptr) {
			if (const world::Entity *e = world->registry.get(conn.link.owned_entity)) {
				ctx.team = e->team;
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

// tag=0x04 join/slot-assignment body (24 B) [orig: NetPacket_WriteSlotAssignment @0x502b30, sent from
// CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0]: [4x u32 netPlayer+60..72 stat dwords (0 on a fresh
// join)][u8 g_difficulty dword_24D2110][u8 player_slot (slot+20)][u8 slot capacity @0x24c0ca4][u32 0]
// [u8 team (slot+416)]. The retail reader skips the four leading dwords and the
// pad and stores bytes 16/17/18/23; byte 16 lands in the joiner's own
// dword_24D2110, the difficulty word its max-health and score tallies read.
// [orig: NapiNPClientMsg_SessionSlotConfig @0x425410 (@0x4254A8 byte 16);
//  Entity_GetMaxHealthWithDifficulty @0x43B8A0; Score_TallyKillByLocalPlayer @0x4FD160]
std::vector<uint8_t> build_tag04_slot_assignment(uint8_t difficulty, uint8_t player_slot,
                                                 uint8_t slot_capacity, uint8_t team) {
	std::vector<uint8_t> payload(24, 0);
	payload[16] = difficulty;    // [orig dword_24D2110 low byte @0x502B96]
	payload[17] = player_slot;   // the joiner's assigned slot index [orig slot+20]
	payload[18] = slot_capacity; // player-slot array capacity [orig `g_PlayerSlotCapacity` @0x24c0ca4]
	payload[23] = team;          // [orig slot+416]
	return payload;
}

// S2C 0x75 is the requesting player's live slot state, not a fixed
// "spectator flags" fixture. Byte 0 is the spectator/death-screen bit and byte
// 1 is playerSlot+416 (team), written straight into the retail client's
// byte_A85B48 loadout selector. [orig: NetPacket_SerializeHostEntityState (ex sub_510890) @0x510890;
// NapiNPClientMsg_SetSpectatorMode @0x4259E0]
std::vector<uint8_t> build_tag75_player_state(
		const NapiNPConnection &conn, const world::World *world) {
	uint8_t team = conn.assigned_team_valid ? conn.assigned_team : 0;
	if (world != nullptr && conn.link.owned_entity.valid()) {
		if (const world::Entity *entity = world->registry.get(conn.link.owned_entity))
			team = entity->team;
	}
	return {static_cast<uint8_t>(conn.link.spectator ? 0x01u : 0x00u), team};
}

// tag=0x02 GLB_JOIN admission boundary. [orig: NapiNPServerMsg_0x002 @0x512FD0 emits
// 0x01/0x7A/0x7B/0x03.] CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0 emits the later
// 0x03/settings/0x05/0x04/0x7B boundary via build_spawn_pump_metadata. Both bodies and the
// intervening packet boundary are witnessed by the PR #403 retail LAN capture:
//   0x00 (settings flag 0xA0) = CS-config update [orig: CNapiNPConnection_SendConfigUpdate @0x6286e0:
//        [u8 direction==0][u32 bit mask][u32 value per set bit]] — ours sets field idx 3 = 12 for both
//        directions, matching the golden.
//   0x03 = NetPacket_WriteWeaponRestrictionFlag @0x502ac0: [u8 1][u16 list-node count][u16 weapon mask
//        (netPlayer inner+76)] when restriction data exists, else [u8 0].
//   0x05 = NetPacket_WriteBoolTrue @0x502c00: exactly {0x01}.
//   0x04 = NetPacket_WriteSlotAssignment @0x502b30 (build_tag04_slot_assignment above).]
// The 0x04 carries THIS connection's live slot + the host's slot capacity + the assigned team —
// byte 17 lands in g_LocalPlayerSlotId (the client's own slot id) and byte 18 in g_MaxPlayerSlots
// (g_MaxPlayerSlots, the 0x46/0x22 roster-walk terminator): the prior hardcoded (1, 2, 2)
// capped every client's walk at slot 1, so a third player's slot never bound (D-NET-158).
void append_connection_settings(const GameConfig &cfg,
                                std::vector<ProtocolMessage> &out) {
	const uint32_t configured_holdoff = cfg.effective_send_holdoff_ticks();
	const uint8_t holdoff = static_cast<uint8_t>(
			configured_holdoff < 255 ? configured_holdoff : 255);
	out.push_back(make_protocol_message(
			hightag::CS_CONFIG_UPDATE,
			{0, 0x08, 0, 0, 0, holdoff, 0, 0, 0},
			PROTOCOL_MSG_FLAG_SETTINGS_UPDATE | PROTOCOL_MSG_FLAG_LEN8));
	out.push_back(make_protocol_message(
			hightag::CS_CONFIG_UPDATE,
			{1, 0x08, 0, 0, 0, holdoff, 0, 0, 0},
			PROTOCOL_MSG_FLAG_SETTINGS_UPDATE | PROTOCOL_MSG_FLAG_LEN8));
}

bool emit_admission_metadata(const GameConfig &cfg, NapiNPConnection &conn,
                             std::vector<NapiNPConnection> &roster,
                             world::World *world, std::vector<ProtocolMessage> &out) {
	const uint8_t capacity =
			static_cast<uint8_t>(cfg.total_player_slot_capacity()); // [orig @0x24c0ca4]
	const std::optional<uint8_t> player_slot =
			Server_ReservePlayerSlot(roster, conn, capacity);
	if (!player_slot.has_value()) return false;
	// Team is allocated with the roster slot, even though it is intentionally
	// not serialized until the later spawn-pump packet. Player-add consumes this
	// reservation, so the delayed 0x04 and the live entity cannot disagree.
	if (world != nullptr) {
		(void)Server_ReservePlayerTeam(
				cfg, /*is_in_session=*/true, roster, conn, *world);
	}

	// The both-direction send-holdoff dictation (H:0x00 mask 8 / CS field 3)
	// [orig: NapiNPServer_UpdateHoldoffTicks @0x4c5f40 sends @0x4c5fc0/@0x4c5fcb;
	// the golden NovaWorld capture carries 0x0C = 12 here]. OpenNova dictates
	// the resolved retail session period, unless the host supplied an explicit
	// GameConfig override (D-NET-197).
	const uint32_t configured_holdoff = cfg.effective_send_holdoff_ticks();
	append_connection_settings(cfg, out);
	conn.s2c_send_holdoff_ticks = clamp_send_holdoff_ticks(configured_holdoff);
	reset_s2c_send_holdoff_counter(conn);
	out.push_back(make_protocol_message(s2c::SYNC_STATE, {0x01, 0x00, 0x00, 0x00}));
	out.push_back(make_protocol_message(s2c::PLAYER_NAME, build_tag7a_pcid(cfg)));
	out.push_back(make_protocol_message(
			s2c::FULL_PLAYER_INFO, build_tag7b_session_summary(cfg, conn)));
	// [u8 1][u16 count=1][u16 mask=1] — the golden's live restriction record shape.
	out.push_back(make_protocol_message(s2c::SYNC_TICK, {0x01, 0x01, 0x00, 0x01, 0x00}));
	return true;
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

// The C2S 0x00 JOIN body's CD identity blob: the flat-TLV run is walked for
// the "CD" field, whose value packs [name\0][value\0] pairs back to back (the
// NovaWorld NAMEINFO / PCID / SQUADINFO / JOINTICKET cookies). A LAN join
// carries no CD and yields no pairs. [orig: NapiNPServer_HandlePlayerJoinMessage
//  @0x512AA0 "CD" -> the cookie buffer; KeyValueBuffer_FindValue @0x4C2A30]
std::vector<std::pair<std::string, std::string>> parse_join_identity_pairs(
		const std::vector<uint8_t> &payload) {
	std::vector<std::pair<std::string, std::string>> pairs;
	size_t pos = 0;
	while (pos < payload.size()) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(payload.data(), payload.size(), pos, field);
		if (next == kFlatTlvEnd || field.name.empty()) break;
		pos = next;
		if (!strutil::iequals(field.name, "CD")) continue;
		const uint8_t *cursor = field.value;
		const uint8_t *end = field.value + field.size;
		while (cursor < end) {
			const uint8_t *name_end = std::find(cursor, end, uint8_t{0});
			if (name_end == end) break;
			const uint8_t *value_end = std::find(name_end + 1, end, uint8_t{0});
			if (value_end == end) break;
			pairs.emplace_back(std::string(cursor, name_end), std::string(name_end + 1, value_end));
			cursor = value_end + 1;
		}
	}
	return pairs;
}

} // namespace

// The authority's session variables as its serializer writes them: the
// configured server name; the mission title, or the map filename for the
// stock Co-op selector (objective/waypoint Co-op, bit 0x20000, and every other
// mode keep MissionText's display title); the game type; the custom text; the
// map file; score.ini's EXP_FANFARE lo/hi thresholds (the joiner arms its
// kill / headshot tones only when lo is nonzero and lo < hi, so {0,0}
// silences them).
// [orig: Game_SerializeMissionInfoToDataStream @0x523620 -- SERVERNAME
//  <- g_ServerNameStr @0x5236b1, MISSIONNAME the coop arm @0x523758 else
//  [info]/title @0x5237df, GAMETYPE @0x5238c7, CUSTOMTEXT @0x523914,
//  MISSIONFILENAME @0x5239a6, EXP_FANFARE word_24C1170 @0x523a39;
//  NapiNPClientMsg_ScoreDeltaSound @0x42A0B0]
SessionVars host_session_vars(const GameConfig &cfg) {
	SessionVars vars;
	vars.server_name = cfg.server_name;
	vars.mission_name =
			game_type::is_stock_coop(cfg.game_type) ? cfg.mission_file : cfg.mission_name;
	vars.game_type = cfg.game_type;
	vars.custom_text = cfg.custom_text;
	vars.mission_file = cfg.mission_file;
	vars.exp_fanfare = cfg.exp_fanfare;
	return vars;
}

std::vector<ProtocolMessage> build_spawn_pump_metadata(
		const GameConfig &config, NapiNPConnection &conn,
		const std::vector<NapiNPConnection> &roster, world::World *world) {
	std::vector<ProtocolMessage> messages;
	const uint8_t capacity = static_cast<uint8_t>(
			config.total_player_slot_capacity());
	const std::optional<uint8_t> player_slot =
			Server_ReservePlayerSlot(roster, conn, capacity);
	if (!player_slot.has_value()) return messages;

	uint8_t team = 2; // only used by a World-less test seam
	if (world != nullptr) {
		const world::Entity *entity = conn.link.owned_entity.valid()
				? world->registry.get(conn.link.owned_entity)
				: nullptr;
		team = entity != nullptr
				? entity->team
				: Server_ReservePlayerTeam(
						config, /*is_in_session=*/true, roster, conn, *world);
	}

	// Exact CNapiServer_ProcessPendingPlayerSpawns ordering from the retail LAN
	// witness. 0x03 is the one-byte reset variant, distinct from the five-byte
	// restriction record returned by the C2S 0x02 handler.
	messages.push_back(make_protocol_message(s2c::SYNC_TICK, {0x00}));
	append_connection_settings(config, messages);
	messages.push_back(make_protocol_message(s2c::GAME_START_SIGNAL, {0x01}));
	// config_bytes[6] is the session's dword_24D2110 low byte (the S2C 0x08
	// record's seventh byte) — the same word the 0x04 hands the joiner.
	messages.push_back(make_protocol_message(
			0x04, build_tag04_slot_assignment(
					config.config_bytes[6], *player_slot, capacity, team)));
	messages.push_back(make_protocol_message(
			s2c::FULL_PLAYER_INFO, build_tag7b_session_summary(config, conn)));
	return messages;
}

std::vector<uint8_t> build_spawn_wave_status_body(
		const world::World &world, world::EntityHandle requester) {
	std::vector<uint8_t> body;
	const world::Entity *recipient = world.registry.get(requester);
	const uint8_t team = recipient != nullptr ? recipient->team : 0;
	const world::SpawnZoneRegistry zones = world.zones.build_spawn_zone_list();
	std::vector<const world::SpawnWaveEntry *> visible;
	for (const world::SpawnWaveEntry &entry : world.zones.spawn_waves.entries())
		if (entry.team == team && visible.size() < 0xFFu)
			visible.push_back(&entry);
	body.reserve(1 + visible.size() * 9);
	body.push_back(static_cast<uint8_t>(visible.size()));
	for (const world::SpawnWaveEntry *entry : visible) {
		const uint16_t zone = entry->zone.valid()
				? entry->zone.packed
				: uint16_t{0xFFFF};
		body.push_back(static_cast<uint8_t>(zone));
		body.push_back(static_cast<uint8_t>(zone >> 8));
		const int zone_index = world::spawn_zone_index_of(zones, entry->zone);
		const uint16_t wire_index = zone_index >= 0
				? static_cast<uint16_t>(zone_index)
				: uint16_t{0xFFFF};
		body.push_back(static_cast<uint8_t>(wire_index));
		body.push_back(static_cast<uint8_t>(wire_index >> 8));
		body.push_back(static_cast<uint8_t>(entry->queued.size()));
		const uint16_t countdown = entry->requester_countdown(requester);
		body.push_back(static_cast<uint8_t>(countdown));
		body.push_back(static_cast<uint8_t>(countdown >> 8));
		for (const world::EntityHandle member : entry->queued) {
			body.push_back(static_cast<uint8_t>(member.packed));
			body.push_back(static_cast<uint8_t>(member.packed >> 8));
		}
	}
	return body;
}

std::vector<ProtocolMessage> Server_ReleasePlayerDeployment(
		const GameConfig &config, NapiNPConnection &conn,
		world::World &world, world::EntityHandle target_zone) {
	std::vector<ProtocolMessage> replies;
	if (!conn.link.owned_entity.valid()) return replies;
	world::Entity *player = world.registry.get(conn.link.owned_entity);
	if (player == nullptr) return replies;
	// Server_ProcessPlayerDeath always releases the old seat before inspecting
	// a requested mobile spawn. If the requester was the vehicle's primary
	// occupant, this clears +0x170 and the subsequent mobile-spawn gate rejects.
	// [orig: Server_ProcessPlayerDeath @0x5177A9..0x5177DB]
	if (player->mounted)
		world.vehicles.detach(player->handle);
	const world::Entity *target = world.registry.get(target_zone);
	const bool mobile_spawn = target != nullptr && target->is_spawn_point &&
			target->item_type == 1;
	if (mobile_spawn) {
		world::VehicleSeatSelection selected;
		if (!target->primary_occupant.valid() ||
				!world::find_best_vehicle_seat(
						world, target_zone, player->handle, selected))
			return replies;
	}
	// The no-pick arm reads the slot's spectator latch/restore team beside the
	// team byte [orig: Server_ProcessPlayerDeath @0x517863 ->
	// Server_PositionPlayerForSpawn @0x50D17C..0x50D1C6].
	const world::SpawnSlotState slot_state{
			conn.link.spectator, conn.spectator_restore_team};
	world::SpawnPointResult pose = world::resolve_player_spawn_pose(
			world, player->handle, target_zone, conn.reply.player_slot,
			player->team, config.game_type, slot_state);
	// A medic revive lands the player on the pose the revive transaction
	// saved (its death position raised 0x4000) instead of a marker, and the
	// revive latch (+89932) skips the 620-tick spawn protection.
	// [orig: GameEvent_RevivePlayer @0x517DCD..0x517E09 saves; the deploy leg
	//  reads the latch @0x51791C]
	const bool revive_deploy = conn.reply.revive_pose_valid;
	if (revive_deploy) {
		player->position.x = opennova::io::fp16_16_to_float(conn.reply.revive_pos[0]);
		player->position.y = opennova::io::fp16_16_to_float(conn.reply.revive_pos[1]);
		player->position.z = opennova::io::fp16_16_to_float(conn.reply.revive_pos[2]);
		player->yaw = conn.reply.revive_yaw;
		player->pitch = conn.reply.revive_pitch;
		player->roll = conn.reply.revive_roll;
		conn.reply.revive_pose_valid = false;
	} else if (pose.found) {
		player->position = pose.position;
		player->yaw = pose.yaw;
		player->pitch = pose.pitch;
		player->roll = pose.roll;
	} else {
		// No authored marker: the deploy transaction restores the position
		// recorded by the previous Entity_ResetToSpawnState. Keeping this in the
		// shared release makes C2S, wave, and listen-host paths identical.
		// [orig: Server_ProcessPlayerDeath @0x517740 ->
		// Entity_ResetToSpawnState @0x4B9610; D-NET-66]
		player->position = player->spawn_position;
	}
	// The Co-op marker arm's chute bit and queued carrier mount survive the
	// spawn-state reset below (it clears only the dead bit).
	// [orig: Server_PositionPlayerForSpawn @0x50D424..0x50D45A]
	world::apply_spawn_point_latches(*player, pose);
	world::entity_reset_to_spawn_state(*player);
	if (world.tables.player.has_item_def && world.tables.player.item_hp != 0)
		player->health = world::retail_signed_i16(world.tables.player.item_hp);
	else if (player->health_max > 0)
		player->health = player->health_max;
	else
		player->health = 100;
	player->alive = true;
	// Spawn protection: every deploy of a non-bot slot whose revive latch (+89932)
	// is clear stores 620 authority ticks into entity+292 (the reset above never
	// touches it); the per-tick arm in Server_TickUpdate counts it down, the first
	// validated fire clears it, and Projectile_ProcessDamageOnTarget zeroes damage
	// while it is nonzero. The bot (+96483) and revive-latch 0-stores have no
	// roster feature to reach them here.
	// [orig: Server_ProcessPlayerDeath @0x517740 — 620 @0x517937/@0x517952/
	//  @0x517960; 0 @0x51790a (bot) / @0x51791c (revive)]
	player->damage_state = revive_deploy ? 0 : 620;
	if (world::AiEntity *motor =
			world.ai.for_handle(player->handle);
			motor != nullptr && motor->inf.active) {
		const int32_t motor_position[3] = {
				world::to_fixed(player->position.x),
				world::to_fixed(player->position.y),
				world::to_fixed(player->position.z),
		};
		world::infantry_respawn_snap(
				*motor, motor_position,
				world::bam_heading_from_mission_yaw_deg(player->yaw),
				player->health);
	}
	// [orig: Server_ProcessPlayerDeath @0x5178aa]
    if (player->handle == world.cached.local_player && world.local_player_state != nullptr)
        world.local_player_state->reset_for_new_round();
	// A deploy that is not a medic revive, of a slot that is not a spectator,
	// removes the devices the player placed in its last life, each through the
	// notifying removal, after the spawn-state reset and ahead of the loadout.
	// The devices stay armed from the death until this deploy.
	// [orig: Server_ProcessPlayerDeath @0x517740 — the revive latch test
	//  @0x5178C5, the spectator latch test @0x5178CD, the
	//  Entity_RemovePlacedDevicesByOwner call @0x5178D8, ahead of the
	//  PlayerSlot_InitWeaponsFromLoadout call @0x5178E1]
	if (!revive_deploy && !conn.link.spectator)
		world.commands.remove_placed_devices_by_owner(player->handle);
	conn.discard_pre_deploy_uplinks = true;
	// [orig: Server_ProcessPlayerDeath @0x517791 `and byte ptr [esi+15F38h], 0EFh`]
	conn.link.respawn_pending = false;
	world.match.set_player_respawn_pending(player->handle, false);
	conn.link.respawn_delay_seconds = 0;
	conn.link.spawn_target_hold_seconds = 0;
	conn.link.respawn_hold_armed = false;
	conn.link.downed_revive_seconds = 0;
	conn.link.medic_request_active = false;
	conn.link.death_cause_revivable = false;
	// The deploy leg overwrites the whole +89912 state byte (1, or 3 while the
	// pre-round timer runs: bit 1 is the pre-round loadout latch) and zeroes the
	// +356 armory cooldown. [orig: Server_ProcessPlayerDeath @0x517803/@0x517812;
	//  slot+0x164 = 0 @0x517900]
	conn.link.preround_loadout_latch = world.preround_delay_seconds != 0;
	conn.reply.frontier_hint_pending = false; // bits 0x04/0x08 of the same byte
	conn.reply.capture_nag_held = false;
	conn.link.armory_reuse_seconds = 0;
	conn.link.last_deploy_tick = world.logic_tick;
	conn.link.last_deploy_tick_valid = true;
	player->flags &= ~1u;
	if (mobile_spawn) {
		// Retail repeats FindBestSeatSlot after the reset, then requests the
		// authoritative attach against the returned root/child seat owner.
		// [orig: Server_ProcessPlayerDeath @0x517A3E..0x517A74]
		world::VehicleSeatSelection selected;
		if (world::find_best_vehicle_seat(
					world, target_zone, player->handle, selected))
			world.vehicles.attach_to_seat(player->handle, selected);
	}
	replies.push_back(make_protocol_message(
			0x5A, build_current_loadout_reply(
					conn.reply.last_loadout_reply, player->player_class)));
	replies.push_back(make_protocol_message(
			0x61, Server_RerollPlayerTickSeed(conn)));
	const uint8_t frontier = world.zones.frontier_zone(player->team);
	if (frontier != 0)
		replies.push_back(make_protocol_message(
				s2c::GAME_EVENT, build_tag_1e_frontier_hint(frontier)));
	return replies;
}

bool is_medic_recipient(const NapiNPConnection &candidate,
		const world::World &world, uint8_t team) {
	// The mask-0x580 group: bit 0x80 admits player-slot state 6/7 (deployed /
	// round ended — the in-match state; the ordinary death path never rewrites
	// slot+0x20), bit 0x100 compares the slot team byte, bit 0x400 the Medic
	// class attribute. No leg reads entity health or the Flags dead bit, so a
	// dead or respawn-pending same-team Medic still receives the record.
	// [orig: NapiNPServer_SendFiltered @0x4C87E0 — roster gate @0x4C889F,
	//  0x80 @0x4C8948..0x4C8953, 0x100 @0x4C896A..0x4C8977,
	//  0x400 @0x4C8990..0x4C89A8]
	if (!is_in_match(candidate) || candidate.link.transport == nullptr ||
			!candidate.link.owned_entity.valid())
		return false;
	const world::Entity *medic = world.registry.get(candidate.link.owned_entity);
	return medic != nullptr && medic->team == team &&
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
	const IntegrityChallengeProfile *integrity_profile =
			find_integrity_challenge_profile(config.integrity_profile);

	// The loadout gate (burst phase 7→8) is opened ONLY by the C2S 0x2F handler (case 0x2F below).
	// A prior auto-advance ("any C2S at phase 8") fired on stale handshake messages before the
	// client had processed 0x1A, defeating the pacing. The loopback (type-2) bypasses the gate.

	if (conn.admission_stage != GameAdmissionStage::Complete) {
		const ProtocolMessage *admission_message = nullptr;
		for (const ProtocolMessage &message : messages) {
			if (message.flags.settings_update || message.full_tag >= PROTOCOL_FULL_TAG_HIGH_BASE) continue;
			// Each leg is a distinct sequenced request/reply turn. Two gameplay records in one
			// datagram cannot be a valid 0x00 -> 0x01 or 0x01 -> 0x02 exchange because the client
			// has not received the intervening acknowledgement/challenge.
			if (admission_message != nullptr) {
				conn.admission_stage = GameAdmissionStage::Rejected;
				return {};
			}
			admission_message = &message;
		}
		// Header-only ACKs are part of the captured flow and do not advance or violate the FSM.
		if (admission_message == nullptr) return {};

		switch (conn.admission_stage) {
			case GameAdmissionStage::AwaitJoinRequest: {
				if (admission_message->tag != 0x00) {
					conn.admission_stage = GameAdmissionStage::Rejected;
					return {};
				}
				// The JOIN handler latches the spectator request before validation;
				// the local loopback bypasses the complete remote validator.
				// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512aa0;
				// Server_ValidatePlayerJoinRequest @0x5121ab..0x5121af -> @0x512a76 (conn+0x2E set = accept unvalidated)]
				if (conn.type != NapiNPConnection::kTypeClientSide) {
					conn.link.spectator = conn.join_spectator_request != 0;
					uint32_t reject_dpc = validate_join_environment(conn.join_environment);
					if (reject_dpc == 0)
						reject_dpc = validate_join_request(config, admission_message->payload);
					if (reject_dpc == 0)
						reject_dpc = validate_spectator_join(config, conn, roster);
					if (reject_dpc == 0)
						reject_dpc = validate_side_password(config, conn);
					if (reject_dpc != 0) {
						(void)stage_join_gate_reject(conn, reject_dpc);
						conn.admission_stage = GameAdmissionStage::Rejected;
						return {};
					}
				}
				replies.push_back(make_protocol_message(s2c::INIT, {}));
				conn.admission_stage = GameAdmissionStage::AwaitFormPost;
				return replies;
			}

			case GameAdmissionStage::AwaitFormPost: {
				if (admission_message->tag != 0x01 ||
				    admission_message->payload != std::vector<uint8_t>{0x00}) {
					conn.admission_stage = GameAdmissionStage::Rejected;
					return {};
				}
				std::vector<uint8_t> challenge = build_tag02_push(now_tick);
				conn.admission_padding_x = read_u32_le(challenge.data());
				conn.admission_padding_y = read_u32_le(challenge.data() + 4);
				replies.push_back(
						make_protocol_message(s2c::JOIN_PADDING_PROBE, std::move(challenge)));
				conn.admission_stage = GameAdmissionStage::AwaitPaddingEcho;
				return replies;
			}

			case GameAdmissionStage::AwaitPaddingEcho:
				if (admission_message->tag != 0x02 ||
				    !validates_padding_echo(conn, admission_message->payload)) {
					conn.admission_stage = GameAdmissionStage::Rejected;
					return {};
				}
				if (!st.admission_metadata_pushed) {
					if (!emit_admission_metadata(
								config, conn, roster, world, replies)) {
						conn.admission_stage = GameAdmissionStage::Rejected;
						return {};
					}
					st.admission_metadata_pushed = true;
					st.admission_completed_tick = now_tick;
				}
				conn.admission_stage = GameAdmissionStage::Complete;
				conn.self_id_seen = true;
				return replies;

			case GameAdmissionStage::Rejected:
				return {};
			case GameAdmissionStage::Complete:
				break;
		}
	}

	for (const ProtocolMessage &msg : messages) {
		// Protocol-control + settings-update frames are not gameplay messages (the original filters
		// these before the gameplay dispatch). [orig: dispatch_in_match_session_messages gate]
		if (msg.flags.settings_update || msg.full_tag >= PROTOCOL_FULL_TAG_HIGH_BASE) continue;

		switch (msg.tag) {
			case c2s::JOIN: // JOIN ack [orig: NapiNPServer_HandlePlayerJoinMessage @0x512AA0]
				conn.join_identity_pairs = parse_join_identity_pairs(msg.payload);
				replies.push_back(make_protocol_message(s2c::INIT, {}));
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
						preference, consumed) || preference.enabled;
				break;
			}
			case c2s::MEDIC_REQUEST: { // [orig: Server_BroadcastMedicRequest @0x515390]
				std::vector<ProtocolMessage> own = Server_HandleMedicRequest(
						inputs.medic_request_format, conn, roster, world);
				for (ProtocolMessage &m : own) replies.push_back(std::move(m));
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
						read_u32_le(msg.payload.data()) == st.time_sync_sequence &&
						st.time_sync_round_host_ms != 0) {
					const uint32_t client_ms =
							read_u32_le(msg.payload.data() + 4);
					if (st.time_sync_client_baseline_ms == 0)
						st.time_sync_client_baseline_ms = client_ms;
					if (st.time_sync_previous_client_ms == 0) {
						st.time_sync_previous_client_ms = client_ms;
					} else {
						st.time_sync_latest_client_ms = client_ms;
						st.time_sync_current_host_ms =
								host_milliseconds_for_logic_tick(now_tick);
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
				// after byte 4. Id 0xFF selects the whole WeaponSlotDef table;
				// any other id selects one AnimDef row. An explicit profile is
				// essential: an unknown corpus or uncovered individual row is
				// silence, never an invented CRC-zero source.
				// [orig: Server_ValidateWeaponCRC @0x501F70]
				if (integrity_profile == nullptr) break;
				const uint8_t id = msg.payload.empty() ? 0 : msg.payload[0];
				const uint32_t received = read_u32_le_lenient(msg.payload, 1);
				uint32_t source = 0;
				if (id == 0xFF) {
					source = integrity_profile->weapon_slot_table_crc;
				} else if (!integrity_profile->find_animation_definition_crc(
							id, source)) {
					break;
				}
				if (!weapon_integrity_reply_matches(
							id, source, st.integrity_weapon_crc_salt, received) &&
						stage_integrity_crc_punt(conn, "PUNT WCRC"))
					return {};
				break;
			}
			case c2s::CHECKSUM_REPLY: {
				// C2S 0x21 shares the lenient five-byte prefix, but casts its
				// index to signed char before the live AmmoDef-capacity gate.
				// The retail client commonly appends the echoed four-byte request
				// key; the host consumes only index+CRC and ignores that tail.
				// [orig: NapiNPServerMsg_HandleAntiCheatCRCCheck (0x21) @0x502050]
				if (integrity_profile == nullptr) break;
				const uint8_t raw_index =
						msg.payload.empty() ? 0 : msg.payload[0];
				const int32_t signed_index = raw_index < 0x80
						? static_cast<int32_t>(raw_index)
						: static_cast<int32_t>(raw_index) - 0x100;
				if (signed_index < 0 ||
						static_cast<uint16_t>(signed_index) >=
								integrity_profile->ammo_definition_count)
					break;
				uint32_t source = 0;
				if (!integrity_profile->find_ammo_definition_crc(
							raw_index, source))
					break;
				const uint32_t received = read_u32_le_lenient(msg.payload, 1);
				if ((source ^ st.integrity_ammo_crc_salt) != received &&
						stage_integrity_crc_punt(conn, "PUNT ACRC"))
					return {};
				break;
			}
			case c2s::JOIN_FORM_POST: // handshake push -> S2C 0x02 GLB_JOIN push [orig: NapiNPServerMsg_HandleJoinRequest @0x512ED0]
				// Golden round-trip (f131-134): C 0x01 -> S 0x02 push -> C 0x02 -> S post-handshake
				// burst. The retail client's join-FSM verification (state 6->7, 0x424740) needs this
				// SEQUENCED exchange — a collapsed all-at-once burst leaves it stuck at "Verifying".
				replies.push_back(make_protocol_message(s2c::JOIN_PADDING_PROBE, build_tag02_push(now_tick)));
				break;
			case c2s::JOIN_PADDING_ECHO: // duplicate GLB_JOIN reply after admission
				// The first valid echo is consumed by the admission FSM above. A synthetic
				// caller entering here gets the same one-shot metadata; spawn metadata and
				// roster remain tick-owned boundaries.
				if (!st.admission_metadata_pushed) {
					if (!emit_admission_metadata(
								config, conn, roster, world, replies)) {
						conn.admission_stage = GameAdmissionStage::Rejected;
						return {};
					}
					st.admission_metadata_pushed = true;
					st.admission_completed_tick = now_tick;
				}
				break;
			case c2s::PLAYER_SYNC_REQUEST: { // player-sync request [u8 slot][u16 fieldFlags] -> S2C 0x46 player-sync.
				// [orig: NapiNPServerMsg_0x022 @0x514C90; §5.33] The client requests a SPECIFIC slot
				// (body byte 0) + a fieldFlags word (bytes 1-2); reply 0x46 for THAT slot so the joiner
				// binds it. The ACK-WALK is CLIENT-driven: the server ECHOES bit 0x4000 from the request
				// into the reply (NetPacket_SerializePlayerSync0x46 @0x505f05 `if (fieldFlags &
				// 0x4000) adjusted |= 0x4000`), and the CLIENT, on seeing the echoed ack, re-requests
				// slot+1 with fieldFlags 0x5CF7 until slot+1 >= its max-player count g_MaxPlayerSlots
				// (NapiNPClientMsg_PlayerSync @0x431370 tail) — the server never terminates the walk.
				// An inactive slot / NULL entity forces a 0x8000 removal reply (@0x505ecb..0x505ee0).
				// Without the walk the joiner only ever binds its OWN slot, leaving other players (the
				// host's serve-and-play player) unbound (residual 0x0F flood grill 2026-07-01).
				const uint8_t req_slot = msg.payload.empty() ? rep.player_slot : msg.payload[0];
				const uint16_t req_flags =
						msg.payload.size() >= 3
								? static_cast<uint16_t>(msg.payload[1] | (msg.payload[2] << 8))
								: 0;
				bool slot_has_player = false;
				for (const NapiNPConnection &c : roster) {
					if (c.phase >= ConnectionPhase::PlayerAdded && c.link.owned_entity.valid() &&
					    c.reply.player_slot == req_slot) {
						slot_has_player = true;
						break;
					}
				}
				// Answer EXACTLY the requested fieldFlags — ack bit included [orig:
				// NetPacket_SerializePlayerSync0x46 @0x505E80 serializes the request mask
				// verbatim; ack echo @0x505f05]. The retail client stops the walk itself at
				// its max-player count g_MaxPlayerSlots (fed by our 0x04 slot-config byte 18), so
				// no server-side cap. A zero/absent mask answers the 0x1CF7 field set.
				const bool echo_ack = (req_flags & kPlayerSyncAck) != 0;
				const uint16_t reply_flags =
						(req_flags & static_cast<uint16_t>(~(kPlayerSyncAck | kPlayerSyncRemoval))) != 0
								? static_cast<uint16_t>(req_flags & ~kPlayerSyncRemoval)
								: static_cast<uint16_t>(kPlayerSyncJoinBroadcastFields |
										(echo_ack ? kPlayerSyncAck : 0u));
				if (slot_has_player) {
					const PlayerReplicationState prs = rep_for_slot(config, roster, req_slot, rep, world);
					replies.push_back(
							make_protocol_message(s2c::PLAYER_SYNC, encode_player_sync(prs, reply_flags)));
				} else {
					// Empty/disconnected slot: the 0x8000 removal record (|0x4000 when the
					// request asked for the ack) — the walk terminator [orig: @0x505ecb..ee0].
					replies.push_back(
							make_protocol_message(s2c::PLAYER_SYNC, encode_player_sync_removal(req_slot, echo_ack)));
				}
				break;
			}
			case c2s::PING: // re-broadcast entity-state request -> 0x75 [orig: handler @0x510ED0]
				replies.push_back(make_protocol_message(
						s2c::SPECTATOR_FLAGS,
						build_tag75_player_state(conn, world)));
				break;
			case c2s::FILE_CHUNK_REQUEST: // server-info request -> 0x60 chunk [orig: NapiNPServerMsg_HandleReplayRequest (0x33) @0x515230]
				replies.push_back(make_protocol_message(
						s2c::FILE_TRANSFER_CHUNK,
						build_tag60_server_info(
								config, inputs.server_info_transfer_id,
								chunk_request_offset(msg.payload, inputs.server_info_transfer_id))));
				break;
			case c2s::MISSION_CHUNK_REQUEST: // mission-file request -> 0x64 chunk only [orig: NapiNPServerMsg_0x037_SendCircularBuffer @0x5152E0]
				replies.push_back(make_protocol_message(
						s2c::MISSION_DATA_CHUNK,
						build_tag64_mission_metadata(
								config, inputs.mission_metadata_blob,
								inputs.mission_metadata_transfer_id,
								chunk_request_offset(msg.payload, inputs.mission_metadata_transfer_id))));
				// The retail 00TRg join tail shares one send boundary:
				// queued 0x75, this 0x64, then the initial 0x16. Publishing the
				// roster on the preceding host tick makes the client request an
				// early 0x46 repair and changes both ordering and packet grouping.
				if (conn.reply.spawn_metadata_pushed &&
				    !conn.reply.roster_pushed) {
					replies.push_back(build_player_list_message(
							config, roster, world));
					conn.reply.roster_pushed = true;
					conn.reply.roster_completed_tick = now_tick;
				}
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
				// The armory-window gate runs BEFORE the body is parsed: a joined (state-10)
				// slot whose pre-round latch is clear, whose entity is neither DEAD (0x2)
				// nor inside an ARMORY zone (0x400000), and no pre-round timer, is answered
				// with its CURRENT slot list and nothing is read or written. A dead player
				// re-arms from the death screen; an alive one only from an armory volume;
				// the pre-round window and the first post-join/deploy submit (the latch)
				// bypass it. [orig: NapiNPServerMsg_HandlePlayerLoadout @0x51581c..0x51583e
				//  -> Server_SendWeaponSlotListToPlayer]
				if (!conn.link.spectator && is_in_match(conn) &&
				    !conn.link.preround_loadout_latch && loadout_player != nullptr &&
				    ((loadout_player->flags | loadout_player->engine_flags) &
				     (world::kEntityFlagDead | world::kEntityFlagArmoryZone)) == 0u &&
				    world->preround_delay_seconds == 0) {
					replies.push_back(make_protocol_message(
							0x5A, build_current_loadout_reply(
										  st.last_loadout_reply,
										  current_player_class(conn, world))));
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
				if (!conn.link.spectator &&
				    !loadout_envelope_accepted(req, config.game_type)) {
					replies.push_back(make_protocol_message(
							0x5A, build_current_loadout_reply(
										  st.last_loadout_reply,
										  current_player_class(conn, world))));
					break;
				}
				// The armory-reuse cooldown: a nonzero soldier type is accepted only while
				// playerSlot+356 has expired or the pre-round timer runs; otherwise the same
				// current-list re-send. Class 0 skips this test. The per-tick decrement is
				// tick_respawn_holds. [orig: @0x5158d0 -> @0x515fa5]
				if (!conn.link.spectator && req.player_class != 0 &&
				    conn.link.armory_reuse_seconds > 0 &&
				    (world == nullptr || world->preround_delay_seconds == 0)) {
					replies.push_back(make_protocol_message(
							0x5A, build_current_loadout_reply(
										  st.last_loadout_reply,
										  current_player_class(conn, world))));
					break;
				}
				const GrantedWeaponLoadout grant = conn.link.spectator
						? GrantedWeaponLoadout{}
						: grant_weapon_loadout(req, config.class_allow_mask, armory);
				// Retain the GRANTED body: the deploy-release bundle re-sends it (the client's
				// 0x5A apply is the deploy un-latcher — resets dword_81474C; §5.30, D-NET-156).
				st.last_loadout_reply = encode_weapon_loadout(grant.reply);
				st.ammo_pools = grant.ammo_pools;
                st.shared_clips = grant.shared_clips;
				// The rebuilt host-side slot table: every granted combo with its drawn clip
				// replaces the previous rows (the 0x06 pipeline used to seed a full clip on
				// first fire; the accept now owns the rows retail's rebuild leaves behind).
				// A table-less host keeps the lazy first-fire seed.
				// [orig: WeaponSlotPool reset + WeaponSlotTable_LoadAllFromDefs +
				//  WeaponSlots_RecalculateAmmoFromCapacity @0x515db5..0x515f4d]
				if (armory != nullptr) {
					conn.weapon_slots.clear();
					for (const GrantedWeaponLoadout::Row &row : grant.rows) {
						WeaponSlotState &slot = conn.weapon_slots[row.combo];
						slot.adm_index = row.adm_index;
						slot.clip = row.clip;
					}
					// The accept points the entity's EquippedSlot at the submitted slot and
					// keeps it only when that slot's def is NoSelect.
					// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515f52..0x515f8a]
					conn.equipped_slot = {};
					const auto submitted = req.weapon_slot_index <= 0xFFFFu
							? conn.weapon_slots.find(static_cast<uint16_t>(req.weapon_slot_index))
							: conn.weapon_slots.end();
					const world::WeaponTableEntry *submitted_def = submitted != conn.weapon_slots.end()
							? armory->by_index(submitted->second.adm_index)
							: nullptr;
					if (submitted_def != nullptr &&
							(submitted_def->flags2 & world::weapon_flag2::kNoSelect) != 0) {
						conn.equipped_slot.kind = NapiNPConnection::EquippedSlotRef::kPersonal;
						conn.equipped_slot.combo = submitted->first;
					}
				}
				replies.push_back(make_protocol_message(s2c::WEAPON_LOADOUT, st.last_loadout_reply));
				// Accepted soldier type -> entity+660 playerClass [orig: @0x515ab0] — feeds the
				// §5.10 field-17 class nibble and the 0x0C/0x18 spawn records for this player.
				if (loadout_player != nullptr) {
					world::Entity *pe = loadout_player;
					pe->player_class = grant.reply.avatar_class;
					pe->carry_flags = (pe->carry_flags & ~0x18u) | grant.carry_flags;
					// The fourth accepted byte is copied to the player-slot table at
					// [ammoDef.index]. Weapon_CalcImpactDamage later interprets 1 as
					// x0.9 and 2 as x1.1 for person targets.
					if (armory != nullptr) {
						pe->ammo_damage_class.assign(world->tables.ammo.entries.size(), 0);
						for (const auto &entry : grant.ammo_damage_classes) {
							const size_t ammo_index = static_cast<size_t>(entry.first);
							if (ammo_index < pe->ammo_damage_class.size())
								pe->ammo_damage_class[ammo_index] = entry.second;
						}
					}
				}
				// The accept re-arms the armory cooldown unless the pre-round latch is
				// set, then clears the latch — class 0 included. A spectator never reaches
				// retail's accept (its handler returns before the parse), so its empty
				// grant seeds nothing. [orig: @0x515b96..0x515bb3 — `if (!(byte & 2))
				//  slot[89] = armoryReuseTime_A38; byte &= 0xFD`]
				if (!conn.link.spectator) {
					if (!conn.link.preround_loadout_latch)
						conn.link.armory_reuse_seconds =
								static_cast<int32_t>(config.armory_reuse_time);
					conn.link.preround_loadout_latch = false;
				}
				st.loadout_synced = true;
				conn.burst.loadout_received = true;
				// Every accepted loadout re-stamps every player's live kit weight
				// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515f9d ->
				//  Server_RecalculateAllPlayerScores @0x5014E0].
				if (world != nullptr)
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
				// dword_81474C (the deploy gate) via NapiClient_WaitForGameStart @0x42cc10,
				// re-blocking deploy after 0x0F cleared it. The 0x16 pushes are proactive
				// (post-handshake + periodic during world-stream), not reactive to 0x0A.
				// [orig: NetPacket_WriteTimestampB @0x5046f0 -> S2C 0x19 @0x5132f1]
				replies.push_back(make_protocol_message(s2c::SPAWN_ACK_TIMESTAMP, build_tag1a_tick(now_tick)));
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
			case c2s::RESPAWN_REQUEST: { // RESPAWN/DEPLOY request [i16 spawnHandle] — the deploy-map pick.
				// [orig: Server_ProcessClientRequestRespawn @0x519AF0; net-re §5.61]. 0xFFFE = the
				// auto frontier pick; a real handle resolves through the SpawnPoint/team gates and a
				// NUMBERED zone additionally requires team match + control >= 1.0 (a contested zone
				// stops accepting spawns). An ALIVE in-session player's request is a no-op [orig: the
				// @0x519cce dead-or-flagged gate; only !is_in_session falls through @0x519cd7]; a DEAD
				// one deploys at the selected zone's wave boundary, or NOW when that zone has no
				// wave row: position at the pick (including §5.61's shared-counter 6007 scatter;
				// only the runtime-named model userpoint remains open) else the per-team marker chain,
				// reset-to-spawn-state, template health —
				// Server_ProcessPlayerDeath's deploy leg [orig: @0x517740 -> Server_PositionPlayerForSpawn
				// @0x50cf60 -> Entity_ResetToSpawnState @0x4B9610]. Deploy-time 0x61 seed re-send and
				// the 0x1D overlay stay burst-only (tracked §5.61 deferral). A newly queued pick
				// receives requester-local unreliable 0x6E immediately; immediate and wave release
				// share Server_ReleasePlayerDeployment. [orig: SpawnWaveList_TryQueuePlayer
				// @0x52A490; Server_SendSpawnWaveStatusToPlayer @0x50FF10]
				if (!conn.burst.spawned) break;
				if (world == nullptr || !conn.link.owned_entity.valid()) {
					// World-less/unit-test path: the golden frame-82540 hint byte, as before.
					replies.push_back(
							make_protocol_message(s2c::GAME_EVENT, build_tag_1e_game_event_post_spawn()));
					break;
				}
				world::Entity *player = world->registry.get(conn.link.owned_entity);
				if (player == nullptr) break;
				// Body: [i16 spawnHandle]; a short/absent body reads 0 [orig: @0x519b42..48].
				const uint16_t pick = msg.payload.size() >= 2
						? static_cast<uint16_t>(msg.payload[0] | (msg.payload[1] << 8))
						: 0;
				const world::Entity *target = nullptr;
				if (pick == world::kDeployPickAutoTeam) {
					// Conquer & Control rejects the auto pick outright, before the frontier
					// resolve: no deploy, no wave queue, no reply.
					// [orig: Server_ProcessClientRequestRespawn @0x519bc8..0x519bd4]
					if (config.game_type == game_type::kConquerAndControl) break;
					// Auto-deploy: the team's frontier zone; null falls back to the marker chain
					// [orig: Spawn_FindEntityForTeam @0x4fc810 -> requestedHandle -1 on miss].
					target = world->zones.find_spawn_zone_for_team(player->team, config.game_type);
				} else if (pick != world::kDeployPickNone) {
					// Only 0xFFFF is the no-target (Default Spawn) pick. Handle 0 — and the
					// absent-body default above — resolves pool 0 index 0 through the ordinary
					// spawn-point attrib/team test like any other handle, so it deploys only
					// when that entity is a same-team spawn point (a player never is).
					// [orig: @0x519c58..0x519c88 resolve then return on null;
					//  Server_ResolveSpawnTargetHandle @0x4FE110 — pool/index decode
					//  @0x4fe13f..0x4fe159, attrib 0x40000 @0x4fe16f, team @0x4fe181]
					target = world->zones.resolve_spawn_target(player->team, pick);
					if (target == nullptr) break; // invalid pick: silent no-op [orig: @0x519c88]
				}
				// +364 is tested only after the requested handle resolves to a
				// real target. Default Spawn, and an auto pick with no frontier,
				// bypass it. [orig: Server_ProcessClientRequestRespawn @0x519c67]
				if (target != nullptr && conn.link.spawn_target_hold_seconds != 0)
					break;
				if (target != nullptr) {
					// The zone-ownership gate [orig: @0x519d5f: entity+538 -> team match AND
					// control(+540) >= 0x10000].
					if (target->zone_number != 0 &&
					    (target->team != player->team || target->zone_control < 0x10000))
						break;
				}
				// Retail's `nodefaultspawnpoints` option is named after its UI,
				// not its actual predicate. It applies only to a target-less pick
				// and walks SpawnZoneList for an unnumbered or fully controlled
				// same-team zone; it never counts living teammates.
				// [orig: Server_ProcessClientRequestRespawn @0x519C8E..0x519CB2;
				// Entity_HasAliveEntityOfTeam @0x4FC7B0]
				if (config.default_spawn_requires_no_team_zone != 0 &&
						target == nullptr &&
						world->zones.team_has_available_spawn_zone(player->team))
					break;
				// The dead-or-pending gate [orig: @0x519cc7 — requester must be dead
				// (entity+36 & 2) OR respawn-flagged (slot+89912 & 0x10)]: an alive DEPLOYED
				// player's request is a no-op; an alive-but-undeployed joiner deploys now.
				if (!conn.link.respawn_pending && player->health > 0) break;
				// +360 follows the dead-or-pending test and silently rejects every
				// pick, including Default Spawn. [orig: @0x519cf2]
				if (conn.link.respawn_delay_seconds != 0) break;
				if (target != nullptr && target->item_type == 1 &&
						target->is_spawn_point) {
					// A vehicle target is selectable only while alive and while its
					// weighted root/child seat walk finds a free slot. The separate
					// +0x170 primary-occupant gate belongs to release, below.
					// [orig: Server_ProcessClientRequestRespawn @0x519D07..0x519D34]
					world::VehicleSeatSelection selected;
					if (!world::find_best_vehicle_seat(
							*world, target->handle, player->handle, selected))
						break;
				}
				if (target != nullptr) {
					if (world->zones.spawn_waves.try_queue(
							*world, target->handle, player->handle)) {
						ProtocolMessage status = make_protocol_message(
								s2c::SPAWN_WAVE_STATUS,
								build_spawn_wave_status_body(*world, player->handle));
						status.reliable = false;
						replies.push_back(std::move(status));
						break;
					}
					if (world->zones.spawn_waves.has_entry(target->handle)) break;
				}
				world->zones.spawn_waves.remove_player(player->handle);
				const world::EntityHandle target_handle = target != nullptr
						? target->handle
						: world::EntityHandle{};
				std::vector<ProtocolMessage> deployment =
						Server_ReleasePlayerDeployment(
								config, conn, *world, target_handle);
				replies.insert(replies.end(),
				               std::make_move_iterator(deployment.begin()),
				               std::make_move_iterator(deployment.end()));
				break;
			}
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
							host_milliseconds_for_logic_tick(now_tick),
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
						host_milliseconds_for_logic_tick(now_tick), *world);
				for (ProtocolMessage &m : chat) replies.push_back(std::move(m));
				break;
			}
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
			case c2s::SPECTATOR_RESPAWN: { // [orig: Server_ProcessClientRequestSpectatorRespawn @0x51C840]
				// Gates in order: no spawn gate, authority, permanent death on, a
				// spectator capacity, a player, not already a spectator; then
				// Server_KillPlayerAndNotify(player, packet i16, 1). The conversion
				// needs the owning context, so it is staged for the host tick.
				if (inputs.server_ctx == nullptr || !inputs.server_ctx->is_authority) break;
				if (!config.permanent_death || config.spectator_slots == 0) break;
				if (!conn.link.owned_entity.valid() || conn.link.spectator) break;
				st.spectator_convert_killer = msg.payload.size() >= 2
						? static_cast<int16_t>(msg.payload[0] | (msg.payload[1] << 8))
						: int16_t{0};
				st.spectator_convert_pending = true;
				break;
			}
			case c2s::MISSION_FILE_STATUS: // mission-file status report [orig: NapiNPServerMsg_PlayerJoinRequest @0x51AB10]
				st.mission_status_received = true;
				break;
			case c2s::FIRED_ROUND: { // client fired round -> ammo authority + the S2C 0x0A tag-2 round echo
				// [orig: NapiNPServerMsg_0x006_ClientFiredRound @0x513310, slot lookup @0x513338]
				// Residual Server_ClientFiredRound compensation (D-NET-152): saved-live-pose
				// subtraction @0x50BBAC and moving-carrier re-anchor branch @0x50BC41.
				// Cease-fire and spectator gates precede the clock, ammo, and round
				// effects. Freshness is checked on this host, never on a joiner's
				// predicted-fire path [orig: Entity_FireWeaponAndSendPacket @0x42BD80].
				if (world == nullptr || !conn.burst.spawned ||
						world->rules.cease_fire || conn.link.spectator) break;
				// The host's own loopback fire is a net-path no-op [orig: @0x50c18d returns 0
				// for the local player — its fire already ran locally].
				if (conn.link.mode == replication::TransportMode::Loopback) break;
				ClientFiredRound fr;
				size_t fire_consumed = 0;
				if (!decode_client_fired_round(msg.payload.data(), msg.payload.size(), fr,
				                               fire_consumed))
					break;
				// Handle validity [orig: @0x513543], then anti-spoof: the claimed shooter must
				// BE this connection's own entity [orig: @0x51358d, doubled at @0x50bf37 -> -9].
				if (wire_handle::is_batch_end_sentinel(fr.shooter_handle))
					break;
				if (!conn.link.owned_entity.valid() ||
				    conn.link.owned_entity.packed != fr.shooter_handle)
					break;
				world::Entity *shooter = world->registry.get(conn.link.owned_entity);
				if (shooter == nullptr) break;
				if (!Server_AcceptsPlayerFireTick(conn, fr.current_tick, world->logic_tick,
						config.effective_send_holdoff_ticks())) break;

				if (((shooter->flags | shooter->engine_flags) & 0x102u) == 0x102u) break;
				const bool alt_fire = (fr.fire_flags & 0x01) != 0; // [orig: @0x50bb0d]
				// ADM + ammo authority — armory-fed hosts only (a table-less host accepts,
				// mirroring the 0x5A echo fallback, D-NET-141). Alt fire skips the adm lookup,
				// the clip, and the equipped mirror [orig: @0x50bb0f / the @0x50be2b alt path].
				if (!world->tables.weapons.empty() && !alt_fire) {
					const world::WeaponTableEntry *adm = world->tables.weapons.by_index(fr.adm_index);
					if (adm == nullptr) break; // [orig: "Tried to fire NULL wpn, %i" @0x50bb49]

                    if (world::weapon_fire_owner_status(*world, *shooter, adm, false) != 0)
                        break;
                    world::WeaponFsmInputs fire_inputs;
                    world::weapon_fire_environment_inputs(*world, *shooter, fire_inputs);
                    world::WeaponFsmDef fire_def = adm->action_fsm;
                    fire_def.flags = adm->flags;
                    fire_def.clip_capacity = adm->clipsize;
                    fire_def.ammo_cost = adm->ammo_class_count;
                    world::WeaponSlotState fire_slot;
                    const int pool = adm->ammo_class_id;
                    fire_slot.reserve = pool >= 0 && pool < 128 ? st.ammo_pools[pool] : 0;
                    fire_slot.clip = 1; // the real mounted/personal clip is checked below
                    if (!world::weapon_fsm_can_fire(fire_def, fire_slot, fire_inputs)) break;
					if (adm->clipsize != -1) {
						world::WeaponSlotState *mounted_slot = nullptr;
						uint8_t mounted_adm = 0xFF;
						if (shooter->use_gun_slot_swapped &&
								shooter->mount_target.valid()) {
							world::Entity *mount =
									world->registry.get(shooter->mount_target);
							if (mount != nullptr) {
								mounted_slot = world->vehicles.resolve_mounted_ammo_slot(*mount);
								if (mounted_slot == &mount->primary_weapon_slot) {
									mounted_adm = mount->primary_weapon_slot_adm;
								} else if (mounted_slot != nullptr) {
									world::Entity *parent =
											world->registry.get(mount->ground_target);
									if (parent != nullptr && mounted_slot ==
											&parent->primary_weapon_slot)
										mounted_adm = parent->primary_weapon_slot_adm;
								}
							}
							// A borrowed EquippedSlot with an invalid route is a reject,
							// never a fallback into the player's personal combo map.
							if (mounted_slot == nullptr || mounted_adm != fr.adm_index)
								break;
						}
						if (mounted_slot != nullptr) {
							if (static_cast<int16_t>(mounted_slot->clip) == 0) break;
							--mounted_slot->clip;
						} else {
							const uint16_t combo =
									uint16_t(adm->category) * 65u + adm->rank;
							WeaponSlotState &slot = conn.weapon_slots[combo];
							if (slot.adm_index != fr.adm_index) {
								slot.adm_index = fr.adm_index;
								slot.clip = adm->clipsize;
							}
							if (adm->ammo_bucket != 0) {
                                const uint32_t bucket = static_cast<uint32_t>(adm->ammo_bucket);
                                if (bucket >= st.shared_clips.size() ||
                                        bucket >= world->tables.weapons.ammo_class_names.size() ||
                                        st.shared_clips[bucket] == 0) break;
                                --st.shared_clips[bucket];
                            } else {
                                if (slot.clip == 0) break;
                                --slot.clip;
                            }
						}
					}
					// The accepted fire re-points the shooter's EquippedSlot: the borrowed
					// mount slot it fired through, else the personal slot of the fired
					// combo. [orig: Server_ClientFiredRound @0x50c1d4..0x50c28d]
					if (shooter->use_gun_slot_swapped && shooter->mount_target.valid()) {
						conn.equipped_slot.kind = NapiNPConnection::EquippedSlotRef::kMount;
						conn.equipped_slot.mount = shooter->mount_target;
					} else {
						conn.equipped_slot.kind = NapiNPConnection::EquippedSlotRef::kPersonal;
						conn.equipped_slot.combo = uint16_t(adm->category) * 65u + adm->rank;
					}
					// Primary fire mirrors the equipped weapon onto the entity
					// [orig: @0x50bd56 entity+688 = adm — the §5.10 off-16 source].
					shooter->equipped_adm_index = fr.adm_index;
				}
				// Stamp the claimed target on the shooter; the tag-2 serializer reads it LIVE
				// [orig: @0x50c2ad shooter+104->+12; NetPacket_SerializeRoundEvent @0x50485a].
				world::EntityHandle fire_target{};
				if (!wire_handle::is_batch_end_sentinel(fr.target_handle)) {
					const world::EntityHandle th{fr.target_handle};
					if (world->registry.get(th) != nullptr) fire_target = th; // [orig: @0x5135d2]
				}
				shooter->last_fire_target = fire_target;
				// A primary round re-enters through the adm fire action, whose
				// local pass scores the shot (scorer event 1) once it clears the
				// pose checks; alt fire returns through RoundData_AddRound first.
				// [orig: Server_ClientFiredRound @0x50BAA0 — alt @0x50BB0D, the
				//  event-1 call @0x50C727]
				if (!alt_fire) world->match.record_shot(*world, conn.link.owned_entity);
				// The validated round ends the shooter's spawn protection: an in-session
				// authority clears entity+292 for a non-spectator slot whose value is
				// nonzero. A rejected fire never reaches this store. The listen host's own
				// fire takes the same clear through Entity_FireWeaponAndSendPacket's
				// authority leg (world/player_weapon.cpp local commit).
				// [orig: Server_ClientFiredRound @0x50BAA0 @0x50c736..0x50c75d]
				if (world->rules.mp_session && shooter->damage_state != 0 &&
				    !conn.link.spectator)
					shooter->damage_state = 0;

				// Ring append [orig: RoundData_AddRound @0x4fdb40]. The ring stores the
				// PRE-SPREAD origin/direction — exactly the client's claimed fire pose
				// [orig: @0x4fdbce reads the request before RoundData_SpawnRound's spread].
				world::RoundEvent ev;
				ev.shooter_handle = fr.shooter_handle;
				ev.origin_x = fr.pos_x;
				ev.origin_y = fr.pos_y;
				ev.origin_z = fr.pos_z;
				ev.dir_yaw = int32_t(uint32_t(fr.dir_x) << 16);   // [orig: @0x513436]
				ev.dir_pitch = int32_t(uint32_t(fr.dir_y) << 16); // [orig: @0x513449]
				ev.shot_seq = fr.hit_part;   // [orig: word_B7C670 = hit_part @0x50c2ba -> ring+28]
				ev.mode_flags = fr.fire_flags; // [orig: ring+30 = the fire-mode byte @0x4fdcde]
				// Shooter fire-context composite [orig: ctx & 0x3F @0x50bd83, recombined
				// | (ctx >> 7) << 7 into roundParams[4] @0x50c7bd -> ring+31].
				ev.subtype = uint8_t((fr.extra_byte2 & 0x3F) | ((fr.extra_byte2 >> 7) << 7));
				ev.slot_byte = fr.misc_byte; // [orig: ring+32 <- fireRequest+80 @0x4fdcfc]
				ev.adm_index = fr.adm_index;
				world->out.rounds.add(ev);
				// The authoritative round spawns SYNCHRONOUSLY with the ring append
				// [orig: RoundData_AddRound @0x4fdb40 inline-calls RoundData_SpawnRound
				// @0x4ec0d0 — the ring is only the tag-2 fan-out log; §5.60]. Ammo = the
				// adm's load-time-resolved round_type (+4; +84 is the stat id); an
				// armory- or ammo-less host skips the sim (fire still echoes).
				if (!world->tables.ammo.empty()) {
					const world::WeaponTableEntry *fire_adm =
							world->tables.weapons.by_index(fr.adm_index);
					if (fire_adm != nullptr && fire_adm->ammo_index >= 0) {
						world::RoundSpawnParams rp;
						rp.owner = conn.link.owned_entity;
						rp.shooter_handle = fr.shooter_handle;
						rp.origin.x = opennova::io::fp16_16_to_float(fr.pos_x);
						rp.origin.y = opennova::io::fp16_16_to_float(fr.pos_y);
						rp.origin.z = opennova::io::fp16_16_to_float(fr.pos_z);
						rp.dir_yaw_bam = ev.dir_yaw;
						rp.dir_pitch_bam = ev.dir_pitch;
						rp.ammo_index = fire_adm->ammo_index;
						rp.adm_index = fr.adm_index;
						rp.shot_seq = fr.hit_part;
						rp.subtype = ev.subtype;
						// PowerThrow charge from fireRequest+80; RoundData_SpawnRound
						// scales velocity for bytes 1..254 [orig: @0x4ec5bb].
						rp.charge = fr.misc_byte;
						world->round_sim.spawn(*world, rp);
					}
				}
				// A rejected shot leaves this floor untouched. Alt fire stamps only
				// its tick; primary adds the baked ADM action interval.
				// [orig: NapiNPServerMsg_0x006_ClientFiredRound @0x513310, stamp @0x513740]
				if (conn.fire_tick_mode) {
					const auto *adm = world->tables.weapons.by_index(fr.adm_index);
					conn.fire_tick_floor = fr.current_tick +
							(!alt_fire && adm != nullptr ? adm->fire_interval_ticks() : 0u);
				}
				// No reactive reply — the echo rides the per-frame 0x0A fan (replication
				// select_round_events), reaching every OTHER in-match recipient.
				break;
			}
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
				// @0x541720, called @0x514F03 after the relay iff requester != local player.
				// Refund + ammo-pool clamp deferred -> refill to capacity @0x541811]. The wire
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
						world::WeaponSlotState *slot =
								world->vehicles.resolve_mounted_ammo_slot(*addressed_entity);
						uint8_t slot_adm = addressed_entity->primary_weapon_slot_adm;
						if (slot != nullptr &&
								slot != &addressed_entity->primary_weapon_slot) {
							world::Entity *parent = world->registry.get(
									addressed_entity->ground_target);
							if (parent == nullptr ||
									slot != &parent->primary_weapon_slot)
								slot = nullptr;
							else
								slot_adm = parent->primary_weapon_slot_adm;
						}
						const world::WeaponTableEntry *adm =
								world->tables.weapons.by_index(slot_adm);
						if (slot != nullptr && adm != nullptr &&
								adm->clipsize != -1)
							slot->clip = adm->clipsize;
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
							frec = replication::build_full_entity_spawn(*e, conn.link.owned_entity);
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
				// Count admitted player records only; pending handshake nodes are not in
				// byte_A7628C, and a Goodbye node has already left that registry.
				// [orig: NapiNPServerMsg_0x02D @0x502430 -> Server_BuildAndSerializeSessionStatus @0x4FC990 ->
				// Server_BuildStatusReport @0x530A60]
				uint32_t active_players = 0;
				for (const NapiNPConnection &candidate : roster) {
					if (candidate.phase >= ConnectionPhase::PlayerAdded &&
					    candidate.phase < ConnectionPhase::Goodbye)
						++active_players;
				}
				replies.push_back(make_protocol_message(
						s2c::SESSION_STATUS,
						serialize_session_status(
								config, inputs.session_uptime_ms,
								active_players, world)));
				break;
			}
			case c2s::EMPTY_SLOT_SWEEP_REQUEST: { // EMPTY-SLOT SWEEP REQUEST -> S2C 0x5D to the REQUESTER ONLY.
				// The client queues this inside its S2C 0x0F world-state-load reply burst
				// (@0x42e647). The host walks pool 0 and answers with the index of every
				// EMPTY entry (`entry_dword[7] == 0`); the client destroys whatever it
				// still holds in those slots (NapiNPClientMsg_DestroyEntityList
				// @0x429730). The handler reads NO fields from the request, is
				// authority-gated, and is SKIPPED while g_NetSpawnSuspended or
				// g_SpawnSuccessGate (round over) is set — our reachable analog of the
				// latter is world->match.outcome().ended.
				// Documented delta (D-NET-176, FIXED — entry (b) of its ledger row): retail
				// walks the pool's fixed entry count. Our
				// pool-0 capacity is an OpenNova sizing choice (1024 by default), so the
				// walk stops at the highest OCCUPIED slot: a slot above that high-water
				// mark was never streamed to any client, so listing it is a no-op that
				// would only inflate the datagram.
				// [orig: NapiNPServerMsg_SendEmptySlots @0x51a600; body builder Server_CollectEmptyPool0Slots @0x5160f0]
				std::size_t consumed = 0;
				if (world == nullptr ||
				    !decode_empty_slots_request(
						msg.payload.data(), msg.payload.size(), consumed) ||
				    world->match.outcome().ended) {
					break;
				}
				const std::size_t capacity = world->registry.pool_capacity(0);
				std::size_t high_water = 0;
				for (std::size_t slot = 0; slot < capacity; ++slot) {
					const world::EntityHandle h{static_cast<uint16_t>(slot)};
					if (world->registry.get(h) != nullptr) high_water = slot + 1;
				}
				DestroyEntityList sweep;
				for (std::size_t slot = 0; slot < high_water; ++slot) {
					const world::EntityHandle h{static_cast<uint16_t>(slot)};
					if (world->registry.get(h) == nullptr)
						sweep.pool0_indices.push_back(static_cast<uint16_t>(slot));
				}
				replies.push_back(make_protocol_message(
						0x5D, encode_destroy_entity_list(sweep)));
				break;
			}
			case c2s::VISIBLE_PLAYERS_REQUEST: { // §5.33 burst -> S2C 0x4C snapshot.
				// Reply shape witnessed across the six golden samples:
				// [u8 count] then count x {u8 playerSlot, u16le entityHandle} —
				// e.g. `01 01 01 00` (slot 1 -> entity 0x0001, LAN join) and
				// `02 00 67 00 01 68 00` (slots 0/1 -> 0x67/0x68, co-op join).
				// Every witnessed record maps a player slot to its own live
				// entity, and only in-world players appear (the idle host is
				// absent from the LAN samples). The server-side selection
				// beyond that is a residual witness — the builder inside
				// [orig: NapiNPServerMsg_0x023_WeaponOverlayBroadcast @0x514D50] is undecompiled;
				// list every admitted player with a bound entity.
				std::size_t consumed = 0;
				if (!decode_burst_visible_request(
						msg.payload.data(), msg.payload.size(), consumed))
					break;
				std::vector<uint8_t> snapshot;
				uint8_t count = 0;
				snapshot.push_back(0); // count backpatched below
				for (const NapiNPConnection &c : roster) {
					if (c.phase < ConnectionPhase::PlayerAdded ||
					    c.phase >= ConnectionPhase::Goodbye ||
					    !c.link.owned_entity.valid())
						continue;
					snapshot.push_back(c.reply.player_slot);
					const uint16_t handle = c.link.owned_entity.packed;
					snapshot.push_back(static_cast<uint8_t>(handle & 0xFFu));
					snapshot.push_back(static_cast<uint8_t>(handle >> 8));
					++count;
				}
				snapshot[0] = count;
				replies.push_back(make_protocol_message(
						s2c::TARGET_ASSIGNMENT, std::move(snapshot)));
				break;
			}
			case c2s::LOADOUT_REQUEST: { // §5.33 burst -> S2C 0x4E.
				// Every witnessed reply to the join-burst 0x28 is the 2-byte
				// sentinel `FF FF` (count 0xFFFF, no slots — nw_pp:
				// "batch-despawn count=65535 slots=0" in all three retail
				// goldens). Real kill batches ride the event-driven despawn
				// stream (D-NET-66), not this request reply; a non-sentinel
				// reply form is unwitnessed.
				// [orig: NapiNPServerMsg_HandleWeaponLoadoutRequest @0x51A550]
				BurstLoadoutRequest loadout_req;
				std::size_t consumed = 0;
				if (!decode_burst_loadout_request(
						msg.payload.data(), msg.payload.size(), loadout_req,
						consumed))
					break;
				replies.push_back(make_protocol_message(
						s2c::KILL_BY_SLOT, {0xFF, 0xFF}));
				break;
			}
			default:
				// 0x09 checksum / 0x48 + per-frame client updates: consumed (no reactive reply).
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

void broadcast_player_sync_on_join(const GameConfig &config,
                                   std::vector<NapiNPConnection> &roster,
                                   const NapiNPConnection &joined, const world::World *world) {
	// [orig: Server_PlayerAdd @0x51D296 — one 0x46 (fieldFlags 0x1CF7, no ack: the walk is the
	// JOINER's own concern) to every in-game connection]. Staged on each recipient's transport
	// like the other broadcast handlers (0x49 relay / the leave removal); the per-connection
	// flush frames it with that connection's own sequencing.
	const PlayerReplicationState rep = make_rep_state(config, joined, world);
	const std::vector<uint8_t> body = encode_player_sync(rep, kPlayerSyncJoinBroadcastFields);
	for (NapiNPConnection &c : roster) {
		if (&c == &joined || !is_in_match(c) || c.link.transport == nullptr) continue;
		c.link.transport->host_send(s2c::PLAYER_SYNC, body);
	}
	// Retail's host-client reads the server's player-slot table in-process, so
	// no self-0x46 exists on its wire. Our host's client view is fed over the
	// type-2 loopback instead (ADR 0011 §3; the D-NET-114 self-stream), and it
	// never runs the joiner's C2S 0x22 walk — carry the loopback player's OWN
	// sync so the host-side roster (D-HUD-24) binds its slot and its 0x16 rows
	// are accepted rather than dropped as unknown.
	if (joined.type == NapiNPConnection::kTypeClientSide && joined.link.transport != nullptr)
		joined.link.transport->host_send(s2c::PLAYER_SYNC, body);
}

} // namespace opennova::inmatch
