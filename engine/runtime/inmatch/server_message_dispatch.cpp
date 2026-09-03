#include <runtime/inmatch/server_message_dispatch.h>


#include <runtime/inmatch/integrity_challenge_profile.h>
#include <runtime/inmatch/end_round_protocol.h>
#include <net/npwire/nw_session_framing.h> // make_random_session_u32 (the per-player tick seed)
#include <runtime/inmatch/server_spawn.h> // Server_ReservePlayerTeam (0x04/spawn identity)
#include <runtime/inmatch/server_tick.h> // Server_StageHostDisconnect
#include <runtime/inmatch/session_status.h>
#include <runtime/world/weapon_table_build.h> // loadout_entry_permitted / resolve_loadout_ammo (D-NET-141)

#include <runtime/replication/entity_wire_bridge.h> // build_full_entity_spawn — the 0x0F -> 0x18 repair record

#include <base/gameprofile/game_type.h>       // is_waypoint_family / is_stock_coop (§5.32, D-NET-203/205)
#include <net/npwire/wire_handle.h>     // is_batch_end_sentinel / kInvalid (the wire handle home)
#include <net/npwire/ingame_decode.h>   // decode_entity_packet_sub_header / decode_player_extended_uplink
#include <net/npwire/ingame_encode.h>   // encode_player_sync / encode_player_list (§5.1)
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/replication_model.h> // PlayerReplicationState (POD) — the reply builders' input

#include <runtime/world/entity.h> // world::Entity / EntityHandle — team @entity+344 read through owned_entity
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
// CNapiNPConnection_SendChatMessage @0x4c7ef0 with both strings g_empty_str ->
// NapiNPDataTransfer_SendDescription @0x628c80]
bool stage_join_gate_reject(NapiNPConnection &conn, uint32_t reason_dpc) {
	DisconnectEvent event;
	event.ds = 1;
	event.dc = 2;
	event.dpc = reason_dpc;
	return Server_StageHostDisconnect(conn, event);
}

bool ascii_case_equal(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) !=
		    std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	}
	return true;
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
// (code 14), the `> g_spectator_slots` count @0x5123ca (code 15, type-1 rows
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
	if (!config.spectator_password.empty() &&
	    !ascii_case_equal(
				conn.join_spectator_password, config.spectator_password))
		return 16;
	return 0;
}

} // namespace

