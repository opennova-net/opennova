// Loopback identity test for the in-match S2C encoders (ADR 0011 §4): the
// host's in-process listen-server path serializes real entity state and the
// local client decodes it, so encode -> decode MUST be field-identical.
//
// The encode side is a faithful port of the original serializer; the decode
// side is independently verified against the inverse handler in IDA:
//   encode_pool3_sync_batch  <- [orig: serialize_entity_pool_to_packet @ 0x503460]
//   decode_pool3_sync_batch  <- [orig: NapiNPClientMsg_0x020          @ 0x425C00]
// Because the two ports reference DIFFERENT binary functions (not each other),
// a clean round-trip pins the wire format, not just internal consistency. The
// flag-derivation checks below pin the faithful "gate bit set iff source field
// non-zero" behavior; the length checks pin the exact byte layout.

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace opennova;

namespace {

#define EXPECT(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "FAIL %s:%d  EXPECT(%s)\n", __FILE__, __LINE__, #cond); \
		return 1; \
	} \
} while (0)

// Compare two decoded records field-by-field (the fields the §5.12 codec carries).
bool records_equal(const Pool3SyncRecord &a, const Pool3SyncRecord &b) {
	return a.item_type_id == b.item_type_id && a.is_empty_slot == b.is_empty_slot &&
	       a.flags_byte == b.flags_byte && a.pos_x == b.pos_x && a.pos_y == b.pos_y &&
	       a.pos_z == b.pos_z && a.net_handle == b.net_handle &&
	       a.movement_val == b.movement_val && a.orientation_val == b.orientation_val &&
	       a.ammo_count == b.ammo_count && a.team_byte == b.team_byte &&
	       a.weapon_type == b.weapon_type && a.score_byte == b.score_byte;
}

int test_roundtrip_all_flags() {
	// Every conditional field non-zero -> derived flags must be 0x3F.
	Pool3SyncBatch in;
	in.start_index = 5;
	Pool3SyncRecord r;
	r.item_type_id   = 0x044A;
	r.pos_x          = int32_t(0x11223344);
	r.pos_y          = int32_t(0x55667788);
	r.pos_z          = int32_t(0x003A5E6A);
	r.movement_val   = 0xAABBCCDD; // 0x01
	r.orientation_val= 0x0F0E0D0C; // 0x02
	r.ammo_count     = 0x0102;     // 0x04
	r.net_handle     = 0x108B;     // always
	r.team_byte      = 0x02;       // 0x08
	r.weapon_type    = 0x1357;     // 0x10
	r.score_byte     = 0x09;       // 0x20
	in.records.push_back(r);

	std::vector<uint8_t> wire = encode_pool3_sync_batch(in);

	Pool3SyncBatch out;
	EXPECT(decode_pool3_sync_batch(wire.data(), wire.size(), out)); // consumed exactly
	EXPECT(out.start_index == 5);
	EXPECT(out.entity_count == 1);
	EXPECT(out.records.size() == 1);
	EXPECT(out.records[0].flags_byte == 0x3F);
	EXPECT(records_equal(out.records[0], [&] {
		Pool3SyncRecord e = r; e.flags_byte = 0x3F; return e;
	}()));
	std::printf("PASS roundtrip_all_flags\n");
	return 0;
}

int test_roundtrip_no_flags_and_length() {
	// All conditionals zero -> flags 0x00; body = type(2)+flags(1)+pos(12)+net(2).
	Pool3SyncBatch in;
	in.start_index = 0;
	Pool3SyncRecord r;
	r.item_type_id = 0x0044;
	r.pos_x = 1; r.pos_y = -2; r.pos_z = 3;
	r.net_handle = 0x0001;
	// parent/orientation/ammo/team/weapon/score all left 0.
	in.records.push_back(r);

	std::vector<uint8_t> wire = encode_pool3_sync_batch(in);
	// header(4) + type(2) + flags(1) + pos(12) + net(2) = 21.
	EXPECT(wire.size() == 21);

	Pool3SyncBatch out;
	EXPECT(decode_pool3_sync_batch(wire.data(), wire.size(), out));
	EXPECT(out.records.size() == 1);
	EXPECT(out.records[0].flags_byte == 0x00);
	EXPECT(out.records[0].net_handle == 0x0001);
	EXPECT(out.records[0].pos_y == -2);
	EXPECT(records_equal(out.records[0], r));
	std::printf("PASS roundtrip_no_flags_and_length\n");
	return 0;
}

int test_flag_derivation_is_from_nonzero() {
	// team_byte 0 but ammo non-zero: gate 0x04 set, 0x08 clear (faithful: the
	// original sets a bit only inside `if (value)`).
	Pool3SyncBatch in;
	Pool3SyncRecord r;
	r.item_type_id = 0x00FF;
	r.ammo_count = 5;   // -> 0x04
	r.team_byte = 0;    // -> bit 0x08 must stay clear
	r.net_handle = 0x42;
	in.records.push_back(r);

	std::vector<uint8_t> wire = encode_pool3_sync_batch(in);
	Pool3SyncBatch out;
	EXPECT(decode_pool3_sync_batch(wire.data(), wire.size(), out));
	EXPECT(out.records.size() == 1);
	EXPECT((out.records[0].flags_byte & 0x04) != 0);
	EXPECT((out.records[0].flags_byte & 0x08) == 0);
	EXPECT(out.records[0].ammo_count == 5);
	EXPECT(out.records[0].team_byte == 0);
	std::printf("PASS flag_derivation_is_from_nonzero\n");
	return 0;
}

int test_empty_slot_sentinel_and_mixed() {
	// An empty-slot sentinel (type_id 0) writes a bare [u16 0] and the decoder
	// resumes with the next record. Mix it between two real records.
	Pool3SyncBatch in;
	in.start_index = 100;
	Pool3SyncRecord a; a.item_type_id = 0x0010; a.pos_x = 7; a.net_handle = 0x11;
	Pool3SyncRecord empty; empty.is_empty_slot = true; // item_type_id 0
	Pool3SyncRecord b; b.item_type_id = 0x0020; b.pos_z = 9; b.net_handle = 0x22;
	in.records = {a, empty, b};

	std::vector<uint8_t> wire = encode_pool3_sync_batch(in);
	Pool3SyncBatch out;
	EXPECT(decode_pool3_sync_batch(wire.data(), wire.size(), out));
	EXPECT(out.entity_count == 3);
	EXPECT(out.records.size() == 3);
	EXPECT(!out.records[0].is_empty_slot);
	EXPECT(out.records[0].net_handle == 0x11);
	EXPECT(out.records[1].is_empty_slot);
	EXPECT(out.records[1].item_type_id == 0);
	EXPECT(!out.records[2].is_empty_slot);
	EXPECT(out.records[2].pos_z == 9 && out.records[2].net_handle == 0x22);
	std::printf("PASS empty_slot_sentinel_and_mixed\n");
	return 0;
}

