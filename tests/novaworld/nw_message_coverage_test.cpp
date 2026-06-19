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

#include <novaworld/ingame_decode.h>
#include <novaworld/ingame_encode.h>
#include <novaworld/ingame_message_catalog.h>

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

// S2C 0x0A — minimal valid frame: 12-B anchor + flags + sub-block 0 (aim, 11 B)
// + 7-B tail + tag-0 terminator = 33 B. Asserts the walk consumes it cleanly.
int check_S_0A_frame_update() {
	LE w;
	w.u32(0); w.u32(0); w.u32(0);       // anchor x/y/z
	w.u8(0);                            // flags1
	w.u8(0);                            // flags2 -> sub_block 0, no passenger
	w.zeros(6);                         // aim view0..5
	w.u8(0xFF);                         // aim target_slot (none)
	w.u32(0);                           // aim_extra i32
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
	w.u8(8);            // max_players
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

// C2S 0x29 — entity request: [u16 bufferIndex].
int check_C_29_entity_request() {
	LE w;
	w.u16(0x0042);
	BurstEntityRequest r;
	size_t consumed = 0;
	EXPECT(decode_burst_entity_request(w.b.data(), w.b.size(), r, consumed));
	EXPECT(consumed == 2);
	EXPECT(r.buffer_index == 0x0042);
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

// S2C 0x68 / 0x43 / 0x39 — periodic request trio: a single [u32] (4 B). One
// reader serves all three; cover() each.
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
	if (check_C_29_entity_request()) return 1;
	if (check_C_4C_client_quality()) return 1;
	if (check_rtt_sample()) return 1;
	if (check_u32_scalar_trio()) return 1;
	if (test_decoded_drift_guard()) return 1;
	std::printf("ALL nw_message_coverage tests passed\n");
	return 0;
}
