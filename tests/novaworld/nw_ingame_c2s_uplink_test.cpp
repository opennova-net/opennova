// Byte-witness §5.10 C2S 0x0C extended (type-10) player uplink decoder.
//
// Input vectors are real bytes extracted from
// ~/Desktop/host_and_join_game_on_opennovaworld_loopback_threeplayers_more_gameplay.pcapng
// (the 3-player loopback capture) via nw_pp. No capture dependency at test
// time — vectors are embedded — so the test runs unconditionally in CI.
//
// The frames witnessed here drive the §5.10 cross-witness in
// docs/net/novaworld-net-re.md and let nw_pp regress on either side.

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>

#include <array>
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

// Frame 1905 — joiner s2 stationary on-foot at the southern Laba-Laba
// landing. vehHdl=0xFFFF, position in WORLD coords + map origin (the host's
// case-4 path adds origin on the unmounted branch). Anti-cheat counters
// already non-zero (this is mid-game, ~30 s after the load). Bytes copied
// verbatim from nw_pp's hex dump of the 3-player capture's f=1905 datagram.
const uint8_t kFrame1905_full[] = {
	0x02, 0x00, 0xb9, 0x14, 0x0a, // header: handle=0x0002 typeId=0x14b9 sub_op=0x0a
	0xff, 0xff,                   // body[0..1]  carrier_handle = none
	0x61, 0x48, 0x46, 0xfe,       // body[2..5]  posX = 0xfe464861 (i32 16.16) ≈ -441.7
	0xec, 0xad, 0x78, 0x01,       // body[6..9]  posY = 0x0178adec ≈ 376.7
	0x68, 0xde, 0x0b, 0x00,       // body[10..13] posZ = 0x000bde68 ≈ 11.9
	0x2d, 0x38,                   // body[14..15] heading i16 LE = 0x382D = 14381
	0x00, 0x00,                   // body[16..17] pitch i16 LE = 0
	0x00,                         // body[18]     anticheat_flags
	0x00,                         // body[19]     move_input_byte
	0x01,                         // body[20]     state_flags_byte (wire bit 0; host masks to 0x1C)
	0x00, 0x00, 0x00,             // body[21..23] analog_x/2/3
	0x07,                         // body[24]     equipped_adm_index (entity+0x2B0 — the joiner's
	                              //              equipped weapon; the host echoes it at 0x0A
	                              //              off-16 [orig: @0x4C20A3], D-NET-143)
	0x00, 0x00,                   // body[25..26] stat_byte_0/1
	0x21, 0x10, 0xce, 0x07,       // body[27..30] priority_handle_0 / priority_score_0
	0x2d, 0x10, 0x69, 0x07,       // body[31..34] priority_handle_1 / priority_score_1
	0x00, 0x00, 0x32, 0x07,       // body[35..38] priority_handle_2 / priority_score_2
	0x1e, 0x10, 0x1c, 0x07,       // body[39..42] priority_handle_3 / priority_score_3
};
static_assert(sizeof(kFrame1905_full) == 48,
              "C2S 0x0C extended frame is 5 B header + 43 B body = 48 B");

int test_sub_header_parse() {
	EntityPacketSubHeader hdr;
	size_t consumed = 0;
	const bool ok = decode_entity_packet_sub_header(
		kFrame1905_full, sizeof(kFrame1905_full), hdr, consumed);
	EXPECT(ok);
	EXPECT(consumed == 5);
	EXPECT(hdr.handle == 0x0002);
	EXPECT(hdr.item_type_id == 0x14B9); // player class — §5.10b
	EXPECT(hdr.sub_op == 0x0A);         // extended (type 10)
	std::printf("PASS entity_packet_sub_header\n");
	return 0;
}