int test_empty_batch() {
	Pool3SyncBatch in;
	in.start_index = 42;
	std::vector<uint8_t> wire = encode_pool3_sync_batch(in);
	EXPECT(wire.size() == 4); // [u16 start][u16 count=0]
	Pool3SyncBatch out;
	EXPECT(decode_pool3_sync_batch(wire.data(), wire.size(), out));
	EXPECT(out.start_index == 42);
	EXPECT(out.entity_count == 0);
	EXPECT(out.records.empty());
	std::printf("PASS empty_batch\n");
	return 0;
}

// ---- S2C 0x10 pool-2 static batch (encode_static_entity_batch) -------------

bool records_equal(const StaticEntityRecord &a, const StaticEntityRecord &b) {
	return a.item_type_id == b.item_type_id && a.is_empty_slot == b.is_empty_slot &&
	       a.pos_x == b.pos_x && a.pos_y == b.pos_y && a.pos_z == b.pos_z &&
	       a.euler_z == b.euler_z && a.euler_x == b.euler_x && a.euler_y == b.euler_y &&
	       a.section_mask == b.section_mask && a.team_byte == b.team_byte &&
	       a.entity_flags == b.entity_flags && a.ammo_count == b.ammo_count &&
	       a.bone_a == b.bone_a && a.bone_b == b.bone_b && a.score_flag == b.score_flag &&
	       a.weapon_byte == b.weapon_byte && a.attach_ref == b.attach_ref;
}

int test_static_batch_all_flags() {
	// Every gated field non-zero -> derived field_flags 0x03FF; weapon_byte non-zero
	// triggers attach_ref unconditionally.
	StaticEntityBatch in;
	in.start_index = 9;
	StaticEntityRecord r;
	r.item_type_id = 0x0808;
	r.pos_x        = int32_t(0x11223344);
	r.pos_y        = int32_t(0x55667788);
	r.pos_z        = int32_t(0x00ABCDEF);
	r.euler_z      = int32_t(0x0A0B0C0D); // 0x0001
	r.euler_x      = int32_t(0x01020304); // 0x0002
	r.euler_y      = int32_t(0x05060708); // 0x0004
	r.section_mask = int32_t(0x40);       // 0x0008
	r.team_byte    = 2;                   // 0x0010
	r.entity_flags = 0x04020400u;         // 0x0020 (entity+36 Flags; golden building value)
	r.ammo_count   = 30;                  // always
	r.bone_a       = 5;                   // 0x0040
	r.bone_b       = 6;                   // 0x0080
	r.score_flag   = 1;                   // 0x0100
	r.weapon_byte  = 3;                   // always -> attach_ref follows
	r.attach_ref   = 0x7788;
	in.records.push_back(r);

	std::vector<uint8_t> wire = encode_static_entity_batch(in);
	StaticEntityBatch out;
	EXPECT(decode_static_entity_batch(wire.data(), wire.size(), out));
	EXPECT(out.start_index == 9);
	EXPECT(out.entity_count == 1);
	EXPECT(out.records.size() == 1);
	EXPECT(out.records[0].field_flags == 0x01FF); // 0x0001..0x0100 all set (no 0x0200)
	EXPECT(records_equal(out.records[0], r));
	std::printf("PASS static_batch_all_flags\n");
	return 0;
}

int test_static_batch_minimal_and_length() {
	// Building-shaped record: only pos + euler_z heading + team. body =
	// type(2)+flags(2)+pos(12)+euler_z(4)+team(1)+ammo(1)+weapon(1) = 23; +header(4) = 27.
	StaticEntityBatch in;
	in.start_index = 0;
	StaticEntityRecord r;
	r.item_type_id = 0x044A;
	r.pos_x = 1; r.pos_y = -2; r.pos_z = 3;
	r.euler_z = int32_t(0x0B60B60); // heading set -> 0x0001
	r.team_byte = 1;                // -> 0x0010
	in.records.push_back(r);

	std::vector<uint8_t> wire = encode_static_entity_batch(in);
	EXPECT(wire.size() == 27);
	StaticEntityBatch out;
	EXPECT(decode_static_entity_batch(wire.data(), wire.size(), out));
	EXPECT(out.records.size() == 1);
	EXPECT(out.records[0].field_flags == 0x0011);
	EXPECT(out.records[0].pos_y == -2);
	EXPECT(out.records[0].euler_z == int32_t(0x0B60B60));
	EXPECT(out.records[0].team_byte == 1);
	EXPECT(out.records[0].weapon_byte == 0);
	EXPECT(out.records[0].attach_ref == 0); // weapon_byte 0 && 0x200 clear -> not emitted
	EXPECT(records_equal(out.records[0], r));
	std::printf("PASS static_batch_minimal_and_length\n");
	return 0;
}

int test_static_batch_attach_ref_via_0x200() {
	// weapon_byte 0 but attach_ref populated -> the encoder forces the 0x0200 gate so
	// the field round-trips (faithful: decode reads attach_ref iff weapon_byte||0x200).
	StaticEntityBatch in;
	StaticEntityRecord r;
	r.item_type_id = 0x00FF;
	r.attach_ref = 0x1234;
	r.weapon_byte = 0;
	in.records.push_back(r);

	std::vector<uint8_t> wire = encode_static_entity_batch(in);
	StaticEntityBatch out;
	EXPECT(decode_static_entity_batch(wire.data(), wire.size(), out));
	EXPECT(out.records.size() == 1);
	EXPECT((out.records[0].field_flags & 0x0200) != 0);
	EXPECT(out.records[0].attach_ref == 0x1234);
	EXPECT(out.records[0].weapon_byte == 0);
	std::printf("PASS static_batch_attach_ref_via_0x200\n");
	return 0;
}

int test_static_batch_empty_slot_and_empty_batch() {
	// Empty-slot sentinel mixed between real records, plus a fully empty batch.
	StaticEntityBatch in;
	in.start_index = 100;
	StaticEntityRecord a; a.item_type_id = 0x0010; a.pos_x = 7; a.ammo_count = 1;
	StaticEntityRecord empty; empty.is_empty_slot = true;
	StaticEntityRecord b; b.item_type_id = 0x0020; b.pos_z = 9; b.weapon_byte = 1; b.attach_ref = 0x55;
	in.records = {a, empty, b};

	std::vector<uint8_t> wire = encode_static_entity_batch(in);
	StaticEntityBatch out;
	EXPECT(decode_static_entity_batch(wire.data(), wire.size(), out));
	EXPECT(out.entity_count == 3);
	EXPECT(out.records.size() == 3);
	EXPECT(!out.records[0].is_empty_slot && out.records[0].pos_x == 7);
	EXPECT(out.records[1].is_empty_slot && out.records[1].item_type_id == 0);
	EXPECT(!out.records[2].is_empty_slot && out.records[2].attach_ref == 0x55);

	StaticEntityBatch empty_in; empty_in.start_index = 42;
	std::vector<uint8_t> ew = encode_static_entity_batch(empty_in);
	EXPECT(ew.size() == 4); // [u16 start][u16 count=0]
	StaticEntityBatch eout;
	EXPECT(decode_static_entity_batch(ew.data(), ew.size(), eout));
	EXPECT(eout.start_index == 42 && eout.entity_count == 0 && eout.records.empty());
	std::printf("PASS static_batch_empty_slot_and_empty_batch\n");
	return 0;
}

