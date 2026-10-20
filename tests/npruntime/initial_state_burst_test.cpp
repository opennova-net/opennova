// P3 — the §5.2a two-track initial-state burst machine (server_initial_state.{h,cpp}). Drives the
// host's own loopback connection (the §5.2a step-4 in-process client) through the burst over a real
// World + bms::File + host config and asserts: (1) the full §5.2a emitted tag ORDER; (2) each body
// decodes / round-trips / byte-matches the golden-witnessed value (incl. the 0x2C/0x08/0x2A/0x66/0x76/
// 0x1A serializers ported 2026-06-27); (3) the 0x0C organic carries the host player's dcb at
// entity+0x78 (the §1 wiring, end to end); (4) burst.game_state==9 / spawned at the terminator;
// (5) the full world-stream pages every pool (0x10/0x0D/0x0C/0x20) and conditionally emits the
// mission-text-backed 0x7E briefing pair; only the conditional 0x45 terrain stays absent here.

#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h> // CONTROL_REQUEST_PERIOD_TICKS (the join's quartet seed)

#include "host_test_setup.h"

#include <runtime/inmatch/loopback_channel.h>

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>

#include <net/npwire/ingame_decode.h> // decode_organic_spawn_batch / decode_pool3_sync_batch
#include <net/npwire/ingame_encode.h> // encode_pool_spawn_batch
#include <net/npwire/ingame_message_id.h> // s2c::WORLD_STATE_LOAD
#include <net/npwire/protocol_message.h> // make_protocol_message (the C2S 0x0B)
#include <runtime/replication/client_replica_pipeline.h> // the joiner's 0x0F apply
#include <runtime/replication/entity_wire_bridge.h> // build_pool1_spawn_batch

#include <runtime/mission/mission_text.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

