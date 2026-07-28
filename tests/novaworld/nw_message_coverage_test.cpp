// CI coverage gate for the in-game NAPI message catalog (docs §4 / §5.x).
//
// The net analog of ADR 0002's MNU key-coverage harness: it makes "every wire
// byte accounted for" + "no silently-unhandled tag" enforceable in CI without a
// captured pcap. Three checks:
//
//   (1) Catalog consistency — the shared ingame_message_catalog is well-formed:
//       no duplicate (dir,tag), every entry named, every Decoded entry names its
//       decoder, every Unhandled entry gives a reason.
//   (2) Per-Decoded-tag consumption — every tag classed `Decoded` has its decoder
//       consume a representative valid body to the byte (round-trip via the real
//       encoder where one exists; a crafted minimal body otherwise). A decoder
//       that silently under-reads trips here.
//   (3) Decoded-set drift guard — the set of Decoded tags exercised in (2) equals
//       the catalog's Decoded set, so adding a decoder forces cataloguing + a
//       check (and vice-versa).
//
// Self-contained (no env/capture gate) — inputs are encoder round-trips or crafted
// bodies, mirroring nw_pool_decode_unit_test's inline idiom.

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>
#include <npwire/ingame_message_catalog.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <utility>
#include <vector>

using namespace opennova;

namespace {

#define EXPECT(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "FAIL %s:%d  EXPECT(%s)\n", __FILE__, __LINE__, #cond); \
		return 1; \
	} \
} while (0)

