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
	if (test_decoded_drift_guard()) return 1;
	std::printf("ALL nw_message_coverage tests passed\n");
	return 0;
}
