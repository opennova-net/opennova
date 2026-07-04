// Byte-witness §5.10 / §5.13 / §5.14 compact record decoders.
//
// Hand-built input vectors mirror the wire layout from docs/net/novaworld-net-re.md;
// no capture dependency, so the test runs unconditionally in CI.

#include <npwire/ingame_decode.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace opennova;

namespace {

#define EXPECT(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "FAIL %s:%d  EXPECT(%s)\n", __FILE__, __LINE__, #cond); \
		return 1; \
	} \
} while (0)

int test_player_compact_18_bytes() {
	// §5.10 — 18 B fixed. Build a record on-foot (carrier_handle 0xFFFF).
	// vehicleBone=0, seatType=0, vehHandle=0xFFFF, posXYZ compressed, yaw/pitch,
	// animSlot, stateFlags, weaponId, animChannelRatio, animDef, healthClass.
	const uint8_t body[] = {
		0x00, 0x00,             // vehicle_bone, seat_type
		0xFF, 0xFF,             // carrier_handle (none)
		0x8F, 0xBE,             // posX compressed
		0x04, 0xB5,             // posY
		0xD7, 0xBC,             // posZ
		0x40,                   // yaw_byte
		0x00,                   // pitch_byte
		0x12,                   // move_input_byte
		0x02,                   // state_flags (bit 1 = spawning)
		0x05,                   // anim_state_id
		0x10,                   // anim_channel_ratio
		0x07,                   // anim_def_index
		0x80,                   // health_class_byte
		0xAA,                   // trailing byte to prove prefix-consume
	};
	PlayerCompactRecord rec;
	size_t consumed = 0;
	const bool ok = decode_player_compact_record(body, sizeof(body), rec, consumed);
	EXPECT(ok);
	EXPECT(consumed == 18);
	EXPECT(rec.carrier_handle == 0xFFFF);
	EXPECT(rec.pos_x_compressed == 0xBE8F);
	EXPECT(rec.pos_y_compressed == 0xB504);
	EXPECT(rec.pos_z_compressed == 0xBCD7);
	EXPECT(rec.yaw_byte == 0x40);
	EXPECT(rec.state_flags == 0x02);
	EXPECT(rec.anim_channel_ratio == 0x10);
	EXPECT(rec.health_class_byte == 0x80);
	std::printf("PASS player_compact 18B\n");
	return 0;
}

int test_player_short_buffer_fails() {
	const uint8_t body[10] = {0};
	PlayerCompactRecord rec;
	size_t consumed = 12345;
	const bool ok = decode_player_compact_record(body, sizeof(body), rec, consumed);
	EXPECT(!ok);
	EXPECT(consumed == 0);
	std::printf("PASS player_short_buffer\n");
	return 0;
}

int test_vehicle_mounted_15_bytes() {
	// §5.13 — mounted branch: flags_byte & 4 → 15 B total.
	const uint8_t body[] = {
		0x8B, 0x10,             // parent_slot_handle (pool 1 slot 0x08B)
		0x10, 0x20,             // posX compressed
		0x30, 0x40,             // posY
		0x50, 0x60,             // posZ
		0x00, 0x80,             // euler_z (i16 = -0x8000)
		0x04,                   // flags_byte — bit 2 SET (mounted)
		0x11, 0x22,             // euler_y
		0x33, 0x44,             // euler_x
		0xAA, 0xBB,             // trailing
	};
	VehicleCompactRecord rec;
	size_t consumed = 0;
	const bool ok = decode_vehicle_compact_record(body, sizeof(body), rec, consumed);
	EXPECT(ok);
	EXPECT(consumed == 15);
	EXPECT(rec.is_dead_pose == true);
	EXPECT(rec.parent_slot_handle == 0x108B);
	EXPECT(rec.flags_byte == 0x04);
	EXPECT(rec.euler_y == int16_t(0x2211));
	EXPECT(rec.euler_x == int16_t(0x4433));
	std::printf("PASS vehicle_mounted 15B\n");
	return 0;
}