uint16_t read_u16_le(const std::vector<uint8_t> &bytes, std::size_t offset) {
	return static_cast<uint16_t>(bytes[offset]) |
	       (static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

uint32_t read_u32_le(const std::vector<uint8_t> &bytes, std::size_t offset) {
	return static_cast<uint32_t>(bytes[offset]) |
	       (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
	       (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
	       (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

void write_u16_le(std::vector<uint8_t> &bytes, std::size_t offset, uint16_t value) {
	bytes[offset] = static_cast<uint8_t>(value & 0xFFu);
	bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

int main_impl() {
	// A World with the host player spawned + one 6002 marker (both the spawn-select start AND a
	// pool-3 spawn-marker the 0x20 batch streams).
	w::World world;
	w::AiSystem &ai = world.ai;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(2, 16);
	world.registry.configure_pool(3, 16);
	// items.def's Player row: the host body's ItemTypeIndex and def (the 0x0C skips a row
	// without them, D-NET-487).
	world.tables.player.has_item_def = true;
	world.tables.player.item_type_index = 1;
	world.tables.player.item_type = 3;
	{
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = 6002;
		start.position = {50.0f, 60.0f, 1.0f};
		start.heading = w::placed_heading_bam(0);
		start.item_type_index = 1; // the trait sweep's row: not an empty slot
		start.has_item_def = true;
		world.registry.spawn(3, start);
	}
	{
		// Two 00TRg location markers. Their localized labels are resolved from
		// MissionText before the runtime context is brought up.
		w::Entity location;
		location.kind = w::EntityKind::Marker;
		location.item_id = 2044;
		location.item_type_index = 2; // the trait sweep's row: not an empty slot
		location.has_item_def = true;
		world.registry.spawn(3, location);
		world.registry.spawn(3, location);
	}
	{
		w::WaypointEntry waypoint;
		waypoint.node = 12;
		waypoint.name_id = 0;
		waypoint.done = true; // a marker the round already completed
		world.script.waypoints.entries.push_back(waypoint);
	}
	{
		// A pool-2 building carrying the D-NET-147 wire fields (the golden ASH_I5A values):
		// entity Flags 0x04020400 (indestructible+Building+Reflective), ammo 0xFF, subType 0xFF.
		w::Entity bld;
		bld.kind = w::EntityKind::Building;
		bld.item_id = 0x044c;
		bld.position = {-396.6f, 360.1f, 11.2f};
		bld.heading = w::placed_heading_bam(0);
		bld.engine_flags = 0x04020400u;
		bld.ammo_count = 0xFF;
		bld.sub_type = 0xFF;
		bld.item_type_index = 1; // the trait sweep's row: not an empty slot
		bld.has_item_def = true;
		world.registry.spawn(2, bld);
	}

	// A valid loaded mission whose source loadout chunk has two ignored bytes after
	// its terminator. The parser sanitizes that chunk to a shorter canonical model,
	// while retail's 0x0B sender memcpy's the original loaded 0x268-byte header.
	opennova::bms::File authored_mission;
	opennova::mission::make_default(authored_mission);
	auto &loadout = authored_mission.loadout.entries.emplace_back();
	loadout.name = "WPN_PARITY_TEST";
	loadout.ammo_primary = "1";
	loadout.ammo_secondary = "2";
	loadout.flags = "-1";
	opennova::mission::sync_counts(authored_mission);
	std::vector<uint8_t> source_mission;
	std::string authored_error;
	if (!expect(opennova::bms::write(authored_mission, source_mission, authored_error),
	            "authored BMS fixture serializes")) return 1;
	constexpr std::size_t kLoadoutLenOffset =
			offsetof(opennova::bms::Header, weapon_loadout_chunk_len);
	const uint16_t canonical_loadout_len = read_u16_le(source_mission, kLoadoutLenOffset);
	const uint8_t trailing[] = {0xAB, 0xCD};
	source_mission.insert(
			source_mission.begin() + static_cast<std::ptrdiff_t>(
					opennova::bms::kHeaderSize + canonical_loadout_len),
			std::begin(trailing), std::end(trailing));
	write_u16_le(source_mission, kLoadoutLenOffset,
	             static_cast<uint16_t>(canonical_loadout_len + sizeof(trailing)));
	const std::vector<uint8_t> source_header(
			source_mission.begin(),
			source_mission.begin() + static_cast<std::ptrdiff_t>(opennova::bms::kHeaderSize));

	opennova::bms::File mission;
	std::string mission_error;
	if (!expect(opennova::bms::parse(
				source_mission.data(), source_mission.size(), mission, mission_error),
	            "noncanonical source BMS parses")) return 1;
	std::vector<uint8_t> canonical_header;
	if (!expect(opennova::bms::encode_header_blob(mission, canonical_header, mission_error),
	            "parsed BMS has a canonical header projection")) return 1;
	if (!expect(canonical_header != source_header &&
	                    read_u16_le(canonical_header, kLoadoutLenOffset) == canonical_loadout_len &&
	                    read_u16_le(source_header, kLoadoutLenOffset) ==
	                            canonical_loadout_len + sizeof(trailing),
	            "fixture distinguishes canonical and loaded header lengths")) return 1;

	ns::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig config;
	if (!expect(config.class_allow_mask == 0x03FFu,
	            "GameConfig defaults to retail's all-ten-classes mask")) return 1;
	config.class_allow_mask = 0x0155u; // non-default pins config sourcing, not a hard-coded golden
	config.game_type = 0x00010020u;
	config.default_spawn_requires_no_team_zone = 1;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
	                        /*host_key=*/0, &loopback, config);
	ctx.world = &world;
	ctx.mission = &mission;
	ctx.mission_text_loaded = true;
	ctx.mission_briefing3 = "First briefing page";
	ctx.mission_briefing2 = "Second briefing page";
	ctx.mission_location_names = {"Weapons Cache", "Rebel Outpost"};
	const uint32_t advertised_build_flags = ctx.np_protocol.build_flags;
	ctx.config.server_password = "mutated-after-create";
	if (!expect(inmatch::build_server_config_flags(ctx) != advertised_build_flags,
	            "fixture mutation changes the live BuildFlags computation")) return 1;

	// Spawn the host's own pool-0 player (so the burst's 0x0C has it with dcb 2).
	inmatch::Server_InitNewRoundState(ctx);
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(ctx, world) == 1, "host player spawned")) return 1;

	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list[0];

	// Drive the burst to completion, collecting the emitted (tag, body) in order.
	// The HOST LOOPBACK (type-2) skips the loadout gate and drains in one shot — it has no remote
	// client sending C2S 0x2F. The loadout gate only applies to REMOTE JOINERS (type-1).
	// The world stream's 0x1A leaves the own slot in state 5; the own client's C2S 0x0B (its
	// mission start's end) then joins it from the next receive pump (D-NET-NEW-L4-5).
	std::vector<inmatch::InitialStateMessage> emitted;
	const bool reached =
			inmatch::test::drive_host_own_burst_and_join(ctx, conn, /*now_tick=*/1, emitted);

	if (!expect(reached, "the burst's 0x1A leaves the own slot in state 5 for its C2S 0x0B")) return 1;
	if (!expect(conn.burst.spawned && conn.burst.game_state == 10 &&
	                    conn.slot_state == inmatch::PlayerSlotState::Joined,
	            "the own 0x0B's join marks spawned, game state 10, slot state 6")) return 1;
	if (!expect(conn.phase == inmatch::ConnectionPhase::Spawned, "connection advanced to Spawned")) return 1;

	// (1) The full §5.2a emitted tag order. Player-sync: 0x2C, 0x08, 0x2A×6, 0x1C, 0x0B, 0x66, 0x76,
	// 0x11 (matches the retail-lan-host-join golden frames 144-160). World-stream streams EVERY pool in
	// full (paged ~640 B/datagram): 0x10 pool-2, 0x0D pool-1, 0x0C pool-0, 0x20 pool-3, then the
	// mission-text 0x7E and 0x1A. Then
	// the GAME-START BUNDLE 0x42 / 0x0A / 0x0F / 0x4D / 0x61 / 0x3E (the deploy unsticker — clears the
	// joiner's load-gate; matches golden frames 318-319; the in-session 0x0A is the slot's first,
	// D-NET-501 [orig: Server_OnPlayerJoin @0x51A82D]). (0x45 terrain remains absent.) In THIS
	// minimal World pool-1 is empty (header-only 0x0D page); pool-2 carries one building so the
	// 0x10 page also pins the D-NET-147 fields.
	const std::vector<uint8_t> want_order = {0x2C, 0x08, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
	                                         0x1C, 0x0B, 0x66, 0x76, 0x11, 0x10, 0x0D, 0x0C, 0x20, 0x7E, 0x1A,
	                                         0x42, 0x0A, 0x0F, 0x4D, 0x61, 0x3E};
	std::vector<uint8_t> got_order;
	for (auto &m : emitted) got_order.push_back(m.tag);
	if (!expect(got_order == want_order, "emitted tag order matches §5.2a (full player-sync + world-stream)")) {
		std::fprintf(stderr, "  got:");
		for (uint8_t t : got_order) std::fprintf(stderr, " 0x%02X", t);
		std::fprintf(stderr, "\n");
		return 1;
	}

	// The conditional terrain tag stays absent because this fixture has no .til bytes. The
	// mission-text-backed 0x7E and pool-1 0x0D are both present.
	for (auto &m : emitted) {
		if (m.tag == 0x45) {
			std::fprintf(stderr, "FAIL: conditionally-absent tag 0x%02X was emitted\n", m.tag);
			return 1;
		}
	}

	// Fetch helpers.
	auto body_of = [&](uint8_t tag) -> const std::vector<uint8_t> * {
		for (auto &m : emitted)
			if (m.tag == tag) return &m.body;
		return nullptr;
	};

	{
		// PR #403 retail 00TRg witness: the waypoint gametype serializes the
		// first blue-route pool-3 marker followed by localized deploy-map labels.
		const std::vector<uint8_t> *b = body_of(0x0F);
		opennova::WorldStateLoad state;
		if (!expect(
				b != nullptr && b->size() == 572 &&
				opennova::decode_world_state_load(
						b->data(), b->size(), state, true),
				"0x0F waypoint-gametype body fully decodes at the retail width")) {
			return 1;
		}
		if (!expect(
				state.waypoints.size() == 1 && state.waypoint_count == 1 &&
						// The raw pool-3 record index, exactly as the stock
						// client feeds Pool_GetEntryUnchecked(3, word) — a
						// 0x3000|node handle would index 12288 records past the
						// pool [orig: writer @0x502f35/@0x502f45, reader @0x42e4a3].
						state.waypoints[0].slot_id == 0x000C &&
						state.waypoints[0].name_id == 0 &&
						// The marker's done byte (entity+536), which a DFX2
						// joiner stores [orig: @0x502f5f] [orig: @0x50008f (dfx2.exe)].
						state.waypoints[0].done == 1,
				"0x0F carries 00TRg's blue-route waypoint record")) {
			return 1;
		}
		if (!expect(
				state.team_name_count == 2 &&
						state.team_names == std::vector<std::string>{
								"Weapons Cache", "Rebel Outpost"},
				"0x0F carries MissionText-resolved location labels in spawn order")) {
			return 1;
		}
		if (!expect(state.game_flags == 0x02,
				"0x0F gameFlags bit1 advertises the target-less spawn restriction")) {
			return 1;
		}
		// The i16 yaw is the heading word's high half. This Co-op word authors no
		// 6094 / 6001 start (the 6002 is a deathmatch family's), so the add's
		// placement stores nothing and the fresh player keeps the zeroed entity's
		// heading word 0 (the start marker's 0x4000 rounding is pinned below).
		// [orig: NetPacket_WriteWorldStateLoad0x0F `movzx edx, word ptr [ebp+12h]`
		//  @0x502D6D; Entity_SpawnFromAnimSlotProperty - the memset @0x43C3FE, the
		//  placement @0x43C459]
		if (!expect(state.yaw == 0 && state.pitch == 0 && state.roll == 0,
				"0x0F yaw is the placement heading's high half")) {
			return 1;
		}
	}

	{
		// MissionText [info]/briefing3 then briefing2/briefing fallback, as two
		// consecutive NUL-terminated strings [orig: NetPacket_WriteBriefingText @0x506620].
		const std::vector<uint8_t> *b = body_of(0x7E);
		const std::vector<uint8_t> want = {
			'F','i','r','s','t',' ','b','r','i','e','f','i','n','g',' ','p','a','g','e',0,
			'S','e','c','o','n','d',' ','b','r','i','e','f','i','n','g',' ','p','a','g','e',0,
		};
		if (!expect(b && *b == want, "0x7E = briefing3\\0 + briefing2-or-briefing\\0")) return 1;
	}

	// (2)/(3) 0x0C decodes + carries the host player's dcb at entity+0x78.
	{
		const std::vector<uint8_t> *b = body_of(0x0C);
		opennova::OrganicSpawnBatch batch;
		if (!expect(b && opennova::decode_organic_spawn_batch(b->data(), b->size(), batch),
		            "0x0C body decodes")) return 1;
		bool host_dcb = false;
		for (const auto &r : batch.records)
			if (r.item_type_id == 0x14B9 && r.owner_connection_id == inmatch::kHostPlayerDcb) host_dcb = true;
		if (!expect(host_dcb, "0x0C carries the host player (0x14B9) with entity+0x78 == dcb 2")) return 1;
	}

	// (2) 0x20 decodes + carries the spawn marker.
	{
		const std::vector<uint8_t> *b = body_of(0x20);
		opennova::Pool3SyncBatch batch;
		if (!expect(b && opennova::decode_pool3_sync_batch(b->data(), b->size(), batch),
		            "0x20 body decodes")) return 1;
		bool saw_marker = false;
		for (const auto &r : batch.records)
			if (r.item_type_id == 6002) saw_marker = true;
		if (!expect(saw_marker, "0x20 carries the 6002 spawn marker")) return 1;
	}

	// (2) 0x0B is the exact loaded 616-byte BMS header, not a canonical rewrite.
	{
		const std::vector<uint8_t> *b = body_of(0x0B);
		if (!expect(b && b->size() == opennova::bms::kHeaderSize, "0x0B body is the 616-byte BMS header")) return 1;
		if (!expect((*b)[0] == 'B' && (*b)[1] == 'M' && (*b)[2] == 'S', "0x0B header magic is 'BMS'")) return 1;
		if (!expect(*b == source_header, "0x0B byte-matches the loaded BMS header")) return 1;
		if (!expect(*b != canonical_header,
		            "0x0B preserves source bytes when canonical encoding differs")) return 1;
		if (!expect(read_u16_le(*b, kLoadoutLenOffset) ==
		                    canonical_loadout_len + sizeof(trailing),
		            "0x0B preserves the loaded weapon-loadout chunk length")) return 1;
	}

	// Empty scalar bodies are wire-valid for 0x1C/0x11, but 0x10 is a static-entity batch and even
	// an empty batch must carry its [u16 start_index][u16 count] header.
	if (!expect(body_of(0x1C)->empty() && body_of(0x11)->empty(),
	            "0x1C / 0x11 carry empty bodies")) return 1;
	{
		// The pool-2 building streams with the D-NET-147 fields: entity Flags dword (0x0020),
		// subType (0x0080), and the always-present ammo byte — the golden ASH_I5A building shape
		// (flags 0x0A1, eflags 0x04020400, subType 0xFF, ammo 0xFF).
		const std::vector<uint8_t> *b = body_of(0x10);
		opennova::StaticEntityBatch batch;
		if (!expect(b && opennova::decode_static_entity_batch(b->data(), b->size(), batch),
		            "0x10 static batch decodes")) return 1;
		if (!expect(batch.records.size() == 1 && !batch.records[0].is_empty_slot,
		            "0x10 carries the pool-2 building record")) return 1;
		const opennova::StaticEntityRecord &r = batch.records[0];
		if (!expect(r.item_type_id == 0x044c, "0x10 building item type")) return 1;
		if (!expect(r.field_flags == 0x0A1, "0x10 building field_flags == 0x0A1 (golden shape)")) return 1;
		if (!expect(r.entity_flags == 0x04020400u, "0x10 building entity Flags dword (D-NET-147)")) return 1;
		if (!expect(r.ammo_count == 0xFF && r.bone_b == 0xFF,
		            "0x10 building ammo 0xFF + subType 0xFF (D-NET-147)")) return 1;
	}

	// (6) The §5.2a serializers ported from IDA (grilled 2026-06-27). Bodies cross-checked vs the
	// retail-lan-host-join golden (frames 144-160).
	{
		// 0x2C = server name + mission file, two NUL-terminated C strings (from ctx.config).
		const std::vector<uint8_t> *b = body_of(0x2C);
		const std::string sn = ctx.config.server_name, mf = ctx.config.mission_file;
		std::vector<uint8_t> want;
		want.insert(want.end(), sn.begin(), sn.end()); want.push_back(0);
		want.insert(want.end(), mf.begin(), mf.end()); want.push_back(0);
		if (!expect(b && *b == want, "0x2C = serverName\\0 + missionFile\\0")) return 1;
	}
	{
		// 0x08 = the 51-byte server-config block (10 rule dwords default 0 + 7 bytes + flags dword).
		const std::vector<uint8_t> *b = body_of(0x08);
		if (!expect(b && b->size() == 51, "0x08 server-config is 51 bytes")) return 1;
		bool dwords_match = true;
		for (int i = 0; i < 40; ++i) {
			const uint8_t expected =
					(i == 12 ? 0x20 : i == 14 ? 0x01 : 0x00);
			dwords_match = dwords_match && ((*b)[i] == expected);
		}
		if (!expect(dwords_match,
		            "0x08 rule dwords carry only the 0x00010020 game type")) return 1;
		if (!expect(read_u32_le(*b, 47) == advertised_build_flags,
		            "0x08 BuildFlags stays identical to the create-session P2 snapshot"))
			return 1;
	}
	{
		// 0x2A ×6 — each the const table record {00 04 b0 ab b2 b2 bf bc bd ba} (golden frames 148-158).
		static const std::vector<uint8_t> kRec = {0x00, 0x04, 0xb0, 0xab, 0xb2, 0xb2, 0xbf, 0xbc, 0xbd, 0xba};
		int count_2a = 0;
		for (auto &m : emitted) {
			if (m.tag != 0x2A) continue;
			++count_2a;
			if (!expect(m.body == kRec, "0x2A record == const table payload")) return 1;
		}
		if (!expect(count_2a == 6, "exactly six 0x2A records emitted")) return 1;
	}
	{
		// 0x66 weapon-restrictions: no restrictions -> single count byte 0 (golden frame 160).
		const std::vector<uint8_t> *b = body_of(0x66);
		if (!expect(b && b->size() == 1 && (*b)[0] == 0, "0x66 empty restriction table = {0}")) return 1;
		// 0x76 is the configured class-allow mask, independent of now_tick.
		const std::vector<uint8_t> *t = body_of(0x76);
		if (!expect(t && *t == std::vector<uint8_t>{0x55, 0x01},
		            "0x76 class-allow mask = configured u16, not now_tick")) return 1;
		// 0x1A timestamp = now_tick u32 (now_tick=1 -> 01 00 00 00).
		const std::vector<uint8_t> *ts = body_of(0x1A);
		if (!expect(ts && *ts == std::vector<uint8_t>{0x01, 0x00, 0x00, 0x00}, "0x1A timestamp = now_tick u32")) return 1;
	}

	// --- Regression (D-NET-114 one-shot burst F3 latch): driving the World path through
	// tick_connections must surface BOTH PeerEnteredWorldStreaming (F3) AND PeerSpawned, with F3 first.
	// The one-shot drain latches entity_batch_count and burst.spawned in the SAME call, so a
	// !spawned-gated F3 predicate would drop F3 entirely on the World path (losing the dcb-timing
	// signal). This drives a fresh host loopback (unspawned) so tick_connections runs the full burst. ---
	{
		w::World w2;
		w::AiSystem &ai2 = w2.ai;
		w2.registry.configure_pool(0, 16);
		w2.registry.configure_pool(3, 16);
		{
			w::Entity m;
			m.kind = w::EntityKind::Marker;
			m.item_id = 6002;
			m.position = {10.0f, 20.0f, 1.0f};
			m.item_type_index = 1; // the trait sweep's row: not an empty slot
			m.has_item_def = true;
			w2.registry.spawn(3, m);
		}
		ns::LoopbackChannel lb2;
		inmatch::NapiNPServerCtx ctx2;
		inmatch::test::bring_up_host(ctx2, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
		                        /*host_key=*/0, &lb2);
		ctx2.world = &w2;
		ctx2.mission = &mission;
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(ctx2, w2) == 1,
		            "world-path pose player spawned")) return 1;
		inmatch::NapiNPConnection &pose_conn = ctx2.np_protocol.connection_list[0];
		w::AiEntity *pose_ai = ai2.for_handle(pose_conn.link.owned_entity);
		if (!expect(pose_ai != nullptr, "world-path pose resolves the owned AiEntity")) return 1;
		constexpr int32_t kLookPitchBam = 0x23456789;
		constexpr int16_t kLookPitchHigh = 0x2345;
		pose_ai->pitch = kLookPitchBam;

		const std::vector<inmatch::TickOut> outs = inmatch::tick_connections(ctx2, /*elapsed_ms=*/16, /*now_tick=*/1);
		int f3_at = -1, spawned_at = -1, seq = 0;
		for (const inmatch::TickOut &to : outs) {
			for (const inmatch::HostAcceptEvent &ev : to.events) {
				if (ev.kind == inmatch::HostAcceptEvent::Kind::PeerEnteredWorldStreaming) {
					if (!expect(ev.self_id == inmatch::kHostPlayerDcb, "F3 self_id == host dcb")) return 1;
					if (!expect(ev.pose.pitch == kLookPitchHigh,
					            "F3 world-path pose pitch == AiEntity BAM32 high word")) return 1;
					// The spawn marker's yaw 0 placement heading's high half, 0x4000
					// [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66].
					if (!expect(ev.pose.heading == 0x4000,
					            "F3 world-path pose heading == the placement heading's high half")) return 1;
					if (f3_at < 0) f3_at = seq;
				} else if (ev.kind == inmatch::HostAcceptEvent::Kind::PeerSpawned) {
					if (spawned_at < 0) spawned_at = seq;
				}
				++seq;
			}
		}
		if (!expect(f3_at >= 0, "one-shot World burst still surfaces PeerEnteredWorldStreaming (F3)")) return 1;
		// The burst ends in slot state 5: the own slot joins from its own C2S 0x0B's receiver,
		// not from the producer, so the burst surfaces no PeerSpawned (D-NET-NEW-L4-5).
		if (!expect(spawned_at < 0 && !pose_conn.burst.spawned &&
		                    pose_conn.slot_state == inmatch::PlayerSlotState::AwaitJoin,
		            "the own burst ends in slot state 5, unjoined")) return 1;
		(void)inmatch::test::host_own_join(ctx2, pose_conn, /*now_tick=*/1);
		if (!expect(pose_conn.burst.spawned, "the own C2S 0x0B's join enters the game")) return 1;
	}

	// A bound World entity without an AiEntity keeps the historical zero-pitch fallback. In
	// particular, world::Entity::pitch is not a substitute: it has different ownership/units.
	{
		w::World no_ai_world;
		no_ai_world.registry.configure_pool(0, 1);
		w::Entity entity;
		entity.kind = w::EntityKind::Organic;
		entity.item_id = 0x14B9;
		entity.pitch = 0x1234;
		const w::EntityHandle entity_handle = no_ai_world.registry.spawn(0, entity);
		if (!expect(entity_handle.valid(), "missing-AI pose fixture entity spawned")) return 1;

		ns::LoopbackChannel no_ai_loopback;
		inmatch::NapiNPServerCtx no_ai_ctx;
		inmatch::test::bring_up_host(no_ai_ctx, inmatch::ConnectionMode::HostClient,
		                        inmatch::SocketMode::Socketless, /*host_key=*/0, &no_ai_loopback);
		no_ai_ctx.world = &no_ai_world;
		inmatch::NapiNPConnection &no_ai_conn = no_ai_ctx.np_protocol.connection_list[0];
		no_ai_conn.phase = inmatch::ConnectionPhase::New;
		no_ai_conn.link.owned_entity = entity_handle;
		no_ai_conn.burst.entity_batch_count = 1;

		bool saw_world_pose = false;
		for (const inmatch::TickOut &to : inmatch::tick_connections(
		             no_ai_ctx, /*elapsed_ms=*/16, /*now_tick=*/1)) {
			for (const inmatch::HostAcceptEvent &ev : to.events) {
				if (ev.kind != inmatch::HostAcceptEvent::Kind::PeerEnteredWorldStreaming) continue;
				saw_world_pose = true;
				if (!expect(ev.pose.entity_handle == entity_handle.packed,
				            "missing-AI fallback still uses the bound World entity")) return 1;
				if (!expect(ev.pose.pitch == 0,
				            "missing-AI World-path pose retains zero pitch")) return 1;
			}
		}
		if (!expect(saw_world_pose, "missing-AI World-path pose surfaced")) return 1;
	}

	// --- Loadout gate pacing: a TYPE-1 (remote joiner) connection PAUSES at phase 7→8 until
	// loadout_received is set. The golden (f316-318) shows: S 0x1A (end of world-stream) -> C 0x2F
	// (client sends loadout request) -> S 0x5A + game-start. Without the gate, 0x1A and the
	// game-start would land together and the client couldn't send 0x2F between them. ---
	{
		w::World w3;
		w::AiSystem &ai3 = w3.ai;
		w3.registry.configure_pool(0, 16);
		w3.registry.configure_pool(3, 16);
		{
			w::Entity m;
			m.kind = w::EntityKind::Marker;
			m.item_id = 6002;
			m.position = {10.0f, 20.0f, 1.0f};
			m.item_type_index = 1; // the trait sweep's row: not an empty slot
			m.has_item_def = true;
			w3.registry.spawn(3, m);
		}
		inmatch::NapiNPServerCtx ctx3;
		ns::LoopbackChannel lb3;
		inmatch::test::bring_up_host(ctx3, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
		                        /*host_key=*/0, &lb3);
		ctx3.world = &w3;
		ctx3.mission = &mission;
		inmatch::Server_InitNewRoundState(ctx3);
		inmatch::Server_ProcessPendingPlayerSpawns(ctx3, w3);

		// Simulate a remote joiner by adding a type-1 connection with PlayerAdded phase.
		// Built in place: a NapiNPConnection is several KiB, and this frame's stack is full.
		inmatch::NapiNPConnection &jconn = ctx3.np_protocol.connection_list.emplace_back();
		jconn.type = 1;
		jconn.connection_id = inmatch::kFirstJoinerDcb;
		jconn.phase = inmatch::ConnectionPhase::PlayerAdded;
		jconn.slot_state = inmatch::PlayerSlotState::Added;
		// D-NET-500: the added slot's track waits for the joiner's C2S 0x09 (sent once its
		// mission load completed); until then the producer sends nothing.
		for (int i = 0; i < 4; ++i) {
			inmatch::InitialStateStep s = inmatch::Server_SendInitialGameStateToPlayer(ctx3, jconn, /*now_tick=*/1);
			if (!expect(s.messages.empty(), "no player-sync record before the C2S 0x09")) return 1;
		}
		{
			inmatch::ServerDispatchInputs inputs;
			inputs.server_ctx = &ctx3;
			std::vector<opennova::ProtocolMessage> msgs;
			msgs.push_back(opennova::make_protocol_message(opennova::c2s::CHECKSUM_RESPONSE, {}));
			const std::vector<opennova::ProtocolMessage> replies = inmatch::dispatch_session_replies(
					ctx3.config, jconn, msgs, 1, ctx3.np_protocol.connection_list, &w3, inputs);
			if (!expect(replies.empty() && jconn.slot_state == inmatch::PlayerSlotState::PlayerSync &&
			                    jconn.burst.game_state == 9,
			            "the C2S 0x09 answers nothing and enters slot state 2, game state 9")) return 1;
		}

		// A REMOTE (type-1) joiner is PACED: each call emits only a few datagrams (kPacedMsgsPerTick),
		// so the burst takes many calls to stream the player-sync tail — and then PARKS at sync-state
		// 3 until the client's C2S 0x0A spawn-menu request (D-NET-150): the world stream must never
		// start unrequested [orig: state 3 @0x51c134; 3 -> 4 only via
		// NapiNPServerMsg_HandlePlayerSpawnRequest @0x513260]. Drive to the park, assert nothing
		// world-stream leaked, then apply the 0x0A advance and drive to the phase 7→8 loadout gate.
		bool parked_for_spawn_menu = false;
		bool saw_atomic_player_sync_tail = false;
		int paced_calls = 0;
		for (int i = 0; i < 64 && !parked_for_spawn_menu; ++i) {
			inmatch::InitialStateStep s = inmatch::Server_SendInitialGameStateToPlayer(ctx3, jconn, /*now_tick=*/1);
			++paced_calls;
			if (!expect(!jconn.burst.spawned, "joiner not in-game while paced/gated")) return 1;
			if (!s.messages.empty() && s.messages.front().tag == 0x1C) {
				std::vector<uint8_t> tags;
				for (const auto &m : s.messages) tags.push_back(m.tag);
				if (!expect(tags == std::vector<uint8_t>({0x1C, 0x0B, 0x66, 0x76, 0x11}),
				            "retail player-sync tail is one atomic five-record boundary"))
					return 1;
				saw_atomic_player_sync_tail = true;
			}
			for (const auto &m : s.messages) {
				if (!expect(m.tag != 0x10 && m.tag != 0x0D && m.tag != 0x0C && m.tag != 0x20,
				            "no world-stream tag before the client's C2S 0x0A (D-NET-150)")) return 1;
			}
			if (jconn.slot_state == inmatch::PlayerSlotState::AwaitWorldRequest) parked_for_spawn_menu = true;
		}
		if (!expect(parked_for_spawn_menu, "player-sync tail parks at sync-state 3 (awaiting C2S 0x0A)")) return 1;
		if (!expect(saw_atomic_player_sync_tail,
		            "paced remote emitted the witnessed atomic player-sync tail")) return 1;
		{
			// Park is stable: further ticks emit nothing until the 0x0A arrives.
			inmatch::InitialStateStep s = inmatch::Server_SendInitialGameStateToPlayer(ctx3, jconn, /*now_tick=*/1);
			if (!expect(s.messages.empty(), "parked burst emits nothing without the C2S 0x0A")) return 1;
		}
		// Simulate the client's C2S 0x0A spawn-menu request (what the dispatch case 0x0A applies)
		// [orig: NapiNPServerMsg_HandlePlayerSpawnRequest @0x513260].
		jconn.burst.game_state = 9;
		jconn.slot_state = inmatch::PlayerSlotState::WorldStream;
		jconn.burst.world_stream_phase = 0;
		bool reached_await_join = false;
		for (int i = 0; i < 256 && !reached_await_join; ++i) {
			inmatch::InitialStateStep s = inmatch::Server_SendInitialGameStateToPlayer(ctx3, jconn, /*now_tick=*/1);
			++paced_calls;
			if (!expect(!jconn.burst.spawned, "joiner not in-game while paced/gated")) return 1;
			reached_await_join = jconn.slot_state == inmatch::PlayerSlotState::AwaitJoin;
		}
		if (!expect(reached_await_join && jconn.burst.game_state == 9,
		            "the world stream's 0x1A parks the slot in state 5, game state 9")) return 1;
		if (!expect(paced_calls > 3, "burst was actually PACED across multiple calls (not one-shot)")) return 1;
		if (!expect(!jconn.burst.spawned, "joiner not joined before its C2S 0x0B")) return 1;
		for (int i = 0; i < 4; ++i) {
			inmatch::InitialStateStep s = inmatch::Server_SendInitialGameStateToPlayer(ctx3, jconn, /*now_tick=*/1);
			if (!expect(s.messages.empty(), "the producer sends nothing in slot state 5")) return 1;
		}

		// The stock client's join-time C2S 0x2F pair seeded the armory cooldown, then its
		// C2S 0x0B runs the join [orig: NapiNPServerMsg_PlayerJoinRequest @0x51AB10 ->
		// Server_OnPlayerJoin @0x51A680].
		jconn.burst.loadout_received = true;
		jconn.link.armory_reuse_seconds = 30;
		jconn.reply.chat_last_ms = 1234;
		jconn.reply.rtt_request_countdown = 17;
		jconn.reply.control_request_countdown = 0;
		jconn.reply.charattr_unanswered_count = 3;
		jconn.reply.time_sync_unanswered_count = 2;
		jconn.reply.time_sync_sequence = 9;
		w::PlayerSpawn jbody;
		jbody.team = 1;
		jconn.link.owned_entity = w::spawn_remote_player(w3, jbody);
		// D-NET-374: the session's mp_NoCharAbilities and mp_NoScopeDrift ride the
		// join as one S2C 0x41 each (properties 0 and 7) ahead of the 0x42, whose
		// body is the authority's packed latches [orig: Server_OnPlayerJoin
		// @0x51a7e2 -> Server_SendCharAttrRestrictionsToPlayer @0x509950, then
		// @0x51a802 -> CharAttr_PackDisabledProperties @0x412550].
		ctx3.config.no_char_abilities = 1;
		ctx3.config.no_scope_drift = 1;
		w3.tables.charattr_disabled_word = 0x0012u;
		// The join's state-6 entry zeroes a level stored ahead of it, and its resend flag
		// [orig: Server_OnPlayerJoin @0x51A71F / @0x51A725].
		jconn.reply.client_quality = 3;
		jconn.reply.client_quality_dirty = true;
		std::vector<opennova::ProtocolMessage> join_tail;
		{
			inmatch::ServerDispatchInputs inputs;
			inputs.server_ctx = &ctx3;
			std::vector<opennova::ProtocolMessage> msgs;
			msgs.push_back(opennova::make_protocol_message(opennova::c2s::MISSION_FILE_STATUS, {0x00}));
			join_tail = inmatch::dispatch_session_replies(ctx3.config, jconn, msgs, 2,
					ctx3.np_protocol.connection_list, &w3, inputs);
		}
		bool saw_0f = false, saw_42 = false, saw_3e = false;
		bool saw_transient_42 = false;
		std::vector<std::vector<uint8_t>> clears;
		std::vector<uint8_t> latches;
		for (const auto &m : join_tail) {
			if (m.tag == 0x41 && !saw_42) clears.push_back(m.payload);
			if (m.tag == 0x42) {
				saw_42 = true;
				saw_transient_42 = !m.reliable;
				latches = m.payload;
			}
			if (m.tag == 0x0F) saw_0f = true;
			if (m.tag == 0x3E) saw_3e = true;
		}
		if (!expect(clears == std::vector<std::vector<uint8_t>>({{0x00}, {0x07}}),
		            "the join's 0x41s name the session's restricted properties, ahead of its 0x42")) return 1;
		if (!expect(latches == std::vector<uint8_t>({0x12, 0x00}), "the join's 0x42 carries the authority's latches"))
			return 1;
		if (!expect(jconn.reply.client_quality == 0 && !jconn.reply.client_quality_dirty,
		            "the join zeroes the slot's quality level and its resend flag")) return 1;
		if (!expect(jconn.burst.spawned && jconn.slot_state == inmatch::PlayerSlotState::Joined &&
		                    jconn.burst.game_state == 10,
		            "the 0x0B joins the slot: state 6, game state 10")) return 1;
		if (!expect(saw_42 && saw_0f && saw_3e, "the join sends the game-start messages (0x42/0x0F/0x3E)")) return 1;
		if (!expect(saw_transient_42,
		            "the join's input flags use retail's one-send transient delivery")) return 1;
		// D-NET-502: the join, not the add, zeroes the armory cooldown the join-time 0x2F seeded
		// and rewrites the state byte's bits [orig: @0x51A752; @0x51A6D3..0x51A73A].
		if (!expect(jconn.link.armory_reuse_seconds == 0 && jconn.reply.chat_last_ms == 0 &&
		                    jconn.reply.rtt_request_countdown == 0 &&
		                    !jconn.link.preround_loadout_latch &&
		                    jconn.reply.frontier_hint_pending && !jconn.reply.capture_nag_held &&
		                    jconn.reply.state6_entry_host_ms_valid,
		            "the join writes the slot's join words")) return 1;
		// D-NET-532: the state-6 entry's network reset seeds the quartet countdown with its
		// period and zeroes the silence counters and the time-sync round
		// [orig: PlayerSlot_ResetNetworkState @0x4F9FD0, 744 @0x4FA021].
		if (!expect(jconn.reply.control_request_countdown == inmatch::CONTROL_REQUEST_PERIOD_TICKS &&
		                    inmatch::CONTROL_REQUEST_PERIOD_TICKS == 744u &&
		                    jconn.reply.charattr_unanswered_count == 0 &&
		                    jconn.reply.time_sync_unanswered_count == 0 &&
		                    jconn.reply.time_sync_sequence == 0,
		            "the join's network reset seeds the 744-tick quartet countdown")) return 1;
		// A second 0x0B from the joined slot (game state 10) is dropped.
		{
			inmatch::ServerDispatchInputs inputs;
			inputs.server_ctx = &ctx3;
			std::vector<opennova::ProtocolMessage> msgs;
			msgs.push_back(opennova::make_protocol_message(opennova::c2s::MISSION_FILE_STATUS, {0x00}));
			if (!expect(inmatch::dispatch_session_replies(ctx3.config, jconn, msgs, 3,
					ctx3.np_protocol.connection_list, &w3, inputs).empty(),
			            "a joined slot's second 0x0B is dropped")) return 1;
		}
	}

	// --- The game-start bundle opens on the C2S 0x0B alone, for a spectator (whose 0x2F draws
	// nothing, D-NET-678) and a player alike: the 0x0B receiver tests neither the role nor a
	// loadout, and a stock client sends it like any peer; but it drops a 0x0B from a slot
	// whose entity does not exist yet, so only one sent after the entity opens the bundle
	// [orig: Game_StartMission @0x52624B; NapiNPServerMsg_PlayerJoinRequest @0x51AB10, its
	// gates @0x51AB21..0x51AB4B; NapiNPServerMsg_HandlePlayerLoadout @0x5157EB..0x5157F2]. ---
	for (const bool as_spectator : {true, false}) {
		// Heap-held: main_impl's frame already carries three worlds.
		const auto w4_owner = std::make_unique<w::World>();
		w::World &w4 = *w4_owner;
		w4.registry.configure_pool(0, 16);
		w4.registry.configure_pool(3, 16);
		{
			w::Entity m;
			m.kind = w::EntityKind::Marker;
			m.item_id = 6002;
			m.position = {10.0f, 20.0f, 1.0f};
			m.item_type_index = 1;
			m.has_item_def = true;
			w4.registry.spawn(3, m);
		}
		const auto ctx4_owner = std::make_unique<inmatch::NapiNPServerCtx>();
		inmatch::NapiNPServerCtx &ctx4 = *ctx4_owner;
		const auto lb4 = std::make_unique<ns::LoopbackChannel>();
		inmatch::test::bring_up_host(ctx4, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
		                        /*host_key=*/0, lb4.get());
		ctx4.world = &w4;
		ctx4.mission = &mission;
		inmatch::Server_InitNewRoundState(ctx4);
		inmatch::Server_ProcessPendingPlayerSpawns(ctx4, w4);
		inmatch::NapiNPConnection &sconn = ctx4.np_protocol.connection_list.emplace_back();
		sconn.type = 1;
		sconn.connection_id = inmatch::kFirstJoinerDcb;
		sconn.phase = inmatch::ConnectionPhase::PlayerAdded;
		sconn.link.spectator = as_spectator;
		sconn.slot_state = inmatch::PlayerSlotState::PlayerSync; // its C2S 0x09
		for (int i = 0; i < 64 && sconn.slot_state != inmatch::PlayerSlotState::AwaitWorldRequest; ++i)
			(void)inmatch::Server_SendInitialGameStateToPlayer(ctx4, sconn, /*now_tick=*/1);
		if (!expect(sconn.slot_state == inmatch::PlayerSlotState::AwaitWorldRequest,
		            "the joiner's player sync parks at state 3"))
			return 1;
		sconn.burst.game_state = 9; // the C2S 0x0A advance
		sconn.slot_state = inmatch::PlayerSlotState::WorldStream;
		sconn.burst.world_stream_phase = 0;
		bool gated = false;
		for (int i = 0; i < 256 && !gated; ++i) {
			(void)inmatch::Server_SendInitialGameStateToPlayer(ctx4, sconn, /*now_tick=*/1);
			gated = sconn.slot_state == inmatch::PlayerSlotState::AwaitJoin;
		}
		if (!expect(gated, "the joiner's world stream ends in slot state 5")) return 1;
		// An accepted 0x2F alone opens nothing.
		sconn.burst.loadout_received = true;
		for (int i = 0; i < 16; ++i)
			(void)inmatch::Server_SendInitialGameStateToPlayer(ctx4, sconn, 2);
		if (!expect(!sconn.burst.spawned, "the join does not run on a 0x2F")) return 1;
		sconn.burst.loadout_received = false;
		// The C2S 0x0B through the receiver.
		auto send_0b = [&]() {
			inmatch::ServerDispatchInputs inputs;
			inputs.server_ctx = &ctx4;
			std::vector<opennova::ProtocolMessage> msgs;
			msgs.push_back(opennova::make_protocol_message(opennova::c2s::MISSION_FILE_STATUS, {0x00}));
			(void)inmatch::dispatch_session_replies(ctx4.config, sconn, msgs, 3,
					ctx4.np_protocol.connection_list, &w4, inputs);
		};
		// The slot has no entity yet: retail drops its 0x0B, so the join does not run.
		send_0b();
		if (!expect(!sconn.burst.spawned && sconn.slot_state == inmatch::PlayerSlotState::AwaitJoin,
		            "a 0x0B from a slot without its entity is dropped")) return 1;
		w::PlayerSpawn body;
		body.team = 1;
		sconn.link.owned_entity = w::spawn_remote_player(w4, body);
		if (!expect(sconn.link.owned_entity.valid(), "the slot's entity spawns")) return 1;
		send_0b();
		if (!expect(sconn.burst.spawned,
		            as_spectator ? "a spectator's bundle opens on its 0x0B alone"
		                         : "a player's bundle opens on its 0x0B alone")) return 1;
	}

	std::printf("OK\n");
	return 0;
}

// The S2C 0x0D Flags field is the entity's live dword, raw: every vehicle
// carries the REFLECTABLE 0x400 its init sets (the port's ItemDefType-1
// trait), so its record always emits field 0x20, while a non-vehicle with a
// clear dword keeps the gate clear. The live values are the retail load
// stream's own (fixtures/novaworld/run_20260426_120859/server_load_packets.nwmsg):
// 0x20400 for a vehicle whose motor has run, 0x406 for a wreck.
// [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503ae1..0x503aed;
//  Entity_InitFromModel @0x40e204..0x40e20a]
int pool1_flags_field_impl() {
	w::World world;
	world.registry.configure_pool(1, 8);
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.item_id = 0x004A; // the capture's d_buggy
	vehicle.item_type = 1;
	vehicle.has_item_def = true;  // the trait sweep's row (D-NET-487)
	vehicle.item_type_index = 1;
	const w::EntityHandle vehicle_h = world.registry.spawn(1, vehicle);
	w::Entity crate;
	crate.kind = w::EntityKind::Item;
	crate.item_id = 0x050E;
	crate.has_item_def = true;  // the trait sweep's row (D-NET-487)
	crate.item_type_index = 1;
	const w::EntityHandle crate_h = world.registry.spawn(1, crate);
	const auto record_of = [&](w::EntityHandle h, opennova::PoolSpawnRecord &out) {
		const std::vector<uint8_t> wire =
				opennova::encode_pool_spawn_batch(ns::build_pool1_spawn_batch(world));
		opennova::PoolSpawnBatch decoded;
		if (!opennova::decode_pool_spawn_batch(wire.data(), wire.size(), decoded)) return false;
		for (const opennova::PoolSpawnRecord &r : decoded.records)
			if (r.slot_id == h.packed) {
				out = r;
				return true;
			}
		return false;
	};
	opennova::PoolSpawnRecord rec;
	if (!expect(vehicle_h.valid() && crate_h.valid() && record_of(vehicle_h, rec),
	            "the pool-1 vehicle round-trips through 0x0D")) return 1;
	if (!expect((rec.spawn_flags & opennova::kPoolSpawnHasEntityFlags) != 0 &&
	                    rec.entity_flags == 0x00000400u,
	            "a vehicle with a clear dword emits field 0x20 carrying 0x400")) return 1;
	if (!expect(record_of(crate_h, rec) &&
	                    (rec.spawn_flags & opennova::kPoolSpawnHasEntityFlags) == 0 &&
	                    rec.entity_flags == 0u,
	            "a non-vehicle with a clear dword keeps field 0x20 absent")) return 1;
	// The motor sets the matrix bit every tick it runs (homed on engine_flags).
	world.registry.get(vehicle_h)->engine_flags |= w::kEntityFlagMatrixBuilt;
	if (!expect(record_of(vehicle_h, rec) && rec.entity_flags == 0x00020400u,
	            "a live vehicle streams its matrix bit too: 0x20400")) return 1;
	// A wreck holds the kill bits in both halves.
	w::Entity *wreck = world.registry.get(vehicle_h);
	wreck->flags = w::kEntityFlagDead | w::kEntityFlagHusk;
	wreck->engine_flags = w::kEntityFlagDead | w::kEntityFlagHusk;
	if (!expect(record_of(vehicle_h, rec) && rec.entity_flags == 0x00000406u,
	            "a wreck streams its kill bits: 0x406")) return 1;
	std::printf("PASS pool1_flags_field\n");
	return 0;
}

std::vector<uint8_t> hex_bytes(const char *hex) {
	std::vector<uint8_t> out;
	const auto nibble = [](char c) {
		return static_cast<uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
	};
	for (; hex[0] != '\0' && hex[1] != '\0'; hex += 2)
		out.push_back(static_cast<uint8_t>((nibble(hex[0]) << 4) | nibble(hex[1])));
	return out;
}

// Two retail 0x0D item records of the committed load stream
// (fixtures/novaworld/run_20260426_120859/server_load_packets.nwmsg line 203, slots
// 0x1046 and 0x1048), rebuilt byte for byte from entity state holding retail's values:
// the live Flags dword (the matrix bit an object def's init sets), the entity+290
// byte, the placement heading and no AIData name. Then a whole live retail vehicle
// record (line 191, slot 0x101F), with the AI trailer's spawn x/y a vehicle that drove
// away keeps streaming.
// [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503940 — name @0x503A64..0x503ADF, Flags
//  @0x503AE1, byte @0x503D27..0x503D38, trailer @0x503D3D..0x503DAB, brain byte
//  @0x503E7F..0x503EC0; Entity_InitFromModel @0x40E10C..0x40E11E;
//  Entity_SpawnFromBMSRecord @0x40ED80/@0x40ED8C]
int pool1_fixture_bytes_impl() {
	w::World world;
	world.registry.configure_pool(1, 0x50);
	const auto place = [&](int slot, w::Entity item) {
		item.item_type_index = 1; // the trait sweep's row (D-NET-487)
		return world.registry.spawn_at(w::EntityHandle::make(1, slot), item);
	};
	w::Entity item;
	item.kind = w::EntityKind::Item;
	item.item_id = 0x00BF;
	item.has_item_def = true;
	item.item_type = 6;                         // an object def: no REFLECTABLE
	item.engine_flags = w::kEntityFlagMatrixBuilt; // its init's matrix bit 0x20000
	item.name = "never streamed";               // not an AIData def
	item.position = {1058.0f, 592.0f, 60.0f};
	item.heading = w::placed_heading_bam(60);                              // heading 30 deg: 0x15550000
	item.ammo_count = 0xFF;
	if (!expect(place(0x46, item).valid(), "item 0x1046 spawns")) return 1;
	item.position = {992.0f, -489.0f, static_cast<float>(0x00451200) / 65536.0f};
	item.heading = w::placed_heading_bam(0);                               // heading 90 deg: 0x40000000
	item.ammo_count = 0x00;
	if (!expect(place(0x48, item).valid(), "item 0x1048 spawns")) return 1;
	const std::vector<uint8_t> wire =
			opennova::encode_pool_spawn_batch(ns::build_pool1_spawn_batch(world));
	const std::vector<uint8_t> want = hex_bytes(
			"0200"
			"21004610bf000000000200000022040000500200003c0000005515ff"
			"21004810bf0000000002000000e003000017fe001245000000004000");
	if (!expect(wire == want, "the two retail item records rebuild byte for byte")) return 1;

	// A live retail d_buggy, whole record (line 191, slot 0x101F): its Flags
	// 0x20400 (the mover's matrix bit plus REFLECTABLE), heading 270 deg, team 2,
	// one empty passenger seat, the 0xFF byte, the AI trailer with its spawn x/y
	// and ai_textfile "d_buggy", and the brain byte 0x00 — every one of them
	// present exactly as retail gates it.
	w::World vworld;
	vworld.registry.configure_pool(1, 0x20);
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.item_id = 0x004A;
	vehicle.has_item_def = true;
	vehicle.item_type_index = 1; // the trait sweep's row (D-NET-487)
	vehicle.item_type = 1;
	vehicle.item_attrib = w::kItemAttribAIData;
	vehicle.is_ai_capable = true;
	vehicle.engine_flags = w::kEntityFlagMatrixBuilt; // its mover's per-tick bit
	vehicle.ammo_count = 0xFF;
	vehicle.position = {989.0f, 578.0f, static_cast<float>(0x003C032C) / 65536.0f};
	vehicle.spawn_position = vehicle.position;
	vehicle.heading = w::placed_heading_bam(180); // engine heading 270 deg: 0xC0000000
	vehicle.team = 2;
	w::Seat passenger;
	passenger.type = w::SeatType::Passenger;
	passenger.retail_slot = 0;
	vehicle.seats.push_back(passenger);
	vehicle.ai_text_file = "d_buggy";
	const w::EntityHandle vh = vworld.registry.spawn_at(w::EntityHandle::make(1, 0x1F), vehicle);
	if (!expect(vh.valid(), "the vehicle spawns")) return 1;
	vworld.ai.attach(vh); // the vehicle brain (entity+0x64): a parked hull's byte 0x00
	const std::vector<uint8_t> vwire =
			opennova::encode_pool_spawn_batch(ns::build_pool1_spawn_batch(vworld));
	const std::vector<uint8_t> vwant = hex_bytes(
			"0100"
			"311c1f104a0000000402000000dd03000042022c033c00000000c00201ffffffffffffff"
			"0000dd0300004202645f62756767790000");
	if (!expect(vwire == vwant, "the retail vehicle record rebuilds byte for byte")) return 1;
	// Driven away, it still streams where it spawned in the trailer.
	vworld.registry.get(vh)->position = {1100.0f, -600.0f, 61.0f};
	opennova::PoolSpawnBatch decoded;
	const std::vector<uint8_t> moved =
			opennova::encode_pool_spawn_batch(ns::build_pool1_spawn_batch(vworld));
	if (!expect(opennova::decode_pool_spawn_batch(moved.data(), moved.size(), decoded) &&
	                    decoded.records.size() == 1 && decoded.records[0].pos_x == 1100 * 65536 &&
	                    decoded.records[0].ai_profile_1 == 0x03DD0000u &&
	                    decoded.records[0].ai_profile_2 == 0x02420000u,
	            "the trailer keeps the spawn x/y after the vehicle moved")) return 1;

	// A retail tank's second addeweap child (line 206, slot 0x1058, type 0xB6 on the
	// M1A1 at 0x102A): its own matrix bit only (the carrier's Flags were copied
	// before the carrier's init set REFLECTABLE), the turret's live attitude, team
	// 1, the carrier as its target, the zero byte, the carrier's refNum 1 and its
	// slot index 1 as subType. The position is float-exact near the record's.
	// [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503940; Entity_SpawnAddEWeapChildren
	//  refNum @0x40F3B1, Flags @0x40F404, subType @0x40F40E]
	w::World cworld;
	cworld.registry.configure_pool(1, 0x60);
	w::Entity tank;
	tank.kind = w::EntityKind::Item;
	tank.item_id = 0x00A4;
	tank.has_item_def = true;  // the trait sweep's row (D-NET-487)
	tank.item_type_index = 1;
	const w::EntityHandle tank_h = cworld.registry.spawn_at(w::EntityHandle::make(1, 0x2A), tank);
	w::Entity gun;
	gun.kind = w::EntityKind::Item;
	gun.item_id = 0x00B6;
	gun.has_item_def = true;
	gun.item_type_index = 1; // the trait sweep's row (D-NET-487)
	gun.item_type = 6;
	gun.engine_flags = w::kEntityFlagMatrixBuilt;
	gun.position = {1148.75f, -278.5f, 75.0f};
	gun.veh.yaw_seeded = true;
	gun.veh.yaw_bam = 0x7FFF0090;
	gun.veh.air_pitch_bam = 0x044E02FB;
	gun.veh.air_roll_bam = 0x03E2D951;
	gun.team = 1;
	gun.ground_target = tank_h;
	gun.ref_num = 1;
	gun.sub_type = 1;
	const w::EntityHandle gun_h = cworld.registry.spawn_at(w::EntityHandle::make(1, 0x58), gun);
	if (!expect(tank_h.valid() && gun_h.valid(), "the tank and its gun spawn")) return 1;
	opennova::PoolSpawnBatch children = ns::build_pool1_spawn_batch(cworld);
	children.records.erase(std::remove_if(children.records.begin(), children.records.end(),
			[&](const opennova::PoolSpawnRecord &r) { return r.slot_id != gun_h.packed; }),
			children.records.end());
	const std::vector<uint8_t> cwire = opennova::encode_pool_spawn_batch(children);
	const std::vector<uint8_t> cwant = hex_bytes(
			"0100"
			"f7025810b600000000020000c07c040080e9fe00004b00"
			"9000ff7ffb024e0451d9e203012a10000101");
	if (!expect(cwire == cwant, "the retail child record's layout rebuilds byte for byte")) return 1;

	// The 0x10 static record streams the same live dword: the runtime word joins
	// the spawn-composed one. [orig: NetPacket_SerializePool2StaticToBuffer @0x5044E6]
	w::World sworld;
	sworld.registry.configure_pool(2, 4);
	w::Entity building;
	building.kind = w::EntityKind::Building;
	building.item_id = 0x0057;
	// Indestructible and its init's matrix bit, spawn-composed; the kill
	// bits in the runtime word.
	building.engine_flags = 0x04000000u | w::kEntityFlagMatrixBuilt;
	building.flags = w::kEntityFlagDead | w::kEntityFlagHusk;
	building.item_type_index = 1; // the trait sweep's row: not an empty slot
	building.has_item_def = true;
	sworld.registry.spawn(2, building);
	const std::vector<uint8_t> swire =
			opennova::encode_static_entity_batch(ns::build_pool2_static_batch(sworld));
	opennova::StaticEntityBatch statics;
	if (!expect(opennova::decode_static_entity_batch(swire.data(), swire.size(), statics) &&
	                    statics.records.size() == 1 &&
	                    statics.records[0].entity_flags == 0x04020006u,
	            "the 0x10 Flags field is the whole live dword")) return 1;
	std::printf("PASS pool1_fixture_bytes\n");
	return 0;
}

// The 0x20 page's start word is the pool cursor, a slot: with empty slots
// below the page boundary, every record of every page lands on the slot its
// start word plus its index names, and a zero word counts like a record
// toward the budget. Four freed slots size the first page so that the last of
// them is the record that crosses it (4 + 29 x 21 + 3 x 2 = 619 bytes before
// it, 621 with it, past 650 - 30): it ends the first page, and the second page
// starts on it again (D-NET-416, D-NET-397).
// [orig: NetPacket_SerializeEntityPoolToPacket @0x503460 -- the start word
//  @0x5034C5, the zero word @0x503673, the count @0x503687, the budget
//  @0x50368B..0x503694 (the cursor step @0x503696 skipped on the cross);
//  Server_SendInitialGameStateToPlayer @0x51BBA0, the call @0x51BD8E]
int pool3_page_start_impl() {
	w::World world;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 96);
	for (int i = 0; i < 80; ++i) {
		w::Entity m;
		m.kind = w::EntityKind::Marker;
		m.item_id = 6002; // a 21-byte record: type, flags, x/y/z, heading, handle
		m.position = {static_cast<float>(i), 20.0f, 1.0f};
		m.item_type_index = 1; // the trait sweep's row: not an empty slot
		m.has_item_def = true;
		m.net_id = static_cast<uint16_t>(0x100 + i); // its DcbId, the net word (D-NET-517)
		world.registry.spawn(3, m);
	}
	const std::vector<std::size_t> freed = {5, 10, 15, 32};
	for (const std::size_t slot : freed)
		world.registry.despawn(w::EntityHandle::make(3, static_cast<int>(slot)));
	ns::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
	                        inmatch::SocketMode::Socketless, /*host_key=*/0, &loopback);
	ctx.world = &world;
	inmatch::Server_InitNewRoundState(ctx);
	inmatch::Server_ProcessPendingPlayerSpawns(ctx, world);
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list[0];
	std::vector<std::vector<uint8_t>> pages;
	std::vector<inmatch::InitialStateMessage> emitted;
	const bool reached =
			inmatch::test::drive_host_own_burst_and_join(ctx, conn, /*now_tick=*/1, emitted);
	for (const auto &m : emitted)
		if (m.tag == 0x20) pages.push_back(m.body);
	if (!expect(reached && pages.size() >= 2, "the 80 pool-3 slots take more than one 0x20 page"))
		return 1;
	std::vector<opennova::Pool3SyncBatch> batches(pages.size());
	std::size_t zero_words = 0;
	for (std::size_t p = 0; p < pages.size(); ++p) {
		opennova::Pool3SyncBatch &batch = batches[p];
		if (!expect(opennova::decode_pool3_sync_batch(pages[p].data(), pages[p].size(), batch) &&
		                    pages[p].size() <= 650 + 30,
		            "each 0x20 page decodes within the budget"))
			return 1;
		for (std::size_t i = 0; i < batch.records.size(); ++i) {
			const opennova::Pool3SyncRecord &r = batch.records[i];
			const std::size_t slot = batch.start_index + i;
			if (r.is_empty_slot) {
				if (!expect(std::find(freed.begin(), freed.end(), slot) != freed.end(),
				            "a zero word is a freed slot"))
					return 1;
				++zero_words;
				continue;
			}
			if (!expect(r.net_handle == 0x100 + slot,
			            "a record's slot is the page's start word plus its index"))
				return 1;
		}
	}
	const opennova::Pool3SyncBatch &first = batches[0];
	if (!expect(first.start_index == 0 && pages[0].size() == 621 && first.records.size() == 33 &&
	                    first.records.back().is_empty_slot,
	            "the freed slot 32 is the zero word that crosses the first page's budget"))
		return 1;
	if (!expect(batches[1].start_index == 32 && !batches[1].records.empty() &&
	                    batches[1].records.front().is_empty_slot,
	            "the second page starts on that zero word"))
		return 1;
	if (!expect(zero_words == freed.size() + 1,
	            "each freed slot streams as a zero word, the crossing one twice"))
		return 1;
	std::printf("PASS pool3_page_start\n");
	return 0;
}

