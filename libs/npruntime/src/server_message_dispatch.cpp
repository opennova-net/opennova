#include "npruntime/server_message_dispatch.h"

#include "npruntime/weapon_table_build.h" // loadout_entry_permitted / resolve_loadout_ammo (D-NET-141)

#include <netsim/entity_wire_bridge.h> // build_full_entity_spawn — the 0x0F -> 0x18 repair record

#include <npwire/ingame_decode.h>   // decode_entity_packet_sub_header / decode_player_extended_uplink
#include <npwire/ingame_encode.h>   // encode_player_sync / encode_player_list (§5.1)
#include <npwire/replication_model.h> // PlayerReplicationState (POD) — the reply builders' input

#include <world/entity.h> // world::Entity / EntityHandle — team @entity+344 read through owned_entity
#include <world/entity_spawn.h>  // entity_reset_to_spawn_state — the deploy revive (§5.61)
#include <world/spawn_select.h>  // resolve_spawn_target / find_spawn_zone_for_team / spawn_pose_for_target
#include <world/vehicle_attach.h> // entity_process_vehicle_attach / entity_detach_from_vehicle (0x26/0x27)
#include <world/world.h>  // world::World::registry (the authoritative roster, §6.9)
#include <world/zone_chain.h>    // zone_chain_frontier_zone — the 0x1E ev-0x3A deploy hint

#include <algorithm>
#include <cstring>
#include <utility>

