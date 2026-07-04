// Codec identity vectors — the NET-0 tier-1 byte-stability gate
// (docs/maturity-program.md). Pins the EXACT WIRE BYTES of every in-match
// encoder against committed FNV-1a64 vectors over a deterministic synthetic
// corpus (our bytes only — no retail material, so it runs unconditionally in
// default CI, unlike the env-gated retail golden diff).
//
// Why this exists when nw_ingame_encode_test already round-trips: a SYMMETRIC
// codec change (encoder and decoder edited together) keeps encode->decode
// field-identity green while silently changing the bytes on the wire — which
// is exactly the failure retail interop cannot survive, and exactly what a
// refactor that moves these files (NET-2) must prove it did not do.
//
// Updating a vector is a WIRE-FORMAT CHANGE and needs a wire witness (the
// [orig] citation on the encoder, or a D-NET divergence entry) in the same
// commit — never update a hash to make a refactor pass. To regenerate after
// a witnessed change: NW_CODEC_DUMP=1 ./nw_codec_identity_test prints the
// replacement table.
//
// Inputs mirror nw_ingame_encode_test's proven constructions (all-flags +
// minimal per batch codec, both vehicle poses, every frame-update sub-block),
// so the vectors cover the same branch surface the round-trip suite does.

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova;