// ---- S2C 0x0D pool spawn (encode_pool_spawn_batch) -------------------------

int test_pool_spawn_roundtrip_full() {
	PoolSpawnBatch in;
	PoolSpawnRecord r;
	r.slot_id        = 0x108B;   // pool 1 / slot 139
	r.item_type_id   = 0x04BF;
	r.entity_name    = "tango";
	r.entity_flags   = 2;            // -> 0x0020
	r.pos_x = int32_t(0x05b0be8f);
	r.pos_y = int32_t(0xfa47b504);
	r.pos_z = int32_t(0x001fbcd7);
	r.euler_z = 10;                  // -> 0x0001 (entity+16 yaw heading, 32-bit BAM)
	r.section_mask = 0x40;           // -> 0x0008
	r.team_byte = 1;                 // -> 0x0010 (D-NET-58: gate-0x10 byte @ +354 is team)
	r.parent_handle = 0x1033;        // -> 0x0100
	r.target_handle = 0x2044;        // -> 0x0200
	r.weapon_mask = 0x05;            // bits 0 + 2 -> 0x0400
	r.weapon_handles[0] = 0x3001;
	r.weapon_handles[2] = 0x3002;
	r.extra_handle_0 = 0x4001;
	r.extra_handle_1 = 0x4002;
	r.bone_byte = 2;                 // unconditional +290 (D-NET-58)
	r.ai_profile_1 = 0x11112222;     // -> 0x0800
	r.ai_profile_2 = 0x33334444;
	r.ai_name = "patrol_a";
	r.alert_byte = 7;                // -> 0x0040
	r.action_byte = 9;               // -> 0x0080
	r.weapon_type_byte = 3;          // -> 0x1000
	r.zone_number_rank = 80;              // -> 0x2000 (+ zone_radius)
	r.zone_radius = 1000;
	r.difficulty_byte = 4;           // -> 0x4000
	in.records.push_back(r);

	std::vector<uint8_t> wire = encode_pool_spawn_batch(in);
	PoolSpawnBatch out;
	EXPECT(decode_pool_spawn_batch(wire.data(), wire.size(), out)); // consumed exactly
	EXPECT(out.entity_count == 1);
	EXPECT(out.records.size() == 1);
	const PoolSpawnRecord &d = out.records[0];
	EXPECT(d.slot_id == 0x108B);
	EXPECT(d.item_type_id == 0x04BF);
	EXPECT(d.entity_name == "tango");
	EXPECT(d.entity_flags == 2);
	EXPECT(d.pos_x == int32_t(0x05b0be8f));
	EXPECT(d.pos_z == int32_t(0x001fbcd7));
	EXPECT(d.euler_z == 10);
	EXPECT(d.section_mask == 0x40);
	EXPECT(d.team_byte == 1);
	EXPECT(d.parent_handle == 0x1033);
	EXPECT(d.target_handle == 0x2044);
	EXPECT(d.weapon_mask == 0x05);
	EXPECT(d.weapon_handles[0] == 0x3001);
	EXPECT(d.weapon_handles[2] == 0x3002);
	EXPECT(d.weapon_handles[1] == 0xFFFF); // bit clear -> untouched default
	EXPECT(d.extra_handle_0 == 0x4001);
	EXPECT(d.extra_handle_1 == 0x4002);
	EXPECT(d.bone_byte == 2);
	EXPECT(d.ai_profile_1 == 0x11112222);
	EXPECT(d.ai_profile_2 == 0x33334444);
	EXPECT(d.ai_name == "patrol_a");
	EXPECT(d.alert_byte == 7);
	EXPECT(d.action_byte == 9);
	EXPECT(d.weapon_type_byte == 3);
	EXPECT(d.zone_number_rank == 80);
	EXPECT(d.zone_radius == 1000);
	EXPECT(d.difficulty_byte == 4);
	std::printf("PASS pool_spawn_roundtrip_full\n");
	return 0;
}

int test_pool_spawn_minimal() {
	// No conditional fields -> spawn_flags 0; body = flags(2)+slot(2)+type(2)+
	// name(1 NUL)+pos(12)+bone(1) = 20; + count(2) = 22.
	PoolSpawnBatch in;
	PoolSpawnRecord r;
	r.slot_id = 0x1002;
	r.item_type_id = 0x044A;
	r.pos_x = 1; r.pos_y = 2; r.pos_z = 3;
	r.bone_byte = 1;
	in.records.push_back(r);

	std::vector<uint8_t> wire = encode_pool_spawn_batch(in);
	EXPECT(wire.size() == 22);
	PoolSpawnBatch out;
	EXPECT(decode_pool_spawn_batch(wire.data(), wire.size(), out));
	EXPECT(out.records.size() == 1);
	const PoolSpawnRecord &d = out.records[0];
	EXPECT(d.slot_id == 0x1002 && d.item_type_id == 0x044A);
	EXPECT(d.pos_y == 2 && d.bone_byte == 1);
	EXPECT(d.parent_handle == 0xFFFF && d.target_handle == 0xFFFF); // not emitted
	std::printf("PASS pool_spawn_minimal\n");
	return 0;
}

int test_pool_spawn_health_alt_8000() {
	// zone_number_rank 0 but zone_radius non-zero -> 0x8000 path (zone_radius only).
	PoolSpawnBatch in;
	PoolSpawnRecord r;
	r.slot_id = 0x1003; r.item_type_id = 0x0100;
	r.zone_number_rank = 0; r.zone_radius = 250;
	r.bone_byte = 2;
	in.records.push_back(r);

	std::vector<uint8_t> wire = encode_pool_spawn_batch(in);
	PoolSpawnBatch out;
	EXPECT(decode_pool_spawn_batch(wire.data(), wire.size(), out));
	EXPECT(out.records.size() == 1);
	EXPECT(out.records[0].zone_number_rank == 0);
	EXPECT(out.records[0].zone_radius == 250);
	std::printf("PASS pool_spawn_health_alt_8000\n");
	return 0;
}

int test_decode_weapon_block_mask_zero_dnet56() {
	// D-NET-56 regression: a hand-built 0x0D body with 0x0400 set and mask==0.
	// The encoder never produces this (it only sets 0x0400 when mask != 0), so
	// the wire is laid out by hand. The handler reads the mask byte, then ALWAYS
	// consumes extra_handle_0/1 (LABEL_110 @ 0x4330b1). Pre-fix the decoder would
	// skip the 4 extra bytes and desync (leftover != 0 -> return false).
	const uint8_t body[] = {
		0x01, 0x00,             // count = 1
		0x00, 0x04,             // spawn_flags = 0x0400 (weapon block only)
		0x01, 0x00,             // slot_id = 0x0001 (not a sentinel)
		0x10, 0x00,             // item_type_id = 0x0010
		0x00,                   // entity_name = "" (NUL)
		0x01, 0x00, 0x00, 0x00, // pos_x = 1
		0x02, 0x00, 0x00, 0x00, // pos_y = 2
		0x03, 0x00, 0x00, 0x00, // pos_z = 3
		0x00,                   // weapon_mask = 0  (the latent path)
		0xAA, 0xAA,             // extra_handle_0  (must be consumed)
		0xBB, 0xBB,             // extra_handle_1  (must be consumed)
		0x05,                   // bone_byte = 5 (+290 unconditional)
	};
	PoolSpawnBatch out;
	const bool ok = decode_pool_spawn_batch(body, sizeof(body), out);
	EXPECT(ok); // consumed EXACTLY — the fix reads the extras even with mask==0
	EXPECT(out.records.size() == 1);
	EXPECT(out.records[0].weapon_mask == 0);
	EXPECT(out.records[0].extra_handle_0 == 0xAAAA);
	EXPECT(out.records[0].extra_handle_1 == 0xBBBB);
	EXPECT(out.records[0].bone_byte == 0x05);
	EXPECT(out.records[0].pos_z == 3);
	std::printf("PASS decode_weapon_block_mask_zero (D-NET-56)\n");
	return 0;
}