int test_vehicle_unmounted_21_bytes() {
	// §5.13 — unmounted branch: flags_byte & 4 clear → 21 B total.
	const uint8_t body[] = {
		0xFF, 0xFF,             // parent_slot_handle (none)
		0x01, 0x02,             // posX
		0x03, 0x04,             // posY
		0x05, 0x06,             // posZ
		0x00, 0x40,             // euler_z
		0x01,                   // flags_byte (mounted bit CLEAR)
		0x11, 0x22,             // weapon_x
		0x33, 0x44,             // health_word
		0x55, 0x66,             // weapon_aim_y
		0x77, 0x88,             // weapon_aim_z
		0x99, 0xAA,             // weapon_heading_bam
		0xCC,                   // trailing
	};
	VehicleCompactRecord rec;
	size_t consumed = 0;
	const bool ok = decode_vehicle_compact_record(body, sizeof(body), rec, consumed);
	EXPECT(ok);
	EXPECT(consumed == 21);
	EXPECT(rec.is_dead_pose == false);
	EXPECT(rec.parent_slot_handle == 0xFFFF);
	EXPECT(rec.weapon_x == 0x2211);
	EXPECT(rec.health_word == 0x4433);
	EXPECT(rec.weapon_aim_y == 0x6655);
	EXPECT(rec.weapon_aim_z == 0x8877);
	EXPECT(rec.weapon_heading_bam == int16_t(0xAA99));
	std::printf("PASS vehicle_unmounted 21B\n");
	return 0;
}

int test_round_event_minimal_no_flags() {
	// §5.9.1 — flags=0, no slot_byte, no target_handle → 17 B.
	const uint8_t body[] = {
		0x00,                   // flags = 0
		0x09,                   // adm_index
		0x03,                   // subtype
		0x12, 0x10,             // shooter_handle (pool 1 slot 0x012)
		0x34, 0x12,             // shot_seq
		0x11, 0x22,             // pos_x_compressed
		0x33, 0x44,             // pos_y_compressed
		0x55, 0x66,             // pos_z_compressed
		0x77, 0x88,             // yaw_bam_high
		0x99, 0xAA,             // pitch_bam_high
		0xCC,                   // trailing sentinel
	};
	RoundEventRecord rec;
	size_t consumed = 0;
	const bool ok = decode_round_event_record(body, sizeof(body), rec, consumed);
	EXPECT(ok);
	EXPECT(consumed == 17);
	EXPECT(rec.flags == 0);
	EXPECT(!rec.has_slot_byte());
	EXPECT(!rec.has_target_handle());
	EXPECT(rec.adm_index == 9);
	EXPECT(rec.subtype == 3);
	EXPECT(rec.shooter_handle == 0x1012);
	EXPECT(rec.target_handle == 0xFFFF);  // default sentinel, gate clear
	EXPECT(rec.shot_seq == 0x1234);
	EXPECT(rec.pos_x_compressed == 0x2211);
	EXPECT(rec.pos_z_compressed == 0x6655);
	EXPECT(rec.yaw_bam_high == 0x8877);
	EXPECT(rec.pitch_bam_high == 0xAA99);
	std::printf("PASS round_event minimal 17B\n");
	return 0;
}

int test_round_event_flag_0x80_adds_byte() {
	// §5.9.1 — flags=0x80 adds slot_byte → 18 B.
	const uint8_t body[] = {
		0x80,                   // flags = 0x80 (slot_byte present)
		0x09, 0x03,             // adm_index, subtype
		0x55,                   // slot_byte
		0x12, 0x10,             // shooter_handle
		0x34, 0x12,             // shot_seq
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66,  // pos x/y/z
		0x77, 0x88, 0x99, 0xAA,              // yaw, pitch
		0xCC,                   // trailing
	};
	RoundEventRecord rec;
	size_t consumed = 0;
	const bool ok = decode_round_event_record(body, sizeof(body), rec, consumed);
	EXPECT(ok);
	EXPECT(consumed == 18);
	EXPECT(rec.has_slot_byte());
	EXPECT(!rec.has_target_handle());
	EXPECT(rec.slot_byte == 0x55);
	EXPECT(rec.shooter_handle == 0x1012);
	EXPECT(rec.shot_seq == 0x1234);
	std::printf("PASS round_event flag_0x80 18B\n");
	return 0;
}