namespace {

uint64_t fnv1a64(const std::vector<uint8_t> &bytes) {
	uint64_t h = 1469598103934665603ull;
	for (uint8_t b : bytes) {
		h ^= b;
		h *= 1099511628211ull;
	}
	return h;
}

struct Vector {
	const char *name;
	size_t      len;
	uint64_t    hash;
};

struct Case {
	std::string          name;
	std::vector<uint8_t> bytes;
};

// ---- corpus builders (constructions lifted from nw_ingame_encode_test) ----

std::vector<Case> build_corpus() {
	std::vector<Case> corpus;
	auto add = [&corpus](const char *name, std::vector<uint8_t> bytes) {
		corpus.push_back({name, std::move(bytes)});
	};

	// network_compress_fixedpoint sweep, packed LE as a byte stream.
	{
		const int32_t vals[] = {
			0, -1, 1, -5, 0x20, 0x200, 0x1000, 0x2000, 0x4000, 0x1234,
			-0x1234, 0x12345, -0x12345, 0x100000, -0x100000, 0x3FFFFFF,
			-0x3FFFFFF, 0x7FFFFFFF, int32_t(0x80000001u),
		};
		std::vector<uint8_t> bytes;
		for (int32_t v : vals) {
			const uint16_t c = network_compress_fixedpoint(v);
			bytes.push_back(uint8_t(c & 0xFF));
			bytes.push_back(uint8_t(c >> 8));
		}
		add("compress_fixedpoint_sweep", std::move(bytes));
	}

	// S2C 0x20 pool-3 sync — all conditional fields set (flags 0x3F).
	{
		Pool3SyncBatch in;
		in.start_index = 5;
		Pool3SyncRecord r;
		r.item_type_id = 0x044A;
		r.pos_x = int32_t(0x11223344);
		r.pos_y = int32_t(0x55667788);
		r.pos_z = int32_t(0x003A5E6A);
		r.movement_val = 0xAABBCCDD;
		r.orientation_val = 0x0F0E0D0C;
		r.ammo_count = 0x0102;
		r.net_handle = 0x108B;
		r.team_byte = 0x02;
		r.weapon_type = 0x1357;
		r.score_byte = 0x09;
		in.records.push_back(r);
		add("pool3_sync_all_flags", encode_pool3_sync_batch(in));
	}

	// S2C 0x20 — sentinel mixed between two sparse records.
	{
		Pool3SyncBatch in;
		in.start_index = 100;
		Pool3SyncRecord a; a.item_type_id = 0x0010; a.pos_x = 7; a.net_handle = 0x11;
		Pool3SyncRecord empty; empty.is_empty_slot = true;
		Pool3SyncRecord b; b.item_type_id = 0x0020; b.pos_z = 9; b.net_handle = 0x22;
		in.records = {a, empty, b};
		add("pool3_sync_mixed_sentinel", encode_pool3_sync_batch(in));
	}

	// S2C 0x10 static batch — every gated field (field_flags 0x01FF + attach).
	{
		StaticEntityBatch in;
		in.start_index = 9;
		StaticEntityRecord r;
		r.item_type_id = 0x0808;
		r.pos_x = int32_t(0x11223344);
		r.pos_y = int32_t(0x55667788);
		r.pos_z = int32_t(0x00ABCDEF);
		r.euler_z = int32_t(0x0A0B0C0D);
		r.euler_x = int32_t(0x01020304);
		r.euler_y = int32_t(0x05060708);
		r.section_mask = int32_t(0x40);
		r.team_byte = 2;
		r.entity_flags = 0x04020400u;
		r.ammo_count = 30;
		r.bone_a = 5;
		r.bone_b = 6;
		r.score_flag = 1;
		r.weapon_byte = 3;
		r.attach_ref = 0x7788;
		in.records.push_back(r);
		add("static_batch_all_flags", encode_static_entity_batch(in));
	}

	// S2C 0x10 — building-shaped minimal record (27 B).
	{
		StaticEntityBatch in;
		StaticEntityRecord r;
		r.item_type_id = 0x044A;
		r.pos_x = 1; r.pos_y = -2; r.pos_z = 3;
		r.euler_z = int32_t(0x0B60B60);
		r.team_byte = 1;
		in.records.push_back(r);
		add("static_batch_minimal", encode_static_entity_batch(in));
	}

	// S2C 0x0D pool spawn — the full-flag record.
	{
		PoolSpawnBatch in;
		PoolSpawnRecord r;
		r.slot_id = 0x108B;
		r.item_type_id = 0x04BF;
		r.entity_name = "tango";
		r.entity_flags = 2;
		r.pos_x = int32_t(0x05b0be8f);
		r.pos_y = int32_t(0xfa47b504);
		r.pos_z = int32_t(0x001fbcd7);
		r.euler_z = 10;
		r.section_mask = 0x40;
		r.team_byte = 1;
		r.parent_handle = 0x1033;
		r.target_handle = 0x2044;
		r.weapon_mask = 0x05;
		r.weapon_handles[0] = 0x3001;
		r.weapon_handles[2] = 0x3002;
		r.extra_handle_0 = 0x4001;
		r.extra_handle_1 = 0x4002;
		r.bone_byte = 2;
		r.ai_profile_1 = 0x11112222;
		r.ai_profile_2 = 0x33334444;
		r.ai_name = "patrol_a";
		r.alert_byte = 7;
		r.action_byte = 9;
		r.weapon_type_byte = 3;
		r.zone_number_rank = 80;
		r.zone_radius = 1000;
		r.difficulty_byte = 4;
		in.records.push_back(r);
		add("pool_spawn_full", encode_pool_spawn_batch(in));
	}

	// S2C 0x0D — minimal record (22 B).
	{
		PoolSpawnBatch in;
		PoolSpawnRecord r;
		r.slot_id = 0x1002;
		r.item_type_id = 0x044A;
		r.pos_x = 1; r.pos_y = 2; r.pos_z = 3;
		r.bone_byte = 1;
		in.records.push_back(r);
		add("pool_spawn_minimal", encode_pool_spawn_batch(in));
	}

	// §5.14 infantry compact (14 B fixed).
	{
		InfantryCompactRecord r;
		r.seat_bone_idx = 3;
		r.vehicle_slot_handle = 0x108B;
		r.pos_x_compressed = 0xBE8F;
		r.pos_y_compressed = 0xB504;
		r.pos_z_compressed = 0xBCD7;
		r.yaw_byte = 0x40;
		r.flags_byte = 0x06;
		r.pitch_byte = 0x12;
		r.aim_yaw_byte = 0x80;
		r.anim_byte = 0x05;
		add("infantry_compact", encode_infantry_compact_record(r));
	}

	// §5.13 vehicle compact — mounted (15 B) and unmounted (21 B).
	{
		VehicleCompactRecord r;
		r.parent_slot_handle = 0x1033;
		r.pos_x_compressed = 0x1111;
		r.pos_y_compressed = 0x2222;
		r.pos_z_compressed = 0x3333;
		r.euler_z = int16_t(0x4444);
		r.flags_byte = 0x04;
		r.euler_y = int16_t(0x5555);
		r.euler_x = int16_t(0x6666);
		add("vehicle_compact_mounted", encode_vehicle_compact_record(r));
	}
	{
		VehicleCompactRecord r;
		r.parent_slot_handle = 0xFFFF;
		r.pos_x_compressed = 0xAAAA;
		r.pos_y_compressed = 0xBBBB;
		r.pos_z_compressed = 0xCCCC;
		r.euler_z = int16_t(0x0DDD);
		r.flags_byte = 0x00;
		r.weapon_x = 0x0101;
		r.health_word = 0x0202;
		r.weapon_aim_y = 0x0303;
		r.weapon_aim_z = 0x0404;
		r.weapon_heading_bam = int16_t(0x0505);
		add("vehicle_compact_unmounted", encode_vehicle_compact_record(r));
	}

	// §5.10 player compact (18 B fixed).
	{
		PlayerCompactRecord r;
		r.vehicle_bone = 1;
		r.seat_type = 2;
		r.carrier_handle = 0xFFFF;
		r.pos_x_compressed = 0xBE8F;
		r.pos_y_compressed = 0xB504;
		r.pos_z_compressed = 0xBCD7;
		r.yaw_byte = 0x40;
		r.pitch_byte = 0x10;
		r.move_input_byte = 0x12;
		r.state_flags = 0x06;
		r.anim_state_id = 0x05;
		r.anim_channel_ratio = 0x10;
		r.anim_def_index = 0x07;
		r.health_class_byte = 0x80;
		add("player_compact", encode_player_compact_record(r));
	}

	// §5.58 weapon reload (4 B).
	{
		WeaponReload r;
		r.entity_handle = 0x1005;
		r.reload_param = 0x00C3;
		add("weapon_reload", encode_weapon_reload(r));
	}

	// §5.9.1 round event — both optional fields present (flags 0xC0 gates).
	{
		RoundEventRecord r;
		r.flags = 0xC2;
		r.adm_index = 9;
		r.subtype = 3;
		r.slot_byte = 4;
		r.shooter_handle = 0x100A;
		r.target_handle = 0x2005;
		r.shot_seq = 0x0102;
		r.pos_x_compressed = 0x0011;
		r.pos_y_compressed = 0x0022;
		r.pos_z_compressed = 0x0033;
		add("round_event_full", encode_round_event_record(r));
	}

	// §5.9 frame updates: aim sub-block + 3 record classes + hit; env; passenger.
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
		add("frame_update_aim_records_hit", encode_frame_update(in));
	}
	{
		FrameUpdate in;
		in.flags2 = 0x02;
		in.env.fog_dist = 0x1111; in.env.fog_accel = 0x2222; in.env.tod_fixed = 0x3333;
		in.env.quake_ticks = 0x44; in.env.env_param = 0x55;
		in.mount_handle = 0xFFFF; in.health = 50;
		add("frame_update_env", encode_frame_update(in));
	}
	{
		FrameUpdate in;
		in.flags2 = 0x08;
		in.mount_handle = 0x1005; in.health = 75;
		in.passenger.handle = 0x1006;
		in.passenger.seat_yaw = 0xAA11; in.passenger.seat_pitch = 0xBB22;
		add("frame_update_passenger", encode_frame_update(in));
	}