// ---- S2C 0x0A trailing compact records -------------------------------------

int test_infantry_compact_roundtrip() {
	// §5.14 — 14 B fixed. Positions are raw already-compressed u16s on the wire.
	InfantryCompactRecord r;
	r.seat_bone_idx       = 3;
	r.vehicle_slot_handle = 0x108B;   // pool 1 / slot 139 (mounted)
	r.pos_x_compressed    = 0xBE8F;
	r.pos_y_compressed    = 0xB504;
	r.pos_z_compressed    = 0xBCD7;
	r.yaw_byte            = 0x40;
	r.flags_byte          = 0x06;
	r.pitch_byte          = 0x12;
	r.aim_yaw_byte        = 0x80;
	r.anim_byte           = 0x05;

	std::vector<uint8_t> wire = encode_infantry_compact_record(r);
	EXPECT(wire.size() == 14);

	InfantryCompactRecord d;
	size_t consumed = 0;
	EXPECT(decode_infantry_compact_record(wire.data(), wire.size(), d, consumed));
	EXPECT(consumed == 14);
	EXPECT(d.seat_bone_idx == 3);
	EXPECT(d.vehicle_slot_handle == 0x108B);
	EXPECT(d.pos_x_compressed == 0xBE8F);
	EXPECT(d.pos_y_compressed == 0xB504);
	EXPECT(d.pos_z_compressed == 0xBCD7);
	EXPECT(d.yaw_byte == 0x40);
	EXPECT(d.flags_byte == 0x06);
	EXPECT(d.pitch_byte == 0x12);
	EXPECT(d.aim_yaw_byte == 0x80);
	EXPECT(d.anim_byte == 0x05);
	std::printf("PASS infantry_compact_roundtrip\n");
	return 0;
}

int test_vehicle_compact_roundtrip_mounted() {
	// §5.13 mounted (flags_byte & 4) -> 15 B: header + euler_y + euler_x.
	VehicleCompactRecord r;
	r.parent_slot_handle = 0x1033;
	r.pos_x_compressed = 0x1111;
	r.pos_y_compressed = 0x2222;
	r.pos_z_compressed = 0x3333;
	r.euler_z = int16_t(0x4444);
	r.flags_byte = 0x04;            // mounted
	r.euler_y = int16_t(0x5555);
	r.euler_x = int16_t(0x6666);

	std::vector<uint8_t> wire = encode_vehicle_compact_record(r);
	EXPECT(wire.size() == 15);
	VehicleCompactRecord d;
	size_t consumed = 0;
	EXPECT(decode_vehicle_compact_record(wire.data(), wire.size(), d, consumed));
	EXPECT(consumed == 15);
	EXPECT(d.is_dead_pose);
	EXPECT(d.parent_slot_handle == 0x1033);
	EXPECT(d.pos_y_compressed == 0x2222);
	EXPECT(d.euler_z == int16_t(0x4444));
	EXPECT(d.flags_byte == 0x04);
	EXPECT(d.euler_y == int16_t(0x5555));
	EXPECT(d.euler_x == int16_t(0x6666));
	std::printf("PASS vehicle_compact_roundtrip_mounted\n");
	return 0;
}

int test_vehicle_compact_roundtrip_unmounted() {
	// §5.13 unmounted (flags_byte & 4 clear) -> 21 B: header + turret/weapon-aim block.
	VehicleCompactRecord r;
	r.parent_slot_handle = 0xFFFF;
	r.pos_x_compressed = 0xAAAA;
	r.pos_y_compressed = 0xBBBB;
	r.pos_z_compressed = 0xCCCC;
	r.euler_z = int16_t(0x0DDD);
	r.flags_byte = 0x00;            // unmounted
	r.weapon_x = 0x0101;
	r.health_word = 0x0202;
	r.weapon_aim_y = 0x0303;
	r.weapon_aim_z = 0x0404;
	r.weapon_heading_bam = int16_t(0x0505);

	std::vector<uint8_t> wire = encode_vehicle_compact_record(r);
	EXPECT(wire.size() == 21);
	VehicleCompactRecord d;
	size_t consumed = 0;
	EXPECT(decode_vehicle_compact_record(wire.data(), wire.size(), d, consumed));
	EXPECT(consumed == 21);
	EXPECT(!d.is_dead_pose);
	EXPECT(d.parent_slot_handle == 0xFFFF);
	EXPECT(d.pos_x_compressed == 0xAAAA);
	EXPECT(d.weapon_x == 0x0101);
	EXPECT(d.health_word == 0x0202);
	EXPECT(d.weapon_aim_y == 0x0303);
	EXPECT(d.weapon_aim_z == 0x0404);
	EXPECT(d.weapon_heading_bam == int16_t(0x0505));
	std::printf("PASS vehicle_compact_roundtrip_unmounted\n");
	return 0;
}

int test_player_compact_roundtrip() {
	// §5.10 player compact -> 18 B fixed.
	PlayerCompactRecord r;
	r.vehicle_bone      = 1;
	r.seat_type         = 2;
	r.carrier_handle    = 0xFFFF;     // on foot
	r.pos_x_compressed  = 0xBE8F;
	r.pos_y_compressed  = 0xB504;
	r.pos_z_compressed  = 0xBCD7;
	r.yaw_byte          = 0x40;
	r.pitch_byte        = 0x10;
	r.move_input_byte     = 0x12;
	r.state_flags        = 0x06;      // bit1 spawning + bit2 mounted bits set
	r.anim_state_id          = 0x05;
	r.anim_channel_ratio = 0x10;
	r.anim_def_index     = 0x07;
	r.health_class_byte = 0x80;

	std::vector<uint8_t> wire = encode_player_compact_record(r);
	EXPECT(wire.size() == 18);
	PlayerCompactRecord d;
	size_t consumed = 0;
	EXPECT(decode_player_compact_record(wire.data(), wire.size(), d, consumed));
	EXPECT(consumed == 18);
	EXPECT(d.vehicle_bone == 1 && d.seat_type == 2);
	EXPECT(d.carrier_handle == 0xFFFF);
	EXPECT(d.pos_x_compressed == 0xBE8F);
	EXPECT(d.pos_z_compressed == 0xBCD7);
	EXPECT(d.yaw_byte == 0x40 && d.pitch_byte == 0x10);
	EXPECT(d.move_input_byte == 0x12 && d.state_flags == 0x06);
	EXPECT(d.anim_state_id == 0x05 && d.anim_channel_ratio == 0x10);
	EXPECT(d.anim_def_index == 0x07 && d.health_class_byte == 0x80);
	std::printf("PASS player_compact_roundtrip\n");
	return 0;
}

