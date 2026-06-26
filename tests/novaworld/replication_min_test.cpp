#include <novaworld/replication_min.h>
#include <novaworld/ingame_decode.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

uint16_t le16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t le32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
			(static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool check_tag_0f_size_and_layout() {
	opennova::PlayerReplicationState ctx;
	ctx.spawn_x = 0xfe56f854u;
	ctx.spawn_y = 0x0049f5f0u;
	ctx.spawn_z = 0x003a5e6au;
	const auto buf = opennova::build_tag_0f_game_start(ctx);
	// Phase D.0.18: retail-verbatim 624-byte tag=0x0F (combined from retail
	// capture3 fragments 211497 + 211498). Includes spawn-point name list
	// that cmap.mnu needs for clickable markers. yaw/pitch/roll come from
	// retail's bytes (we don't patch those); only position is patched.
	if (!expect(buf.size() == 624, "tag=0x0F payload is 624 bytes (retail-matched)")) return false;
	// Position patched from ctx.
	if (!expect(le32(buf.data() + 4) == ctx.spawn_x, "spawn X patched")) return false;
	if (!expect(le32(buf.data() + 8) == ctx.spawn_y, "spawn Y patched")) return false;
	if (!expect(le32(buf.data() + 12) == ctx.spawn_z, "spawn Z patched")) return false;
	// Spawn count at bytes 537-538 (u16 LE = 6), names start at byte 539.
	if (!expect(le16(buf.data() + 537) == 6, "spawn_count = 6")) return false;
	const char *first_name = reinterpret_cast<const char *>(buf.data() + 539);
	if (!expect(std::strncmp(first_name, "North Sea Village", 17) == 0,
			"first spawn name is 'North Sea Village'")) return false;
	return true;
}

bool check_tag_16_player_list_layout() {
	opennova::PlayerReplicationState ctx;
	ctx.player_slot = 0;
	ctx.team = 1;
	const auto buf = opennova::build_tag_16_player_list(ctx);
	// Phase D.0.11: matches retail capture frame ~82540 = 31 bytes (was 19
	// in the original Series B minimal version, but HUD_DrawKillList
	// counted 0 players because team=0 doesn't pass the team-in {1,2,3,4}
	// gate in the per-player count loop).
	if (!expect(buf.size() == 31, "tag=0x16 1-player payload is 31 bytes (retail-matched)")) return false;
	if (!expect(buf[0] == 0x01, "byte 0 = 0x01 (matches retail dword_A823B8)")) return false;
	if (!expect(buf[1] == 1, "player_count = 1")) return false;
	if (!expect(buf[2] == ctx.player_slot, "player_id = 0")) return false;
	if (!expect(buf[9] == 0x02, "packed team = (team<<1) = 0x02 (= team 1, no spectator)")) return false;
	if (!expect(buf[10] == 0x02, "team_count = 2 (matches retail)")) return false;
	// 3 team entries × 6 bytes = 18 bytes of zeros at offsets 11..28
	if (!expect(buf[29] == 0x02, "dword_A85B3C trailer = 0x02")) return false;
	if (!expect(buf[30] == 0x00, "dword_A85B40 trailer = 0")) return false;
	return true;
}

bool check_tag_46_player_sync_layout() {
	opennova::PlayerReplicationState ctx;
	ctx.player_name = "DevUser";
	ctx.clan_tag = "";
	ctx.player_slot = 7;
	ctx.entity_handle = 0x0005;
	const auto buf = opennova::build_tag_46_player_sync(ctx);
	// Phase D.0.26.4 retail-match layout:
	//   slot(1) + flags(2) + entity(1) + "DevUser\0"(8) + "\0"(1)
	//   + "A-A02-000000\0"(13) + team(1) + squad(1) + ticket(1)
	//   + byte+48(1) + byte+49(1) + quality(1) + entity_ref(4)
	// = 1+2+1+8+1+13+1+1+1+1+1+1+4 = 36 bytes
	if (!expect(buf.size() == 36, "tag=0x46 retail-match payload is 36 bytes")) return false;
	if (!expect(buf[0] == 7, "player_slot echoed")) return false;
	if (!expect(le16(buf.data() + 1) == 0x1CF7, "flags = 0x1CF7 (retail-match)")) return false;
	if (!expect(buf[3] == 5, "entity slot comes from entity_handle, not player_slot")) return false;
	// Name region: 7 chars + NUL terminator at offset 4..11
	if (!expect(std::strncmp(reinterpret_cast<const char *>(buf.data() + 4), "DevUser", 7) == 0,
			"name 'DevUser' written")) return false;
	if (!expect(buf[11] == 0, "name NUL-terminator")) return false;
	if (!expect(buf[12] == 0, "empty clan reduces to NUL-only")) return false;
	// Label "A-A02-000000\0" at offset 13..25
	if (!expect(std::strncmp(reinterpret_cast<const char *>(buf.data() + 13), "A-A02-000000", 12) == 0,
			"label 'A-A02-000000' written")) return false;
	if (!expect(buf[25] == 0, "label NUL-terminator")) return false;
	if (!expect(buf[26] == 1, "team byte = 1 (default playable team, blue)")) return false;
	if (!expect(buf[27] == 0, "squad = 0 (no squad)")) return false;
	if (!expect(buf[28] == 0, "ticket-validated byte = 0 (retail host-slot default)")) return false;
	if (!expect(buf[29] == 0xFF, "byte+48 = 0xFF (no squad leader)")) return false;
	if (!expect(buf[30] == 0, "byte+49 = 0")) return false;
	if (!expect(buf[31] == 1, "quality = 1")) return false;
	if (!expect(le32(buf.data() + 32) == 0u, "entity_ref = 0 (null pointer)")) return false;
	return true;
}

bool check_tag_10_empty_layout() {
	const auto buf = opennova::build_tag_10_entity_batch_empty();
	if (!expect(buf.size() == 4, "tag=0x10 empty batch is 4 bytes")) return false;
	if (!expect(le16(buf.data() + 0) == 0, "starting_slot = 0")) return false;
	if (!expect(le16(buf.data() + 2) == 0, "count = 0")) return false;
	return true;
}

bool check_tag_10_entity_batch_layout() {
	std::vector<opennova::GameEntitySnapshot> entities = {
		{2, 3, 0x1111, 0x0001, 1, 100, 200, 300},
		{2, 4, 0x2222, 0x0002, 1, -10, -20, -30},
		{2, 7, 0x3333, 0x0003, 2, 400, 500, 600},
	};
	const auto batch = opennova::build_tag_10_entity_batch(entities, 0, 36);
	if (!expect(batch.payload.size() == 36, "tag=0x10 two-record batch is 36 bytes")) return false;
	if (!expect(batch.entity_count == 2, "entity_count = 2")) return false;
	if (!expect(batch.next_cursor == 2, "next cursor advances by two records")) return false;
	if (!expect(le16(batch.payload.data() + 0) == 3, "starting slot = first entity slot")) return false;
	if (!expect(le16(batch.payload.data() + 2) == 2, "count = 2")) return false;
	if (!expect(le16(batch.payload.data() + 4) == 0x1111, "record 0 type_id")) return false;
	if (!expect(le16(batch.payload.data() + 6) == 0x0001, "record 0 flags")) return false;
	if (!expect(le32(batch.payload.data() + 8) == 100, "record 0 x")) return false;
	if (!expect(le32(batch.payload.data() + 12) == 200, "record 0 y")) return false;
	if (!expect(le32(batch.payload.data() + 16) == 300, "record 0 z")) return false;
	if (!expect(le16(batch.payload.data() + 20) == 0x2222, "record 1 type_id")) return false;
	const auto wrapped = opennova::build_tag_10_entity_batch(entities, 2, 20);
	if (!expect(wrapped.payload.size() == 20, "tag=0x10 one-record batch is 20 bytes")) return false;
	if (!expect(wrapped.next_cursor == 0, "cursor wraps after last entity")) return false;
	if (!expect(le16(wrapped.payload.data() + 0) == 7, "wrapped start slot = last entity slot")) return false;
	if (!expect(le16(wrapped.payload.data() + 2) == 1, "wrapped count = 1")) return false;
	return true;
}

bool check_tag_57_rtt_layout() {
	const auto buf = opennova::build_tag_57_rtt_request(0xDEADBEEFu);
	if (!expect(buf.size() == 5, "tag=0x57 RTT request is 5 bytes")) return false;
	if (!expect(le32(buf.data() + 0) == 0xDEADBEEFu, "tick echoed")) return false;
	if (!expect(buf[4] == 1, "echo_request flag = 1")) return false;
	return true;
}

bool check_tag_2a_chat_layout() {
	const auto buf = opennova::build_tag_2a_chat_history(0x11111111u, 0x22222222u, 0x3333);
	if (!expect(buf.size() == 10, "tag=0x2A chat is exactly 10 bytes")) return false;
	if (!expect(le32(buf.data() + 0) == 0x11111111u, "field a echoed")) return false;
	if (!expect(le32(buf.data() + 4) == 0x22222222u, "field b echoed")) return false;
	if (!expect(le16(buf.data() + 8) == 0x3333, "field c echoed")) return false;
	return true;
}

bool check_tag_0a_world_reference_layout() {
	// D-NET-50: the field-driven §5.9 0x0A frame replaces the verbatim retail blob.
	// The anchor (first 12 bytes) is the subject world position; one replicated
	// entity becomes a tag=1 compact record; the whole frame must decode cleanly.
	opennova::PlayerReplicationState ctx;
	ctx.spawn_x = 0x01020304u;
	ctx.spawn_y = 0x11121314u;
	ctx.spawn_z = 0x21222324u;
	std::vector<opennova::GameEntitySnapshot> ents;
	opennova::GameEntitySnapshot e;
	e.pool = 0;
	e.slot = 5;
	e.type_id = 0x14B9;
	e.entity_class = opennova::EntityClass::Infantry;
	e.x = 0x01020404; // small +0x100 delta from the anchor X
	e.y = 0x11121314;
	e.z = 0x21222324;
	ents.push_back(e);

	const auto buf = opennova::build_tag_0a_world_reference(ctx, ents);
	if (!expect(le32(buf.data() + 0) == ctx.spawn_x, "world reference X = anchor")) return false;
	if (!expect(le32(buf.data() + 4) == ctx.spawn_y, "world reference Y = anchor")) return false;
	if (!expect(le32(buf.data() + 8) == ctx.spawn_z, "world reference Z = anchor")) return false;

	auto class_of = [](uint16_t t) {
		return t == 0x14B9 ? opennova::EntityClass::Infantry : opennova::EntityClass::Unknown;
	};
	opennova::FrameUpdate fu;
	if (!expect(opennova::decode_frame_update(buf.data(), buf.size(), class_of, fu),
	            "field-driven 0x0A decodes")) return false;
	if (!expect(fu.complete && fu.consumed == buf.size(), "0x0A consumed exactly")) return false;
	if (!expect(fu.records.size() == 1 &&
	            fu.records[0].cls == opennova::EntityClass::Infantry &&
	            fu.records[0].handle == 5,
	            "replicated entity present as infantry compact")) return false;
	return true;
}

bool check_tag_5a_weapon_loadout_bytes() {
	// Lock the byte sequence to retail capture frame 82537 — any drift here
	// means the loadout we send won't validate against ItemList type-id
	// checks and dword_81474C won't be cleared (i.e., player can't move).
	static constexpr uint8_t kExpected[34] = {
		0x08, 0x02, 0xff, 0xff, 0x00, 0x03, 0x05, 0xff, 0x00, 0x13,
		0x0a, 0xff, 0x00, 0x33, 0x03, 0xff, 0x00, 0x34, 0x03, 0xff,
		0x00, 0x35, 0x03, 0xff, 0x00, 0x37, 0x02, 0xff, 0xff, 0x38,
		0xff, 0xff, 0xff, 0xff
	};
	const auto buf = opennova::build_tag_5a_weapon_loadout();
	if (!expect(buf.size() == 34, "tag=0x5A is 34 bytes (matches retail frame 82537)")) return false;
	for (size_t i = 0; i < 34; ++i) {
		if (buf[i] != kExpected[i]) {
			std::fprintf(stderr, "FAIL: tag=0x5A byte[%zu] = 0x%02X, expected 0x%02X\n",
					i, buf[i], kExpected[i]);
			return false;
		}
	}
	if (!expect(buf[0] == 0x08, "first byte is class_index=8")) return false;
	if (!expect(buf[1] == 0x02, "second byte is first_weapon_id=2 (knife)")) return false;
	if (!expect(buf[33] == 0xff, "trailing terminator byte")) return false;
	return true;
}

bool check_tag_1e_game_event_post_spawn_layout() {
	const auto buf = opennova::build_tag_1e_game_event_post_spawn();
	if (!expect(buf.size() == 8, "tag=0x1E payload is exactly 8 bytes")) return false;
	// Verbatim retail capture frame ~82540 — see net_verification_log.md D.0.7.
	static constexpr uint8_t kExpected[8] = {0x3a, 0x04, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00};
	for (size_t i = 0; i < 8; ++i) {
		if (buf[i] != kExpected[i]) {
			std::fprintf(stderr, "FAIL: tag=0x1E byte[%zu] = 0x%02X, expected 0x%02X\n",
					i, buf[i], kExpected[i]);
			return false;
		}
	}
	if (!expect(buf[0] == 0x3a, "event_type = 0x3A")) return false;
	if (!expect(buf[1] == 0x04, "attacker_pool_idx = 4")) return false;
	if (!expect(buf[2] == 0xff && buf[3] == 0xff, "victim/aux pool = 0xFF (N/A)")) return false;
	return true;
}

bool check_tag_0d_local_player_spawn_layout() {
	opennova::PlayerReplicationState ctx;
	ctx.player_name = "DevUser";
	ctx.player_slot = 0;
	ctx.entity_handle = 0x0005;
	ctx.team = 1;
	ctx.entity_type_id = 0x14B9u;
	ctx.spawn_x = 0xfe56f854u;
	ctx.spawn_y = 0x0049f5f0u;
	ctx.spawn_z = 0x003a5e6au;
	const auto buf = opennova::build_tag_0d_local_player_spawn(ctx);
	// Layout: 2 (count) + 2 (flags) + 2 (slot_id) + 2 (type_id) + 8 ("DevUser\0")
	//       + 4 (entity36) + 4x3 (xyz) + 1 (team) + 1 (bone)
	//       = 2+2+2+2+8+4+12+1+1 = 34 bytes
	if (!expect(buf.size() == 34, "tag=0x0D 1-player payload is 34 bytes")) return false;
	if (!expect(le16(buf.data() + 0) == 1, "count = 1")) return false;
	if (!expect(le16(buf.data() + 2) == 0x0030, "flags = team|entity36 (0x0030)")) return false;
	if (!expect(le16(buf.data() + 4) == 0x0005, "slot_id = entity_handle 0x0005")) return false;
	if (!expect(le16(buf.data() + 6) == 0x14B9, "type_id = 0x14B9 default infantry")) return false;
	if (!expect(std::strncmp(reinterpret_cast<const char *>(buf.data() + 8), "DevUser", 7) == 0,
			"name 'DevUser' written")) return false;
	if (!expect(buf[15] == 0, "name NUL-terminator at offset 15")) return false;
	// entity36 = 0 (the load-bearing zero — clears bit 1 movement-gate)
	if (!expect(le32(buf.data() + 16) == 0, "entity36 = 0 (clears movement gate)")) return false;
	// Spawn coords echoed
	if (!expect(le32(buf.data() + 20) == ctx.spawn_x, "pos_x echoed")) return false;
	if (!expect(le32(buf.data() + 24) == ctx.spawn_y, "pos_y echoed")) return false;
	if (!expect(le32(buf.data() + 28) == ctx.spawn_z, "pos_z echoed")) return false;
	if (!expect(buf[32] == 1, "team byte = 1")) return false;
	if (!expect(buf[33] == 0, "bone_attach = 0")) return false;
	return true;
}

bool check_tag_51_player_spawn_uses_entity_handle() {
	opennova::PlayerReplicationState ctx;
	ctx.player_slot = 0;
	ctx.entity_handle = 0x0005;
	ctx.team = 1;
	const auto buf = opennova::build_tag_51_player_spawn(ctx);
	if (!expect(buf.size() == 8, "tag=0x51 payload is 8 bytes")) return false;
	if (!expect(le16(buf.data() + 0) == 1, "team_u16 = 1")) return false;
	if (!expect(le16(buf.data() + 2) == 0x0005,
			"tag=0x51 entity slot comes from entity_handle, not player_slot")) return false;
	if (!expect(buf[4] == 1, "team byte = 1")) return false;
	if (!expect(le16(buf.data() + 5) == 0, "weapon index = 0")) return false;
	if (!expect(buf[7] == 0, "camera byte = 0")) return false;
	return true;
}

bool check_long_name_truncation() {
	opennova::PlayerReplicationState ctx;
	ctx.player_name = std::string(50, 'A'); // longer than the 32-char field
	const auto buf = opennova::build_tag_46_player_sync(ctx);
	// Walk past slot(1)+flags(2)+entity(1) to find the NUL.
	bool found_nul_within_bound = false;
	for (size_t i = 4; i < buf.size() && i < 4 + 32; ++i) {
		if (buf[i] == 0) { found_nul_within_bound = true; break; }
	}
	if (!expect(found_nul_within_bound, "long name truncated within 32-byte field")) return false;
	return true;
}

bool check_tag_40_capture_zone_state_layout() {
	// Phase D.0.26.8 retail-match: for ASH_I5A.bms dvxi5 4-spawn catalog
	// (pool=1 slots 49-52, teams 0/0/1/2), our builder must emit retail's
	// exact per-team stride bytes.
	std::vector<opennova::GameEntitySnapshot> points = {
		{2, 49, 1359, 0, 0, 10699725, 312269, 3011838},
		{2, 50, 1359, 0, 0, -20319450, -4849379, 4066280},
		{2, 51, 1359, 0, 1, -29158728, -27103112, 1409790},
		{2, 52, 1359, 0, 2, 22161130, 24329628, 1744999},
	};
	const auto buf = opennova::build_tag_40_capture_zone_state(points);
	// Expect 25 bytes = 1 (count) + 4 records × 6
	if (!expect(buf.size() == 25, "tag=0x40 for 4 entities is 25 bytes")) return false;
	static constexpr uint8_t kExpected[25] = {
		0x04,
		0x31, 0x20, 0x00, 0x0c, 0x10, 0xc2,  // pool 2 slot 49, team=0
		0x32, 0x20, 0x00, 0x0c, 0x10, 0xc2,  // pool 2 slot 50, team=0
		0x33, 0x20, 0x00, 0x0a, 0x10, 0x01,  // pool 2 slot 51, team=1
		0x34, 0x20, 0x00, 0x09, 0x10, 0x03,  // pool 2 slot 52, team=2
	};
	for (size_t i = 0; i < 25; ++i) {
		if (buf[i] != kExpected[i]) {
			std::fprintf(stderr, "FAIL: tag=0x40 byte[%zu] = 0x%02X, expected 0x%02X\n",
					i, buf[i], kExpected[i]);
			return false;
		}
	}
	return true;
}

bool check_tag_0d_spawn_points_decode_alignment() {
	std::vector<opennova::SpawnPointEntity> points = {
		{1, 49, 1359, 1, 10699725, 312269, 3011838},
		{1, 50, 1359, 2, -20319450, -4849379, 4066280},
	};
	const auto buf = opennova::build_tag_0d_spawn_points(points);
	opennova::PoolSpawnBatch decoded;
	if (!expect(opennova::decode_pool_spawn_batch(buf.data(), buf.size(), decoded),
			"tag=0x0D spawn-point batch decodes without leftover/misalignment")) return false;
	if (!expect(decoded.records.size() == 2, "two spawn-point records decoded")) return false;
	if (!expect(decoded.records[0].spawn_flags == 0x0030, "record 0 flags = team|entity36")) return false;
	if (!expect(decoded.records[0].slot_id == 0x1031, "record 0 pool/slot")) return false;
	if (!expect(decoded.records[0].team_byte == 1, "record 0 team")) return false;
	if (!expect(decoded.records[0].bone_byte == 0, "record 0 bone byte")) return false;
	if (!expect(decoded.records[1].slot_id == 0x1032, "record 1 stays aligned")) return false;
	if (!expect(decoded.records[1].team_byte == 2, "record 1 team")) return false;
	return true;
}

// D-NET-86: the engine heading (entity+16) threads into the 0x0A compact yaw fields.
// Player/Infantry carry the rounded high byte (v+0x800000)>>24; Vehicle the rounded
// high i16 (v+0x8000)>>16. The 0x10 batch is deliberately NOT touched (no orientation
// on the wire there) — covered by check_tag_10_entity_batch_layout staying green.
bool check_tag_0a_euler_z_threading() {
	opennova::PlayerReplicationState ctx;
	ctx.spawn_x = 0x01020304u;
	ctx.spawn_y = 0x11121314u;
	ctx.spawn_z = 0x21222324u;

	const int32_t euler = 0x12345678;
	const uint8_t want_byte = static_cast<uint8_t>(
			(static_cast<uint32_t>(euler) + 0x00800000u) >> 24); // 0x12
	const int16_t want_i16 = static_cast<int16_t>(
			(static_cast<uint32_t>(euler) + 0x00008000u) >> 16); // 0x1234

	opennova::GameEntitySnapshot inf;
	inf.pool = 0; inf.slot = 5; inf.type_id = 0x2000;
	inf.entity_class = opennova::EntityClass::Infantry;
	inf.x = ctx.spawn_x; inf.y = ctx.spawn_y; inf.z = ctx.spawn_z;
	inf.euler_z = euler;

	opennova::GameEntitySnapshot veh = inf;
	veh.slot = 6; veh.type_id = 0x3000;
	veh.entity_class = opennova::EntityClass::Vehicle;

	const auto buf = opennova::build_tag_0a_world_reference(ctx, {inf, veh});
	auto class_of = [](uint16_t t) {
		if (t == 0x2000) return opennova::EntityClass::Infantry;
		if (t == 0x3000) return opennova::EntityClass::Vehicle;
		return opennova::EntityClass::Unknown;
	};
	opennova::FrameUpdate fu;
	if (!expect(opennova::decode_frame_update(buf.data(), buf.size(), class_of, fu) &&
	            fu.complete, "0x0A with euler_z decodes cleanly")) return false;
	if (!expect(fu.records.size() == 2, "two compact records")) return false;
	if (!expect(fu.records[0].cls == opennova::EntityClass::Infantry &&
	            fu.records[0].infantry.yaw_byte == want_byte,
	            "infantry yaw_byte = (euler+0x800000)>>24")) return false;
	if (!expect(fu.records[1].cls == opennova::EntityClass::Vehicle &&
	            fu.records[1].vehicle.euler_z == want_i16,
	            "vehicle euler_z = (euler+0x8000)>>16")) return false;
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = check_tag_0f_size_and_layout() && ok;
	ok = check_tag_16_player_list_layout() && ok;
	ok = check_tag_46_player_sync_layout() && ok;
	ok = check_tag_10_empty_layout() && ok;
	ok = check_tag_10_entity_batch_layout() && ok;
	ok = check_tag_57_rtt_layout() && ok;
	ok = check_tag_2a_chat_layout() && ok;
	ok = check_tag_0a_world_reference_layout() && ok;
	ok = check_tag_0a_euler_z_threading() && ok;
	ok = check_tag_5a_weapon_loadout_bytes() && ok;
	ok = check_tag_0d_local_player_spawn_layout() && ok;
	ok = check_tag_51_player_spawn_uses_entity_handle() && ok;
	ok = check_tag_1e_game_event_post_spawn_layout() && ok;
	ok = check_long_name_truncation() && ok;
	ok = check_tag_40_capture_zone_state_layout() && ok;
	ok = check_tag_0d_spawn_points_decode_alignment() && ok;
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
