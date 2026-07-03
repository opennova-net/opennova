#include "npruntime/server_message_dispatch.h"

#include "npruntime/weapon_table_build.h" // loadout_entry_permitted / resolve_loadout_ammo (D-NET-141)

#include <netsim/entity_wire_bridge.h> // build_full_entity_spawn — the 0x0F -> 0x18 repair record

#include <novaworld/ingame_decode.h>   // decode_entity_packet_sub_header / decode_player_extended_uplink
#include <novaworld/ingame_encode.h>   // encode_player_sync / encode_player_list (§5.1)
#include <novaworld/replication_model.h> // PlayerReplicationState (POD) — the reply builders' input

#include <world/entity.h> // world::Entity / EntityHandle — team @entity+344 read through owned_entity
#include <world/world.h>  // world::World::registry (the authoritative roster, §6.9)

#include <algorithm>
#include <cstring>
#include <utility>

namespace opennova::np {

namespace {

// ---------------------------------------------------------------------------
// Byte helpers (relocated verbatim from the retired game_session.cpp reply builders). The §5.1
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

bool is_default_ash_session_config(const GameConfig &cfg) {
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
std::vector<uint8_t> build_tag7a_pcid(const GameConfig &cfg) {
	std::vector<uint8_t> payload;
	append_cstr(payload, cfg.pcid);
	return payload;
}

// tag=0x7B session/player info. [orig: NapiNPMsg_0x7B_BuildPayload @0x507740] field order:
// player_name, PCID, server_name, title(mission name), map_file, gametype(u32), empty_str, expansion.
// (The PCID slot previously carried an invented "DEV-A02-0001" literal — D-NET-127; now sourced.)
std::vector<uint8_t> build_tag7b_session_summary(const GameConfig &cfg) {
	std::vector<uint8_t> payload;
	append_cstr(payload, cfg.player_name); // [orig player_data+128]
	append_cstr(payload, cfg.pcid);        // [orig entity+592] PCID
	append_cstr(payload, cfg.server_name); // [orig g_server_name_str]
	append_cstr(payload, cfg.mission_name);// [orig title: MissionText "title" / g_GameType title]
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
	append_string_kv(info_body, "MISSIONNAME", cfg.mission_name);
	uint8_t gametype_le[4] = {
			static_cast<uint8_t>(cfg.game_type & 0xFFu),
			static_cast<uint8_t>((cfg.game_type >> 8) & 0xFFu),
			static_cast<uint8_t>((cfg.game_type >> 16) & 0xFFu),
			static_cast<uint8_t>((cfg.game_type >> 24) & 0xFFu),
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
std::vector<uint8_t> build_tag64_mission_metadata(const GameConfig &cfg) {
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
	write_u32_le(mission_blob, 44, cfg.mp_attributes);
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

// tag=0x16 PLAYER-LIST. [orig: NapiNPClientMsg_0x016 @0x42FAE0] Enumerates the LIVE player roster — the
// host loopback (slot 0) + each spawned joiner (slot 1+) — so a joining client sees ITS OWN slot and can
// bind its local player (golden: 0x16 grows 31 B [host only] -> 39 B [host + joiner] right before the
// joiner deploys). `roster` is the connection_list; `fallback` covers the World-less/test path (no bound
// players -> a single default entry). The wire SERIALIZE lives in encode_player_list (novaworld); this is
// just the npruntime-side roster walk (it reads NapiNPConnection, which novaworld cannot) that builds the
// entry list.
std::vector<uint8_t> build_reply_tag_16(const std::vector<NapiNPConnection> &roster,
                                        const PlayerReplicationState &fallback,
                                        const world::World *world) {
	std::vector<PlayerListEntry> players;
	for (const NapiNPConnection &c : roster) {
		if (c.phase < ConnectionPhase::PlayerAdded || !c.link.owned_entity.valid()) continue;
		// team @entity+344 read THROUGH owned_entity (D-NET-132); the World-less path defaults to 1.
		uint8_t team = 1;
		if (world != nullptr)
			if (const world::Entity *e = world->registry.get(c.link.owned_entity)) team = e->team;
		players.push_back({c.reply.player_slot, team});
	}
	std::sort(players.begin(), players.end(),
	          [](const PlayerListEntry &a, const PlayerListEntry &b) { return a.slot < b.slot; });
	if (players.empty()) players.push_back({fallback.player_slot, fallback.team});
	return encode_player_list(players);
}

// tag=0x5A WEAPON-LOADOUT-SYNC, built from the joiner's own C2S 0x2F loadout submit.
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515790 — parses the request, clamps the soldier
// type to [5,9]-else-8 (@0x515913), stamps entity+660 playerClass (@0x515ab0), loads the entries
// into the player's 780-slot weapon table, then Server_SendWeaponSlotListToPlayer @0x502550
// walks that table ascending by weapon-slot combo (category*65 + rank, @0x5026e5..@0x5028a0),
// filtering each slot by the team/char masks (@0x502716) and emitting one 4-byte group:
// [admIdx = AvatarDef_FindIndexByName @0x50273b][ammoPrimary = WeaponSlot_GetTotalClips
// @0x502794][ammoSecondary = the same count for the first different-ammoclass sub-variant in
// parent+1..parent+LSC (@0x5027c8), else 0xFF][restriction byte, witnessed 0 (@0x502871)].]
//
// With the armory table fed (world::World::weapons), the reply resolves REAL counts through the
// witnessed rules (weapon_table_build: loadout_entry_permitted / resolve_loadout_ammo) — the
// golden ASH_I5A reply {2:255, 3:10, 21:10, 76:1, 77:2, 78:3, 83:3} reproduces from the host's
// own resolved weapon.def (D-NET-141). Table-less hosts (unit paths / no resource root) keep the
// prior request-echo: the client clamps echoed bytes on apply [orig: @0x4295d7-0x4295e9], a
// tracked divergence for that configuration only. The armory-enable restriction table
// (`unused6` @0x515a3f / player+89688) stays unmodeled — 4th byte 0 as witnessed.
std::vector<uint8_t> build_tag_5a_weapon_loadout(const std::vector<uint8_t> &request_payload,
                                                 const world::WeaponTable *table) {
	LoadoutSubmit req;
	decode_loadout_submit(request_payload.data(), request_payload.size(), req);

	WeaponLoadout reply;
	// Soldier-type accept: 5..9 pass, everything else forces 8 [orig: @0x515913; the
	// allowed-class restriction mask dword_24D59FC is server armory config, unmodeled].
	reply.avatar_class =
			(req.soldier_type >= 5 && req.soldier_type <= 9) ? req.soldier_type : 8;

	if (table != nullptr && !table->empty()) {
		// Sort key per accepted entry: the weapon-slot combo the original's table walk implies
		// [orig: slot = category*65 + rank, WeaponSlotTable_LoadAllFromDefs @0x5415D3].
		std::vector<std::pair<uint16_t, WeaponLoadoutSlot>> accepted;
		for (const LoadoutSubmitEntry &e : req.entries) {
			const world::WeaponTableEntry *we = table->by_index(e.adm_index);
			if (we == nullptr) continue; // the AdmDef_GetEntryByIndex fail leg
			if (!loadout_entry_permitted(*we, req.player_class, reply.avatar_class))
				continue; // team/char mask filter [orig: @0x502716]
			const LoadoutAmmoBytes ammo = resolve_loadout_ammo(*table, e.adm_index, e.ammo_primary);
			WeaponLoadoutSlot s;
			s.type_id = e.adm_index;
			s.ammo_primary = ammo.primary;
			s.ammo_secondary = ammo.secondary;
			s.ammo_alt = 0; // restriction byte — witnessed 0 (@0x502871)
			accepted.emplace_back(
					static_cast<uint16_t>(we->category * 65u + we->rank), s);
		}
		std::sort(accepted.begin(), accepted.end(),
		          [](const auto &a, const auto &b) { return a.first < b.first; });
		for (const auto &p : accepted) reply.slots.push_back(p.second);
		return encode_weapon_loadout(reply);
	}

	for (const LoadoutSubmitEntry &e : req.entries) {
		WeaponLoadoutSlot s;
		s.type_id = e.adm_index;
		s.ammo_primary = e.ammo_primary;     // echoed; client clamps on apply (@0x4295d7)
		s.ammo_secondary = e.ammo_secondary; // echoed; sub-slot clamp (@0x429652)
		s.ammo_alt = 0;                      // restriction byte — witnessed 0 (@0x502871)
		reply.slots.push_back(s);
	}
	std::sort(reply.slots.begin(), reply.slots.end(),
	          [](const WeaponLoadoutSlot &a, const WeaponLoadoutSlot &b) {
		          return a.type_id < b.type_id;
	          }); // table-less fallback: ascending adm index (coincides for the golden kit)
	return encode_weapon_loadout(reply);
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
	// player_slot / player_name stay on conn.reply (roster order + the echoed ClientHello.co). A
	// World-less bind (bind_session_reply_player) stamps owned_entity with the bare wire handle, so the
	// handle resolves here and the team keeps its default.
	if (conn.link.owned_entity.valid()) {
		ctx.player_name = conn.reply.player_name;
		ctx.player_slot = conn.reply.player_slot;
		ctx.entity_handle = conn.link.owned_entity.packed;
		if (world != nullptr)
			if (const world::Entity *e = world->registry.get(conn.link.owned_entity)) ctx.team = e->team;
	}
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

// tag=0x02 GLB_JOIN post-handshake burst. [orig: NapiNPServerMsg_0x002 @0x512FD0 emits 0x01/0x7A/0x7B;
// the spawn pump CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0 emits 0x03/0x05/0x04. ALL bodies now
// witnessed (grill 2026-07-01), closing the former D-NET-127 observation-carry for this burst:
//   0x00 (settings flag 0xA0) = CS-config update [orig: CNapiNPConnection_SendConfigUpdate @0x6286e0:
//        [u8 direction==0][u32 bit mask][u32 value per set bit]] — ours sets field idx 3 = 12 for both
//        directions, matching the golden.
//   0x03 = NetPacket_WriteWeaponRestrictionFlag @0x502ac0: [u8 1][u16 list-node count][u16 weapon mask
//        (netPlayer inner+76)] when restriction data exists, else [u8 0].
//   0x05 = NetPacket_WriteBoolTrue @0x502c00: exactly {0x01}.
//   0x04 = NetPacket_WriteSlotAssignment @0x502b30 (build_tag04_slot_assignment above).]
void emit_post_handshake_burst(const GameConfig &cfg, std::vector<ProtocolMessage> &out) {
	out.push_back(make_protocol_message(0x00, {0, 0x08, 0, 0, 0, 0x0C, 0, 0, 0}, 0xA0));
	out.push_back(make_protocol_message(0x00, {1, 0x08, 0, 0, 0, 0x0C, 0, 0, 0}, 0xA0));
	out.push_back(make_protocol_message(0x01, {0x01, 0x00, 0x00, 0x00}));
	out.push_back(make_protocol_message(0x7A, build_tag7a_pcid(cfg)));
	out.push_back(make_protocol_message(0x7B, build_tag7b_session_summary(cfg)));
	// [u8 1][u16 count=1][u16 mask=1] — the golden's live restriction record shape.
	out.push_back(make_protocol_message(0x03, {0x01, 0x01, 0x00, 0x01, 0x00}));
	out.push_back(make_protocol_message(0x05, {0x01}));
	// Golden join: slot 1 of capacity 2, team 2. TODO(roster): thread the live slot/capacity/team of
	// the joining connection here once emit_post_handshake_burst receives the connection identity.
	out.push_back(make_protocol_message(0x04, build_tag04_slot_assignment(1, 2, 2)));
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
	    hdr.sub_op == 0x0A) {
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

std::vector<ProtocolMessage> dispatch_session_replies(const GameConfig &config,
                                                      NapiNPConnection &conn,
                                                      const std::vector<ProtocolMessage> &messages,
                                                      uint32_t now_tick,
                                                      std::vector<NapiNPConnection> &roster,
                                                      world::World *world) {
	std::vector<ProtocolMessage> replies;
	SessionReplyState &st = conn.reply;
	const PlayerReplicationState rep = make_rep_state(config, conn, world);

	// The loadout gate (burst phase 7→8) is opened ONLY by the C2S 0x2F handler (case 0x2F below).
	// A prior auto-advance ("any C2S at phase 8") fired on stale handshake messages before the
	// client had processed 0x1A, defeating the pacing. The loopback (type-2) bypasses the gate.

	for (const ProtocolMessage &msg : messages) {
		// Protocol-control + settings-update frames are not gameplay messages (the original filters
		// these before the gameplay dispatch). [orig: dispatch_in_match_session_messages gate]
		if (msg.flags.settings_update || msg.full_tag >= 0x100u) continue;

		switch (msg.tag) {
			case 0x00: // JOIN ack [orig: NapiNPServerMsg_0x000 @0x512AA0]
				replies.push_back(make_protocol_message(0x00, {}));
				break;
			case 0x01: // handshake push -> S2C 0x02 GLB_JOIN push [orig: NapiNPServerMsg_0x001 @0x512ED0]
				// Golden round-trip (f131-134): C 0x01 -> S 0x02 push -> C 0x02 -> S post-handshake
				// burst. The retail client's join-FSM verification (state 6->7, 0x424740) needs this
				// SEQUENCED exchange — a collapsed all-at-once burst leaves it stuck at "Verifying".
				replies.push_back(make_protocol_message(0x02, build_tag02_push(now_tick)));
				break;
			case 0x02: // GLB_JOIN reply -> the post-handshake burst + 0x16 [orig: NapiNPServerMsg_0x002 @0x512FD0]
				// Reactive to the client's C2S 0x02 (the golden's f133->f134 leg). Sets roster_pushed,
				// which unblocks the §5.2a world-stream burst in tick_connections — so the post-handshake
				// (0x01/0x7a/0x7b/0x03/0x16) reliably PRECEDES the world-stream (golden f134-142 vs f144).
				if (!st.roster_pushed) {
					emit_post_handshake_burst(config, replies);
					replies.push_back(make_protocol_message(0x16, build_reply_tag_16(roster, rep, world)));
					st.roster_pushed = true;
				}
				break;
			case 0x22: { // player-sync request [u8 slot][u16 fieldFlags] -> S2C 0x46 player-sync.
				// [orig: NapiNPServerMsg_0x022 @0x514C90; §5.33] The client requests a SPECIFIC slot
				// (body byte 0) + a fieldFlags word (bytes 1-2); reply 0x46 for THAT slot so the joiner
				// binds it. The ACK-WALK is CLIENT-driven: the server ECHOES bit 0x4000 from the request
				// into the reply (NetPacket_SerializeWeaponOverlaySlotState @0x505f05 `if (fieldFlags &
				// 0x4000) adjusted |= 0x4000`), and the CLIENT, on seeing the echoed ack, re-requests
				// slot+1 with fieldFlags 0x5CF7 until slot+1 >= its max-player count byte_A860D1
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
				// Echo the client's ack bit [orig: @0x505f05]. The retail client stops the walk itself at
				// its max-player count (fed from our 0x08 ServerConfig block), so no server-side cap.
				const bool echo_ack = (req_flags & 0x4000u) != 0;
				if (slot_has_player) {
					const PlayerReplicationState prs = rep_for_slot(config, roster, req_slot, rep, world);
					replies.push_back(make_protocol_message(0x46, encode_player_sync(prs, echo_ack)));
				} else {
					replies.push_back(
							make_protocol_message(0x46, encode_player_sync_removal(req_slot, echo_ack)));
				}
				break;
			}
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
			case 0x2F: { // loadout request -> 0x5A [orig: NapiNPServerMsg_0x02F @0x515790]
				// Golden (f317-318): C 0x2F -> S 0x5A + game-start bundle. The 0x5A populates
				// the joiner's weapon slots BEFORE the first 0x0A; without it every 0x0A triggers
				// the weapon-slot mismatch -> C2S 0x0F flood. Also unlatches the burst's phase 8
				// gate so the game-start bundle emits on the next tick. The reply derives from
				// THIS request + the armory table when the host fed one (see
				// build_tag_5a_weapon_loadout; D-NET-141).
				const world::WeaponTable *armory =
						(world != nullptr && !world->weapons.empty()) ? &world->weapons : nullptr;
				replies.push_back(
						make_protocol_message(0x5A, build_tag_5a_weapon_loadout(msg.payload, armory)));
				// Accepted soldier type -> entity+660 playerClass [orig: @0x515ab0] — feeds the
				// §5.10 field-17 class nibble and the 0x0C/0x18 spawn records for this player.
				if (world != nullptr && conn.link.owned_entity.valid()) {
					if (world::Entity *pe = world->registry.get(conn.link.owned_entity)) {
						LoadoutSubmit req;
						decode_loadout_submit(msg.payload.data(), msg.payload.size(), req);
						pe->player_class =
								(req.soldier_type >= 5 && req.soldier_type <= 9)
										? req.soldier_type : 8;
					}
				}
				st.loadout_synced = true;
				conn.burst.loadout_received = true;
				break;
			}
			case 0x0A: // spawn-menu request [orig: NapiNPServerMsg_HandlePlayerSpawnRequest @0x513260]
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
					conn.burst.world_stream_phase = 0;      // [orig: @0x5132f6]
				}
				// Golden (f161-162): C 0x0A (empty) -> S 0x19 (4 B tick) ONLY. The prior
				// emit_roster sent 0x46/0x16/0x19/0x1A — the unsolicited 0x1A re-sets
				// dword_81474C (the deploy gate) via NapiClient_WaitForGameStart @0x42cc10,
				// re-blocking deploy after 0x0F cleared it. The 0x16 pushes are proactive
				// (post-handshake + periodic during world-stream), not reactive to 0x0A.
				// [orig: NetPacket_WriteTimestampB @0x5046f0 -> S2C 0x19 @0x5132f1]
				replies.push_back(make_protocol_message(0x19, build_tag1a_tick(now_tick)));
				break;
			case 0x29: // team/spawn ack [u16 team_change_index] — NO reply on a plain join.
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
			case 0x2C: { // RTT probe [orig: NapiNPServerMsg_HandlePingResponse @0x515070]
				// [u32 timestamp][u8 echo_flag]. echo_flag != 0 -> bounce S2C 0x57 [u32 ts][u8 0]; the
				// echo_flag == 0 return leg is server-internal RTT stat + min/max-ping kick (no reply).
				if (msg.payload.size() >= 5 && msg.payload[4] != 0) {
					const uint32_t ts = static_cast<uint32_t>(msg.payload[0]) |
					                    (static_cast<uint32_t>(msg.payload[1]) << 8) |
					                    (static_cast<uint32_t>(msg.payload[2]) << 16) |
					                    (static_cast<uint32_t>(msg.payload[3]) << 24);
					replies.push_back(make_protocol_message(0x57, build_tag57_pong(ts)));
				}
				break;
			}
			case 0x0B: // mission-file status report [orig: NapiNPServerMsg_0x00B @0x51AB10]
				st.mission_status_received = true;
				break;
			case 0x25: { // reload request -> S2C 0x49 BROADCAST [orig: NapiNPServerMsg_HandleReloadRequest
				// @0x514DF0 — validates the [u16 entityHandle][u16 weaponSlotCombo] body, then relays it
				// verbatim as S2C 0x49 via two NapiNPServer_SendFiltered @0x4C87E0 sends that together
				// reach ALL in-match connections INCLUDING the requester. The client's 0x49 apply is the
				// ONLY place its clip refills / the slot's 0x80 reload-pending flag clears — a host that
				// ignores 0x25 wedges the joiner's weapon after one attempt (§5.58, D-NET-142). Staged on
				// each recipient's transport; the per-connection flush frames it with that connection's
				// own sequencing. Host-side WeaponSlot_ReloadAmmo bookkeeping needs the weapon-slot/pool
				// model — deferred (tracked, D-NET-142 tail).]
				WeaponReload req;
				size_t consumed = 0;
				if (!decode_weapon_reload(msg.payload.data(), msg.payload.size(), req, consumed))
					break;
				const std::vector<uint8_t> body = encode_weapon_reload(req); // rebuilt, never raw (ADR 0003)
				for (NapiNPConnection &c : roster) {
					if (!is_in_match(c) || c.link.transport == nullptr) continue;
					c.link.transport->host_send(0x49, body);
				}
				break;
			}
			case 0x0F: { // entity-info query [u16 handle] -> S2C 0x18 FULL-ENTITY-SPAWN (the self-heal).
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
							frec = netsim::build_full_entity_spawn(*e, conn.link.owned_entity);
						} else {
							frec.slot_id = handle; // empty slot: type-0 record clears the client's entity
						}
						replies.push_back(make_protocol_message(0x18, encode_full_entity_spawn(frec)));
					}
				}
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
	conn.reply.player_name = std::move(player_name);
	conn.reply.player_slot = player_slot;
	return true;
}

ProtocolMessage build_player_list_message(const GameConfig &config,
                                          const std::vector<NapiNPConnection> &roster,
                                          const world::World *world) {
	// `fallback` only matters for an empty roster (World-less path); a real host always has >=1 bound
	// player, so the enumerated roster wins. Build a minimal fallback rep from the config.
	PlayerReplicationState fallback;
	fallback.player_name = config.player_name;
	return make_protocol_message(0x16, build_reply_tag_16(roster, fallback, world));
}

} // namespace opennova::np