// The 0x0F location names come from the bring-up's install over the promoted
// world: each type-2044 marker the spawn admitted names itself by its own
// record's name_index (TDH_I3A's markers carry 0,1,4,3,5,2 in file order, 0
// keying LOCATION000 like any other), in spawn order, and a record without a
// text names itself by its key string. A refused record (its slot left empty)
// registers no name, so the count skips it (D-NET-511). Each name is copied
// whole into its 64-byte slot, so a 70-byte one runs on into the next slot,
// which the next name overwrites: the 0x0F carries that slot's first 64
// bytes and the next name, and a joiner's handler keeps 63 bytes of each
// (D-NET-657).
// [orig: Entity_SpawnFromBMSRecord @0x40F17C..0x40F20C, the copies @0x40F1D3..0x40F1DF /
//  @0x40F200..0x40F20A; the refusals @0x40EA94..0x40EAC2, @0x40EAD2..0x40EB05,
//  @0x40EB1C..0x40EB2E; NetPacket_WriteWorldStateLoad0x0F @0x502D10, the strlen
//  @0x502EA0..0x502EAC; NapiNPClientMsg_0x00F's bounded copy @0x42e538..0x42e584]
int location_names_by_name_index_impl() {
	w::World world;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 16);
	opennova::bms::File mission;
	const auto add_marker = [&](int32_t type_id, int32_t name_index, bool admitted) {
		opennova::bms::Entity record{};
		record.type_id = type_id;
		record.name_index = name_index;
		mission.markers.push_back(record);
		w::Entity m;
		m.kind = w::EntityKind::Marker;
		m.item_id = type_id;
		m.position = {static_cast<float>(mission.markers.size()), 20.0f, 1.0f};
		m.item_type_index = 1; // the trait sweep's row: not an empty slot
		m.has_item_def = true;
		// The record's slot is its index; a refused record leaves its slot empty.
		if (admitted)
			world.registry.spawn_at(
					w::EntityHandle::make(3, static_cast<int>(mission.markers.size() - 1)), m);
	};
	add_marker(6002, 0, true); // the host player's start
	for (const int32_t name_index : {0, 1, 4, 3, 5, 2}) add_marker(2044, name_index, true);
	add_marker(2044, 6, false); // refused at its spawn: no name, no count
	add_marker(2044, 9, true);  // no [Locations] text: the key string
	opennova::mission::MissionText text;
	text.loaded = true;
	text.table.sections.push_back({"Locations", 0});
	const std::string long_name = "Place 4 " + std::string(62, 'x'); // 70 bytes
	for (int32_t i = 0; i <= 6; ++i) {
		opennova::rtxt::Entry entry;
		entry.key = "LOCATION00" + std::to_string(i);
		entry.text = i == 4 ? long_name : "Place " + std::to_string(i);
		text.table.entries.push_back(entry);
		++text.table.sections.back().string_count;
	}

	ns::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
	                        inmatch::SocketMode::Socketless, /*host_key=*/0, &loopback);
	ctx.world = &world;
	inmatch::install_mission_location_names(ctx, world, mission, text);
	const std::vector<std::string> want = {"Place 0", "Place 1", long_name.substr(0, 64) + "Place 3",
	                                       "Place 3", "Place 5", "Place 2", "LOCATION009"};
	if (!expect(ctx.mission_location_names == want,
	            "the install keys each admitted marker by its record's name_index, as its slot reads"))
		return 1;
	inmatch::Server_InitNewRoundState(ctx);
	inmatch::Server_ProcessPendingPlayerSpawns(ctx, world);
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list[0];
	const std::vector<uint8_t> *body = nullptr;
	std::vector<inmatch::InitialStateMessage> emitted;
	const bool reached =
			inmatch::test::drive_host_own_burst_and_join(ctx, conn, /*now_tick=*/1, emitted);
	for (const inmatch::InitialStateMessage &m : emitted)
		if (m.tag == 0x0F) body = &m.body;
	opennova::WorldStateLoad state;
	if (!expect(reached && body != nullptr &&
	                    opennova::decode_world_state_load(body->data(), body->size(), state),
	            "the burst carries a decodable 0x0F"))
		return 1;
	if (!expect(state.team_name_count == want.size() && state.team_names == want,
	            "the 0x0F location names are the admitted markers' name_index texts in spawn order"))
		return 1;
	auto joiner_storage = std::make_unique<ns::ClientReplicaPipeline>();
	ns::ClientReplicaPipeline &joiner = *joiner_storage;
	joiner.apply(opennova::s2c::WORLD_STATE_LOAD, *body);
	std::vector<std::string> kept = want;
	kept[2] = long_name.substr(0, 63);
	if (!expect(joiner.state().location_names == kept,
	            "a joiner's 0x0F handler keeps each name to 63 bytes"))
		return 1;
	std::printf("PASS location_names_by_name_index\n");
	return 0;
}