int test_weapon_reload_roundtrip() {
	// §5.58 S2C 0x49 (the C2S 0x25 relay) -> 4 B fixed.
	WeaponReload r;
	r.entity_handle = 0x1005; // pool 1, slot 5
	r.reload_param  = 0x00C3; // weaponSlotCombo = category*65 + rank
	std::vector<uint8_t> wire = encode_weapon_reload(r);
	EXPECT(wire.size() == 4);
	WeaponReload d;
	size_t consumed = 0;
	EXPECT(decode_weapon_reload(wire.data(), wire.size(), d, consumed));
	EXPECT(consumed == 4);
	EXPECT(d.entity_handle == 0x1005);
	EXPECT(d.reload_param == 0x00C3);
	std::printf("PASS weapon_reload_roundtrip\n");
	return 0;
}

// C2S 0x2F loadout submit: encode_loadout_submit <-> decode_loadout_submit identity,
// plus the golden capture-default pair byte shape (the joiner's canned submission).
// [orig: NetPacket_SendLoadoutSubmit @0x42cdc0 / NapiNPServerMsg_HandlePlayerLoadout @0x515790]
int test_loadout_submit_roundtrip() {
	LoadoutSubmit s;
	s.team = 2;
	s.player_class = 6;
	s.weapon_slot_index = 212;
	s.entries.push_back(LoadoutSubmitEntry{0x18, 0x03, 0xFF, 0xFF});
	s.entries.push_back(LoadoutSubmitEntry{0x2C, 0xFF, 0x02, 0x01});
	const std::vector<uint8_t> wire = encode_loadout_submit(s);
	EXPECT(wire == std::vector<uint8_t>({0x02, 0x06, 0xD4, 0x00, 0x00, 0x00,
	                                     0x18, 0x03, 0xFF, 0xFF,
	                                     0x2C, 0xFF, 0x02, 0x01, 0xFF}));
	LoadoutSubmit d;
	EXPECT(decode_loadout_submit(wire.data(), wire.size(), d));
	EXPECT(d.team == 2 && d.player_class == 6 && d.weapon_slot_index == 212);
	EXPECT(d.entries.size() == 2 && d.terminated);
	EXPECT(d.entries[0].adm_index == 0x18 && d.entries[0].ammo_primary == 0x03);
	EXPECT(d.entries[1].adm_index == 0x2C && d.entries[1].variant == 0x01);
	// The empty-kit form stays a bare header + terminator (7 B).
	LoadoutSubmit empty;
	empty.team = 1;
	empty.player_class = 8;
	empty.weapon_slot_index = 195;
	const std::vector<uint8_t> empty_wire = encode_loadout_submit(empty);
	EXPECT(empty_wire ==
	       std::vector<uint8_t>({0x01, 0x08, 0xC3, 0x00, 0x00, 0x00, 0xFF}));
	std::printf("PASS loadout_submit_roundtrip\n");
	return 0;
}

// network_compress_fixedpoint <-> network_decompress_fixedpoint, the position
// codec the field-driven 0x0A builder uses. The codec is lossy (12-bit float-like)
// and its XOR-fold is asymmetric for negatives, so only representable positives are
// exact; everything else must land within its quantization step.
// [orig: Network_CompressFixedPoint @ 0x4C2780 / Network_DecompressFixedPoint @ 0x4C27E0]
int test_compress_fixedpoint_roundtrip() {
	// bsr-undefined inputs (fold==0) round-trip exactly via the sign guard.
	EXPECT(network_compress_fixedpoint(0) == 0);
	EXPECT(network_decompress_fixedpoint(network_compress_fixedpoint(0)) == 0);
	EXPECT(network_decompress_fixedpoint(network_compress_fixedpoint(-1)) == -1);

	// Exactly-representable positives (mantissa<<shift, mantissa a multiple of 0x10).
	const int32_t exact[] = {0x20, 0x200, 0x1000, 0x2000, 0x4000};
	for (int32_t v : exact)
		EXPECT(network_decompress_fixedpoint(network_compress_fixedpoint(v)) == v);

	// General values round-trip within one quantization step (~|v| >> 11); allow
	// |v|/1024 + 32 as a safe upper bound. Covers negatives + the full i32 range.
	const int32_t vals[] = {
		1, -1, 5, -5, 0x1234, -0x1234, 0x12345, -0x12345, -0x2000,
		0x100000, -0x100000, 0x3FFFFFF, -0x3FFFFFF, 0x7FFFFFFF, int32_t(0x80000001u),
	};
	for (int32_t v : vals) {
		const int32_t rt = network_decompress_fixedpoint(network_compress_fixedpoint(v));
		int64_t err = int64_t(rt) - int64_t(v);
		if (err < 0) err = -err;
		int64_t mag = int64_t(v) < 0 ? -int64_t(v) : int64_t(v);
		EXPECT(err <= (mag >> 10) + 32);
	}
	std::printf("PASS compress_fixedpoint_roundtrip\n");
	return 0;
}