int test_extended_uplink_stationary_on_foot() {
	// Frame 1905 — joiner s2, on foot, holding still. Heading 14381 (BAM high),
	// pitch 0. state_flags_byte=0x01 (wire byte; host masks to 0x1C before XOR).
	PlayerExtendedUplink r;
	size_t consumed = 0;
	const bool ok = decode_player_extended_uplink(
		kFrame1905_full + 5, sizeof(kFrame1905_full) - 5, r, consumed);
	EXPECT(ok);
	EXPECT(consumed == 43);

	EXPECT(r.carrier_handle == 0xFFFF);
	EXPECT(uint32_t(r.pos_x) == 0xfe464861u);
	EXPECT(uint32_t(r.pos_y) == 0x0178adecu);
	EXPECT(uint32_t(r.pos_z) == 0x000bde68u);
	EXPECT(r.heading == 14381);  // body[14..15] = `2d 38` LE
	EXPECT(r.pitch == 0);
	EXPECT(r.anticheat_flags == 0x00);
	EXPECT(r.move_input_byte == 0x00);
	EXPECT(r.state_flags_byte == 0x01);
	EXPECT(r.analog_x == 0x00);
	EXPECT(r.analog_y == 0x00);
	EXPECT(r.analog_z == 0x00);
	EXPECT(r.equipped_adm_index == 0x07); // the joiner's live equipped weapon (entity+0x2B0)
	EXPECT(r.stat_byte_0 == 0x00);
	EXPECT(r.stat_byte_1 == 0x00);

	EXPECT(r.priority_handle_0 == 0x1021);    EXPECT(r.priority_score_0 == 0x07ce);
	EXPECT(r.priority_handle_1 == 0x102d);    EXPECT(r.priority_score_1 == 0x0769);
	EXPECT(r.priority_handle_2 == 0x0000);    EXPECT(r.priority_score_2 == 0x0732);
	EXPECT(r.priority_handle_3 == 0x101e);    EXPECT(r.priority_score_3 == 0x071c);
	std::printf("PASS extended_uplink stationary on-foot\n");
	return 0;
}

// Frame 2053 — joiner just mounted vehicle 0x1033 (pool 1 = items, slot 51).
// vehHdl ≠ 0xFFFF triggers the vehicle-LOCAL position branch (no map-origin
// add), so positions are now small near-zero deltas instead of world coords.
// Anti-cheat counters carry across the mount.
const uint8_t kFrame2053_body[] = {
	0x33, 0x10,                   // carrier_handle = 0x1033 (pool 1, slot 51)
	0x76, 0x01, 0x00, 0x00,       // posX (vehicle-local i32 16.16) = 0x00000176 ≈ 0.006
	0x66, 0x37, 0x00, 0x00,       // posY ≈ 0.216
	0x91, 0x80, 0xf8, 0xff,       // posZ ≈ -7.997 (i32 negative)
	0xd8, 0x22,                   // heading = 0x22d8 = 8920 (i16)
	0x00, 0x00,                   // pitch = 0
	0x00, 0x00,                   // anticheat_flags / move_input_byte
	0x00, 0x00, 0x00, 0x00, 0x00, // state_flags_byte / anim_defs / equipped_adm_index
	0x36, 0x00,                   // stat_byte_0 = 0x36, stat_byte_1 = 0
	0x21, 0x10, 0xce, 0x07,
	0x2d, 0x10, 0x69, 0x07,
	0x00, 0x00, 0x32, 0x07,
	0x1e, 0x10, 0x1c, 0x07,
};
static_assert(sizeof(kFrame2053_body) == 43,
              "extended body is 43 B");

int test_extended_uplink_mounted_vehicle() {
	PlayerExtendedUplink r;
	size_t consumed = 0;
	const bool ok = decode_player_extended_uplink(
		kFrame2053_body, sizeof(kFrame2053_body), r, consumed);
	EXPECT(ok);
	EXPECT(consumed == 43);
	EXPECT(r.carrier_handle == 0x1033);  // mounted — vehicle-local branch
	// Position is i32 16.16 vehicle-local. Sanity-check magnitudes only here;
	// the wire-format guarantee is "i32 LE", which the byte-exact frame-1905
	// test pins.
	EXPECT(r.heading == 8920);
	EXPECT(r.pitch == 0);
	EXPECT(r.stat_byte_0 == 0x36);
	std::printf("PASS extended_uplink mounted on vehicle 0x1033\n");
	return 0;
}

