#pragma once

// The decoded values of a retail Delta Force: Xtreme 2 LAN pair's in-match bodies (the
// 2026-10-14 onHook capture: a dfx2.exe host and joiner, co-op on CP_01_CP), and the bodies our
// own encoders write from them. tests/fixtures/dfx2_wire_gen.cpp mints the fixtures from these;
// tests/novaworld/nw_dfx2_frame_test.cpp decodes the fixtures back to them. No capture byte is
// carried: each body is the encoder's output over these values.

#include <net/novacrypto/cdkey.h>
#include <net/npwire/flat_tlv.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dfx2_wire {

// The co-op host's phase-3 0x0A to its joiner (capture frame 168): anchor, flags, the four
// objective values, the checkpoint (0xFFFF: no player-route marker done yet), the recipient's
// tail and the first record, its own player.
inline constexpr int32_t kAnchor[3] = {static_cast<int32_t>(0xfe7ca9deu), 0x043e4c20, 0x00131817};
inline constexpr uint8_t kFlags1 = 0x02; // undeployed (DFX2-11)
inline constexpr uint8_t kFlags2 = 0x03; // phase 3: the objective sub-block
inline constexpr int32_t kObjective[4] = {0, 0, 2, 0};
inline constexpr uint32_t kCheckpoint = 0x0000FFFFu;
inline constexpr int16_t kHealth = 100;
inline constexpr uint16_t kPlayerHandle = 0x0070;
inline constexpr uint16_t kPlayerTypeId = 0x14b9;

inline opennova::PlayerCompactRecord capture_player_record() {
	opennova::PlayerCompactRecord r;
	r.vehicle_bone = 0;
	r.seat_type = 0;
	r.carrier_handle = 0xFFFF;
	r.yaw_byte = 0xc0;
	r.pitch_byte = 0x00;
	r.move_input_byte = 0;
	r.state_flags = 0x01;
	r.anim_state_id = 43;
	r.anim_channel_ratio = 83;
	r.anim_def_index = 1;
	r.health_class_byte = 0x28;
	return r;
}

inline opennova::FrameUpdate coop_objective_update() {
	opennova::FrameUpdate fu;
	fu.anchor_x = kAnchor[0];
	fu.anchor_y = kAnchor[1];
	fu.anchor_z = kAnchor[2];
	fu.flags1 = kFlags1;
	fu.flags2 = kFlags2;
	fu.objective.present = true;
	for (int i = 0; i < 4; ++i) fu.objective.state[i] = kObjective[i];
	fu.objective.has_coop_checkpoint = true;
	fu.objective.coop_checkpoint = kCheckpoint;
	fu.state_flag_byte = 0;
	fu.carried_handle = 0xFFFF;
	fu.health = kHealth;
	fu.state_word = 0;
	opennova::FrameUpdateRecord rec;
	rec.handle = kPlayerHandle;
	rec.type_id = kPlayerTypeId;
	rec.cls = opennova::EntityClass::Player;
	rec.player = capture_player_record();
	fu.records.push_back(rec);
	return fu;
}

inline std::vector<uint8_t> coop_objective_frame() {
	return opennova::encode_frame_update(coop_objective_update());
}

// The serial the capture's joiner presented as its CDKIID (58 characters on the wire: 17 bytes
// and three keys' CRCs), and the JOIN it sent: CDKIID, then VERSIONCRCSTRING "0".
inline const char *kJoinSerial = "ONHOOK-LAN-TEST-1";
inline constexpr std::size_t kJoinCdkiidChars = 58;

inline std::vector<uint8_t> lan_join_body() {
	std::vector<uint8_t> body;
	const auto append_string = [&body](const char *name, const std::string &value) {
		std::string with_nul = value;
		with_nul.push_back('\0');
		opennova::append_flat_tlv(body, name, reinterpret_cast<const uint8_t *>(with_nul.data()),
				static_cast<uint16_t>(with_nul.size()));
	};
	append_string("CDKIID", opennova::cdkey::make_wire_key(kJoinSerial));
	append_string("VERSIONCRCSTRING", "0");
	return body;
}

} // namespace dfx2_wire