// Little-endian wire-body builder (matches the decoders' Cursor byte order).
struct LE {
	std::vector<uint8_t> b;
	void u8(uint8_t v) { b.push_back(v); }
	void u16(uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
	void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
	void zeros(size_t n) { b.insert(b.end(), n, uint8_t(0)); }
};

// (dir,tag) pairs whose decoder was driven to a clean full-consumption in (2).
std::set<std::pair<char, int>> g_covered;
void cover(char dir, int tag) { g_covered.insert({dir, tag}); }

// ---------------------------------------------------------------------------
// (1) Catalog consistency
// ---------------------------------------------------------------------------
int test_catalog_consistency() {
	size_t n = 0;
	const MsgCatalogEntry *t = ingame_message_catalog(&n);
	EXPECT(n > 0);
	for (size_t i = 0; i < n; ++i) {
		EXPECT(t[i].dir == 'S' || t[i].dir == 'C');
		EXPECT(t[i].name != nullptr && t[i].name[0] != '\0');
		EXPECT(t[i].note != nullptr && t[i].note[0] != '\0');
		// Decoded entries must name their decoder so the cross-reference holds.
		if (t[i].coverage == MsgCoverage::Decoded)
			EXPECT(std::strstr(t[i].note, "decode_") != nullptr);
		// No duplicate (dir,tag).
		for (size_t j = i + 1; j < n; ++j)
			EXPECT(!(t[i].dir == t[j].dir && t[i].tag == t[j].tag));
	}
	// Lookup helpers behave: known tag resolves, uncatalogued tag is null.
	const char *name = ingame_message_name('S', 0x0A);
	EXPECT(name != nullptr && std::strcmp(name, "per-frame-update") == 0);
	EXPECT(ingame_message_name('S', 0xEE) == nullptr);
	EXPECT(lookup_ingame_message('C', 0x06) != nullptr);
	std::printf("PASS catalog_consistency (%zu entries)\n", n);
	return 0;
}

// ---------------------------------------------------------------------------
// (2) Per-Decoded-tag consumption
// ---------------------------------------------------------------------------

// S2C 0x0A — minimal valid frame: 12-B anchor + flags + sub-block 0 (weapon, 11 B)
// + 7-B tail + tag-0 terminator = 33 B. Asserts the walk consumes it cleanly.
int check_S_0A_frame_update() {
	LE w;
	w.u32(0); w.u32(0); w.u32(0);       // anchor x/y/z
	w.u8(0);                            // flags1
	w.u8(0);                            // flags2 -> sub_block 0, no passenger
	w.zeros(6);                         // weapon: preround + slot-state bytes
	w.u8(0xFF);                         // weapon reload_seconds (belt-fed special)
	w.u32(0);                           // uniform_team_mask i32
	w.u8(0); w.u16(0); w.u16(0); w.u16(0); // tail: state_flag, mount, health, state_word
	w.u8(0);                            // event-loop terminator (tag 0)
	EXPECT(w.b.size() == 33);
	std::function<EntityClass(uint16_t)> noclass;
	FrameUpdate fu;
	EXPECT(decode_frame_update(w.b.data(), w.b.size(), noclass, fu));
	EXPECT(fu.complete);
	EXPECT(fu.consumed == 33);
	cover('S', 0x0A);
	return 0;
}

// S2C 0x0D — pool spawn batch, one record, via the real encoder (round-trip).
int check_S_0D_pool_spawn() {
	PoolSpawnBatch in;
	PoolSpawnRecord r;
	r.slot_id = 0x1000;          // pool 1, slot 0 (not the 0xFFFF/>=0x5000 sentinel)
	r.item_type_id = 5;
	r.entity_name = "ai";
	r.pos_x = 100; r.pos_y = 200; r.pos_z = 300;
	r.bone_byte = 7;
	r.team_byte = 2;             // -> spawn_flags 0x0010 (exercises a gated field)
	in.records.push_back(r);
	std::vector<uint8_t> wire = encode_pool_spawn_batch(in);
	PoolSpawnBatch out;
	EXPECT(decode_pool_spawn_batch(wire.data(), wire.size(), out));
	EXPECT(out.records.size() == 1);
	EXPECT(out.records[0].slot_id == 0x1000);
	EXPECT(out.records[0].team_byte == 2);
	cover('S', 0x0D);
	return 0;
}

// S2C 0x20 — pool-3 sync batch, one record, via the real encoder (round-trip).
int check_S_20_pool3_sync() {
	Pool3SyncBatch in;
	in.start_index = 0;
	Pool3SyncRecord r;
	r.item_type_id = 5;
	r.movement_val = 0x40000000; // -> flags 0x01 (BAM heading; exercises a gated field)
	r.pos_x = 10; r.pos_y = 20; r.pos_z = 30;
	r.net_handle = 0x1001;
	in.records.push_back(r);
	std::vector<uint8_t> wire = encode_pool3_sync_batch(in);
	Pool3SyncBatch out;
	EXPECT(decode_pool3_sync_batch(wire.data(), wire.size(), out));
	EXPECT(out.records.size() == 1);
	EXPECT(out.records[0].movement_val == 0x40000000);
	EXPECT(out.records[0].net_handle == 0x1001);
	cover('S', 0x20);
	return 0;
}

// S2C 0x0C — organic spawn batch: one empty-body record (slot + has_body=0).
int check_S_0C_organic() {
	LE w;
	w.u16(1);        // entity_count
	w.u16(0x0001);   // slot_id (not a sentinel)
	w.u8(0);         // has_body = 0 -> record ends here
	OrganicSpawnBatch out;
	EXPECT(decode_organic_spawn_batch(w.b.data(), w.b.size(), out));
	EXPECT(out.records.size() == 1);
	EXPECT(!out.records[0].has_body);
	cover('S', 0x0C);
	return 0;
}

// S2C 0x40 — capture-zone overlay: [u8 count=1][6-byte entry].
int check_S_40_capture_zone() {
	LE w;
	w.u8(1);         // count
	w.u16(0x1002);   // handle
	w.u8(0x03);      // param
	w.u8(0x0a);      // icon_color
	w.u8(0x10);      // flags
	w.u8(0x00);      // source
	EXPECT(w.b.size() == 7);
	CaptureZoneOverlayBatch out;
	EXPECT(decode_capture_zone_overlay(w.b.data(), w.b.size(), out));
	EXPECT(out.entries.size() == 1);
	EXPECT(out.entries[0].handle == 0x1002);
	cover('S', 0x40);
	return 0;
}

// S2C 0x1E — game event: fixed 8 B.
int check_S_1E_game_event() {
	LE w;
	w.u8(4);         // event_type (a kill)
	w.u8(0); w.u8(1); w.u8(0xFF); // attacker, victim, aux indices
	w.u16(50);       // pos_x (i16)
	w.u16(60);       // pos_y (i16)
	EXPECT(w.b.size() == 8);
	GameEventRecord rec;
	size_t consumed = 0;
	EXPECT(decode_game_event(w.b.data(), w.b.size(), rec, consumed));
	EXPECT(consumed == 8);
	cover('S', 0x1E);
	return 0;
}

// S2C 0x61 — the per-player TICK SEED (not an SCRK exchange). Four LE bytes, and a body
// shorter than that seeds ZERO rather than failing: zero is itself the witnessed round-end
// disarm value, so the decode always succeeds and the caller applies whatever it yields.
// [orig: NapiNPClientMsg_HandleSessionKey @0x4297c0 — the four-byte guard @0x4297eb;
//  the sender Server_SendRandomSeedToPlayer @0x5101a0 rolls ((rand() & 0xFE) + 1) << 16]
int check_S_61_tick_seed() {
	LE w;
	w.u32(0x00110000u);
	EXPECT(w.b.size() == 4);
	uint32_t seed = 0xDEADBEEFu;
	EXPECT(decode_tick_seed(w.b.data(), w.b.size(), seed));
	EXPECT(seed == 0x00110000u);
	// Short body -> a seed of zero, never a garbage read past the payload.
	const uint8_t short_body[2] = {0x11, 0x22};
	seed = 0xDEADBEEFu;
	EXPECT(decode_tick_seed(short_body, sizeof(short_body), seed));
	EXPECT(seed == 0);
	// The witnessed disarm form.
	const uint8_t zero_body[4] = {0, 0, 0, 0};
	seed = 0xDEADBEEFu;
	EXPECT(decode_tick_seed(zero_body, sizeof(zero_body), seed));
	EXPECT(seed == 0);
	cover('S', 0x61);
	return 0;
}

// S2C 0x26 — kill record: [u16 victim][u16 attacker].
int check_S_26_kill() {
	LE w;
	w.u16(0x1003);   // victim slot
	w.u16(0x1004);   // attacker
	KillRecord rec;
	size_t consumed = 0;
	EXPECT(decode_kill_record(w.b.data(), w.b.size(), rec, consumed));
	EXPECT(consumed == 4);
	EXPECT(rec.victim_slot == 0x1003);
	cover('S', 0x26);
	return 0;
}

// S2C 0x4E — batch despawn: [u16 count][u16 slot...].
int check_S_4E_batch_kill() {
	LE w;
	w.u16(1);        // count (echo only)
	w.u16(0x1234);   // one slot
	BatchKillBatch out;
	EXPECT(decode_batch_kill(w.b.data(), w.b.size(), out));
	EXPECT(out.slots.size() == 1);
	EXPECT(out.slots[0] == 0x1234);
	cover('S', 0x4E);
	return 0;
}

// S2C 0x10 — static-entity batch: [u16 startIdx][u16 count] + one record with
// flags=team-only, exercising the unconditional ammo/weapon bytes and the
// no-attachRef path (weaponByte==0 && !(flags & 0x200)).
int check_S_10_static_entity() {
	LE w;
	w.u16(0);          // start_index
	w.u16(1);          // entity_count
	w.u16(0x0465);     // item_type_id (armory; non-zero, not the empty-slot sentinel)
	w.u16(0x0010);     // field_flags -> team present only
	w.u32(0xFFAC0000); w.u32(0x00010000); w.u32(0x00180000); // pos x/y/z
	w.u8(0x01);        // team (flags & 0x10)
	w.u8(0x00);        // ammo_count (unconditional)
	w.u8(0x00);        // weapon_byte (unconditional); 0 && no 0x200 -> no attach_ref
	EXPECT(w.b.size() == 23);
	StaticEntityBatch out;
	EXPECT(decode_static_entity_batch(w.b.data(), w.b.size(), out));
	EXPECT(out.records.size() == 1);
	EXPECT(out.records[0].item_type_id == 0x0465);
	EXPECT(out.records[0].team_byte == 0x01);
	cover('S', 0x10);
	return 0;
}

// S2C 0x16 — player-list: header + 1 player row + team_count=1 (2 team rows) + trailer.
int check_S_16_player_list() {
	LE w;
	w.u8(1);            // flags (bit0 team-mode)
	w.u8(1);            // player_count
	w.u8(0x00);         // row: slot_id
	w.u16(0);           //      ping
	w.u16(10);          //      score1
	w.u16(20);          //      score2
	w.u8(0x02);         //      flags -> team1
	w.u8(1);            // team_count -> 2 team rows (T0 + T1)
	for (int i = 0; i < 2; ++i) { w.u16(0); w.u16(0); w.u8(0); w.u8(0); }
	w.u8(0); w.u8(0);   // trailer extra1/extra2
	EXPECT(w.b.size() == 25);
	PlayerList out;
	EXPECT(decode_player_list(w.b.data(), w.b.size(), out));
	EXPECT(out.players.size() == 1);
	EXPECT(out.teams.size() == 2);
	EXPECT(out.players[0].flags == 0x02);
	cover('S', 0x16);
	return 0;
}

// S2C 0x46 — player-sync: name(0x0001) + team(0x0004) + ack(0x4000), source order.
int check_S_46_player_sync() {
	LE w;
	w.u8(0x01);         // slot_id
	w.u16(0x4005);      // bitmask: name | team | ack
	w.u8(0x05);         // entity_slot_id
	w.u8('P'); w.u8(0); // name cstr "P"
	w.u8(0x02);         // team
	EXPECT(w.b.size() == 7);
	PlayerSync out;
	EXPECT(decode_player_sync(w.b.data(), w.b.size(), out));
	EXPECT(!out.removal);
	EXPECT(out.entity_slot_id == 0x05);
	EXPECT(out.name == "P");
	EXPECT(out.team == 0x02);
	EXPECT(out.queue_ack);
	cover('S', 0x46);
	return 0;
}

// C2S 0x0C — extended player uplink body: fixed 43 B.
int check_C_0C_extended_uplink() {
	LE w;
	w.zeros(43);
	PlayerExtendedUplink rec;
	size_t consumed = 0;
	EXPECT(decode_player_extended_uplink(w.b.data(), w.b.size(), rec, consumed));
	EXPECT(consumed == 43);
	cover('C', 0x0C);
	return 0;
}

// C2S 0x06 — client fired round: fixed 45 B.
int check_C_06_fired_round() {
	LE w;
	w.zeros(45);
	ClientFiredRound rec;
	size_t consumed = 0;
	EXPECT(decode_client_fired_round(w.b.data(), w.b.size(), rec, consumed));
	EXPECT(consumed == 45);
	cover('C', 0x06);
	return 0;
}

// C2S 0x21 — anti-cheat CRC reply: 5 B effective (u8 + u32).
int check_C_21_checksum_reply() {
	LE w;
	w.u8(7);         // player_index
	w.u32(0xDEADBEEF); // expected_crc
	ClientChecksumReply rec;
	size_t consumed = 0;
	EXPECT(decode_client_checksum_reply(w.b.data(), w.b.size(), rec, consumed));
	EXPECT(consumed == 5);
	EXPECT(rec.expected_crc == 0xDEADBEEF);
	cover('C', 0x21);
	return 0;
}

// S2C 0x5A — weapon-loadout: avatarClass + one slot + 0xFF terminator.
int check_S_5A_weapon_loadout() {
	LE w;
	w.u8(3);                       // avatar_class
	w.u8(5);                       // slot 0 type id (AdmDef index)
	w.u8(10); w.u8(11); w.u8(12);  // ammo primary/secondary/alt
	w.u8(0xFF);                    // terminator (replaces next typeId)
	EXPECT(w.b.size() == 6);
	WeaponLoadout out;
	EXPECT(decode_weapon_loadout(w.b.data(), w.b.size(), out));
	EXPECT(out.avatar_class == 3);
	EXPECT(out.slots.size() == 1);
	EXPECT(out.slots[0].type_id == 5 && out.slots[0].ammo_alt == 12);
	cover('S', 0x5A);
	return 0;
}

// S2C 0x6E — roster: one team with one member.
int check_S_6E_roster() {
	LE w;
	w.u8(1);          // team_count
	w.u16(0x1000);    // team_entity_handle
	w.u16(0);         // team_slot_index
	w.u8(1);          // member_count
	w.u16(0x2000);    // team_slot_handle
	w.u16(0x0004);    // member handle
	EXPECT(w.b.size() == 10);
	RosterSync out;
	EXPECT(decode_roster_sync(w.b.data(), w.b.size(), out));
	EXPECT(out.teams.size() == 1);
	EXPECT(out.teams[0].members.size() == 1);
	EXPECT(out.teams[0].members[0] == 0x0004);
	cover('S', 0x6E);
	return 0;
}

// S2C 0x7B — full player info: 5 cstrings + u32 + 2 cstrings.
int check_S_7B_full_player_info() {
	LE w;
	auto str = [&](const char *s) {
		for (const char *p = s; *p; ++p) w.u8(uint8_t(*p));
		w.u8(0);
	};
	str("Name"); str("00000003"); str("biggy"); str("MyMission"); str("m.bms");
	w.u32(0xCAFEBABE);
	str("motd here"); str("game here");
	FullPlayerInfo out;
	EXPECT(decode_full_player_info(w.b.data(), w.b.size(), out));
	EXPECT(out.player_name == "Name");
	EXPECT(out.player_id == "00000003");   // NovaWorld player/account ID, not a clan tag
	EXPECT(out.server_name == "biggy");
	EXPECT(out.map_file == "m.bms");
	EXPECT(out.extra == 0xCAFEBABE);
	EXPECT(out.game_name == "game here");
	cover('S', 0x7B);
	return 0;
}

// S2C 0x0F — world-state-load: header + fixed 128-i32 score block + 0 waypoints
// (gametype hint off) + 0 team names.
int check_S_0F_world_state() {
	LE w;
	w.u32(0x11223344);                       // session_tick
	w.u32(100); w.u32(200); w.u32(300);      // pos x/y/z (16.16)
	w.u16(0x4000); w.u16(0); w.u16(0);       // yaw/pitch/roll (i16)
	w.u8(0x00);                              // game_flags
	for (int i = 0; i < kWorldStateScoreCount; ++i) w.u32(0); // 128 scores
	w.u16(0);                                // waypoint_count
	w.u16(0);                                // team_name_count
	EXPECT(w.b.size() == size_t(4 + 12 + 6 + 1 + 4 * kWorldStateScoreCount + 2 + 2));
	WorldStateLoad out;
	EXPECT(decode_world_state_load(w.b.data(), w.b.size(), out));
	EXPECT(out.session_tick == 0x11223344);
	EXPECT(out.yaw == 0x4000);
	EXPECT(out.waypoint_count == 0);
	EXPECT(out.team_scores.size() == size_t(kWorldStateScoreCount));
	cover('S', 0x0F);
	return 0;
}

// S2C 0x60 / 0x64 — file-transfer chunk: 12-B header + raw payload. One decoder
// serves both tags; cover() each so the drift guard balances.
int check_S_60_64_file_transfer() {
	LE w;
	w.u32(1);            // transfer_id
	w.u32(8);            // total_size
	w.u32(0);            // chunk_offset
	w.u32(0xDEADBEEF);   // 8 raw payload bytes (offset 0 + 8 == total -> final chunk)
	w.u32(0x0BADF00D);
	EXPECT(w.b.size() == 12 + 8);
	FileTransferChunk out;
	EXPECT(decode_file_transfer_chunk(w.b.data(), w.b.size(), out));
	EXPECT(out.transfer_id == 1);
	EXPECT(out.chunk_size == 8);
	EXPECT(out.is_final());
	cover('S', 0x60);
	cover('S', 0x64);
	return 0;
}

// C2S 0x22 — player-sync request: [u8 slot][u16 fieldFlags].
int check_C_22_player_sync_request() {
	LE w;
	w.u8(0x02);
	w.u16(0x5CF7);
	BurstPlayerSyncRequest r;
	size_t consumed = 0;
	EXPECT(decode_burst_player_sync_request(w.b.data(), w.b.size(), r, consumed));
	EXPECT(consumed == 3);
	EXPECT(r.field_flags == 0x5CF7);
	cover('C', 0x22);
	return 0;
}

// C2S 0x23 — visible-players request: empty body.
int check_C_23_visible_request() {
	size_t consumed = 1;
	EXPECT(decode_burst_visible_request(nullptr, 0, consumed));
	EXPECT(consumed == 0);
	cover('C', 0x23);
	return 0;
}

// C2S 0x28 — loadout request: [u32][u32][u16].
int check_C_28_loadout_request() {
	LE w;
	w.u32(0x11111111);
	w.u32(0x22222222);
	w.u16(0x3333);
	BurstLoadoutRequest r;
	size_t consumed = 0;
	EXPECT(decode_burst_loadout_request(w.b.data(), w.b.size(), r, consumed));
	EXPECT(consumed == 10);
	EXPECT(r.flags == 0x22222222);
	cover('C', 0x28);
	return 0;
}

// C2S 0x29 — team/spawn ack: [u16 team_change_index].
int check_C_29_team_spawn_ack() {
	LE w;
	w.u16(0x0042);
	TeamSpawnAck r;
	size_t consumed = 0;
	EXPECT(decode_team_spawn_ack(w.b.data(), w.b.size(), r, consumed));
	EXPECT(consumed == 2);
	EXPECT(r.team_change_index == 0x0042);
	cover('C', 0x29);
	return 0;
}

// C2S 0x4C — client quality byte: [u8] (server clamps to 4).
int check_C_4C_client_quality() {
	LE w;
	w.u8(9);             // > 4 -> clamps to 4
	BurstClientQuality r;
	size_t consumed = 0;
	EXPECT(decode_burst_client_quality(w.b.data(), w.b.size(), r, consumed));
	EXPECT(consumed == 1);
	EXPECT(r.value == 4);
	cover('C', 0x4C);
	return 0;
}

// S2C 0x57 / C2S 0x2C — RTT ping/pong: [u32 timestamp][u8 echo_flag] (5 B). One
// decoder serves both directions; cover() each so the drift guard balances.
int check_rtt_sample() {
	LE w;
	w.u32(0x22330011);   // timestamp
	w.u8(1);             // echo_flag
	EXPECT(w.b.size() == 5);
	RttSample r;
	size_t consumed = 0;
	EXPECT(decode_rtt_sample(w.b.data(), w.b.size(), r, consumed));
	EXPECT(consumed == 5);
	EXPECT(r.timestamp == 0x22330011);
	EXPECT(r.echo_flag == 1);
	cover('S', 0x57);
	cover('C', 0x2C);
	return 0;
}

// S2C 0x68 / 0x43 / 0x39 / 0x19 — periodic scalar quartet: a single [u32] (4 B).
// One reader serves all four; cover() each.
int check_u32_scalar_trio() {
	LE w;
	w.u32(0x00003D5D);
	EXPECT(w.b.size() == 4);
	uint32_t v = 0;
	size_t consumed = 0;
	EXPECT(decode_u32_scalar(w.b.data(), w.b.size(), v, consumed));
	EXPECT(consumed == 4);
	EXPECT(v == 0x00003D5D);
	cover('S', 0x68);
	cover('S', 0x43);
	cover('S', 0x39);
	cover('S', 0x19);
	return 0;
}

// S2C 0x6B — minimap overlay: [u8 count=1] + one 12-B record (handle + 10 raw).
int check_S_6B_minimap() {
	LE w;
	w.u8(1);            // count
	w.u16(0x0005);      // record handle
	w.zeros(10);        // 10 raw trailing bytes (unused by the handler)
	EXPECT(w.b.size() == 13);
	MinimapOverlayBatch out;
	EXPECT(decode_minimap_overlay_batch(w.b.data(), w.b.size(), out));
	EXPECT(out.entries.size() == 1);
	EXPECT(out.entries[0].handle == 0x0005);
	cover('S', 0x6B);
	return 0;
}

// S2C 0x49 — weapon reload: [u16 handle][u16 reloadParam] (4 B).
int check_S_49_weapon_reload() {
	LE w;
	w.u16(0x0006);
	w.u16(0x00C3);
	EXPECT(w.b.size() == 4);
	WeaponReload r;
	size_t consumed = 0;
	EXPECT(decode_weapon_reload(w.b.data(), w.b.size(), r, consumed));
	EXPECT(consumed == 4);
	EXPECT(r.entity_handle == 0x0006);
	EXPECT(r.reload_param == 0x00C3);
	cover('S', 0x49);
	return 0;
}

// C2S 0x25 — weapon-reload request: same 4-B [u16 handle][u16 weaponSlotCombo] body the host
// relays back as S2C 0x49 (§5.58). Round-trip via the real encoder.
int check_C_25_reload_request() {
	WeaponReload out;
	out.entity_handle = 0x0006;
	out.reload_param  = 0x00C3; // weaponSlotCombo = category*65 + rank
	const std::vector<uint8_t> wire = encode_weapon_reload(out);
	EXPECT(wire.size() == 4);
	WeaponReload r;
	size_t consumed = 0;
	EXPECT(decode_weapon_reload(wire.data(), wire.size(), r, consumed));
	EXPECT(consumed == 4);
	EXPECT(r.entity_handle == 0x0006);
	EXPECT(r.reload_param == 0x00C3);
	cover('C', 0x25);
	return 0;
}

// S2C 0x13 — entity death (second path): [u16 handle][i16 killerSource] (4 B).
int check_S_13_entity_death() {
	LE w;
	w.u16(0x0006);
	w.u16(0xFFFF);     // killer_source = -1
	EXPECT(w.b.size() == 4);
	EntityDeathRecord d;
	size_t consumed = 0;
	EXPECT(decode_entity_death(w.b.data(), w.b.size(), d, consumed));
	EXPECT(consumed == 4);
	EXPECT(d.entity_handle == 0x0006);
	EXPECT(d.killer_source == -1);
	cover('S', 0x13);
	return 0;
}

// S2C 0x30 — entity-checksum request: [u8 entityId][u16 checksum] (3 B).
int check_S_30_checksum_request() {
	LE w;
	w.u8(0x02);
	w.u16(0xBEEF);
	EXPECT(w.b.size() == 3);
	EntityChecksumRequest r;
	size_t consumed = 0;
	EXPECT(decode_entity_checksum_request(w.b.data(), w.b.size(), r, consumed));
	EXPECT(consumed == 3);
	EXPECT(r.entity_id == 0x02);
	EXPECT(r.checksum == 0xBEEF);
	cover('S', 0x30);
	return 0;
}

// S2C 0x31 — loadout/ammo CRC request: [u8 ammoIndex][u16 xorKey] (3 B).
int check_S_31_loadout_crc_request() {
	LE w;
	w.u8(0x07);
	w.u16(0x1234);
	EXPECT(w.b.size() == 3);
	LoadoutCrcRequest r;
	size_t consumed = 0;
	EXPECT(decode_loadout_crc_request(w.b.data(), w.b.size(), r, consumed));
	EXPECT(consumed == 3);
	EXPECT(r.ammo_index == 0x07);
	EXPECT(r.xor_key == 0x1234);
	cover('S', 0x31);
	return 0;
}

// S2C 0x42 — input/state-flags: [u16] (2 B).
int check_S_42_input_flags() {
	LE w;
	w.u16(0x1234);
	uint16_t flags = 0;
	size_t consumed = 0;
	EXPECT(decode_input_state_flags(w.b.data(), w.b.size(), flags, consumed));
	EXPECT(consumed == 2);
	EXPECT(flags == 0x1234);
	cover('S', 0x42);
	return 0;
}

// S2C 0x79 — spectator-mode flag: [u8] (1 B).
int check_S_79_spectator_flag() {
	LE w;
	w.u8(1);
	uint8_t flag = 0;
	size_t consumed = 0;
	EXPECT(decode_spectator_flag(w.b.data(), w.b.size(), flag, consumed));
	EXPECT(consumed == 1);
	EXPECT(flag == 1);
	cover('S', 0x79);
	return 0;
}

// S2C 0x2A — chat-history entry: [i32 a][i32 b][i16 c] (10 B).
int check_S_2A_chat_history() {
	LE w;
	w.u32(0x11223344);
	w.u32(0x55667788);
	w.u16(0x99AA);
	EXPECT(w.b.size() == 10);
	ChatHistoryEntry e;
	size_t consumed = 0;
	EXPECT(decode_chat_history_entry(w.b.data(), w.b.size(), e, consumed));
	EXPECT(consumed == 10);
	EXPECT(uint32_t(e.field_a) == 0x11223344);
	EXPECT(uint32_t(e.field_b) == 0x55667788);
	cover('S', 0x2A);
	return 0;
}

// S2C 0x59 — deployed-item / weapon-overlay spawn: fixed 32 B.
int check_S_59_deployed_item() {
	LE w;
	w.u16(0x0362);  // item_id
	w.u16(0x0005);  // owner_handle
	w.u16(0x0362);  // friendly_item_id
	w.u16(0x0000);  // enemy_item_id
	w.u16(0x100B);  // slot_handle
	w.u16(0xFFFF);  // parent_handle (none)
	w.u32(0x00357A0F); w.u32(0xFFE41A76); w.u32(0x000B9705); // pos x/y/z
	w.u16(0xAF58); w.u16(0xCBBA); w.u16(0x0000); // angles
	w.u16(0x0000);  // reserved
	EXPECT(w.b.size() == 32);
	DeployedItemSpawn d;
	size_t consumed = 0;
	EXPECT(decode_deployed_item_spawn(w.b.data(), w.b.size(), d, consumed));
	EXPECT(consumed == 32);
	EXPECT(d.slot_handle == 0x100B);
	EXPECT(d.owner_handle == 0x0005);
	EXPECT(uint32_t(d.pos_x) == 0x00357A0F);
	EXPECT(d.parent_handle == 0xFFFF);
	cover('S', 0x59);
	return 0;
}

// S2C 0x45 — terrain-tile load: a header chunk ('til0' magic + tile_count) plus
// one 12-B opaque tile entry. Asserts the framing + raw tile copy consume cleanly.
int check_S_45_terrain_load() {
	LE w;
	w.u16(0xFFFF);      // header-chunk marker (wire start word)
	w.u16(1);           // end_index -> 1 tile
	w.u32(0x74696C30);  // 'til0' magic
	w.u32(1);           // tile_count
	w.u32(0);           // hdr2
	w.u32(0);           // hdr3
	w.u32(0x11111111); w.u32(0x22222222); w.u32(0x33333333); // one 12-B tile entry
	EXPECT(w.b.size() == 32);
	TerrainLoadBatch out;
	EXPECT(decode_terrain_load_batch(w.b.data(), w.b.size(), out));
	EXPECT(out.has_header);
	EXPECT(out.start_index == 0 && out.end_index == 1);
	EXPECT(out.tile_count == 1);
	EXPECT(out.tiles.size() == 1);
	EXPECT(out.tiles[0].word1 == 0x22222222);
	cover('S', 0x45);
	return 0;
}

// S2C 0x18 — full-entity-spawn (§5.46): the reply to a C2S 0x0F entity-info query.
// A hand-built player record per the witnessed serializer layout
// [orig: serialize_object_to_buffer @0x504d10]; asserts the client-handler field
// order [orig: NapiNPClientMsg_FullEntitySpawn @0x433780] consumes cleanly.
int check_S_18_full_entity_spawn() {
	LE w;
	w.u16(0x0000);      // slot handle (pool 0, slot 0)
	w.u16(0x14B9);      // wire type id ("Player #1, Multiplayer")
	w.u8(3);            // itemDef type = ItemType_Person
	w.u8(1);            // team (entity+354)
	w.u16(0x0100);      // Flags word (entity+36) — remote player
	w.u32(0x0000000C);  // owner connection id (entity+120)
	w.u8('P'); w.u8('l'); w.u8('r'); w.u8(0); // name cstr
	w.u16(0xFFFF);      // parent vehicle (entity+368)
	w.u16(0xFFFF);      // ground entity (entity+40)
	w.u16(0xFFFF);      // parent entity (entity+364)
	w.u8(0x02);         // seat mask: only seat 1 carried
	w.u16(0x0001);      // seat-1 occupant handle
	w.u16(0); w.u16(0); // mount handles 8/9 (entity+416/418)
	w.u32(0x00120000); w.u32(0x00340000); w.u32(0x00050000); // pos 16.16
	w.u16(0x005A);      // yaw high word
	w.u16(0x0000);      // pitch high word
	w.u8(0);            // ai_state (entity+692)
	w.u8(0);            // anim_slot (entity+884)
	w.u16(0x0200);      // minimap net_id (entity+348)
	w.u8(8);            // playerClass (entity+660)
	w.u8(0);            // hard-0 skip byte
	w.u8(0);            // entity+340
	w.u8(0);            // entity+533
	w.u8(0);            // entity+532
	EXPECT(w.b.size() == 54);
	FullEntitySpawnRecord r;
	EXPECT(decode_full_entity_spawn(w.b.data(), w.b.size(), r));
	EXPECT(r.slot_id == 0x0000);
	EXPECT(r.item_type_id == 0x14B9);
	EXPECT(r.item_type == 3);
	EXPECT(r.team == 1);
	EXPECT(r.minimap_flags == 0x0100);
	EXPECT(r.entity_flags == 0x0C);
	EXPECT(r.entity_name == "Plr");
	EXPECT(r.seat_mask == 0x02);
	EXPECT(r.mount_handles[0] == 0xFFFF && r.mount_handles[1] == 0x0001);
	EXPECT(uint32_t(r.pos_x) == 0x00120000);
	EXPECT(r.heading_hi == 0x005A);
	EXPECT(r.net_id == 0x0200);
	EXPECT(r.player_class == 8);
	cover('S', 0x18);
	return 0;
}

// S2C 0x58 — session-status block (§5.48): names + 3 bytes + uptime + 39 stat
// values + 2 kv pairs. [orig: SessionStatus_ParseFromBuffer @ 0x530ED0]
int check_S_58_session_status() {
	LE w;
	auto str = [&](const char *s) { for (const char *p = s; *p; ++p) w.u8(uint8_t(*p)); w.u8(0); };
	str("biggy");            // server name
	str("ASH_I5A");          // mission name
	w.u8(1); w.u8(2); w.u8(3);
	w.u32(123456);           // uptime ms at send
	for (int i = 0; i < 39; ++i) w.u32(uint32_t(i == 4 ? -5 : (i == 0 ? 10 : 0)));
	w.u8(2);                 // kv count
	w.u8(1); w.u32(100);
	w.u8(9); w.u32(200);
	SessionStatusBlock out;
	EXPECT(decode_session_status(w.b.data(), w.b.size(), out));
	EXPECT(out.server_name == "biggy");
	EXPECT(out.mission_name == "ASH_I5A");
	EXPECT(out.uptime_ms == 123456);
	EXPECT(out.stat_values[0] == 10 && out.stat_values[4] == -5);
	EXPECT(out.kv.size() == 2 && out.kv[1].key == 9 && out.kv[1].value == 200);
	cover('S', 0x58);
	return 0;
}

// S2C 0x6F — zone-timer value (§5.49): fixed 15 B.
int check_S_6F_zone_timer_value() {
	LE w;
	w.u16(0x3001);           // zone entity handle (pool 3)
	w.u8(2);                 // mode
	w.u32(uint32_t(30));     // value seconds
	w.u32(uint32_t(60));     // limit seconds
	w.u16(1);                // rate (i16)
	w.u8(0xAA); w.u8(0xBB);  // entity+544 / +545
	EXPECT(w.b.size() == 15);
	ZoneTimerValue out;
	size_t consumed = 0;
	EXPECT(decode_zone_timer_value(w.b.data(), w.b.size(), out, consumed));
	EXPECT(consumed == 15);
	EXPECT(out.zone_handle == 0x3001);
	EXPECT(out.value_s == 30 && out.limit_s == 60 && out.rate == 1);
	cover('S', 0x6F);
	return 0;
}

// S2C 0x53 — zone-timer window (§5.49): fixed 9 B.
int check_S_53_zone_timer_window() {
	LE w;
	w.u16(0x3001);
	w.u8(1); w.u8(4);        // modeA / modeB (entity+547)
	w.u16(10); w.u16(40);    // window seconds
	w.u8(2);                 // rate
	EXPECT(w.b.size() == 9);
	ZoneTimerWindow out;
	size_t consumed = 0;
	EXPECT(decode_zone_timer_window(w.b.data(), w.b.size(), out, consumed));
	EXPECT(consumed == 9);
	EXPECT(out.zone_handle == 0x3001 && out.mode_b == 4);
	EXPECT(out.start_s == 10 && out.end_s == 40 && out.rate == 2);
	cover('S', 0x53);
	return 0;
}

// S2C 0x34 — play-sound (§5.50): flag 1 carries the 3×i16 position block.
int check_S_34_play_sound() {
	LE w;
	w.u8(1);                 // positioned 3D
	w.u8('w'); w.u8('a'); w.u8('v'); w.u8(0);
	w.u16(uint16_t(100)); w.u16(uint16_t(-50)); w.u16(uint16_t(7));
	PlaySoundCommand out;
	EXPECT(decode_play_sound(w.b.data(), w.b.size(), out));
	EXPECT(out.flag == 1 && out.has_pos);
	EXPECT(out.sound_name == "wav");
	EXPECT(out.pos_x == 100 && out.pos_y == -50 && out.pos_z == 7);
	// flag 0: no position block on the wire.
	LE w0;
	w0.u8(0);
	w0.u8('s'); w0.u8(0);
	PlaySoundCommand flat;
	EXPECT(decode_play_sound(w0.b.data(), w0.b.size(), flat));
	EXPECT(!flat.has_pos);
	cover('S', 0x34);
	return 0;
}

// S2C 0x2C — mission + map names (§5.51): two cstrings.
int check_S_2C_mission_map_names() {
	LE w;
	auto str = [&](const char *s) { for (const char *p = s; *p; ++p) w.u8(uint8_t(*p)); w.u8(0); };
	str("ASH_I5A.bms");
	str("ASH_I5A");
	MissionMapNames out;
	EXPECT(decode_mission_map_names(w.b.data(), w.b.size(), out));
	EXPECT(out.session_name == "ASH_I5A.bms");
	EXPECT(out.map_file_name == "ASH_I5A");
	cover('S', 0x2C);
	return 0;
}

// §5.52 chat pair — C2S 0x0D uplink and its S2C 0x14 fan-out.
int check_chat_pair() {
	LE up;
	up.u8(2);                 // team channel
	up.u8('h'); up.u8('i'); up.u8(0);
	ChatUplink u;
	EXPECT(decode_chat_uplink(up.b.data(), up.b.size(), u));
	EXPECT(u.channel == 2 && u.text == "hi");
	cover('C', 0x0D);
	LE dn;
	dn.u8(3);                 // sender slot
	dn.u8(2);                 // channel
	dn.u8('P'); dn.u8(':'); dn.u8('h'); dn.u8('i'); dn.u8(0);
	ChatBroadcast b;
	EXPECT(decode_chat_broadcast(dn.b.data(), dn.b.size(), b));
	EXPECT(b.sender_slot == 3 && b.channel == 2 && b.text == "P:hi");
	cover('S', 0x14);
	return 0;
}

// S2C 0x04 — session slot config (§5.53): fixed 24 B.
int check_S_04_session_slot_config() {
	LE w;
	for (int i = 0; i < 4; ++i) w.u32(0x11111111u * unsigned(i + 1));
	w.u8(5);                 // session config
	w.u8(1);                 // the recipient's own roster slot (g_local_player_slot_id)
	w.u8(32);                // max players (g_max_player_slots — the roster-walk terminator)
	w.u32(0xAABBCCDD);
	w.u8(9);
	EXPECT(w.b.size() == 24);
	SessionSlotConfig out;
	EXPECT(decode_session_slot_config(w.b.data(), w.b.size(), out));
	EXPECT(out.session_config == 5 && out.local_player_slot == 1 && out.max_players == 32);
	EXPECT(out.trailing == 9);
	cover('S', 0x04);
	return 0;
}

// S2C 0x08 — session config (§5.54): fixed 51 B; fields[3] = gameType.
int check_S_08_session_config() {
	LE w;
	for (int i = 0; i < 10; ++i) w.u32(uint32_t(i == 3 ? 0x10001 : i));
	for (int i = 0; i < 7; ++i) w.u8(uint8_t(i));
	w.u32(uint32_t((1u << 13) | (1u << 16)));
	EXPECT(w.b.size() == 51);
	SessionConfig out;
	EXPECT(decode_session_config(w.b.data(), w.b.size(), out));
	EXPECT(out.fields[3] == 0x10001);
	EXPECT(out.bytes[6] == 6);
	EXPECT((out.bitflags & (1u << 13)) != 0);
	cover('S', 0x08);
	return 0;
}

// S2C 0x02 — join position-ack + padding probe (§5.55): 12-B header + filler.
int check_S_02_join_padding_probe() {
	LE w;
	w.u32(0x00120000);       // pos x
	w.u32(0x00340000);       // pos y
	w.u32(500);              // padding_len the client must echo
	w.zeros(20);             // ignored wire filler
	JoinPaddingProbe out;
	EXPECT(decode_join_padding_probe(w.b.data(), w.b.size(), out));
	EXPECT(uint32_t(out.pos_x) == 0x00120000);
	EXPECT(out.padding_len == 500);
	EXPECT(out.filler_bytes == 20);
	cover('S', 0x02);
	return 0;
}

// C2S 0x2F — loadout submit (§5.56): header + 2 ADM entries + 0xFF terminator.
int check_C_2F_loadout_submit() {
	LE w;
	w.u8(2);                 // team
	w.u8(8);                 // player class
	w.u32(1);                // weapon slot index
	w.u8(10); w.u8(3); w.u8(2); w.u8(0);   // entry: adm 10
	w.u8(24); w.u8(1); w.u8(0); w.u8(5);   // entry: adm 24
	w.u8(0xFF);              // terminator
	EXPECT(w.b.size() == 15);
	LoadoutSubmit out;
	EXPECT(decode_loadout_submit(w.b.data(), w.b.size(), out));
	EXPECT(out.team == 2 && out.player_class == 8);
	EXPECT(out.entries.size() == 2);
	EXPECT(out.entries[1].adm_index == 24 && out.entries[1].variant == 5);
	EXPECT(out.terminated);
	cover('C', 0x2F);
	return 0;
}

// S2C 0x50 — team assign (§5.62): 6 B, round-tripped through the real encoder,
// plus the witnessed short-body default-to-zero tail.
// [orig: NapiNPClientMsg_0x050 @0x431910]
int check_S_50_team_assign() {
	TeamAssign in;
	in.entity_handle = 0x0007;
	in.team = 2;
	in.net_id = 0x1234;
	in.anim_slot = 1;
	const std::vector<uint8_t> wire = encode_team_assign(in);
	EXPECT(wire.size() == 6);
	TeamAssign out;
	size_t consumed = 0;
	EXPECT(decode_team_assign(wire.data(), wire.size(), out, consumed));
	EXPECT(consumed == 6);
	EXPECT(out.entity_handle == 0x0007);
	EXPECT(out.team == 2);
	EXPECT(out.net_id == 0x1234);
	EXPECT(out.anim_slot == 1);
	// Short body: the handle + team arrive, the remaining fields stay zero.
	const uint8_t short_body[3] = {0x08, 0x00, 0x03};
	TeamAssign shortened;
	size_t short_consumed = 0;
	EXPECT(decode_team_assign(short_body, sizeof(short_body), shortened, short_consumed));
	EXPECT(short_consumed == 3);
	EXPECT(shortened.entity_handle == 0x0008 && shortened.team == 3);
	EXPECT(shortened.net_id == 0 && shortened.anim_slot == 0);
	// A body too short even for the handle is not a team assign.
	const uint8_t stub[1] = {0x08};
	TeamAssign rejected;
	size_t rejected_consumed = 1;
	EXPECT(!decode_team_assign(stub, sizeof(stub), rejected, rejected_consumed));
	cover('S', 0x50);
	return 0;
}

// S2C 0x5D — empty-slot sweep: a bare `[u16 pool0Index] x N` run, no count word.
// [orig: NapiNPClientMsg_DestroyEntityList @0x429730]
int check_S_5D_destroy_list() {
	DestroyEntityList in;
	in.pool0_indices = {3, 9, 41};
	const std::vector<uint8_t> wire = encode_destroy_entity_list(in);
	EXPECT(wire.size() == 6);
	DestroyEntityList out;
	EXPECT(decode_destroy_entity_list(wire.data(), wire.size(), out));
	EXPECT(out.pool0_indices.size() == 3);
	EXPECT(out.pool0_indices[0] == 3 && out.pool0_indices[2] == 41);
	// An EMPTY sweep (no empty slots) is valid and carries no entries.
	DestroyEntityList empty;
	EXPECT(decode_destroy_entity_list(nullptr, 0, empty));
	EXPECT(empty.pool0_indices.empty());
	// A trailing odd byte is not a whole index — the walk must reject it.
	const uint8_t ragged[3] = {0x01, 0x00, 0x02};
	DestroyEntityList bad;
	EXPECT(!decode_destroy_entity_list(ragged, sizeof(ragged), bad));
	cover('S', 0x5D);
	return 0;
}

// C2S 0x32 — empty-slot sweep request: the host reads no fields.
// [orig: NapiNPServerMsg_SendEmptySlots @0x51a600]
int check_C_32_empty_slots_request() {
	size_t consumed = 1;
	EXPECT(decode_empty_slots_request(nullptr, 0, consumed));
	EXPECT(consumed == 0);
	cover('C', 0x32);
	return 0;
}

// ---------------------------------------------------------------------------
// (3) Decoded-set drift guard
// ---------------------------------------------------------------------------
int test_decoded_drift_guard() {
	std::set<std::pair<char, int>> catalog_decoded;
	size_t n = 0;
	const MsgCatalogEntry *t = ingame_message_catalog(&n);
	for (size_t i = 0; i < n; ++i)
		if (t[i].coverage == MsgCoverage::Decoded)
			catalog_decoded.insert({t[i].dir, int(t[i].tag)});
	// Every Decoded catalog tag was exercised, and nothing extra.
	EXPECT(g_covered == catalog_decoded);
	std::printf("PASS decoded_drift_guard (%zu Decoded tags exercised)\n",
	            g_covered.size());
	return 0;
}

} // namespace