// encode_frame_update <-> decode_frame_update: the §5.9 0x0A whole-message pair the
// field-driven builder relies on. Covers the aim sub-block + all three compact
// classes + a weapon-hit, the env sub-block, and the conditional passenger record.
int test_frame_update_roundtrip() {
	auto class_of = [](uint16_t t) -> EntityClass {
		if (t == 100) return EntityClass::Player;
		if (t == 200) return EntityClass::Vehicle;
		if (t == 300) return EntityClass::Infantry;
		return EntityClass::Unknown;
	};

	// (a) aim sub-block (flags2=0x00) + player/vehicle/infantry records + 1 hit.
	{
		FrameUpdate in;
		in.anchor_x = 0x00112233; in.anchor_y = int32_t(0xFFAABBCC); in.anchor_z = 0x0044EE55;
		in.flags1 = 0x02; in.flags2 = 0x00;
		in.weapon.preround_timer = 1; in.weapon.slot_state360 = 2; in.weapon.slot_state368 = 3;
		in.weapon.slot_state364 = 4; in.weapon.slot_state356 = 5; in.weapon.slot_state460 = 6;
		in.weapon.reload_seconds = 0xFF; in.weapon.uniform_team_mask = 0x12345678;
		in.state_flag_byte = 0x07; in.mount_handle = 0xFFFF; in.health = 96; in.state_word = -3;

		FrameUpdateRecord p; p.handle = 0x0001; p.type_id = 100; p.cls = EntityClass::Player;
		p.player.carrier_handle = 0xFFFF; p.player.pos_x_compressed = 0x1234; p.player.yaw_byte = 0x40;
		in.records.push_back(p);
		FrameUpdateRecord v; v.handle = 0x1002; v.type_id = 200; v.cls = EntityClass::Vehicle;
		v.vehicle.parent_slot_handle = 0xFFFF; v.vehicle.flags_byte = 0x00;
		v.vehicle.weapon_x = 0x0101; v.vehicle.weapon_aim_y = 0x0303;
		in.records.push_back(v);
		FrameUpdateRecord inf; inf.handle = 0x2003; inf.type_id = 300; inf.cls = EntityClass::Infantry;
		inf.infantry.vehicle_slot_handle = 0xFFFF; inf.infantry.pos_x_compressed = 0xABCD;
		in.records.push_back(inf);

		RoundEventRecord hit; hit.flags = 0x00; hit.adm_index = 9; hit.shooter_handle = 0x100A;
		hit.pos_x_compressed = 0x0011; in.round_events.push_back(hit);

		std::vector<uint8_t> wire = encode_frame_update(in);
		FrameUpdate out;
		EXPECT(decode_frame_update(wire.data(), wire.size(), class_of, out));
		EXPECT(out.complete);
		EXPECT(out.consumed == wire.size());
		EXPECT(out.anchor_x == in.anchor_x && out.anchor_y == in.anchor_y && out.anchor_z == in.anchor_z);
		EXPECT(out.flags1 == 0x02 && out.flags2 == 0x00);
		EXPECT(out.weapon.present && out.weapon.uniform_team_mask == 0x12345678 && out.weapon.slot_state460 == 6);
		EXPECT(out.health == 96 && out.state_word == -3 && out.mount_handle == 0xFFFF);
		EXPECT(out.records.size() == 3);
		EXPECT(out.records[0].cls == EntityClass::Player && out.records[0].handle == 0x0001);
		EXPECT(out.records[0].player.pos_x_compressed == 0x1234);
		EXPECT(out.records[1].cls == EntityClass::Vehicle && out.records[1].vehicle.weapon_x == 0x0101);
		EXPECT(out.records[1].vehicle.weapon_aim_y == 0x0303);
		EXPECT(out.records[2].cls == EntityClass::Infantry && out.records[2].infantry.pos_x_compressed == 0xABCD);
		EXPECT(out.round_events.size() == 1 && out.round_events[0].adm_index == 9 &&
		       out.round_events[0].shooter_handle == 0x100A);
	}

	// (b) env sub-block (flags2=0x02), no records.
	{
		FrameUpdate in;
		in.flags2 = 0x02;
		in.env.fog_dist = 0x1111; in.env.fog_accel = 0x2222; in.env.tod_fixed = 0x3333;
		in.env.quake_ticks = 0x44; in.env.env_param = 0x55;
		in.mount_handle = 0xFFFF; in.health = 50;
		std::vector<uint8_t> wire = encode_frame_update(in);
		FrameUpdate out;
		EXPECT(decode_frame_update(wire.data(), wire.size(), class_of, out));
		EXPECT(out.complete && out.consumed == wire.size());
		EXPECT(out.sub_block == 2 && out.env.present);
		EXPECT(out.env.fog_dist == 0x1111 && out.env.tod_fixed == 0x3333 && out.env.env_param == 0x55);
		EXPECT(out.records.empty());
	}

	// (c) passenger record (flags2=0x08 -> aim sub-block + passenger gate) with seat.
	{
		FrameUpdate in;
		in.flags2 = 0x08;
		in.mount_handle = 0x1005; in.health = 75;
		in.passenger.handle = 0x1006;
		in.passenger.seat_yaw = 0xAA11; in.passenger.seat_pitch = 0xBB22;
		std::vector<uint8_t> wire = encode_frame_update(in);
		FrameUpdate out;
		EXPECT(decode_frame_update(wire.data(), wire.size(), class_of, out));
		EXPECT(out.complete && out.consumed == wire.size());
		EXPECT(out.passenger.present && out.passenger.handle == 0x1006);
		EXPECT(out.passenger.has_seat && out.passenger.seat_yaw == 0xAA11 && out.passenger.seat_pitch == 0xBB22);
	}

	// (d) objective-gametype phase 3 carries a wire-invisible 16-byte block.
	// The receiver must learn this layout from 0x7B extra == g_GameType.
	{
		FrameUpdate in;
		in.flags2 = 0x03;
		in.objective.present = true;
		in.objective.state[0] = 0x11223344;
		in.objective.state[1] = -2;
		in.objective.state[2] = 0x55667788;
		in.objective.state[3] = -4;
		in.mount_handle = 0xFFFF;
		in.health = 88;
		std::vector<uint8_t> wire = encode_frame_update(in);
		FrameUpdate out;
		EXPECT(decode_frame_update(wire.data(), wire.size(), class_of, out,
		                           /*is_objective_gametype=*/true));
		EXPECT(out.complete && out.consumed == wire.size());
		EXPECT(out.objective.present && out.objective.state[0] == 0x11223344 &&
		       out.objective.state[1] == -2 && out.objective.state[2] == 0x55667788 &&
		       out.objective.state[3] == -4);
		EXPECT(out.local_tail_present && out.health == 88);
	}

	std::printf("PASS frame_update_roundtrip\n");
	return 0;
}

int test_frame_update_known_null_callback_record_is_header_only() {
	const EntityClass cls = class_from_tag("bldg");
	EXPECT(cls != EntityClass::Unknown);

	FrameUpdate in;
	in.flags2 = 0x02; // env block keeps the fixture compact and body-width fixed.
	in.env.fog_dist = 0x1111;
	in.env.fog_accel = 0x2222;
	in.env.tod_fixed = 0x3333;
	in.mount_handle = 0xFFFF;
	in.health = 100;

	FrameUpdateRecord header_only;
	header_only.handle = 0x2007;
	header_only.type_id = 0x0465;
	header_only.cls = cls;
	in.records.push_back(header_only);

	std::vector<uint8_t> wire = encode_frame_update(in);
	FrameUpdate out;
	auto class_of = [](uint16_t t) {
		return t == 0x0465 ? class_from_tag("bldg") : EntityClass::Unknown;
	};
	EXPECT(decode_frame_update(wire.data(), wire.size(), class_of, out));
	EXPECT(out.complete && out.consumed == wire.size());
	EXPECT(out.records.size() == 1);
	EXPECT(out.records[0].handle == 0x2007);
	EXPECT(out.records[0].type_id == 0x0465);
	EXPECT(out.records[0].cls == cls);

	std::printf("PASS frame_update_known_null_callback_record_is_header_only\n");
	return 0;
}

int test_frame_update_unknown_class_still_fails_closed() {
	FrameUpdate in;
	in.flags2 = 0x02;
	in.env.fog_dist = 0x1111;
	in.env.fog_accel = 0x2222;
	in.env.tod_fixed = 0x3333;
	in.mount_handle = 0xFFFF;
	in.health = 100;

	FrameUpdateRecord unknown;
	unknown.handle = 0x2008;
	unknown.type_id = 0x9999;
	unknown.cls = class_from_tag("zzzz");
	EXPECT(unknown.cls == EntityClass::Unknown);
	in.records.push_back(unknown);

	std::vector<uint8_t> wire = encode_frame_update(in);
	const auto terminator = wire.back();
	EXPECT(terminator == 0);
	wire.insert(wire.end() - 1, {0x01, 0x08, 0x20, 0x99, 0x99});

	FrameUpdate out;
	auto class_of = [](uint16_t) { return EntityClass::Unknown; };
	EXPECT(!decode_frame_update(wire.data(), wire.size(), class_of, out));
	EXPECT(!out.complete);

	std::printf("PASS frame_update_unknown_class_still_fails_closed\n");
	return 0;
}

