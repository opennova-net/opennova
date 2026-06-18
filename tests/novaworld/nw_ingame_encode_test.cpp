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

#include <novaworld/ingame_decode.h>
#include <novaworld/ingame_encode.h>

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
	r.vel_x = 10;                    // -> 0x0001
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
	r.health_byte = 80;              // -> 0x2000 (+ health_short)
	r.health_short = 1000;
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
	EXPECT(d.vel_x == 10);
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
	EXPECT(d.health_byte == 80);
	EXPECT(d.health_short == 1000);
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
	// health_byte 0 but health_short non-zero -> 0x8000 path (health_short only).
	PoolSpawnBatch in;
	PoolSpawnRecord r;
	r.slot_id = 0x1003; r.item_type_id = 0x0100;
	r.health_byte = 0; r.health_short = 250;
	r.bone_byte = 2;
	in.records.push_back(r);

	std::vector<uint8_t> wire = encode_pool_spawn_batch(in);
	PoolSpawnBatch out;
	EXPECT(decode_pool_spawn_batch(wire.data(), wire.size(), out));
	EXPECT(out.records.size() == 1);
	EXPECT(out.records[0].health_byte == 0);
	EXPECT(out.records[0].health_short == 250);
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
	EXPECT(d.is_mounted);
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
	r.turret_pitch_raw = int16_t(0x0202);
	r.weapon_aim_y = 0x0303;
	r.weapon_aim_z = 0x0404;
	r.weapon_heading_bam = int16_t(0x0505);

	std::vector<uint8_t> wire = encode_vehicle_compact_record(r);
	EXPECT(wire.size() == 21);
	VehicleCompactRecord d;
	size_t consumed = 0;
	EXPECT(decode_vehicle_compact_record(wire.data(), wire.size(), d, consumed));
	EXPECT(consumed == 21);
	EXPECT(!d.is_mounted);
	EXPECT(d.parent_slot_handle == 0xFFFF);
	EXPECT(d.pos_x_compressed == 0xAAAA);
	EXPECT(d.weapon_x == 0x0101);
	EXPECT(d.turret_pitch_raw == int16_t(0x0202));
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
	r.vehicle_handle    = 0xFFFF;     // on foot
	r.pos_x_compressed  = 0xBE8F;
	r.pos_y_compressed  = 0xB504;
	r.pos_z_compressed  = 0xBCD7;
	r.yaw_byte          = 0x40;
	r.pitch_byte        = 0x10;
	r.anim_slot_low     = 0x12;
	r.state_flags       = 0x06;       // bit1 spawning + bit2 mounted bits set
	r.weapon_anim_state = 0x05;
	r.priority          = 0x10;
	r.anim_def_index    = 0x07;
	r.health_class_byte = 0x80;

	std::vector<uint8_t> wire = encode_player_compact_record(r);
	EXPECT(wire.size() == 18);
	PlayerCompactRecord d;
	size_t consumed = 0;
	EXPECT(decode_player_compact_record(wire.data(), wire.size(), d, consumed));
	EXPECT(consumed == 18);
	EXPECT(d.vehicle_bone == 1 && d.seat_type == 2);
	EXPECT(d.vehicle_handle == 0xFFFF);
	EXPECT(d.pos_x_compressed == 0xBE8F);
	EXPECT(d.pos_z_compressed == 0xBCD7);
	EXPECT(d.yaw_byte == 0x40 && d.pitch_byte == 0x10);
	EXPECT(d.anim_slot_low == 0x12 && d.state_flags == 0x06);
	EXPECT(d.weapon_anim_state == 0x05 && d.priority == 0x10);
	EXPECT(d.anim_def_index == 0x07 && d.health_class_byte == 0x80);
	std::printf("PASS player_compact_roundtrip\n");
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
		in.aim.view0 = 1; in.aim.view1 = 2; in.aim.view2 = 3;
		in.aim.view3 = 4; in.aim.view4 = 5; in.aim.view5 = 6;
		in.aim.target_slot = 0xFF; in.aim.aim_extra = 0x12345678;
		in.state_flag_byte = 0x07; in.mount_handle = 0xFFFF; in.health = 96; in.state_word = -3;

		FrameUpdateRecord p; p.handle = 0x0001; p.type_id = 100; p.cls = EntityClass::Player;
		p.player.vehicle_handle = 0xFFFF; p.player.pos_x_compressed = 0x1234; p.player.yaw_byte = 0x40;
		in.records.push_back(p);
		FrameUpdateRecord v; v.handle = 0x1002; v.type_id = 200; v.cls = EntityClass::Vehicle;
		v.vehicle.parent_slot_handle = 0xFFFF; v.vehicle.flags_byte = 0x00;
		v.vehicle.weapon_x = 0x0101; v.vehicle.weapon_aim_y = 0x0303;
		in.records.push_back(v);
		FrameUpdateRecord inf; inf.handle = 0x2003; inf.type_id = 300; inf.cls = EntityClass::Infantry;
		inf.infantry.vehicle_slot_handle = 0xFFFF; inf.infantry.pos_x_compressed = 0xABCD;
		in.records.push_back(inf);

		WeaponHitRecord hit; hit.flags = 0x00; hit.adm_index = 9; hit.target_handle = 0x100A;
		hit.pos_x_compressed = 0x0011; in.hits.push_back(hit);

		std::vector<uint8_t> wire = encode_frame_update(in);
		FrameUpdate out;
		EXPECT(decode_frame_update(wire.data(), wire.size(), class_of, out));
		EXPECT(out.complete);
		EXPECT(out.consumed == wire.size());
		EXPECT(out.anchor_x == in.anchor_x && out.anchor_y == in.anchor_y && out.anchor_z == in.anchor_z);
		EXPECT(out.flags1 == 0x02 && out.flags2 == 0x00);
		EXPECT(out.aim.present && out.aim.aim_extra == 0x12345678 && out.aim.view5 == 6);
		EXPECT(out.health == 96 && out.state_word == -3 && out.mount_handle == 0xFFFF);
		EXPECT(out.records.size() == 3);
		EXPECT(out.records[0].cls == EntityClass::Player && out.records[0].handle == 0x0001);
		EXPECT(out.records[0].player.pos_x_compressed == 0x1234);
		EXPECT(out.records[1].cls == EntityClass::Vehicle && out.records[1].vehicle.weapon_x == 0x0101);
		EXPECT(out.records[1].vehicle.weapon_aim_y == 0x0303);
		EXPECT(out.records[2].cls == EntityClass::Infantry && out.records[2].infantry.pos_x_compressed == 0xABCD);
		EXPECT(out.hits.size() == 1 && out.hits[0].adm_index == 9 && out.hits[0].target_handle == 0x100A);
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

	std::printf("PASS frame_update_roundtrip\n");
	return 0;
}

} // namespace

int main() {
	int rc = 0;
	rc |= test_compress_fixedpoint_roundtrip();
	rc |= test_frame_update_roundtrip();
	rc |= test_roundtrip_all_flags();
	rc |= test_roundtrip_no_flags_and_length();
	rc |= test_flag_derivation_is_from_nonzero();
	rc |= test_empty_slot_sentinel_and_mixed();
	rc |= test_empty_batch();
	rc |= test_pool_spawn_roundtrip_full();
	rc |= test_pool_spawn_minimal();
	rc |= test_pool_spawn_health_alt_8000();
	rc |= test_decode_weapon_block_mask_zero_dnet56();
	rc |= test_infantry_compact_roundtrip();
	rc |= test_vehicle_compact_roundtrip_mounted();
	rc |= test_vehicle_compact_roundtrip_unmounted();
	rc |= test_player_compact_roundtrip();
	if (rc == 0) std::printf("ALL nw_ingame_encode tests passed\n");
	return rc;
}