int main() {
	if (test_catalog_consistency()) return 1;
	// (2) — must run before the drift guard to populate g_covered.
	if (check_S_0A_frame_update()) return 1;
	if (check_S_0D_pool_spawn()) return 1;
	if (check_S_20_pool3_sync()) return 1;
	if (check_S_0C_organic()) return 1;
	if (check_S_40_capture_zone()) return 1;
	if (check_S_1E_game_event()) return 1;
	if (check_S_61_tick_seed()) return 1;
	if (check_S_26_kill()) return 1;
	if (check_S_4E_batch_kill()) return 1;
	if (check_S_10_static_entity()) return 1;
	if (check_S_16_player_list()) return 1;
	if (check_S_46_player_sync()) return 1;
	if (check_C_0C_extended_uplink()) return 1;
	if (check_C_06_fired_round()) return 1;
	if (check_C_21_checksum_reply()) return 1;
	if (check_S_5A_weapon_loadout()) return 1;
	if (check_S_6E_roster()) return 1;
	if (check_S_7B_full_player_info()) return 1;
	if (check_S_0F_world_state()) return 1;
	if (check_S_60_64_file_transfer()) return 1;
	if (check_C_22_player_sync_request()) return 1;
	if (check_C_23_visible_request()) return 1;
	if (check_C_28_loadout_request()) return 1;
	if (check_C_29_team_spawn_ack()) return 1;
	if (check_C_4C_client_quality()) return 1;
	if (check_rtt_sample()) return 1;
	if (check_u32_scalar_trio()) return 1;
	if (check_S_6B_minimap()) return 1;
	if (check_S_49_weapon_reload()) return 1;
	if (check_C_25_reload_request()) return 1;
	if (check_S_13_entity_death()) return 1;
	if (check_S_30_checksum_request()) return 1;
	if (check_S_31_loadout_crc_request()) return 1;
	if (check_S_42_input_flags()) return 1;
	if (check_S_79_spectator_flag()) return 1;
	if (check_S_2A_chat_history()) return 1;
	if (check_S_59_deployed_item()) return 1;
	if (check_S_45_terrain_load()) return 1;
	if (check_S_18_full_entity_spawn()) return 1;
	if (check_S_58_session_status()) return 1;
	if (check_S_6F_zone_timer_value()) return 1;
	if (check_S_53_zone_timer_window()) return 1;
	if (check_S_34_play_sound()) return 1;
	if (check_S_2C_mission_map_names()) return 1;
	if (check_chat_pair()) return 1;
	if (check_S_04_session_slot_config()) return 1;
	if (check_S_08_session_config()) return 1;
	if (check_S_02_join_padding_probe()) return 1;
	if (check_C_2F_loadout_submit()) return 1;
	if (check_S_50_team_assign()) return 1;
	if (check_S_5D_destroy_list()) return 1;
	if (check_C_32_empty_slots_request()) return 1;
	if (test_decoded_drift_guard()) return 1;
	std::printf("ALL nw_message_coverage tests passed\n");
	return 0;
}