// Frame 2061 — joiner s2 fires a round. Bytes copied verbatim from the
// 3-player capture's f=2061 datagram (45 B):
// `24 00 ed 00 02 00 02 07 f0 3e 43 fe f0 d7 62 fe 23 92 0e 00 69 39 00 00
//  20 00 00 00 ff ff 02 04 03 0c 00 54 3b 59 42 b9 d8 00 00 8c 01`
const uint8_t kFiredRound_f2061[] = {
	0x24, 0x00, 0xed, 0x00,        // current_tick = 0x00ed0024 LE
	0x02, 0x00,                    // shooter_handle = 0x0002 (joiner s2)
	0x02,                          // fire_flags = 0x02
	0x07,                          // adm_index = 7
	0xf0, 0x3e, 0x43, 0xfe,        // pos_x = 0xfe433ef0
	0xf0, 0xd7, 0x62, 0xfe,        // pos_y = 0xfe62d7f0
	0x23, 0x92, 0x0e, 0x00,        // pos_z = 0x000e9223
	0x69, 0x39, 0x00, 0x00,        // dir_x = 0x00003969
	0x20, 0x00, 0x00, 0x00,        // dir_y = 0x00000020
	0xff, 0xff,                    // target_handle = 0xFFFF (no specific target)
	0x02, 0x04,                    // hit_part = 0x0402
	0x03,                          // extra_byte1
	0x0c,                          // extra_byte2
	0x00,                          // misc_byte
	0x54, 0x3b,                    // delta_x = 0x3b54
	0x59, 0x42,                    // delta_y = 0x4259
	0xb9, 0xd8,                    // delta_z = 0xd8b9
	0x00, 0x00,                    // delta_yaw = 0
	0x8c, 0x01,                    // delta_pitch = 0x018c
};
static_assert(sizeof(kFiredRound_f2061) == 45, "C2S 0x06 body is 45 B");

int test_client_fired_round() {
	ClientFiredRound r;
	size_t consumed = 0;
	const bool ok = decode_client_fired_round(
		kFiredRound_f2061, sizeof(kFiredRound_f2061), r, consumed);
	EXPECT(ok);
	EXPECT(consumed == 45);
	EXPECT(r.current_tick == 0x00ed0024u);
	EXPECT(r.shooter_handle == 0x0002);
	EXPECT(r.fire_flags == 0x02);
	EXPECT(r.adm_index == 0x07);
	EXPECT(uint32_t(r.pos_x) == 0xfe433ef0u);
	EXPECT(uint32_t(r.pos_y) == 0xfe62d7f0u);
	EXPECT(uint32_t(r.pos_z) == 0x000e9223u);
	EXPECT(uint32_t(r.dir_x) == 0x00003969u);
	EXPECT(uint32_t(r.dir_y) == 0x00000020u);
	EXPECT(r.target_handle == 0xFFFF);
	EXPECT(r.hit_part == 0x0402);
	EXPECT(r.extra_byte1 == 0x03);
	EXPECT(r.extra_byte2 == 0x0c);
	EXPECT(r.misc_byte == 0x00);
	EXPECT(r.delta_x == 0x3b54);
	EXPECT(r.delta_y == 0x4259);
	EXPECT(r.delta_z == 0xd8b9);
	EXPECT(r.delta_pitch == 0x018c);
	const std::vector<uint8_t> encoded = encode_client_fired_round(r);
	EXPECT(encoded.size() == sizeof(kFiredRound_f2061));
	EXPECT(std::memcmp(encoded.data(), kFiredRound_f2061,
	                   sizeof(kFiredRound_f2061)) == 0);
	std::printf("PASS client_fired_round\n");
	return 0;
}