namespace opennova::np {

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
		// Rows carry only IN-GAME players — a still-loading joiner (mid world-stream) is
		// excluded until its burst completes, matching the golden 31 B -> 39 B grow right
		// as the joiner enters the match [orig: the scoreboard rows exclude slots with the
		// not-yet-in-game byte slot+100579; Server_BuildAndBroadcastScoreboard @0x50D960].
		if (!is_in_match(c)) continue;
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
// The 0x04 carries THIS connection's live slot + the host's slot capacity + the assigned team —
// byte 17 lands in g_local_player_slot_id (the client's own slot id) and byte 18 in g_max_player_slots
// (g_max_player_slots, the 0x46/0x22 roster-walk terminator): the prior hardcoded (1, 2, 2)
// capped every client's walk at slot 1, so a third player's slot never bound (D-NET-158).
void emit_post_handshake_burst(const GameConfig &cfg, const NapiNPConnection &conn,
                               const world::World *world, std::vector<ProtocolMessage> &out) {
	out.push_back(make_protocol_message(0x00, {0, 0x08, 0, 0, 0, 0x0C, 0, 0, 0}, 0xA0));
	out.push_back(make_protocol_message(0x00, {1, 0x08, 0, 0, 0, 0x0C, 0, 0, 0}, 0xA0));
	out.push_back(make_protocol_message(0x01, {0x01, 0x00, 0x00, 0x00}));
	out.push_back(make_protocol_message(0x7A, build_tag7a_pcid(cfg)));
	out.push_back(make_protocol_message(0x7B, build_tag7b_session_summary(cfg)));
	// [u8 1][u16 count=1][u16 mask=1] — the golden's live restriction record shape.
	out.push_back(make_protocol_message(0x03, {0x01, 0x01, 0x00, 0x01, 0x00}));
	out.push_back(make_protocol_message(0x05, {0x01}));
	uint8_t team = 2; // the golden joiner default for a pre-add edge (spawn pump not yet run)
	if (world != nullptr && conn.link.owned_entity.valid())
		if (const world::Entity *e = world->registry.get(conn.link.owned_entity)) team = e->team;
	const uint8_t capacity =
			static_cast<uint8_t>(cfg.max_players < 251 ? cfg.max_players : 251); // [orig @0x24c0ca4]
	out.push_back(make_protocol_message(
			0x04, build_tag04_slot_assignment(conn.reply.player_slot, capacity, team)));
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
                                                      world::World *world,
                                                      uint32_t session_seed) {
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
					emit_post_handshake_burst(config, conn, world, replies);
					replies.push_back(make_protocol_message(0x16, build_reply_tag_16(roster, rep, world)));
					st.roster_pushed = true;
				}
				break;
			case 0x22: { // player-sync request [u8 slot][u16 fieldFlags] -> S2C 0x46 player-sync.
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
				const bool echo_ack = (req_flags & 0x4000u) != 0;
				const uint16_t reply_flags =
						(req_flags & ~0xC000u) != 0
								? static_cast<uint16_t>(req_flags & ~0x8000u)
								: static_cast<uint16_t>(0x1CF7u | (echo_ack ? 0x4000u : 0u));
				if (slot_has_player) {
					const PlayerReplicationState prs = rep_for_slot(config, roster, req_slot, rep, world);
					replies.push_back(
							make_protocol_message(0x46, encode_player_sync(prs, reply_flags)));
				} else {
					// Empty/disconnected slot: the 0x8000 removal record (|0x4000 when the
					// request asked for the ack) — the walk terminator [orig: @0x505ecb..ee0].
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
				// Retain the GRANTED body: the deploy-release bundle re-sends it (the client's
				// 0x5A apply is the deploy un-latcher — resets dword_81474C; §5.30, D-NET-156).
				st.last_loadout_reply = build_tag_5a_weapon_loadout(msg.payload, armory);
				replies.push_back(make_protocol_message(0x5A, st.last_loadout_reply));
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
			case 0x0E: { // RESPAWN/DEPLOY request [i16 spawnHandle] — the deploy-map pick.
				// [orig: Server_ProcessClientRequestRespawn @0x519AF0; net-re §5.61]. 0xFFFE = the
				// auto frontier pick; a real handle resolves through the SpawnPoint/team gates and a
				// NUMBERED zone additionally requires team match + control >= 1.0 (a contested zone
				// stops accepting spawns). An ALIVE in-session player's request is a no-op [orig: the
				// @0x519cce dead-or-flagged gate; only !is_in_session falls through @0x519cd7]; a DEAD
				// one deploys NOW: position at the pick (zone origin, §5.61 6007-scatter/userpoint
				// deferral) else the per-team marker chain, reset-to-spawn-state, template health —
				// Server_ProcessPlayerDeath's deploy leg [orig: @0x517740 -> Server_PositionPlayerForSpawn
				// @0x50cf60 -> Entity_ResetToSpawnState @0x4B9610]. Deploy-time 0x61 seed re-send and
				// the 0x1D overlay stay burst-only (tracked §5.61 deferral). Deferred with the wave
				// system: g_spawn_wave_list queueing + the 0x6E status (host wave options unmodeled —
				// retail with default options deploys immediately, which this matches). The reply is
				// the private 0x1E ev-0x3A frontier hint [orig: @0x517A1D, mask 0x20].
				if (!conn.burst.spawned) break;
				if (world == nullptr || !conn.link.owned_entity.valid()) {
					// World-less/unit-test path: the golden frame-82540 hint byte, as before.
					replies.push_back(
							make_protocol_message(0x1E, build_tag_1e_game_event_post_spawn()));
					break;
				}
				world::Entity *player = world->registry.get(conn.link.owned_entity);
				if (player == nullptr) break;
				// Body: [i16 spawnHandle]; a short/absent body reads 0 [orig: @0x519b42..48].
				const uint16_t pick = msg.payload.size() >= 2
						? static_cast<uint16_t>(msg.payload[0] | (msg.payload[1] << 8))
						: 0;
				const world::Entity *target = nullptr;
				if (pick == 0xFFFE) {
					// Auto-deploy: the team's frontier zone; null falls back to the marker chain
					// [orig: find_spawn_entity_for_team @0x4fc810 -> requestedHandle -1 on miss].
					target = world::find_spawn_zone_for_team(*world, world->zone_chain,
					                                         player->team, config.game_type);
				} else if (pick != 0 && pick != 0xFFFF) {
					target = world::resolve_spawn_target(*world, player->team, pick);
					if (target == nullptr) break; // invalid pick: silent no-op [orig: @0x519c88]
					// The zone-ownership gate [orig: @0x519d5f: entity+538 -> team match AND
					// control(+540) >= 0x10000].
					if (target->zone_number != 0 &&
					    (target->team != player->team || target->zone_control < 0x10000))
						break;
				}
				// The dead-or-pending gate [orig: @0x519cc7 — requester must be dead
				// (entity+36 & 2) OR respawn-flagged (slot+89912 & 0x10)]: an alive DEPLOYED
				// player's request is a no-op; an alive-but-undeployed joiner deploys now.
				if (!conn.link.respawn_pending && player->health > 0) break;
				world::SpawnPointResult pose;
				if (target != nullptr) {
					pose = world::spawn_pose_for_target(*target);
				} else {
					// No/auto pick: the per-team start-marker chain (6096-6099 -> 6003/6004/...)
					// [orig: Server_PositionPlayerForSpawn @0x50cf60 path B; §5.2c/§5.61].
					pose = world::select_player_spawn_for_team(*world, player->team,
					                                           config.game_type);
				}
				if (pose.found) {
					player->position = pose.position;
					player->yaw = pose.yaw;
				} // no marker at all: redeploy in place (never an NPC position)
				// entity_reset_to_spawn_state re-backs spawn_position from the new pose and
				// clears the movement gate [orig: Entity_ResetToSpawnState @0x4B9610].
				world::entity_reset_to_spawn_state(*player);
				if (world->player_item_hp > 0) player->health = world->player_item_hp;
				else if (player->health_max > 0) player->health = player->health_max;
				else player->health = 100; // [orig: Entity_InitFromItemDef @0x49e550]
				// Successful deploy CLEARS the respawn-pending flag + the hidden bit — the
				// next 0x0A's flags1 bit1 drops, the client closes the deploy screen and
				// enters the world; byte13 loses its 0x01. [orig: Server_ProcessPlayerDeath
				// @0x517791 `and 0xEF` on slot+89912; the entity bit0 stops being re-ORed]
				conn.link.respawn_pending = false;
				player->flags &= ~1u;
				player->alive = true;
				// THE DEPLOY-RELEASE BUNDLE [orig: Server_ProcessPlayerDeath's deploy tail —
				// the loadout re-send (Server_SendWeaponSlotListToPlayer @0x502550) + the 0x61
				// seed (Server_SendRandomSeedToPlayer @0x5101a0, mode 1) + the 0x1E hint; golden
				// deploy frame 240018 carries 0x5A + 0x61 + 0x1E in ONE datagram]. The 0x5A is
				// the client's deploy UN-LATCHER: the 0x0E pick set its dword_81474C wait-gate
				// (Input case 12 @0x49b17b) and ONLY the 0x5A apply resets it (§5.30,
				// NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0) — without this bundle the
				// client NEVER resumes its per-frame C2S 0x0C uplink (v32 live: both joiners'
				// uplinks stopped at the pick frame forever; the host-side entity pinned at the
				// deploy spot = the rubber-band). Re-send the retained granted body; a client
				// that never submitted 0x2F (unit paths) gets the armory-built default shape.
				if (!st.last_loadout_reply.empty()) {
					replies.push_back(make_protocol_message(0x5A, st.last_loadout_reply));
				} else {
					const world::WeaponTable *armory =
							(world != nullptr && !world->weapons.empty()) ? &world->weapons
							                                              : nullptr;
					replies.push_back(make_protocol_message(
							0x5A, build_tag_5a_weapon_loadout({}, armory)));
				}
				replies.push_back(make_protocol_message(
						0x61, {static_cast<uint8_t>(session_seed & 0xFFu),
						       static_cast<uint8_t>((session_seed >> 8) & 0xFFu),
						       static_cast<uint8_t>((session_seed >> 16) & 0xFFu),
						       static_cast<uint8_t>((session_seed >> 24) & 0xFFu)}));
				// The frontier hint, only when a frontier zone exists [orig: the @0x5179e0
				// `if (AvailableSlot)` gate].
				const uint8_t frontier = zone_chain_frontier_zone(*world, world->zone_chain,
				                                                  player->team);
				if (frontier != 0)
					replies.push_back(
							make_protocol_message(0x1E, build_tag_1e_frontier_hint(frontier)));
				break;
			}
			case 0x0C: // C2S player-input uplink — cache the pre-spawn pose
				cache_client_pose(msg.payload, st);
				break;
			case 0x1D: { // STANCE CHANGE [i16 stanceCode] — the crouch/prone replication leg.
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
			case 0x26: { // VEHICLE ATTACH [u16 senderHandle][u16 vehicleHandle][u8 bone][u8 pad]
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
			case 0x27: { // VEHICLE DETACH [u16 selfHandle][u16 vehicleHandle][u16 junk]
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
			case 0x06: { // client fired round -> ammo authority + the S2C 0x0A tag-2 round echo
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
				if (conn.link.mode == netsim::TransportMode::Loopback) break;
				ClientFiredRound fr;
				size_t fire_consumed = 0;
				if (!decode_client_fired_round(msg.payload.data(), msg.payload.size(), fr,
				                               fire_consumed))
					break;
				// Handle validity [orig: @0x513543], then anti-spoof: the claimed shooter must
				// BE this connection's own entity [orig: @0x51358d, doubled at @0x50bf37 -> -9].
				if (fr.shooter_handle == 0xFFFF || (fr.shooter_handle & 0xF000u) >= 0x5000u)
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
					if (adm->clipsize != -1) { // [orig: adm+88 != -1 gates the ammo check
						                       // @0x541caa AND the consume @0x542c6d]
						const uint16_t combo =
								uint16_t(adm->category) * 65u + adm->rank; // [orig: @0x50c0d7]
						WeaponSlotState &slot = conn.weapon_slots[combo];
						if (slot.adm_index != fr.adm_index) {
							// First sight / weapon swap on this combo: bind + seed a full
							// magazine [orig: the loadout binds slot+32; WeaponSlot_ReloadAmmo
							// @0x541811 fills to capacity clamped by the ammo pool — pool
							// clamp deferred].
							slot.adm_index = fr.adm_index;
							slot.clip = adm->clipsize;
						}
						if (slot.clip <= 0) break; // [orig: "(NO AMMO!)" reject @0x50c15c]
						--slot.clip; // [orig: consume_weapon_ammo @0x540913 --u16 slot+16]
					}
					// Primary fire mirrors the equipped weapon onto the entity
					// [orig: @0x50bd56 entity+688 = adm — the §5.10 off-16 source].
					shooter->equipped_adm_index = fr.adm_index;
				}
				// Stamp the claimed target on the shooter; the tag-2 serializer reads it LIVE
				// [orig: @0x50c2ad shooter+104->+12; NetPacket_SerializeRoundEvent @0x50485a].
				world::EntityHandle fire_target{};
				if (fr.target_handle != 0xFFFF && (fr.target_handle & 0xF000u) < 0x5000u) {
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
						world->round_sim.spawn(*world, rp);
					}
				}
				// No reactive reply — the echo rides the per-frame 0x0A fan (netsim
				// select_round_events), reaching every OTHER in-match recipient.
				break;
			}
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
				// Host-side clip refill for a REMOTE requester [orig: WeaponSlot_ReloadAmmo
				// @0x541720, called @0x514F03 after the relay iff requester != local player.
				// Refund + ammo-pool clamp deferred -> refill to capacity @0x541811]. The wire
				// combo (the second u16) keys the same slot the 0x06 pipeline decrements
				// [orig: slotIndex = HIWORD @0x514f03; slot = playerSlot+464+100*combo @0x54176d].
				if (world != nullptr && !world->weapons.empty() &&
				    conn.link.mode != netsim::TransportMode::Loopback) {
					auto slot_it = conn.weapon_slots.find(req.reload_param);
					if (slot_it != conn.weapon_slots.end()) {
						const world::WeaponTableEntry *adm =
								world->weapons.by_index(slot_it->second.adm_index);
						if (adm != nullptr && adm->clipsize != -1)
							slot_it->second.clip = adm->clipsize; // [orig: slot+16 @0x541850]
					}
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

void broadcast_player_sync_on_join(const GameConfig &config,
                                   std::vector<NapiNPConnection> &roster,
                                   const NapiNPConnection &joined, const world::World *world) {
	// [orig: Server_PlayerAdd @0x51D296 — one 0x46 (fieldFlags 0x1CF7, no ack: the walk is the
	// JOINER's own concern) to every in-game connection]. Staged on each recipient's transport
	// like the other broadcast handlers (0x49 relay / the leave removal); the per-connection
	// flush frames it with that connection's own sequencing.
	const PlayerReplicationState rep = make_rep_state(config, joined, world);
	const std::vector<uint8_t> body = encode_player_sync(rep, 0x1CF7);
	for (NapiNPConnection &c : roster) {
		if (&c == &joined || !is_in_match(c) || c.link.transport == nullptr) continue;
		c.link.transport->host_send(0x46, body);
	}
}

} // namespace opennova::np
