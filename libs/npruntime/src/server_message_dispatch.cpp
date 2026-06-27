#include "npruntime/server_message_dispatch.h"

#include <novaworld/ingame_decode.h>   // decode_entity_packet_sub_header / decode_player_extended_uplink
#include <novaworld/replication_min.h> // PlayerReplicationState (POD, kept) — the reply builders' input

#include <algorithm>
#include <cstring>
#include <utility>

namespace opennova::np {

namespace {

// ---------------------------------------------------------------------------
// Byte helpers (relocated verbatim from the retired game_session.cpp / replication_min.cpp). The §5.1
// identity bodies (0x7A PCID, 0x7B session info) are now FAITHFUL ports of the witnessed serializers
// (NetPacket_WritePCID @0x5076e0, NapiNPMsg_0x7B_BuildPayload @0x507740; net-re §5.45, grilled
// 2026-06-27). The remaining reply bodies (0x46 NetPacket_SerializeWeaponOverlaySlotState @0x505e80 —
// a flag-driven slot-state record, and 0x51 write_entity_packet @0x506bb0) carry approximations
// pending slot-state modeling; the witnessed field maps are landed in net-re §5.45 (D-NET-127).
// ---------------------------------------------------------------------------

void append_u16_le(std::vector<uint8_t> &out, uint16_t v) {
	out.push_back(static_cast<uint8_t>(v & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void append_u32_le(std::vector<uint8_t> &out, uint32_t v) {
	out.push_back(static_cast<uint8_t>(v & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void write_u32_le(std::vector<uint8_t> &out, size_t off, uint32_t v) {
	out[off + 0] = static_cast<uint8_t>(v & 0xFFu);
	out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
	out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
	out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
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

inline void push_u8(std::vector<uint8_t> &buf, uint8_t v) { buf.push_back(v); }
inline void push_u16(std::vector<uint8_t> &buf, uint16_t v) {
	buf.push_back(static_cast<uint8_t>(v & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}
inline void push_u32(std::vector<uint8_t> &buf, uint32_t v) {
	buf.push_back(static_cast<uint8_t>(v & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}
void push_cstr(std::vector<uint8_t> &buf, const std::string &s, size_t max_chars) {
	const size_t n = std::min(s.size(), max_chars > 0 ? max_chars - 1 : 0);
	for (size_t i = 0; i < n; ++i) buf.push_back(static_cast<uint8_t>(s[i]));
	buf.push_back(0);
}

bool is_default_ash_session_config(const SessionReplyConfig &cfg) {
	return cfg.mission_name == "AS - Dormant Volcano Isle" && cfg.mission_file == "ASH_I5A.BMS";
}

// ---------------------------------------------------------------------------
// §5.1 reply bodies — handshake + server-info + mission-metadata (relocated from game_session.cpp).
// ---------------------------------------------------------------------------

// tag=0x02 server push. [orig: NapiNPClientMsg_0x002 @0x42E0F0 reads 12 B; D-NET-127 carried 512.]
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
std::vector<uint8_t> build_tag7a_pcid(const SessionReplyConfig &cfg) {
	std::vector<uint8_t> payload;
	append_cstr(payload, cfg.pcid);
	return payload;
}

// tag=0x7B session/player info. [orig: NapiNPMsg_0x7B_BuildPayload @0x507740] field order:
// player_name, PCID, server_name, title(mission name), map_file, gametype(u32), empty_str, expansion.
// (The PCID slot previously carried an invented "DEV-A02-0001" literal — D-NET-127; now sourced.)
std::vector<uint8_t> build_tag7b_session_summary(const SessionReplyConfig &cfg) {
	std::vector<uint8_t> payload;
	append_cstr(payload, cfg.player_name); // [orig player_data+128]
	append_cstr(payload, cfg.pcid);        // [orig entity+592] PCID
	append_cstr(payload, cfg.server_name); // [orig g_server_name_str]
	append_cstr(payload, cfg.mission_name);// [orig title: MissionText "title" / g_GameType title]
	append_cstr(payload, cfg.mission_file);// [orig g_map_file_name]
	append_u32_le(payload, cfg.gametype);  // [orig g_GameType]
	payload.push_back(0);                  // [orig g_empty_str] empty C string
	append_cstr(payload, cfg.expansion);   // [orig g_ExpansionName]
	return payload;
}

// tag=0x60 chunked server-info KV transfer. [orig: NapiNPClientMsg_HandleFileTransferChunk @0x432350]
std::vector<uint8_t> build_tag60_server_info(const SessionReplyConfig &cfg) {
	std::vector<uint8_t> info_body;
	append_string_kv(info_body, "SERVERNAME", cfg.server_name);
	append_string_kv(info_body, "MISSIONNAME", cfg.mission_name);
	uint8_t gametype_le[4] = {
			static_cast<uint8_t>(cfg.gametype & 0xFFu),
			static_cast<uint8_t>((cfg.gametype >> 8) & 0xFFu),
			static_cast<uint8_t>((cfg.gametype >> 16) & 0xFFu),
			static_cast<uint8_t>((cfg.gametype >> 24) & 0xFFu),
	};
	append_kv(info_body, "GAMETYPE", gametype_le, 4);
	const std::string custom_text = "OpenNova dev server.";
	append_string_kv(info_body, "CUSTOMTEXT", custom_text);
	append_string_kv(info_body, "MISSIONFILENAME", cfg.mission_file);
	const uint8_t exp_fanfare[] = {0, 0};
	append_kv(info_body, "EXP_FANFARE", exp_fanfare, 2);

	std::vector<uint8_t> reply(12, 0);
	reply[0] = 0x01; // offset=1.
	write_u32_le(reply, 4, static_cast<uint32_t>(info_body.size()));
	reply.insert(reply.end(), info_body.begin(), info_body.end());
	return reply;
}

// tag=0x64 chunked mission-metadata transfer. [orig: NapiNPClientMsg_0x064 @0x432410]
std::vector<uint8_t> build_tag64_mission_metadata(const SessionReplyConfig &cfg) {
	std::vector<uint8_t> mission_blob(180, 0);
	auto write_str = [&](size_t off, const std::string &s) {
		const size_t n = std::min<size_t>(s.size(), 31);
		std::memcpy(mission_blob.data() + off, s.data(), n);
	};
	static const uint8_t HASH_PREFIX[32] = {
		0x79, 0x09, 0x05, 0x38, 0xf6, 0x29, 0x3d, 0x87,
		0xd1, 0xc8, 0xda, 0x39, 0xa0, 0x1d, 0xa8, 0xae,
		0xa9, 0x84, 0x1f, 0xa2, 0xc6, 0xf9, 0x73, 0x96,
		0xc7, 0x39, 0x03, 0xe5, 0xb2, 0xf9, 0x83, 0x61,
	};
	static const uint8_t HASH_SUFFIX[32] = {
		0xdd, 0xa8, 0xe1, 0x6b, 0x7c, 0x7a, 0x34, 0x5c,
		0x0c, 0x07, 0x7d, 0x26, 0x8e, 0x93, 0x53, 0x19,
		0xbd, 0x0e, 0x16, 0xff, 0x69, 0x0a, 0x9b, 0x37,
		0x46, 0x4f, 0xb3, 0xbe, 0x95, 0x3a, 0x02, 0x3b,
	};
	std::memcpy(mission_blob.data() + 0, HASH_PREFIX, 32);
	write_u32_le(mission_blob, 32, 5705);
	write_u32_le(mission_blob, 36, 2);
	mission_blob[40] = 0x10;
	mission_blob[42] = 0x01;
	write_u32_le(mission_blob, 44, cfg.mpattrib);
	write_u32_le(mission_blob, 48, 1);
	write_str(52, cfg.server_name);
	write_str(84, cfg.mission_file);
	write_str(116, cfg.mission_file);
	std::memcpy(mission_blob.data() + 148, HASH_SUFFIX, 32);

	std::vector<uint8_t> reply(12 + mission_blob.size(), 0);
	reply[0] = 0x01;
	write_u32_le(reply, 4, static_cast<uint32_t>(mission_blob.size()));
	std::memcpy(reply.data() + 12, mission_blob.data(), mission_blob.size());
	return reply;
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
// §5.1 reply bodies — roster / loadout / spawn-confirm (relocated from replication_min.cpp).
// ---------------------------------------------------------------------------

// tag=0x16 PLAYER-LIST. [orig: NapiNPClientMsg_0x016 @0x42FAE0]
std::vector<uint8_t> build_reply_tag_16(const PlayerReplicationState &ctx) {
	std::vector<uint8_t> buf;
	buf.reserve(32);
	push_u8(buf, 0x01);            // dword_A823B8 = 1 (HUD time-vs-score format flag)
	push_u8(buf, 1);              // player_count = 1
	push_u8(buf, ctx.player_slot); // player_id
	push_u16(buf, 0);             // v42
	push_u16(buf, 0);             // v44
	push_u16(buf, 0);             // v45
	const uint8_t packed_team = static_cast<uint8_t>(ctx.team << 1); // (team<<1)|spectator; team=1 -> 0x02
	push_u8(buf, packed_team);
	push_u8(buf, 0x02);           // team_count = 2 (matches retail)
	for (int i = 0; i < 3; ++i) { // (team_count + 1) iterations
		push_u16(buf, 0); push_u16(buf, 0);
		push_u8(buf, 0); push_u8(buf, 0);
	}
	push_u8(buf, 0x02);           // dword_A85B3C
	push_u8(buf, 0x00);           // dword_A85B40
	return buf;
}

// tag=0x46 PLAYER-SYNC. [orig: NapiNPClientMsg_0x046 @0x431370; server NetPacket_SerializeWeaponOverlay
// SlotState @0x505e80 — deferred; D-NET-127 fixture.]
std::vector<uint8_t> build_reply_tag_46(const PlayerReplicationState &ctx) {
	std::vector<uint8_t> buf;
	buf.reserve(48);
	push_u8(buf, ctx.player_slot);
	const uint16_t flags = 0x1CF7u;
	push_u16(buf, flags);
	push_u8(buf, static_cast<uint8_t>(ctx.entity_handle & 0x00FFu));
	push_cstr(buf, ctx.player_name, 32);
	push_cstr(buf, ctx.clan_tag, 16);
	push_cstr(buf, std::string("A-A02-000000"), 16);
	push_u8(buf, ctx.team);  // team byte (entity+354)
	push_u8(buf, 0);         // squad
	push_u8(buf, 0);         // ticket-validated
	push_u8(buf, 0xFF);      // byte+48 — no squad leader
	push_u8(buf, 0);         // byte+49
	push_u8(buf, 1);         // quality
	push_u32(buf, 0);        // entity_ref — null
	return buf;
}

// tag=0x5A WEAPON-LOADOUT-SYNC — verbatim retail frame-82537 payload. [orig: NapiNPClientMsg_0x05A
// @0x4290e0; clears dword_81474C -> unblocks the joiner's input emission.]
std::vector<uint8_t> build_tag_5a_weapon_loadout() {
	static constexpr uint8_t kRetailTag5aPayload[34] = {
		0x08, 0x02, 0xff, 0xff, 0x00, 0x03, 0x05, 0xff, 0x00, 0x13,
		0x0a, 0xff, 0x00, 0x33, 0x03, 0xff, 0x00, 0x34, 0x03, 0xff,
		0x00, 0x35, 0x03, 0xff, 0x00, 0x37, 0x02, 0xff, 0xff, 0x38,
		0xff, 0xff, 0xff, 0xff
	};
	return std::vector<uint8_t>(std::begin(kRetailTag5aPayload), std::end(kRetailTag5aPayload));
}

// tag=0x51 PLAYER-SPAWN. [orig: NapiNPClientMsg_HandlePlayerSpawn @0x431BB0; server write_entity_packet
// @0x506bb0 — deferred; D-NET-127 8-byte fixture.]
std::vector<uint8_t> build_reply_tag_51(const PlayerReplicationState &ctx) {
	std::vector<uint8_t> buf;
	buf.reserve(8);
	push_u16(buf, ctx.team);
	push_u16(buf, ctx.entity_handle);
	push_u8(buf, static_cast<uint8_t>(ctx.team & 0xFF));
	push_u16(buf, 0);
	push_u8(buf, 0);
	return buf;
}

// tag=0x1E GAME-EVENT (post-spawn) — verbatim retail frame-82540 payload. [orig: GameEvent_BuildPayload
// @0x5054E0 / NetPacket_HandleGameEvent @0x426270.]
std::vector<uint8_t> build_tag_1e_game_event_post_spawn() {
	static constexpr uint8_t kRetailTag1ePayload[8] = {0x3a, 0x04, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00};
	return std::vector<uint8_t>(std::begin(kRetailTag1ePayload), std::end(kRetailTag1ePayload));
}

// ---------------------------------------------------------------------------
// Dispatch helpers.
// ---------------------------------------------------------------------------

// Build the per-reply PlayerReplicationState from the session config + the connection's binding (the
// retired GameSession::player_replication_state). [orig: the player slot the reply serializers read]
PlayerReplicationState make_rep_state(const SessionReplyConfig &cfg, const SessionReplyState &st) {
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
	if (st.binding_valid) {
		ctx.player_name = st.player_name;
		ctx.player_slot = st.player_slot;
		ctx.entity_handle = st.player_entity_handle;
	} else if (st.player_entity_handle != 0) {
		ctx.entity_handle = st.player_entity_handle;
	}
	return ctx;
}

// tag=0x02 GLB_JOIN post-handshake burst. [orig: NapiNPServerMsg_0x002 @0x512FD0 — witnessed handler
// emits 0x01/0x7A/0x7B/0x03; the extra 0x00x2/0x05/0x04 are captured-from-observation, D-NET-127.]
void emit_post_handshake_burst(const SessionReplyConfig &cfg, std::vector<ProtocolMessage> &out) {
	out.push_back(make_protocol_message(0x00, {0, 0x08, 0, 0, 0, 0x0C, 0, 0, 0}, 0xA0));
	out.push_back(make_protocol_message(0x00, {1, 0x08, 0, 0, 0, 0x0C, 0, 0, 0}, 0xA0));
	out.push_back(make_protocol_message(0x01, {0x01, 0x00, 0x00, 0x00}));
	out.push_back(make_protocol_message(0x7A, build_tag7a_pcid(cfg)));
	out.push_back(make_protocol_message(0x7B, build_tag7b_session_summary(cfg)));
	out.push_back(make_protocol_message(0x03, {0x01, 0x01, 0x00, 0x01, 0x00}));
	out.push_back(make_protocol_message(0x05, {0x01}));
	out.push_back(make_protocol_message(0x04, {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02,
	}));
}

// tag=0x0A initial-sync roster. [orig: NapiNPServerMsg_0x00A @0x513260 -> player-sync + tick.]
void emit_roster(const PlayerReplicationState &rep, uint32_t now_tick, std::vector<ProtocolMessage> &out) {
	out.push_back(make_protocol_message(0x46, build_reply_tag_46(rep)));
	out.push_back(make_protocol_message(0x16, build_reply_tag_16(rep)));
	out.push_back(make_protocol_message(0x19, {0x00, 0x00, 0x00, 0x00}));
	out.push_back(make_protocol_message(0x1A, build_tag1a_tick(now_tick)));
}

// tag=0x0C C2S player-input uplink — cache the joiner's pre-spawn pose. [orig: NapiNPServerMsg_0x00C
// @0x501c30 stores the uplink into the player slot.]
void cache_client_pose(const std::vector<uint8_t> &payload, SessionReplyState &st) {
	EntityPacketSubHeader hdr;
	size_t header_consumed = 0;
	if (decode_entity_packet_sub_header(payload.data(), payload.size(), hdr, header_consumed) &&
	    hdr.sub_op == 0x0A) {
		PlayerExtendedUplink uplink;
		size_t body_consumed = 0;
		if (decode_player_extended_uplink(payload.data() + header_consumed,
		                                  payload.size() - header_consumed, uplink, body_consumed) &&
		    header_consumed + body_consumed == payload.size()) {
			st.client_entity_handle = hdr.handle;
			st.client_item_type_id = hdr.item_type_id;
			st.client_vehicle_handle = uplink.vehicle_handle;
			st.client_pos_x = static_cast<uint32_t>(uplink.pos_x);
			st.client_pos_y = static_cast<uint32_t>(uplink.pos_y);
			st.client_pos_z = static_cast<uint32_t>(uplink.pos_z);
			st.client_heading = uplink.heading;
			st.client_pitch = uplink.pitch;
			st.client_pos_valid = true;
		}
	}
}

} // namespace

std::vector<ProtocolMessage> dispatch_session_replies(const SessionReplyConfig &config,
                                                      NapiNPConnection &conn,
                                                      const std::vector<ProtocolMessage> &messages,
                                                      uint32_t now_tick) {
	std::vector<ProtocolMessage> replies;
	SessionReplyState &st = conn.reply;
	const PlayerReplicationState rep = make_rep_state(config, st);

	for (const ProtocolMessage &msg : messages) {
		// Protocol-control + settings-update frames are not gameplay messages (the original filters
		// these before the gameplay dispatch). [orig: dispatch_in_match_session_messages gate]
		if (msg.flags.settings_update || msg.full_tag >= 0x100u) continue;

		switch (msg.tag) {
			case 0x00: // JOIN ack [orig: NapiNPServerMsg_0x000 @0x512AA0]
				replies.push_back(make_protocol_message(0x00, {}));
				break;
			case 0x01: // handshake push [orig: NapiNPServerMsg_0x001 @0x512ED0]
				replies.push_back(make_protocol_message(0x02, build_tag02_push(now_tick)));
				break;
			case 0x02: // GLB_JOIN burst [orig: NapiNPServerMsg_0x002 @0x512FD0]
				emit_post_handshake_burst(config, replies);
				break;
			case 0x47: // re-broadcast entity-state request -> 0x75 [orig: handler @0x510ED0]
				replies.push_back(make_protocol_message(0x75, {0x00, 0x02}));
				break;
			case 0x33: // server-info request -> 0x60 chunk [orig: NapiNPServerMsg_0x033 @0x515230]
				replies.push_back(make_protocol_message(0x60, build_tag60_server_info(config)));
				break;
			case 0x37: // mission-file request -> 0x75 + 0x64 chunk [orig: NapiNPServerMsg_0x037 @0x5152E0]
				replies.push_back(make_protocol_message(0x75, {0x00, 0x02}));
				replies.push_back(make_protocol_message(0x64, build_tag64_mission_metadata(config)));
				break;
			case 0x2F: // loadout request -> 0x5A [orig: NapiNPServerMsg_0x02F @0x515790]
				replies.push_back(make_protocol_message(0x5A, build_tag_5a_weapon_loadout()));
				st.loadout_synced = true;
				break;
			case 0x0A: // initial-sync roster [orig: NapiNPServerMsg_0x00A @0x513260]
				emit_roster(rep, now_tick, replies);
				break;
			case 0x29: // spawn-slot request -> 0x51 PLAYER-SPAWN [orig: NapiNPServerMsg_0x029 @0x514F10].
				// Echo-loop guard: HandlePlayerSpawn @0x431BB0 re-sends 0x29 after our 0x51, so reply once.
				if (!st.player_spawn_confirmed) {
					replies.push_back(make_protocol_message(0x51, build_reply_tag_51(rep)));
					st.player_spawn_confirmed = true;
				}
				break;
			case 0x0E: // respawn request -> 0x1E game-event, once spawned [orig: NapiNPServerMsg_0x00E
				// @0x519AF0 -> Server_ProcessPlayerDeath emits 0x1E]. The INITIAL spawn is the one-shot
				// world-stream burst (Server_SendInitialGameStateToPlayer), not a reactive reply here.
				if (conn.burst.spawned) {
					replies.push_back(make_protocol_message(0x1E, build_tag_1e_game_event_post_spawn()));
				}
				break;
			case 0x0C: // C2S player-input uplink — cache the pre-spawn pose
				cache_client_pose(msg.payload, st);
				break;
			case 0x0B: // mission-file status report [orig: NapiNPServerMsg_0x00B @0x51AB10]
				st.mission_status_received = true;
				break;
			default:
				// 0x2C rtt echo / 0x22 player-sync ack / 0x0F entity-info query / 0x09 checksum / 0x48 +
				// per-frame client updates: consumed (no reactive reply). The original 0x22->0x46 /
				// 0x0F->0x18 paths are part of the deferred body grill wave.
				break;
		}
	}
	return replies;
}

bool bind_session_reply_player(NapiNPConnection &conn, std::string player_name, uint8_t player_slot,
                               uint16_t entity_handle) {
	conn.reply.binding_valid = true;
	conn.reply.player_name = std::move(player_name);
	conn.reply.player_slot = player_slot;
	conn.reply.player_entity_handle = entity_handle;
	return true;
}

} // namespace opennova::np