int test_client_fired_round_pose_deltas() {
	ClientFiredRound r;
	const std::array<int32_t, 5> shooter_pose = {
		int32_t(0x12345678u),
		int32_t(0x89abcdefu),
		int32_t(0x00010010u),
		int32_t(0x10203040u),
		int32_t(0x7fff8000u),
	};
	// High halves deliberately differ: retail subtracts only the low words.
	// The expected words reproduce the independently witnessed f=2061
	// trailing signature from worked pose pairs.
	const std::array<int32_t, 5> fire_pose = {
		int32_t(0x777791ccu), // 0x91cc - 0x5678 = 0x3b54
		int32_t(0x22221048u), // 0x1048 - 0xcdef = 0x4259 modulo 2^16
		int32_t(0x3333d8c9u), // 0xd8c9 - 0x0010 = 0xd8b9
		int32_t(0x44443040u), // 0x3040 - 0x3040 = 0
		int32_t(0x5555818cu), // 0x818c - 0x8000 = 0x018c
	};
	set_client_fired_round_pose(r, fire_pose, shooter_pose);

	EXPECT(uint32_t(r.pos_x) == 0x777791ccu);
	EXPECT(uint32_t(r.pos_y) == 0x22221048u);
	EXPECT(uint32_t(r.pos_z) == 0x3333d8c9u);
	EXPECT(uint32_t(r.dir_x) == 0x00004444u);
	EXPECT(uint32_t(r.dir_y) == 0x00005556u);
	EXPECT(r.delta_x == 0x3b54);
	EXPECT(r.delta_y == 0x4259);
	EXPECT(r.delta_z == 0xd8b9);
	EXPECT(r.delta_yaw == 0x0000);
	EXPECT(r.delta_pitch == 0x018c);

	const std::vector<uint8_t> encoded = encode_client_fired_round(r);
	const uint8_t expected_words[] = {
		0x54, 0x3b, 0x59, 0x42, 0xb9, 0xd8, 0x00, 0x00, 0x8c, 0x01,
	};
	EXPECT(encoded.size() == 45);
	EXPECT(std::memcmp(encoded.data() + 35, expected_words,
	                   sizeof(expected_words)) == 0);
	std::printf("PASS client_fired_round retail pose deltas\n");
	return 0;
}

int test_client_fired_round_angle_rounding_boundaries() {
	ClientFiredRound r;
	const std::array<int32_t, 5> shooter_pose = {};

	set_client_fired_round_pose(r, {
			0, 0, 0,
			int32_t(0x00007fffu),
			int32_t(0x00008000u),
	}, shooter_pose);
	EXPECT(r.dir_x == 0);
	EXPECT(r.dir_y == 1);

	set_client_fired_round_pose(r, {
			0, 0, 0,
			int32_t(0xffff8000u),
			int32_t(0x7fffffffu),
	}, shooter_pose);
	EXPECT(r.dir_x == 0);
	EXPECT(r.dir_y == -32768);
	std::printf("PASS client_fired_round angle rounding boundaries\n");
	return 0;
}

// Frame 2101 — first C2S 0x21 anti-cheat reply.
// `12 29 3f a1 42 00 00 00 00` — index=0x12, expected_crc = 0x42a13f29 LE.
const uint8_t kChecksumReply_f2101[] = {
	0x12, 0x29, 0x3f, 0xa1, 0x42, 0x00, 0x00, 0x00, 0x00,
};

int test_client_checksum_reply() {
	ClientChecksumReply r;
	size_t consumed = 0;
	const bool ok = decode_client_checksum_reply(
		kChecksumReply_f2101, sizeof(kChecksumReply_f2101), r, consumed);
	EXPECT(ok);
	EXPECT(consumed == 5);
	EXPECT(r.player_index == 0x12);
	EXPECT(r.expected_crc == 0x42a13f29u);
	// 4 trailing zero bytes are not read by the handler.
	std::printf("PASS client_checksum_reply\n");
	return 0;
}