int test_round_event_flag_0x40_adds_u16() {
	// §5.9.1 — flags=0x40 adds target_handle → 19 B.
	const uint8_t body[] = {
		0x40,                   // flags = 0x40 (target_handle present)
		0x09, 0x03,             // adm_index, subtype
		0x12, 0x10,             // shooter_handle
		0xAB, 0x1A,             // target_handle (pool 1 slot 0xAAB)
		0x34, 0x12,             // shot_seq
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66,  // pos x/y/z
		0x77, 0x88, 0x99, 0xAA,              // yaw, pitch
		0xCC,                   // trailing
	};
	RoundEventRecord rec;
	size_t consumed = 0;
	const bool ok = decode_round_event_record(body, sizeof(body), rec, consumed);
	EXPECT(ok);
	EXPECT(consumed == 19);
	EXPECT(!rec.has_slot_byte());
	EXPECT(rec.has_target_handle());
	EXPECT(rec.target_handle == 0x1AAB);
	EXPECT(rec.shooter_handle == 0x1012);
	std::printf("PASS round_event flag_0x40 19B\n");
	return 0;
}

int test_round_event_both_flags_20_bytes() {
	// §5.9.1 — flags=0xC0 adds both → 20 B.
	const uint8_t body[] = {
		0xC0,                   // flags = 0xC0 (both)
		0x09, 0x03,             // adm_index, subtype
		0x55,                   // slot_byte
		0x12, 0x10,             // shooter_handle
		0xAB, 0x1A,             // target_handle
		0x34, 0x12,             // shot_seq
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66,  // pos x/y/z
		0x77, 0x88, 0x99, 0xAA,              // yaw, pitch
		0xCC,                   // trailing
	};
	RoundEventRecord rec;
	size_t consumed = 0;
	const bool ok = decode_round_event_record(body, sizeof(body), rec, consumed);
	EXPECT(ok);
	EXPECT(consumed == 20);
	EXPECT(rec.has_slot_byte());
	EXPECT(rec.has_target_handle());
	EXPECT(rec.slot_byte == 0x55);
	EXPECT(rec.target_handle == 0x1AAB);
	std::printf("PASS round_event both_flags 20B\n");
	return 0;
}

int test_round_event_short_buffer_fails() {
	// Truncated body should fail closed.
	const uint8_t body[5] = {0xC0, 0x09, 0x03, 0x55, 0x12};
	RoundEventRecord rec;
	size_t consumed = 99;
	const bool ok = decode_round_event_record(body, sizeof(body), rec, consumed);
	EXPECT(!ok);
	EXPECT(consumed == 0);
	std::printf("PASS round_event short_buffer\n");
	return 0;
}

int test_infantry_14_bytes() {
	// §5.14 — fixed 14 B.
	const uint8_t body[] = {
		0x05,                   // seat_bone_idx
		0x8B, 0x10,             // vehicle_slot_handle (pool 1 slot 0x08B)
		0x10, 0x20,             // posX compressed
		0x30, 0x40,             // posY
		0x50, 0x60,             // posZ
		0x80,                   // yaw_byte
		0x02,                   // flags_byte
		0x10,                   // pitch_byte
		0x40,                   // aim_yaw_byte
		0x07,                   // anim_byte
		0xFF,                   // trailing
	};
	InfantryCompactRecord rec;
	size_t consumed = 0;
	const bool ok = decode_infantry_compact_record(body, sizeof(body), rec, consumed);
	EXPECT(ok);
	EXPECT(consumed == 14);
	EXPECT(rec.seat_bone_idx == 0x05);
	EXPECT(rec.vehicle_slot_handle == 0x108B);
	EXPECT(rec.pos_x_compressed == 0x2010);
	EXPECT(rec.yaw_byte == 0x80);
	EXPECT(rec.flags_byte == 0x02);
	EXPECT(rec.anim_byte == 0x07);
	std::printf("PASS infantry_compact 14B\n");
	return 0;
}

} // namespace

int main(void) {
	int rc = 0;
	rc |= test_player_compact_18_bytes();
	rc |= test_player_short_buffer_fails();
	rc |= test_vehicle_mounted_15_bytes();
	rc |= test_vehicle_unmounted_21_bytes();
	rc |= test_infantry_14_bytes();
	rc |= test_round_event_minimal_no_flags();
	rc |= test_round_event_flag_0x80_adds_byte();
	rc |= test_round_event_flag_0x40_adds_u16();
	rc |= test_round_event_both_flags_20_bytes();
	rc |= test_round_event_short_buffer_fails();
	if (rc == 0) std::printf("ALL PASS\n");
	return rc;
}