// A host context brought up with no resolved MissionText location table (a
// tool or test embedder) names the 0x0F deploy-map labels from the promoted
// type-2044 markers' own labels, in spawn order.
int location_names_fall_back_to_marker_labels_impl() {
	w::World world;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 16);
	const auto add_marker = [&](int32_t type_id, const char *label) {
		w::Entity m;
		m.kind = w::EntityKind::Marker;
		m.item_id = type_id;
		m.name = label;
		m.position = {static_cast<float>(world.registry.live_count()), 20.0f, 1.0f};
		m.item_type_index = 1; // the trait sweep's row: not an empty slot
		m.has_item_def = true;
		world.registry.spawn(3, m);
	};
	add_marker(6002, "");
	add_marker(2044, "North Ridge");
	add_marker(2044, "South Pier");
	ns::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
	                        inmatch::SocketMode::Socketless, /*host_key=*/0, &loopback);
	ctx.world = &world;
	ctx.mission_location_names.clear();
	inmatch::Server_InitNewRoundState(ctx);
	inmatch::Server_ProcessPendingPlayerSpawns(ctx, world);
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list[0];
	const std::vector<uint8_t> *body = nullptr;
	std::vector<inmatch::InitialStateMessage> emitted;
	const bool reached =
			inmatch::test::drive_host_own_burst_and_join(ctx, conn, /*now_tick=*/1, emitted);
	for (const inmatch::InitialStateMessage &m : emitted)
		if (m.tag == 0x0F) body = &m.body;
	opennova::WorldStateLoad state;
	if (!expect(reached && body != nullptr &&
	                    opennova::decode_world_state_load(body->data(), body->size(), state),
	            "the fallback burst carries a decodable 0x0F"))
		return 1;
	const std::vector<std::string> want = {"North Ridge", "South Pier"};
	if (!expect(state.team_name_count == want.size() && state.team_names == want,
	            "the 0x0F names fall back to the location markers' labels in spawn order"))
		return 1;
	std::printf("PASS location_names_fall_back_to_marker_labels\n");
	return 0;
}
} // namespace

int main() {
	const int burst = main_impl();
	const int flags = pool1_flags_field_impl();
	const int bytes = pool1_fixture_bytes_impl();
	const int pool3 = pool3_page_start_impl();
	const int locations = location_names_by_name_index_impl();
	const int fallback = location_names_fall_back_to_marker_labels_impl();
	return burst != 0 ? burst
	                  : (flags != 0 ? flags
	                                : (bytes != 0 ? bytes
	                                              : (pool3 != 0 ? pool3 : (locations != 0 ? locations : fallback))));
}