	// §5.37 terrain-tile load — header chunk (56 B) + continuation (28 B).
	{
		TerrainLoadBatch in{};
		in.has_header = true;
		in.end_index = 3;
		in.magic = 0x74696C30u;
		in.tile_count = 7;
		in.header_field2 = 0x11223344u;
		in.header_field3 = 0xAABBCCDDu;
		in.tiles = { {1, 2, 3}, {4, 5, 6}, {7, 8, 9} };
		add("terrain_load_header_chunk", encode_terrain_load_batch(in));
	}
	{
		TerrainLoadBatch in{};
		in.has_header = false;
		in.start_index = 3;
		in.end_index = 5;
		in.tiles = { {0xDEADBEEFu, 0, 1}, {2, 3, 0xFEEDFACEu} };
		add("terrain_load_continuation", encode_terrain_load_batch(in));
	}

	// §5.23 organic spawn batch — default-empty batch pins the header shape.
	{
		OrganicSpawnBatch in{};
		add("organic_spawn_empty", encode_organic_spawn_batch(in));
	}

	// §5.46 full-entity-spawn — player layout, seat block, empty slot.
	{
		FullEntitySpawnRecord in{};
		in.slot_id = 0x0000;
		in.item_type_id = 0x14B9;
		in.item_type = 3;
		in.team = 1;
		in.minimap_flags = 0x0100;
		in.entity_flags = 0x0C;
		in.entity_name = "Player";
		in.pos_x = 0x1234;
		in.pos_y = 0x5678;
		in.pos_z = 0x0C50;
		in.heading_hi = 0x005A;
		in.net_id = 0x0200;
		in.player_class = 8;
		in.skip_byte = 0xEE; // encoder writes the original's hard 0
		add("full_entity_spawn_player", encode_full_entity_spawn(in));
	}
	{
		FullEntitySpawnRecord in{};
		in.slot_id = 0x1002;
		in.item_type_id = 0x050B;
		in.item_type = 1;
		in.seat_mask = 0x05;
		in.mount_handles[0] = 0x0001;
		in.mount_handles[2] = 0xFFFF;
		in.mount_handles[3] = 0x0BAD; // masked out — must not hit the wire
		add("full_entity_spawn_seats", encode_full_entity_spawn(in));
	}
	{
		FullEntitySpawnRecord in{};
		in.slot_id = 0x0003;
		add("full_entity_spawn_empty_slot", encode_full_entity_spawn(in));
	}