// The relocated §5.1 reply encoders (moved from npruntime/server_message_dispatch.cpp in B3) round-trip
// through their ingame_decode partners — the encode/decode-in-one-lib symmetry the move enables.
int test_player_sync_roundtrip() {
	PlayerReplicationState rep;
	rep.player_slot = 3;
	rep.entity_handle = 0x0042; // pool-0 slot 0x42
	rep.player_name = "SocketJoiner";
	rep.clan_tag = "OPN";
	rep.team = 2;
	const std::vector<uint8_t> wire = encode_player_sync(rep);
	PlayerSync out;
	EXPECT(decode_player_sync(wire.data(), wire.size(), out));
	EXPECT(out.slot_id == rep.player_slot);
	EXPECT(out.field_bitmask == 0x1CF7u);
	EXPECT(out.entity_slot_id == static_cast<uint8_t>(rep.entity_handle & 0xFF));
	EXPECT(out.name == rep.player_name);
	EXPECT(out.clan == rep.clan_tag);
	EXPECT(out.team == rep.team);
	EXPECT(out.quality == 1);
	std::printf("PASS player_sync_roundtrip\n");
	return 0;
}

int test_player_list_roundtrip() {
	std::vector<PlayerListEntry> players = {{0, 1}, {1, 2}, {2, 2}};
	const std::vector<uint8_t> wire = encode_player_list(players);
	PlayerList out;
	EXPECT(decode_player_list(wire.data(), wire.size(), out));
	EXPECT(out.player_count == players.size());
	EXPECT(out.players.size() == players.size());
	for (size_t i = 0; i < players.size(); ++i) {
		EXPECT(out.players[i].slot_id == players[i].slot);
		EXPECT(static_cast<uint8_t>(out.players[i].flags >> 1) == players[i].team); // team = flags >> 1
	}
	EXPECT(out.team_count == 2);
	// Live trailer counts (D-NET-158): inGame = row count, spectators unmodeled 0 — the HUD
	// player count is acceptedRows − spectatorCount, so a hardcoded trailer pinned it at 2.
	EXPECT(out.in_game_count == players.size());
	EXPECT(out.spectator_count == 0);
	std::printf("PASS player_list_roundtrip\n");
	return 0;
}

// §5.37 S2C 0x45 terrain-tile (.til) load — encode/decode byte-identical round-trip.
static int test_terrain_load_header_chunk_roundtrip() {
	TerrainLoadBatch in{};
	in.has_header = true;
	in.end_index = 3;                 // header chunk carries tiles [0,3)
	in.magic = 0x74696C30u;           // 'til0'
	in.tile_count = 7;                // full set is larger than this chunk
	in.header_field2 = 0x11223344u;
	in.header_field3 = 0xAABBCCDDu;
	in.tiles = { {1, 2, 3}, {4, 5, 6}, {7, 8, 9} };

	std::vector<uint8_t> wire = encode_terrain_load_batch(in);
	// 4 (start/end) + 16 (magic+count+hdr2+hdr3) + 3*12 = 56 B
	EXPECT(wire.size() == 56);
	EXPECT(wire[0] == 0xFF && wire[1] == 0xFF); // first-chunk sentinel

	TerrainLoadBatch out{};
	EXPECT(decode_terrain_load_batch(wire.data(), wire.size(), out)); // consumed exactly
	EXPECT(out.has_header);
	EXPECT(out.start_index == 0);
	EXPECT(out.end_index == 3);
	EXPECT(out.magic == 0x74696C30u);
	EXPECT(out.tile_count == 7);
	EXPECT(out.header_field2 == 0x11223344u);
	EXPECT(out.header_field3 == 0xAABBCCDDu);
	EXPECT(out.tiles.size() == 3);
	EXPECT(out.tiles[2].word0 == 7 && out.tiles[2].word1 == 8 && out.tiles[2].word2 == 9);
	std::printf("PASS terrain_load_header_chunk_roundtrip\n");
	return 0;
}

static int test_terrain_load_continuation_chunk_roundtrip() {
	TerrainLoadBatch in{};
	in.has_header = false;
	in.start_index = 3;
	in.end_index = 5;                 // continuation chunk carries tiles [3,5)
	in.tiles = { {0xDEADBEEFu, 0, 1}, {2, 3, 0xFEEDFACEu} };

	std::vector<uint8_t> wire = encode_terrain_load_batch(in);
	// 4 (start/end) + 2*12 = 28 B, no header block
	EXPECT(wire.size() == 28);
	EXPECT(!(wire[0] == 0xFF && wire[1] == 0xFF)); // start_index=3, not the sentinel

	TerrainLoadBatch out{};
	EXPECT(decode_terrain_load_batch(wire.data(), wire.size(), out));
	EXPECT(!out.has_header);
	EXPECT(out.start_index == 3);
	EXPECT(out.end_index == 5);
	EXPECT(out.tiles.size() == 2);
	EXPECT(out.tiles[0].word0 == 0xDEADBEEFu);
	EXPECT(out.tiles[1].word2 == 0xFEEDFACEu);
	std::printf("PASS terrain_load_continuation_chunk_roundtrip\n");
	return 0;
}