MissionMetadataBlob build_mission_metadata_blob(
		const GameConfig &config, bool is_mp_session_peer) {
	MissionMetadataBlob blob{};
	auto write_u32 = [&](std::size_t offset, uint32_t value) {
		blob[offset + 0] = static_cast<uint8_t>(value);
		blob[offset + 1] = static_cast<uint8_t>(value >> 8);
		blob[offset + 2] = static_cast<uint8_t>(value >> 16);
		blob[offset + 3] = static_cast<uint8_t>(value >> 24);
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
// Byte helpers (relocated verbatim from the retired game_session.cpp reply builders). The §5.1
// identity bodies (0x7A PCID, 0x7B session info) are now FAITHFUL ports of the witnessed serializers
// (NetPacket_WritePCID @0x5076e0, NapiNPMsg_0x7B_BuildPayload @0x507740; net-re §5.45, grilled
// 2026-06-27). The remaining reply bodies (0x46 NetPacket_SerializePlayerSync0x46 @0x505e80 —
// a flag-driven slot-state record, and 0x51 write_entity_packet @0x506bb0) carry approximations
// pending slot-state modeling; the witnessed field maps are landed in net-re §5.45 (D-NET-127).
// ---------------------------------------------------------------------------

void append_u16_le(std::vector<uint8_t> &out, uint16_t v) {
	opennova::io::append_u16_le(out, v);
}

void append_u32_le(std::vector<uint8_t> &out, uint32_t v) {
	opennova::io::append_u32_le(out, v);
}

void write_u32_le(std::vector<uint8_t> &out, size_t off, uint32_t v) {
	out[off + 0] = static_cast<uint8_t>(v & 0xFFu);
	out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
	out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
	out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint32_t read_u32_le(const uint8_t *data) {
	return opennova::io::read_u32_le(data);
}

bool read_join_string_tlv(const std::vector<uint8_t> &payload, std::size_t &offset,
		std::string &name, std::string &value) {
	const auto name_end = std::find(
			payload.begin() + static_cast<std::ptrdiff_t>(offset),
			payload.end(), uint8_t{0});
	if (name_end == payload.end()) return false;
	name.assign(
			reinterpret_cast<const char *>(payload.data() + offset),
			static_cast<std::size_t>(name_end - payload.begin()) - offset);
	offset = static_cast<std::size_t>(name_end - payload.begin()) + 1;
	if (offset + 2 > payload.size()) return false;
	const uint16_t value_size = static_cast<uint16_t>(
			payload[offset] | (static_cast<uint16_t>(payload[offset + 1]) << 8));
	offset += 2;
	if (value_size == 0 || offset + value_size > payload.size() ||
	    payload[offset + value_size - 1] != 0) {
		return false;
	}
	// String-valued join fields have one terminal NUL and no embedded NULs.
	if (std::find(
				payload.begin() + static_cast<std::ptrdiff_t>(offset),
				payload.begin() + static_cast<std::ptrdiff_t>(
						offset + value_size - 1),
				uint8_t{0}) !=
	    payload.begin() + static_cast<std::ptrdiff_t>(
				offset + value_size - 1)) {
		return false;
	}
	value.assign(
			reinterpret_cast<const char *>(payload.data() + offset),
			value_size - 1);
	offset += value_size;
	return true;
}

bool validates_join_request(
		const GameConfig &config, const std::vector<uint8_t> &payload) {
	std::size_t offset = 0;
	bool saw_expansion = false;
	bool saw_version_crc = false;
	while (offset < payload.size()) {
		std::string name;
		std::string value;
		if (!read_join_string_tlv(payload, offset, name, value)) return false;
		if (name == "EXP" && !saw_expansion && !config.expansion.empty() &&
		    value == config.expansion) {
			saw_expansion = true;
		} else if (name == "VERSIONCRCSTRING" && !saw_version_crc) {
			// Retail compares atol(value) against its own g_expansion_checksum ONLY
			// while an expansion is active; a base-game host stores the TLV without
			// looking at it [orig: Server_ValidatePlayerJoinRequest — the
			// g_ExpansionName[0] gate @0x51231e, the atol compare @0x512331, reject
			// DPC=48 @0x512341]. The checksum is CRC-32/MPEG-2 of the loose
			// expansion/<name>/version.txt, formatted "%ld" by the client — signed
			// decimal, so strtol matches retail's 32-bit atol on real inputs.
			if (!config.expansion.empty() &&
			    static_cast<int32_t>(std::strtol(value.c_str(), nullptr, 10)) !=
			            config.expansion_version_checksum) {
				return false;
			}
			saw_version_crc = true;
		} else {
			return false;
		}
	}
	return saw_version_crc &&
	       (config.expansion.empty() ? !saw_expansion : saw_expansion);
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

void append_kv(std::vector<uint8_t> &out, const char *name, const void *data, uint32_t len) {
	const size_t nlen = std::strlen(name);
	out.insert(out.end(), name, name + nlen);
	out.push_back(0);
	append_u32_le(out, len);
	const auto *bytes = static_cast<const uint8_t *>(data);
	out.insert(out.end(), bytes, bytes + len);
}

void append_string_kv(std::vector<uint8_t> &out, const char *name, const std::string &value) {
	append_kv(out, name, value.c_str(), static_cast<uint32_t>(value.size() + 1));
}

bool is_default_ash_session_config(const GameConfig &cfg) {
	return cfg.mission_name == "AS - Dormant Volcano Isle" && cfg.mission_file == "ASH_I5A.BMS";
}

// ---------------------------------------------------------------------------
// §5.1 reply bodies — handshake + server-info + mission-metadata (relocated from game_session.cpp).
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
// selection. Field 5 is always g_map_file_name.
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
	append_cstr(payload, cfg.server_name); // [orig g_server_name_str]
	append_cstr(payload, advertised_mission);
	append_cstr(payload, cfg.mission_file);// [orig g_map_file_name]
	append_u32_le(payload, cfg.game_type);  // [orig g_GameType]
	payload.push_back(0);                  // [orig g_empty_str] empty C string
	append_cstr(payload, cfg.expansion);   // [orig g_ExpansionName]
	return payload;
}

// tag=0x60 chunked server-info KV transfer. [orig: NapiNPClientMsg_HandleFileTransferChunk @0x432350]
std::vector<uint8_t> build_tag60_server_info(const GameConfig &cfg) {
	std::vector<uint8_t> info_body;
	append_string_kv(info_body, "SERVERNAME", cfg.server_name);
	// Retail's serialize_mission_info_to_datastream @0x523620 substitutes the
	// map filename only for the stock Co-op selector. Objective/waypoint Co-op
	// (bit 0x20000) and every other mode retain MissionText's display title.
	const bool uses_coop_filename = game_type::is_stock_coop(cfg.game_type);
	append_string_kv(
			info_body,
			"MISSIONNAME",
			uses_coop_filename ? cfg.mission_file : cfg.mission_name);
	uint8_t gametype_le[4] = {
			static_cast<uint8_t>(cfg.game_type & 0xFFu),
			static_cast<uint8_t>((cfg.game_type >> 8) & 0xFFu),
			static_cast<uint8_t>((cfg.game_type >> 16) & 0xFFu),
			static_cast<uint8_t>((cfg.game_type >> 24) & 0xFFu),
	};
	append_kv(info_body, "GAMETYPE", gametype_le, 4);
	append_string_kv(info_body, "CUSTOMTEXT", cfg.custom_text);
	append_string_kv(info_body, "MISSIONFILENAME", cfg.mission_file);
	const uint8_t exp_fanfare[] = {0, 0};
	append_kv(info_body, "EXP_FANFARE", exp_fanfare, 2);

	std::vector<uint8_t> reply(12, 0);
	reply[0] = 0x01; // offset=1.
	write_u32_le(reply, 4, static_cast<uint32_t>(info_body.size()));
	reply.insert(reply.end(), info_body.begin(), info_body.end());
	return reply;
}

// tag=0x64 chunked mission-metadata transfer. [orig: NapiNPClientMsg_HandleMissionDataChunk (0x64) @0x432410]
std::vector<uint8_t> build_tag64_mission_metadata(
		const GameConfig &cfg, const MissionMetadataBlob *session_blob) {
	MissionMetadataBlob fallback{};
	if (session_blob == nullptr) {
		fallback = build_mission_metadata_blob(cfg, true);
		session_blob = &fallback;
	}
	std::vector<uint8_t> reply(12 + session_blob->size(), 0);
	reply[0] = 0x01;
	write_u32_le(reply, 4, static_cast<uint32_t>(session_blob->size()));
	std::memcpy(reply.data() + 12, session_blob->data(), session_blob->size());
	return reply;
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
// joiner deploys). `roster` is the connection_list; `fallback` covers the World-less/test path (no bound
// players -> a single default entry). The wire SERIALIZE lives in encode_player_list (novaworld); this is
// just the npruntime-side roster walk (it reads NapiNPConnection, which novaworld cannot) that builds the
// entry list.
std::vector<uint8_t> build_reply_tag_16(const GameConfig &config,
	                                    const std::vector<NapiNPConnection> &roster,
                                        const PlayerReplicationState &fallback,
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
	if (frame.players.empty()) {
		PlayerListEntry row;
		row.slot = fallback.player_slot;
		row.team = fallback.team;
		frame.players.push_back(row);
	}
	frame.teams.resize(size_t(frame.team_count) + 1);
	for (size_t team = 1; team < frame.teams.size(); ++team) {
		const world::MatchLiveTeamScore &source = scoreboard.teams[team];
		frame.teams[team].score1 = static_cast<uint16_t>(source.primary_score);
		frame.teams[team].score2 = static_cast<uint16_t>(source.points);
		frame.teams[team].koth_hold = source.alive_players;
		frame.teams[team].ctf_flag = source.authored_objectives;
	}
	frame.in_game_count = static_cast<uint8_t>(
			std::min<size_t>(frame.players.size(), 0xFFu));
	frame.spectator_count = static_cast<uint8_t>(std::min<size_t>(
			std::count_if(rows.begin(), rows.end(),
					[](const ScoredPlayerListEntry &entry) {
						return entry.row.spectator;
					}),
			0xFFu));
	return encode_player_list(frame);
}

// tag=0x5A WEAPON-LOADOUT-SYNC, built from the joiner's own C2S 0x2F loadout submit.
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515790 — parses the request, validates the
// envelope (loadout_envelope_accepted), clamps the in-range soldier type to [5,9]-else-8
// (@0x515913), stamps entity+660 playerClass (@0x515ab0), loads the entries
// into the player's 780-slot weapon table, then Server_SendWeaponSlotListToPlayer @0x502550
// walks that table ascending by weapon-slot combo (category*65 + rank, @0x5026e5..@0x5028a0),
// filtering each slot by the team/char masks (@0x502716) and emitting one 4-byte group:
// [admIdx = AvatarDef_FindIndexByName @0x50273b][ammoPrimary = WeaponSlot_GetTotalClips
// @0x502794][ammoSecondary = the same count for the first different-ammoclass sub-variant in
// parent+1..parent+LSC (@0x5027c8), else 0xFF][per-ammo damage class from
// player+89688: 1 = x0.9, 2 = x1.1, other/default = 0].]
//
// With the armory table fed (world::World::weapons), the reply resolves REAL counts through the
// witnessed rules (weapon_table_build: loadout_entry_permitted / resolve_loadout_ammo) — the
// golden ASH_I5A reply {2:255, 3:10, 21:10, 76:1, 77:2, 78:3, 83:3} reproduces from the host's
// own resolved weapon.def (D-NET-141). Table-less hosts (unit paths / no resource root) keep the
// prior request-echo: the client clamps echoed bytes on apply [orig: @0x4295d7-0x4295e9], a
// tracked divergence for that configuration only. The accepted fourth byte is the
// player+89688 per-ammo damage class; captured 0xFF defaults normalize to 0.
struct GrantedWeaponLoadout {
	WeaponLoadout reply;
	// The serverPlayer+88664 authority pool image copied by S2C 0x0F. Each
	// accepted request writes its ammo class in wire order, so a later weapon
	// sharing that class wins exactly as retail does.
	std::array<int32_t, 128> slot_type_scores{};
	// Final player+89688 values, keyed by the resolved AmmoDef index. The retail
	// request walk overwrites this table in request order, so the last accepted
	// weapon using an ammo type controls every granted slot that uses that ammo.
	std::vector<std::pair<int16_t, uint8_t>> ammo_damage_classes;
};

uint8_t normalized_damage_class(uint8_t value) {
	return (value == 1 || value == 2) ? value : 0;
}

void set_ammo_damage_class(std::vector<std::pair<int16_t, uint8_t>> &classes,
						   int16_t ammo_index, uint8_t value) {
	if (ammo_index < 0) return;
	for (auto &entry : classes) {
		if (entry.first == ammo_index) {
			entry.second = value;
			return;
		}
	}
	classes.emplace_back(ammo_index, value);
}

uint8_t find_ammo_damage_class(const std::vector<std::pair<int16_t, uint8_t>> &classes,
							   int16_t ammo_index, uint8_t fallback) {
	if (ammo_index < 0) return fallback;
	for (const auto &entry : classes)
		if (entry.first == ammo_index) return entry.second;
	return 0;
}

// The 0x2F submission envelope, checked BEFORE anything is applied: the team byte must be 1..4 —
// above 4 passes only in a team-less game type — and a NONZERO class byte must be 5..9. Both header
// bytes are read SIGNED, so 0x80..0xFF is negative and fails the low bound. A failing envelope
// aborts the handler: retail re-sends the player's current slot list and writes nothing.
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x5158a9 (team) / @0x5158b1 -> @0x515fa5 (class).
// Retail additionally requires the armory timer player+356 to have expired before it accepts a
// nonzero class (@0x5158d0) — that armory rate limit is unmodeled.]
bool loadout_envelope_accepted(const LoadoutSubmit &req, uint32_t game_type) {
	const int8_t team = static_cast<int8_t>(req.team);
	if (team < 1 || (team > 4 && game_type != 0)) return false;
	const int8_t player_class = static_cast<int8_t>(req.player_class);
	return player_class == 0 || (player_class >= 5 && player_class <= 9);
}

GrantedWeaponLoadout grant_weapon_loadout(const LoadoutSubmit &req,
										  uint16_t class_allow_mask,
										  const world::WeaponTable *table) {
	GrantedWeaponLoadout grant;
	WeaponLoadout &reply = grant.reply;
	// Class 0 passes the envelope and applies with an EMPTY grant: its soldier-type mask is 0, so
	// no request entry can match, and entity+660 is stamped 0 [orig: type_mask default @0x5159af,
	// the stamp @0x515ab0].
	if (req.player_class == 0) return grant; // avatar_class stays 0
	// Class accept: a valid but disabled request scans the whole Soldier Class range from 5 and
	// takes its first enabled bit. With no enabled bit the already-valid request survives; the
	// final 8 clamp only covers an invalid value [orig: g_hostClassAllowMask @0x24D59FC,
	// @0x5158d6..@0x515915].
	reply.avatar_class = req.player_class;
	if ((class_allow_mask & (uint16_t{1} << reply.avatar_class)) == 0) {
		for (uint8_t candidate = 5; candidate <= 9; ++candidate) {
			if ((class_allow_mask & (uint16_t{1} << candidate)) != 0) {
				reply.avatar_class = candidate;
				break;
			}
		}
	}
	if (reply.avatar_class < 5 || reply.avatar_class > 9) reply.avatar_class = 8;

	if (table != nullptr && !table->empty()) {
		struct GrantedSlot {
			uint16_t combo = 0;
			WeaponLoadoutSlot wire;
			int16_t ammo_index = -1;
			uint8_t request_damage_class = 0;
		};
		// The original first loads a 780-slot table keyed by category*65+rank. Repeating
		// that slot replaces it; the later reply walk therefore emits it exactly once.
		std::vector<GrantedSlot> accepted;
		for (const LoadoutSubmitEntry &e : req.entries) {
			const world::WeaponTableEntry *we = table->by_index(e.adm_index);
			if (we == nullptr) continue; // the AdmDef_GetEntryByIndex fail leg
			if (!world::loadout_entry_permitted(*we, req.team, reply.avatar_class))
				continue; // team/char mask filter [orig: @0x502716]
			if (we->ammo_class_id >= 0 && we->ammo_class_id < 128) {
				int32_t pool = e.ammo_primary != 0xFF
						? std::min<int32_t>(e.ammo_primary, we->maxclips) *
								we->clipsize
						: we->startrounds;
				if (we->ammo_class_id <
				    static_cast<int>(table->ammo_class_caps.size())) {
					const int32_t cap = table->ammo_class_caps[
							static_cast<size_t>(we->ammo_class_id)];
					if (pool > cap) pool = cap;
				}
				grant.slot_type_scores[
						static_cast<size_t>(we->ammo_class_id)] = pool;
			}
			const uint8_t damage_class = normalized_damage_class(e.variant);
			set_ammo_damage_class(grant.ammo_damage_classes, we->ammo_index, damage_class);
			const world::LoadoutAmmoBytes ammo =
					world::resolve_loadout_ammo(*table, e.adm_index, e.ammo_primary);
			GrantedSlot s;
			s.combo = static_cast<uint16_t>(we->category * 65u + we->rank);
			s.wire.type_id = e.adm_index;
			s.wire.ammo_primary = ammo.primary;
			s.wire.ammo_secondary = ammo.secondary;
			s.ammo_index = we->ammo_index;
			s.request_damage_class = damage_class;
			auto existing = std::find_if(accepted.begin(), accepted.end(),
									 [combo = s.combo](const GrantedSlot &slot) {
										 return slot.combo == combo;
									 });
			if (existing == accepted.end()) accepted.push_back(s);
			else *existing = s; // the last request entry loaded into this retail slot wins
		}
		std::sort(accepted.begin(), accepted.end(),
		          [](const GrantedSlot &a, const GrantedSlot &b) { return a.combo < b.combo; });
		for (GrantedSlot &slot : accepted) {
			// Rebuilding a retail weapon-slot table finishes by drawing one initial
			// clip from every populated slot before S2C 0x0F copies player+88664.
			// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515db5-@0x515de3 and
			// @0x515e0d-@0x515f4d -> WeaponSlots_RecalculateAmmoFromCapacity]
			// Golden team-1 witness: 300->270 (M16), 70->63 (.45), 3->2
			// (AT4/smoke), 2->1 (HE), and 1->0 (flashbang).
			const world::WeaponTableEntry *we = table->by_index(slot.wire.type_id);
			if (we != nullptr && we->ammo_class_id >= 0 &&
			    we->ammo_class_id < static_cast<int>(grant.slot_type_scores.size()) &&
			    we->ammo_class_count != 0 && we->clipsize != -1) {
				int32_t &pool = grant.slot_type_scores[
						static_cast<size_t>(we->ammo_class_id)];
				int32_t draw = static_cast<int32_t>(we->clipsize) * we->ammo_class_count;
				if (draw > pool) draw = pool;
				pool -= draw;
				if (pool < 0) pool = 0; // WeaponSlot_DecrementAmmo's lower clamp
			}
			slot.wire.ammo_alt = find_ammo_damage_class(
					grant.ammo_damage_classes, slot.ammo_index, slot.request_damage_class);
			reply.slots.push_back(slot.wire);
		}
		return grant;
	}

	// Resource-less test/diagnostic fallback: no AmmoDef relationship exists, but duplicate
	// ADM slots still behave like a table load (last entry wins) and serialize only once.
	for (const LoadoutSubmitEntry &e : req.entries) {
		WeaponLoadoutSlot s;
		s.type_id = e.adm_index;
		s.ammo_primary = e.ammo_primary;     // echoed; client clamps on apply (@0x4295d7)
		s.ammo_secondary = e.ammo_secondary; // echoed; sub-slot clamp (@0x429652)
		s.ammo_alt = normalized_damage_class(e.variant);
		auto existing = std::find_if(reply.slots.begin(), reply.slots.end(),
								 [type_id = s.type_id](const WeaponLoadoutSlot &slot) {
									 return slot.type_id == type_id;
								 });
		if (existing == reply.slots.end()) reply.slots.push_back(s);
		else *existing = s;
	}
	std::sort(reply.slots.begin(), reply.slots.end(),
	          [](const WeaponLoadoutSlot &a, const WeaponLoadoutSlot &b) {
		          return a.type_id < b.type_id;
	          }); // table-less fallback: ascending adm index (coincides for the golden kit)
	return grant;
}

// The player's live soldier class (entity+660), the header byte a current-slot-list re-send carries
// [orig: Server_SendWeaponSlotListToPlayer @0x502550 reads player+89820 = player[22455]].
uint8_t current_player_class(const NapiNPConnection &conn, const world::World *world) {
	if (world == nullptr || !conn.link.owned_entity.valid()) return 0;
	const world::Entity *pe = world->registry.get(conn.link.owned_entity);
	return pe != nullptr ? pe->player_class : 0;
}

// The tag=0x5A body for every re-send that grants NOTHING new (the 0x2F envelope abort, the deploy
// release): the retained granted body when one exists, else the empty-table shape headed by the
// player's live class. [orig: Server_SendWeaponSlotListToPlayer @0x502550 — header byte
// player+89820, rows walked off the player's own 780-slot weapon table, which a player that never
// submitted a loadout has none of.]
std::vector<uint8_t> build_current_loadout_reply(const std::vector<uint8_t> &retained,
                                                 uint8_t player_class,
                                                 const world::WeaponTable *armory) {
	if (!retained.empty()) return retained;
	(void)armory;
	WeaponLoadout current;
	current.avatar_class = player_class;
	return encode_weapon_loadout(current);
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
	// The recipient's deploy-map owned-zone mask (0x0F variant-0 u32) from the live chain; a
	// chain-less world keeps the golden ASH_I5A default 0x8. [orig: ZoneSlotChain_GetOwnedZoneMask
	// @0x4a2620 per recipient team @0x4ff9a3; net-re §5.61]
	if (world != nullptr && !world->zone_chain.empty())
		ctx.uniform_team_mask =
				world::zone_chain_owned_zone_mask(*world, world->zone_chain, ctx.team);
	return ctx;
}

// tag=0x04 join/slot-assignment body (24 B) [orig: NetPacket_WriteSlotAssignment @0x502b30, sent from
// CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0]: [4x u32 netPlayer+60..72 stat dwords (0 on a fresh
// join)][u8 g_mode dword_24D2110][u8 player_slot (slot+20)][u8 slot capacity @0x24c0ca4][u32 0]
// [u8 team (slot+416)]. Every fixture byte is now witnessed field-for-field (grill 2026-07-01).
std::vector<uint8_t> build_tag04_slot_assignment(uint8_t player_slot, uint8_t slot_capacity,
                                                 uint8_t team) {
	std::vector<uint8_t> payload(24, 0);
	payload[16] = 0;             // g_mode byte [orig dword_24D2110 low byte] — 0 on the golden host
	payload[17] = player_slot;   // the joiner's assigned slot index [orig slot+20]
	payload[18] = slot_capacity; // player-slot array capacity [orig `capacity` @0x24c0ca4]
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
// byte 17 lands in g_local_player_slot_id (the client's own slot id) and byte 18 in g_max_player_slots
// (g_max_player_slots, the 0x46/0x22 roster-walk terminator): the prior hardcoded (1, 2, 2)
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

} // namespace

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
	messages.push_back(make_protocol_message(
			0x04, build_tag04_slot_assignment(*player_slot, capacity, team)));
	messages.push_back(make_protocol_message(
			s2c::FULL_PLAYER_INFO, build_tag7b_session_summary(config, conn)));
	return messages;
}

std::vector<uint8_t> build_spawn_wave_status_body(
		const world::World &world, world::EntityHandle requester) {
	std::vector<uint8_t> body;
	const world::Entity *recipient = world.registry.get(requester);
	const uint8_t team = recipient != nullptr ? recipient->team : 0;
	const world::SpawnZoneRegistry zones = world::build_spawn_zone_list(world);
	std::vector<const world::SpawnWaveEntry *> visible;
	for (const world::SpawnWaveEntry &entry : world.spawn_waves.entries())
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
		world::entity_detach_from_vehicle(world, player->handle);
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
	world::SpawnPointResult pose = world::resolve_player_spawn_pose(
			world, player->handle, target_zone, conn.reply.player_slot,
			player->team, config.game_type);
	if (pose.found) {
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
	world::entity_reset_to_spawn_state(*player);
	if (world.player_has_item_def && world.player_item_hp != 0)
		player->health = world::retail_signed_i16(world.player_item_hp);
	else if (player->health_max > 0)
		player->health = player->health_max;
	else
		player->health = 100;
	player->alive = true;
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
	conn.discard_pre_deploy_uplinks = true;
	conn.link.respawn_pending = false;
	conn.link.respawn_delay_seconds = 0;
	conn.link.spawn_target_hold_seconds = 0;
	conn.link.respawn_hold_armed = false;
	conn.link.downed_revive_seconds = 0;
	conn.link.medic_request_active = false;
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
			world::attach_to_vehicle_seat(world, player->handle, selected);
	}
	const world::WeaponTable *armory = !world.weapons.empty()
			? &world.weapons
			: nullptr;
	replies.push_back(make_protocol_message(
			0x5A, build_current_loadout_reply(
					conn.reply.last_loadout_reply, player->player_class, armory)));
	replies.push_back(make_protocol_message(
			0x61, Server_RerollPlayerTickSeed(conn)));
	const uint8_t frontier = world::zone_chain_frontier_zone(
			world, world.zone_chain, player->team);
	if (frontier != 0)
		replies.push_back(make_protocol_message(
				s2c::GAME_EVENT, build_tag_1e_frontier_hint(frontier)));
	return replies;
}

bool is_medic_recipient(const NapiNPConnection &candidate,
		const world::World &world, uint8_t team) {
	if (!is_in_match(candidate) || candidate.link.transport == nullptr ||
			!candidate.link.owned_entity.valid())
		return false;
	const world::Entity *medic = world.registry.get(candidate.link.owned_entity);
	return medic != nullptr && medic->alive &&
			(medic->flags & world::kEntityFlagDead) == 0u &&
			medic->team == team &&
			world.class_has_attribute(medic->player_class,
					world::World::kCharAttrMedic);
}

namespace {

// The retail handler runs `sprintf(msg_buffer[128], format, name)`; the
// shipped STRSRV_MEDREQ carries one `%s`. Substitute it here (the CRT call is a
// platform primitive) and keep retail's 127-character buffer bound.
std::string format_medic_request(const std::string &format,
		const std::string &name) {
	std::string out;
	const size_t marker = format.find("%s");
	if (marker == std::string::npos) {
		out = format;
	} else {
		out = format.substr(0, marker) + name + format.substr(marker + 2);
	}
	if (out.size() > 127) out.resize(127);
	return out;
}

} // namespace

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
				if (admission_message->tag != 0x00 ||
				    !validates_join_request(config, admission_message->payload)) {
					conn.admission_stage = GameAdmissionStage::Rejected;
					return {};
				}
				// The spectator legs run here, at the game-layer join gate,
				// after the expansion/CRC checks — the same position they hold
				// inside Server_ValidatePlayerJoinRequest. A failure answers
				// the witnessed DPC 14/15/16 description punt, not a 0x82.
				if (const uint32_t reject_dpc =
							validate_spectator_join(config, conn, roster);
				    reject_dpc != 0) {
					(void)stage_join_gate_reject(conn, reject_dpc);
					conn.admission_stage = GameAdmissionStage::Rejected;
					return {};
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
			case c2s::MEDIC_REQUEST: {
				// A downed player's manual medic call. The body is never read:
				// the requester is the connection's own player. In order: the
				// null GameText lookup no-ops the whole handler; the slot must
				// be a live-roster player with an armed revive window
				// (`!slot+100567 && slot+368 > 0`); the requester leaves its
				// spawn-wave group; the message is formatted from the slot name;
				// a manual-preference, not-yet-latched requester publishes the
				// live window (no bit 7) to the 0x580 medic set; the chat goes
				// reliably to that set minus the requester, then to the
				// requester alone (the chat is NOT preference-gated); the
				// once-only latch closes; and the MEDIC_REQUEST composite sound
				// fans to alive players. The listen host's own player runs
				// this handler too (no loopback early-out).
				// [orig: Server_BroadcastMedicRequest @0x515390 — the GameText
				// lookup @0x5153C9, gates @0x515406, SpawnWaveList_RemovePlayer
				// @0x515412, sprintf @0x515421, 0x54 @0x515432..0x515484,
				// 0x14 @0x5154AC..0x51550B, latch @0x515510, sound
				// @0x515519..0x51552F]
				if (inputs.medic_request_format == nullptr ||
						inputs.medic_request_format->empty())
					break;
				if (world == nullptr || !conn.burst.spawned ||
						!conn.link.owned_entity.valid())
					break;
				const world::Entity *requester =
						world->registry.get(conn.link.owned_entity);
				if (requester == nullptr || conn.link.downed_revive_seconds == 0u)
					break;
				world->spawn_waves.remove_player(conn.link.owned_entity);
				const std::string message = format_medic_request(
						*inputs.medic_request_format, requester->name);
				if (!conn.link.auto_medic_enabled &&
						!conn.link.medic_request_active) {
					PlayerDownedState state;
					state.entity_handle = conn.link.owned_entity.packed;
					state.revive_seconds = static_cast<uint8_t>(
							conn.link.downed_revive_seconds);
					state.medic_request_active = false; // the raw slot+368 byte
					const std::vector<uint8_t> downed =
							encode_player_downed_state(state);
					for (NapiNPConnection &candidate : roster) {
						if (!is_medic_recipient(candidate, *world, requester->team))
							continue;
						candidate.link.transport->host_send(
								s2c::PLAYER_DOWNED_STATE, downed);
					}
				}
				ChatBroadcast chat;
				chat.channel = 2;
				chat.sender_slot = conn.reply.player_slot;
				chat.text = message;
				const std::vector<uint8_t> chat_body = encode_chat_broadcast(chat);
				for (NapiNPConnection &candidate : roster) {
					if (&candidate == &conn ||
							!is_medic_recipient(candidate, *world, requester->team))
						continue;
					candidate.link.transport->host_send(
							s2c::CHAT_BROADCAST, chat_body);
				}
				replies.push_back(make_protocol_message(
						s2c::CHAT_BROADCAST, chat_body));
				conn.link.medic_request_active = true;
				// The help call: the requester's body-model composite
				// "<prefix>_MEDIC_REQUEST", positioned at the requester, to
				// every alive player (mask 128). The listen host's own copy
				// rides the local slot-sound route the death scream uses.
				// [orig: SoundProfile_FindByEntityAndType(entity, 1)
				// @0x515526 -> Server_SendOverlayActionToAlive @0x50A1B0]
				{
					char set_name[24] = {};
					audio::compose_entity_sound_set(requester->anim_slot,
							audio::kEntitySoundMedicRequest, set_name,
							sizeof(set_name));
					PlaySoundCommand cmd;
					cmd.flag = 1;
					cmd.sound_name = set_name;
					cmd.has_pos = true;
					cmd.pos_x = static_cast<int16_t>(
							world::to_fixed(requester->position.x) >> 16);
					cmd.pos_y = static_cast<int16_t>(
							world::to_fixed(requester->position.y) >> 16);
					cmd.pos_z = static_cast<int16_t>(
							world::to_fixed(requester->position.z) >> 16);
					const std::vector<uint8_t> sound_body = encode_play_sound(cmd);
					for (NapiNPConnection &candidate : roster) {
						if (!is_in_match(candidate) ||
								candidate.link.transport == nullptr ||
								!candidate.link.owned_entity.valid())
							continue;
						const world::Entity *listener =
								world->registry.get(candidate.link.owned_entity);
						if (listener == nullptr || listener->health <= 0)
							continue; // the mask-128 alive filter
						if (candidate.link.mode == replication::TransportMode::Loopback) {
							world::SoundSlotEvent local;
							local.source_handle = conn.link.owned_entity.packed;
							local.pos[0] = world::to_fixed(requester->position.x);
							local.pos[1] = world::to_fixed(requester->position.y);
							local.pos[2] = world::to_fixed(requester->position.z);
							local.slot = 0;
							std::memcpy(local.set_name, set_name, sizeof(set_name));
							world->slot_sounds.push_back(local);
							continue;
						}
						candidate.link.transport->host_send(
								s2c::PLAY_SOUND, sound_body, /*reliable=*/false);
					}
				}
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
				// [orig: validate_time_sync @0x502210, reset @0x502273]
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
				// [orig: handle_anti_cheat_crc_check (0x21) @0x502050]
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
				// slot+1 with fieldFlags 0x5CF7 until slot+1 >= its max-player count g_max_player_slots
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
				// its max-player count g_max_player_slots (fed by our 0x04 slot-config byte 18), so
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
				replies.push_back(make_protocol_message(s2c::FILE_TRANSFER_CHUNK, build_tag60_server_info(config)));
				break;
			case c2s::MISSION_CHUNK_REQUEST: // mission-file request -> 0x64 chunk only [orig: NapiNPServerMsg_0x037_SendCircularBuffer @0x5152E0]
				replies.push_back(make_protocol_message(
						s2c::MISSION_DATA_CHUNK,
						build_tag64_mission_metadata(config, inputs.mission_metadata_blob)));
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
				// The decoder owns the framing contract: header, whole 4-byte entries, and one
				// terminal 0xFF. Trailing bytes after the terminator are accepted exactly as
				// retail accepts them (no end check after the 0xFF exit @0x515a99); only an
				// unterminated list is ignored — the crash-safe stand-in for retail's
				// zero-fill infinite loop — without opening the phase-8 gate or disturbing
				// the last valid grant.
				if (!decode_loadout_submit(msg.payload.data(), msg.payload.size(), req)) break;
				const world::WeaponTable *armory =
						(world != nullptr && !world->weapons.empty()) ? &world->weapons : nullptr;
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
										  current_player_class(conn, world), armory)));
					break;
				}
				const GrantedWeaponLoadout grant = conn.link.spectator
						? GrantedWeaponLoadout{}
						: grant_weapon_loadout(req, config.class_allow_mask, armory);
				// Retain the GRANTED body: the deploy-release bundle re-sends it (the client's
				// 0x5A apply is the deploy un-latcher — resets dword_81474C; §5.30, D-NET-156).
				st.last_loadout_reply = encode_weapon_loadout(grant.reply);
				st.slot_type_scores = grant.slot_type_scores;
				replies.push_back(make_protocol_message(s2c::WEAPON_LOADOUT, st.last_loadout_reply));
				// Accepted soldier type -> entity+660 playerClass [orig: @0x515ab0] — feeds the
				// §5.10 field-17 class nibble and the 0x0C/0x18 spawn records for this player.
				if (world != nullptr && conn.link.owned_entity.valid()) {
					if (world::Entity *pe = world->registry.get(conn.link.owned_entity)) {
						pe->player_class = grant.reply.avatar_class;
						// The fourth accepted byte is copied to the player-slot table at
						// [ammoDef.index]. Weapon_CalcImpactDamage later interprets 1 as
						// x0.9 and 2 as x1.1 for person targets.
						if (armory != nullptr) {
							pe->ammo_damage_class.assign(world->ammo.entries.size(), 0);
							for (const auto &entry : grant.ammo_damage_classes) {
								const size_t ammo_index = static_cast<size_t>(entry.first);
								if (ammo_index < pe->ammo_damage_class.size())
									pe->ammo_damage_class[ammo_index] = entry.second;
							}
						}
					}
				}
				st.loadout_synced = true;
				conn.burst.loadout_received = true;
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
			case c2s::TEAM_SPAWN_ACK: // team/spawn ack [u16 team_change_index] — NO reply on a plain join.
				// [orig: NapiNPServerMsg_0x029 @0x514F10] replies S2C 0x51 ONLY when the index
				// resolves to a pending entity in g_team_change_entity_list @0xC947C8 (gated
				// !g_net_spawn_suspended && !g_spawn_success_gate), and that reply is a REAL
				// write_entity_packet @0x506BB0 record. The golden retail-ashi5a session's
				// deploy-time C 0x29 draws NO 0x51 anywhere. The client FIELD-PARSES 0x51 —
				// NapiNPClientMsg_HandlePlayerSpawn @0x431BB0 stamps team (+354) and NetId
				// (@0x431cad) and REBINDS CharacterEntity (@0x431cf3) — so our former
				// unconditional zero-id 0x51 "spawn-confirm" re-bound the joiner's own player
				// to a vehicle archetype: the retail-join DBuggy1 shadow (D-NET-148). Team
				// change is unmodeled; when it lands, port the @0x514F10 list lookup +
				// write_entity_packet — never an echo.
				break;
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
					// Auto-deploy: the team's frontier zone; null falls back to the marker chain
					// [orig: find_spawn_entity_for_team @0x4fc810 -> requestedHandle -1 on miss].
					target = world::find_spawn_zone_for_team(*world, world->zone_chain,
					                                         player->team, config.game_type);
				} else if (pick != 0 && pick != 0xFFFF) {
					target = world::resolve_spawn_target(*world, player->team, pick);
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
						world::team_has_available_spawn_zone(*world, player->team))
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
					if (world->spawn_waves.try_queue(
							*world, target->handle, player->handle)) {
						ProtocolMessage status = make_protocol_message(
								s2c::SPAWN_WAVE_STATUS,
								build_spawn_wave_status_body(*world, player->handle));
						status.reliable = false;
						replies.push_back(std::move(status));
						break;
					}
					if (world->spawn_waves.has_entry(target->handle)) break;
				}
				world->spawn_waves.remove_player(player->handle);
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
			case c2s::ENTITY_UPLINK: // C2S player-input uplink — cache the pre-spawn pose
				cache_client_pose(msg.payload, st);
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
						!world::vehicle_prepare_weapon_slot(*world, *mount))
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
						!world::vehicle_prepare_weapon_slot(*world, *parent))
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
				world::entity_process_vehicle_attach(*world, conn.link.owned_entity,
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
				world::entity_detach_from_vehicle(*world, conn.link.owned_entity);
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
				// echo_flag == 0 return leg is server-internal RTT stat + min/max-ping kick (no reply).
				if (msg.payload.size() >= 5 && msg.payload[4] != 0) {
					const uint32_t ts = static_cast<uint32_t>(msg.payload[0]) |
					                    (static_cast<uint32_t>(msg.payload[1]) << 8) |
					                    (static_cast<uint32_t>(msg.payload[2]) << 16) |
					                    (static_cast<uint32_t>(msg.payload[3]) << 24);
					ProtocolMessage pong = make_protocol_message(
							s2c::RTT_ECHO, build_tag57_pong(ts));
					pong.reliable = false; // SendFiltered @0x51510C userParam=1
					replies.push_back(std::move(pong));
				}
				break;
			}
			case c2s::MISSION_FILE_STATUS: // mission-file status report [orig: NapiNPServerMsg_PlayerJoinRequest @0x51AB10]
				st.mission_status_received = true;
				break;
			case c2s::FIRED_ROUND: { // client fired round -> ammo authority + the S2C 0x0A tag-2 round echo
				// [orig: NapiNPServerMsg_0x006_ClientFiredRound @0x513310 ->
				// Server_ClientFiredRound @0x50baa0]. An accepted PRIMARY fire re-enters the
				// validator locally through the adm 'fire' action (WeaponAction_Fire @0x542b10
				// -> Entity_FireWeaponAndSendPacket @0x42bd80), and RoundData_AddRound @0x4fdb40
				// appends the g_round_ring event the per-recipient 0x0A fan serializes as the
				// §5.9.1 tag-2 round event; ALT fire appends directly. Our altitude: validate ->
				// clip bookkeeping -> ring append. Deferred (D-NET-152 tails): the round SPAWN
				// (RoundData_SpawnRound @0x4ec0d0 — projectile entity, spread, damage), the
				// cease-fire gate (g_InCeaseFire unmodeled), the +96472 fire-rate stamp (the adm
				// cooldown dword adm[276] is unparsed), the savedLivePose warp compensation
				// (@0x50bbac — our net-snapped peers have zero intra-tick motion), and the
				// moving-carrier re-anchor (@0x50bc41).
				if (world == nullptr || !conn.burst.spawned) break; // [orig: gates @0x513338..62]
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

				const bool alt_fire = (fr.fire_flags & 0x01) != 0; // [orig: @0x50bb0d]
				// ADM + ammo authority — armory-fed hosts only (a table-less host accepts,
				// mirroring the 0x5A echo fallback, D-NET-141). Alt fire skips the adm lookup,
				// the clip, and the equipped mirror [orig: @0x50bb0f / the @0x50be2b alt path].
				if (!world->weapons.empty() && !alt_fire) {
					const world::WeaponTableEntry *adm = world->weapons.by_index(fr.adm_index);
					if (adm == nullptr) break; // [orig: "Tried to fire NULL wpn, %i" @0x50bb49]
					if (adm->clipsize != -1) {
						world::WeaponSlotState *mounted_slot = nullptr;
						uint8_t mounted_adm = 0xFF;
						if (shooter->use_gun_slot_swapped &&
								shooter->mount_target.valid()) {
							world::Entity *mount =
									world->registry.get(shooter->mount_target);
							if (mount != nullptr) {
								mounted_slot = world::resolve_mounted_ammo_slot(
										*world, *mount);
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
							if (mounted_slot->clip <= 0) break;
							--mounted_slot->clip;
						} else {
							const uint16_t combo =
									uint16_t(adm->category) * 65u + adm->rank;
							WeaponSlotState &slot = conn.weapon_slots[combo];
							if (slot.adm_index != fr.adm_index) {
								slot.adm_index = fr.adm_index;
								slot.clip = adm->clipsize;
							}
							if (slot.clip <= 0) break;
							--slot.clip;
						}
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
				world->rounds.add(ev);
				// The authoritative round spawns SYNCHRONOUSLY with the ring append
				// [orig: RoundData_AddRound @0x4fdb40 inline-calls RoundData_SpawnRound
				// @0x4ec0d0 — the ring is only the tag-2 fan-out log; §5.60]. Ammo = the
				// adm's load-time-resolved round_type (adm+84 pair in the original); an
				// armory- or ammo-less host skips the sim (fire still echoes).
				if (!world->ammo.empty()) {
					const world::WeaponTableEntry *fire_adm =
							world->weapons.by_index(fr.adm_index);
					if (fire_adm != nullptr && fire_adm->ammo_index >= 0) {
						world::RoundSpawnParams rp;
						rp.owner = conn.link.owned_entity;
						rp.shooter_handle = fr.shooter_handle;
						rp.origin.x = static_cast<float>(fr.pos_x) / 65536.0f;
						rp.origin.y = static_cast<float>(fr.pos_y) / 65536.0f;
						rp.origin.z = static_cast<float>(fr.pos_z) / 65536.0f;
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
				// No reactive reply — the echo rides the per-frame 0x0A fan (netsim
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
							s2c::WEAPON_RELOAD, body, /*reliable=*/false);
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
				if (!world->weapons.empty() &&
				    conn.link.mode != replication::TransportMode::Loopback) {
					// EWeap reload ignores the wire combo and follows the same live
					// child/groundEntity route as fire and phase-8 serialization.
					// [orig: WeaponSlot_ReloadAmmo @0x541720 -> @0x5460e0]
					if (addressed_entity != nullptr &&
							addressed_entity->has_item_def &&
							(addressed_entity->item_attrib &
									world::kItemAttribEweap) != 0u &&
							world::vehicle_prepare_weapon_slot(
									*world, *addressed_entity)) {
						world::WeaponSlotState *slot =
								world::resolve_mounted_ammo_slot(
										*world, *addressed_entity);
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
								world->weapons.by_index(slot_adm);
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
								world->weapons.by_index(slot_it->second.adm_index);
						if (adm != nullptr && adm->clipsize != -1)
							slot_it->second.clip = adm->clipsize; // [orig: slot+16 @0x541850]
					}
				}
				break;
			}
			case c2s::ENTITY_INFO_QUERY: { // entity-info query [u16 handle] -> S2C 0x18 FULL-ENTITY-SPAWN (the self-heal).
				// [orig: NapiNPServerMsg_HandlePlayerInfoRequest @0x514180 — validates pool <= 1 &&
				// slot < capacity, serializes the REQUESTED entity via serialize_object_to_buffer
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
				// authority-gated, and is SKIPPED while g_net_spawn_suspended or
				// g_spawn_success_gate (round over) is set — our reachable analog of the
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
	// `fallback` only matters for an empty roster (World-less path); a real host always has >=1 bound
	// player, so the enumerated roster wins. Build a minimal fallback rep from the config.
	PlayerReplicationState fallback;
	fallback.player_name = config.player_name;
	ProtocolMessage message = make_protocol_message(
			s2c::PLAYER_LIST, build_reply_tag_16(config, roster, fallback, world));
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