	// §5.15 guided groups — full TargetTypePos and delta TargetPos.
	{
		GuidedRecord r;
		r.target_slot = 0x1044;
		r.weapon_type = 0x0007;
		r.pos_x = 0x01020304; r.pos_y = 0x05060708; r.pos_z = 0x090A0B0C;
		r.attach_x = 0x11; r.attach_y = 0x22; r.attach_z = 0x33;
		add("guided_full_target_type_pos",
		    encode_guided_field_group(GuidedMode::WriteFull, GuidedFieldGroup::TargetTypePos, r));
		add("guided_delta_target_pos",
		    encode_guided_field_group(GuidedMode::WriteDelta, GuidedFieldGroup::TargetPos, r));
		add("guided_attach_offsets",
		    encode_guided_field_group(GuidedMode::WriteFull, GuidedFieldGroup::AttachOffsets, r));
	}

	// C2S 0x0C sub-header (5 B) + §5.10 extended uplink (43 B fixed).
	{
		EntityPacketSubHeader h;
		h.handle = 0x1005;
		h.item_type_id = 0x14B9;
		h.sub_op = 0x0A;
		add("entity_packet_sub_header", encode_entity_packet_sub_header(h));
	}
	{
		PlayerExtendedUplink r;
		r.carrier_handle = 0xFFFF;
		r.pos_x = 0x00112233;
		r.pos_y = int32_t(0xFFAABBCC);
		r.pos_z = 0x0044EE55;
		r.heading = int16_t(0x1234);
		r.pitch = int16_t(0xFEDC);
		r.anticheat_flags = 0x05;
		r.move_input_byte = 0x12;
		r.state_flags_byte = 0x1C;
		add("player_extended_uplink", encode_player_extended_uplink(r));
	}

	// §5.1 reply encoders: player sync (default 0x1CF7 mask), removal, list.
	{
		PlayerReplicationState rep;
		rep.player_slot = 3;
		rep.entity_handle = 0x0042;
		rep.player_name = "SocketJoiner";
		rep.clan_tag = "OPN";
		rep.team = 2;
		add("player_sync_default_mask", encode_player_sync(rep));
		add("player_sync_removal", encode_player_sync_removal(7, true));
	}
	{
		std::vector<PlayerListEntry> players = {{0, 1}, {1, 2}, {2, 2}};
		add("player_list", encode_player_list(players));
	}

	return corpus;
}

// ---- committed vectors (regenerate ONLY with a wire witness; see header) ----