// §5.46 S2C 0x18 FULL-ENTITY-SPAWN — the 0x0F-query reply record.
// Byte layout pinned against the witnessed serializer [orig: serialize_object_to_buffer
// @0x504d10]; the decode side is the independent inverse port of the client handler
// [orig: NapiNPClientMsg_FullEntitySpawn @0x433780].
static int test_full_entity_spawn_player_layout() {
	FullEntitySpawnRecord in{};
	in.slot_id = 0x0000;         // host player, pool 0 slot 0
	in.item_type_id = 0x14B9;    // "Player #1, Multiplayer" wire id
	in.item_type = 3;            // ItemType_Person
	in.team = 1;
	in.minimap_flags = 0x0100;   // remote player (not the recipient's own)
	in.entity_flags = 0x0C;      // owning connection id
	in.entity_name = "Player";
	in.pos_x = 0x1234;
	in.pos_y = 0x5678;
	in.pos_z = 0x0C50;
	in.heading_hi = 0x005A;
	in.net_id = 0x0200;
	in.player_class = 8;
	in.skip_byte = 0xEE;         // encoder must write the original's hard 0 instead

	const std::vector<uint8_t> wire = encode_full_entity_spawn(in);
	// 12 head + 7 name + 6 links + 1 seat mask (no seats) + 4 mount8/9 + 12 pos
	// + 4 yaw/pitch + 2 ai/anim + 2 net_id + 5 tail = 55 B
	EXPECT(wire.size() == 55);
	EXPECT(wire[0] == 0x00 && wire[1] == 0x00);   // slot
	EXPECT(wire[2] == 0xB9 && wire[3] == 0x14);   // type id
	EXPECT(wire[4] == 0x03);                      // itemDef type = person
	EXPECT(wire[5] == 0x01);                      // team
	EXPECT(wire[6] == 0x00 && wire[7] == 0x01);   // flags 0x0100
	EXPECT(wire[8] == 0x0C && wire[11] == 0x00);  // owner connection id u32
	EXPECT(wire[12] == 'P' && wire[18] == 0x00);  // "Player" + NUL
	EXPECT(wire[19] == 0xFF && wire[24] == 0xFF); // three 0xFFFF links
	EXPECT(wire[25] == 0x00);                     // seat mask 0 -> no occupant words
	EXPECT(wire[30] == 0x34 && wire[31] == 0x12); // pos_x LE
	EXPECT(wire[42] == 0x5A && wire[43] == 0x00); // heading high word
	EXPECT(wire[48] == 0x00 && wire[49] == 0x02); // minimap net_id
	EXPECT(wire[50] == 0x08);                     // playerClass
	EXPECT(wire[51] == 0x00);                     // hard 0 (input 0xEE ignored)

	FullEntitySpawnRecord out{};
	EXPECT(decode_full_entity_spawn(wire.data(), wire.size(), out));
	EXPECT(out.slot_id == in.slot_id);
	EXPECT(out.item_type_id == in.item_type_id);
	EXPECT(out.item_type == in.item_type);
	EXPECT(out.team == in.team);
	EXPECT(out.minimap_flags == in.minimap_flags);
	EXPECT(out.entity_flags == in.entity_flags);
	EXPECT(out.entity_name == in.entity_name);
	EXPECT(out.parent_vehicle_handle == 0xFFFF);
	EXPECT(out.ground_entity_handle == 0xFFFF);
	EXPECT(out.parent_entity_handle == 0xFFFF);
	EXPECT(out.seat_mask == 0);
	EXPECT(out.pos_x == in.pos_x && out.pos_y == in.pos_y && out.pos_z == in.pos_z);
	EXPECT(out.heading_hi == in.heading_hi && out.pitch_hi == 0);
	EXPECT(out.net_id == in.net_id);
	EXPECT(out.player_class == in.player_class);
	EXPECT(out.skip_byte == 0);
	std::printf("PASS full_entity_spawn_player_layout\n");
	return 0;
}

// Sparse seat mask: only the set bits carry an occupant word, and the decoder leaves
// unset seats at the handler's 0xFFFF prefill (@0x433935).
static int test_full_entity_spawn_seat_block_roundtrip() {
	FullEntitySpawnRecord in{};
	in.slot_id = 0x1002;         // pool-1 vehicle, slot 2
	in.item_type_id = 0x050B;    // dune buggy
	in.item_type = 1;            // ItemType_Vehicle
	in.seat_mask = 0x05;         // seats 0 and 2 only
	in.mount_handles[0] = 0x0001;
	in.mount_handles[2] = 0xFFFF; // offered but empty
	in.mount_handles[3] = 0x0BAD; // NOT in the mask -> must not hit the wire

	const std::vector<uint8_t> wire = encode_full_entity_spawn(in);
	// 12 head + 1 empty name + 6 links + 1 mask + 2*2 occupants + 4 mount8/9 + 12 pos
	// + 4 yaw/pitch + 2 ai/anim + 2 net_id + 5 tail = 53 B
	EXPECT(wire.size() == 53);

	FullEntitySpawnRecord out{};
	EXPECT(decode_full_entity_spawn(wire.data(), wire.size(), out));
	EXPECT(out.seat_mask == 0x05);
	EXPECT(out.mount_handles[0] == 0x0001);
	EXPECT(out.mount_handles[1] == 0xFFFF); // prefill, not on the wire
	EXPECT(out.mount_handles[2] == 0xFFFF);
	EXPECT(out.mount_handles[3] == 0xFFFF); // masked-out input never crossed
	std::printf("PASS full_entity_spawn_seat_block_roundtrip\n");
	return 0;
}

// An in-capacity EMPTY pool slot: retail serializes the slot with a null itemDef ->
// type_id/item_type 0 + empty name; the client destroys+memsets and stops at the
// type gate (@0x433b5a) — the "clear your stale entity" reply.
static int test_full_entity_spawn_empty_slot_record() {
	FullEntitySpawnRecord in{};
	in.slot_id = 0x0003;

	const std::vector<uint8_t> wire = encode_full_entity_spawn(in);
	// 12 head + 1 empty name + 6 links + 1 mask + 4 + 12 + 4 + 2 + 2 + 5 = 49 B
	EXPECT(wire.size() == 49);
	EXPECT(wire[2] == 0x00 && wire[3] == 0x00); // type id 0
	EXPECT(wire[4] == 0x00);                    // itemDef type 0 -> client skips the rebuild

	FullEntitySpawnRecord out{};
	EXPECT(decode_full_entity_spawn(wire.data(), wire.size(), out));
	EXPECT(out.slot_id == 0x0003);
	EXPECT(out.item_type_id == 0 && out.item_type == 0);
	EXPECT(out.entity_name.empty());
	std::printf("PASS full_entity_spawn_empty_slot_record\n");
	return 0;
}

} // namespace

int main() {
	int rc = 0;
	rc |= test_compress_fixedpoint_roundtrip();
	rc |= test_frame_update_roundtrip();
	rc |= test_frame_update_known_null_callback_record_is_header_only();
	rc |= test_frame_update_unknown_class_still_fails_closed();
	rc |= test_roundtrip_all_flags();
	rc |= test_roundtrip_no_flags_and_length();
	rc |= test_flag_derivation_is_from_nonzero();
	rc |= test_empty_slot_sentinel_and_mixed();
	rc |= test_empty_batch();
	rc |= test_static_batch_all_flags();
	rc |= test_static_batch_minimal_and_length();
	rc |= test_static_batch_attach_ref_via_0x200();
	rc |= test_static_batch_empty_slot_and_empty_batch();
	rc |= test_pool_spawn_roundtrip_full();
	rc |= test_pool_spawn_minimal();
	rc |= test_pool_spawn_health_alt_8000();
	rc |= test_decode_weapon_block_mask_zero_dnet56();
	rc |= test_infantry_compact_roundtrip();
	rc |= test_vehicle_compact_roundtrip_mounted();
	rc |= test_vehicle_compact_roundtrip_unmounted();
	rc |= test_player_compact_roundtrip();
	rc |= test_weapon_reload_roundtrip();
	rc |= test_loadout_submit_roundtrip();
	rc |= test_player_sync_roundtrip();
	rc |= test_player_list_roundtrip();
	rc |= test_terrain_load_header_chunk_roundtrip();
	rc |= test_terrain_load_continuation_chunk_roundtrip();
	rc |= test_full_entity_spawn_player_layout();
	rc |= test_full_entity_spawn_seat_block_roundtrip();
	rc |= test_full_entity_spawn_empty_slot_record();
	if (rc == 0) std::printf("ALL nw_ingame_encode tests passed\n");
	return rc;
}