// The joiner-side encoder must reproduce the exact wire bytes the host decodes:
// decode a real capture frame, re-encode, assert byte-identity. This is the
// wire-compat bar for the C2S 0x0C uplink (the inverse-pair guarantee).
int test_extended_uplink_roundtrip() {
	size_t consumed = 0;

	// 5-byte sub-header re-encodes to the exact wire bytes.
	EntityPacketSubHeader hdr;
	EXPECT(decode_entity_packet_sub_header(
		kFrame1905_full, sizeof(kFrame1905_full), hdr, consumed));
	const std::vector<uint8_t> hb = encode_entity_packet_sub_header(hdr);
	EXPECT(hb.size() == 5);
	EXPECT(std::memcmp(hb.data(), kFrame1905_full, 5) == 0);

	// On-foot body (f=1905) round-trips byte-for-byte (world coords + map origin).
	PlayerExtendedUplink r1;
	EXPECT(decode_player_extended_uplink(
		kFrame1905_full + 5, sizeof(kFrame1905_full) - 5, r1, consumed));
	const std::vector<uint8_t> b1 = encode_player_extended_uplink(r1);
	EXPECT(b1.size() == 43);
	EXPECT(std::memcmp(b1.data(), kFrame1905_full + 5, 43) == 0);

	// Mounted body (f=2053) round-trips byte-for-byte (vehicle-local branch).
	PlayerExtendedUplink r2;
	EXPECT(decode_player_extended_uplink(
		kFrame2053_body, sizeof(kFrame2053_body), r2, consumed));
	const std::vector<uint8_t> b2 = encode_player_extended_uplink(r2);
	EXPECT(b2.size() == 43);
	EXPECT(std::memcmp(b2.data(), kFrame2053_body, 43) == 0);

	std::printf("PASS extended_uplink encode round-trip (on-foot + mounted)\n");
	return 0;
}

int test_extended_uplink_short_body() {
	// 42 B is one short of the fixed 43 B body — decoder must reject.
	uint8_t short_body[42] = {};
	PlayerExtendedUplink r;
	size_t consumed = 0;
	const bool ok = decode_player_extended_uplink(
		short_body, sizeof(short_body), r, consumed);
	EXPECT(!ok);
	std::printf("PASS extended_uplink short body rejected\n");
	return 0;
}

// C2S 0x06 hit_part is a PACKED word, not a bare sequence:
// (roster slot << 9) | (shot seq & 0x1FF) [orig: Server_ClientFiredRound @0x50bda5].
// Pinned against the two live captures taken 2026-07-26: a retail joiner at
// mySlot=1 sent 0x0201/0x0202/0x0203 for shots 1/2/3, while our client sent
// 0x0001/0x0002/0x0003 — slot bits zero, i.e. roster slot 0, which on a listen host
// is the HOST ITSELF. The host copies the raw word into the global word_B7C670
// (@0x50c2ba) and attributed our rounds to its own player, so its first-person
// weapon reacted every time the joiner fired.
static int test_fired_round_hit_part_packing() {
	// The exact bytes a retail joiner at roster slot 1 put on the wire.
	EXPECT(pack_fired_round_hit_part(1, 1) == 0x0201);
	EXPECT(pack_fired_round_hit_part(1, 2) == 0x0202);
	EXPECT(pack_fired_round_hit_part(1, 3) == 0x0203);
	// Slot 0 must stay reachable — a listen host's own player legitimately packs 0.
	EXPECT(pack_fired_round_hit_part(0, 1) == 0x0001);
	// The sequence is 9 bits and wraps inside the field without disturbing the slot.
	EXPECT(pack_fired_round_hit_part(1, 0x1FF) == 0x03FF);
	EXPECT(pack_fired_round_hit_part(1, 0x200) == 0x0200);
	EXPECT(pack_fired_round_hit_part(3, 5) == 0x0605);
	// Round-trip both fields back out.
	for (uint8_t slot = 0; slot < 8; ++slot) {
		for (uint16_t seq = 0; seq < 0x200; seq += 37) {
			const uint16_t packed = pack_fired_round_hit_part(slot, seq);
			EXPECT(fired_round_hit_part_slot(packed) == slot);
			EXPECT(fired_round_hit_part_seq(packed) == seq);
		}
	}
	std::printf("PASS fired_round hit_part packing (slot << 9 | seq)\n");
	return 0;
}

} // namespace

int main() {
	int rc = 0;
	rc |= test_sub_header_parse();
	rc |= test_extended_uplink_stationary_on_foot();
	rc |= test_extended_uplink_mounted_vehicle();
	rc |= test_extended_uplink_roundtrip();
	rc |= test_extended_uplink_short_body();
	rc |= test_client_fired_round();
	rc |= test_client_fired_round_pose_deltas();
	rc |= test_client_fired_round_angle_rounding_boundaries();
	rc |= test_client_checksum_reply();
	rc |= test_fired_round_hit_part_packing();
	if (rc == 0) std::printf("nw_ingame_c2s_uplink_test: ALL PASS\n");
	return rc;
}