const Vector kExpected[] = {
	{"compress_fixedpoint_sweep", 38u, 0x8ddd62dd9223ca35ull},
	{"pool3_sync_all_flags", 35u, 0xbf361d2ff94e2be9ull},
	{"pool3_sync_mixed_sentinel", 40u, 0x359fdefb532fe0f5ull},
	{"static_batch_all_flags", 48u, 0x9cb861f910a19b30ull},
	{"static_batch_minimal", 27u, 0x5246d32b476fc9c8ull},
	{"pool_spawn_full", 77u, 0xff8d1914d6b0f038ull},
	{"pool_spawn_minimal", 22u, 0xe11b7d2d4c4d22e1ull},
	{"infantry_compact", 14u, 0x3c26dcbb4e80914dull},
	{"vehicle_compact_mounted", 15u, 0xad1270cc190986a6ull},
	{"vehicle_compact_unmounted", 21u, 0x56c7130b52ce38a5ull},
	{"player_compact", 18u, 0xbafd9f635ff779ddull},
	{"weapon_reload", 4u, 0x8782b4b93b79634dull},
	{"round_event_full", 20u, 0xd2e48e24792ddda7ull},
	{"frame_update_aim_records_hit", 119u, 0x349686e3ae154f3dull},
	{"frame_update_env", 33u, 0x1973cd328c07f6f4ull},
	{"frame_update_passenger", 39u, 0x85c11fb07e2c99c1ull},
	{"terrain_load_header_chunk", 56u, 0x6e19dba1c492390dull},
	{"terrain_load_continuation", 28u, 0x3895c6c422e05dc0ull},
	{"organic_spawn_empty", 2u, 0x9a691300c548b8fbull},
	{"full_entity_spawn_player", 55u, 0x0d410d0016bd084eull},
	{"full_entity_spawn_seats", 53u, 0x0c8e8c01d76bfe76ull},
	{"full_entity_spawn_empty_slot", 49u, 0x5e227b21a4075162ull},
	{"guided_full_target_type_pos", 18u, 0xde6ce04b9e009304ull},
	{"guided_delta_target_pos", 2u, 0x9b503b00c60d2b0full},
	{"guided_attach_offsets", 12u, 0xe468f48f6e088e53ull},
	{"entity_packet_sub_header", 5u, 0xdd69bfc265f5ae43ull},
	{"player_extended_uplink", 43u, 0x6d73bfd35efb4ac5ull},
	{"player_sync_default_mask", 32u, 0xa53c98b5b727a16eull},
	{"player_sync_removal", 3u, 0xb1ee804f3f7b976cull},
	{"player_list", 47u, 0x2fcbe1019372dd9full},
};

void dump(const std::vector<Case> &corpus) {
	std::printf("const Vector kExpected[] = {\n");
	for (const Case &c : corpus)
		std::printf("\t{\"%s\", %zuu, 0x%016llxull},\n", c.name.c_str(), c.bytes.size(),
		            (unsigned long long)fnv1a64(c.bytes));
	std::printf("};\n");
}

void print_hex(const std::vector<uint8_t> &bytes) {
	for (size_t i = 0; i < bytes.size(); ++i)
		std::printf("%02x%s", bytes[i], ((i + 1) % 32 == 0) ? "\n" : "");
	if (bytes.size() % 32)
		std::printf("\n");
}

} // namespace

int main() {
	const std::vector<Case> corpus = build_corpus();

	if (const char *d = std::getenv("NW_CODEC_DUMP"); d && *d) {
		dump(corpus);
		return 0;
	}

	const size_t expected_count = sizeof(kExpected) / sizeof(kExpected[0]);
	int failures = 0;

	if (expected_count != corpus.size()) {
		std::printf("FAIL: corpus has %zu cases but %zu vectors are committed "
		            "(regenerate with NW_CODEC_DUMP=1 — wire-witness rule applies)\n",
		            corpus.size(), expected_count);
		++failures;
	}

	for (size_t i = 0; i < corpus.size() && i < expected_count; ++i) {
		const Case &c = corpus[i];
		const Vector &e = kExpected[i];
		const uint64_t h = fnv1a64(c.bytes);
		if (c.name != e.name || c.bytes.size() != e.len || h != e.hash) {
			std::printf("FAIL %s: len %zu hash 0x%016llx, expected %s len %zu "
			            "hash 0x%016llx\n",
			            c.name.c_str(), c.bytes.size(), (unsigned long long)h,
			            e.name, e.len, (unsigned long long)e.hash);
			std::printf("actual bytes:\n");
			print_hex(c.bytes);
			++failures;
		}
	}

	if (failures) {
		std::printf("\n%d codec identity vector(s) diverged. The wire bytes "
		            "changed: if unintentional, fix the codec; if witnessed, "
		            "update the vectors in the SAME commit as the [orig]/D-NET "
		            "justification (NW_CODEC_DUMP=1 prints the table).\n",
		            failures);
		return 1;
	}
	std::printf("OK — %zu codec identity vectors stable\n", corpus.size());
	return 0;
}
