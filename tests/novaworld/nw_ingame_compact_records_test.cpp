// Byte-witness §5.10 / §5.13 / §5.14 compact record decoders.
//
// Hand-built input vectors mirror the wire layout from docs/net/novaworld-net-re.md;
// no capture dependency, so the test runs unconditionally in CI.

#include <novaworld/ingame_decode.h>

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
	// §5.10 — 18 B fixed. Build a record on-foot (vehicle_handle 0xFFFF).
	// vehicleBone=0, seatType=0, vehHandle=0xFFFF, posXYZ compressed, yaw/pitch,
	// animSlot, stateFlags, weapAnim, priority, animDef, healthClass.
	const uint8_t body[] = {
		0x00, 0x00,             // vehicle_bone, seat_type
		0xFF, 0xFF,             // vehicle_handle (none)
		0x8F, 0xBE,             // posX compressed
		0x04, 0xB5,             // posY
		0xD7, 0xBC,             // posZ
		0x40,                   // yaw_byte
		0x00,                   // pitch_byte
		0x12,                   // anim_slot_low
		0x02,                   // state_flags (bit 1 = spawning)
		0x05,                   // weapon_anim_state
		0x10,                   // priority
		0x07,                   // anim_def_index
		0x80,                   // health_class_byte
		0xAA,                   // trailing byte to prove prefix-consume
	};
	PlayerCompactRecord rec;
	size_t consumed = 0;
	const bool ok = decode_player_compact_record(body, sizeof(body), rec, consumed);
	EXPECT(ok);
	EXPECT(consumed == 18);
	EXPECT(rec.vehicle_handle == 0xFFFF);
	EXPECT(rec.pos_x_compressed == 0xBE8F);
	EXPECT(rec.pos_y_compressed == 0xB504);
	EXPECT(rec.pos_z_compressed == 0xBCD7);
	EXPECT(rec.yaw_byte == 0x40);
	EXPECT(rec.state_flags == 0x02);
	EXPECT(rec.priority == 0x10);
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
		0x00, 0x80,             // yaw_high (i16 = -0x8000)
		0x04,                   // flags_byte — bit 2 SET (mounted)
		0x11, 0x22,             // secondary_heading
		0x33, 0x44,             // final_heading
		0xAA, 0xBB,             // trailing
	};
	VehicleCompactRecord rec;
	size_t consumed = 0;
	const bool ok = decode_vehicle_compact_record(body, sizeof(body), rec, consumed);
	EXPECT(ok);
	EXPECT(consumed == 15);
	EXPECT(rec.is_mounted == true);
	EXPECT(rec.parent_slot_handle == 0x108B);
	EXPECT(rec.flags_byte == 0x04);
	EXPECT(rec.secondary_heading == 0x2211);
	EXPECT(rec.final_heading == 0x4433);
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
		0x00, 0x40,             // yaw_high
		0x01,                   // flags_byte (mounted bit CLEAR)
		0x11, 0x22,             // weapon_x_compressed
		0x33, 0x44,             // weapon_y_raw
		0x55, 0x66,             // weapon_z_compressed
		0x77, 0x88,             // weapon_heading_compressed
		0x99, 0xAA,             // final_heading
		0xCC,                   // trailing
	};
	VehicleCompactRecord rec;
	size_t consumed = 0;
	const bool ok = decode_vehicle_compact_record(body, sizeof(body), rec, consumed);
	EXPECT(ok);
	EXPECT(consumed == 21);
	EXPECT(rec.is_mounted == false);
	EXPECT(rec.parent_slot_handle == 0xFFFF);
	EXPECT(rec.weapon_x_compressed == 0x2211);
	EXPECT(rec.weapon_y_raw == 0x4433);
	EXPECT(rec.weapon_z_compressed == 0x6655);
	EXPECT(rec.weapon_heading_compressed == 0x8877);
	EXPECT(rec.final_heading == 0xAA99);
	std::printf("PASS vehicle_unmounted 21B\n");
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
	if (rc == 0) std::printf("ALL PASS\n");
	return rc;
}
