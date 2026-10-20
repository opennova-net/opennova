// D-NET-NEW-L6-2: an entity's Yaw (+0x10) is one BAM32 word retail keeps per entity. A
// placed row's word is ((90 - yaw) << 16) / 360 << 16 with a truncating signed divide, so
// its low half is zero [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66], and every
// reader reads that word: the spawn-state reset's heading [orig: Entity_ResetToSpawnState
// @0x4B9610 - `mov eax, [esi+10h]` @0x4B964A, spawnHeading +0x330 @0x4B965C], the 0x0C
// organic record [orig: NetPacket_SerializeEntityStatesToBuffer @0x5030A0], the 0x0D and
// 0x10 euler [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503940;
// NetPacket_SerializePool2StaticToBuffer @0x5042F0], the 0x20 marker word
// [orig: NetPacket_SerializeEntityPoolToPacket @0x503460], the 0x0F's high half
// [orig: NetPacket_WriteWorldStateLoad0x0F `movzx edx, word ptr [ebp+12h]` @0x502D6D] and the
// fire origin's carrier test [orig: Server_ClientFiredRound @0x50C379..0x50C3D4]. Ours
// rebuilt the word at each reader from a whole-degree mirror through a wrapped floor: yaw
// 100 gave 0xF8E38E38 (one lower in the high half), yaw 80 0x071C71C7 (the low half). The
// expected words below are retail's arithmetic; yaw 135 (90 - yaw = -45, a whole eighth
// turn) is the row where both conversions agree.

#include "conn_fixture.h"
#include "host_test_setup.h"

#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <formats/mission/bms.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <runtime/mission/promote.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/weapon_fire_gate.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;
namespace bms = opennova::bms;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// Retail's placed words: (90 - 100) << 16 = -655360, / 360 = -1820 (truncated), << 16;
// (90 - 135) << 16 / 360 = -8192 exactly; (90 - 80) << 16 / 360 = 1820.
constexpr int32_t kYaw100 = static_cast<int32_t>(0xF8E40000u);
constexpr int32_t kYaw135 = static_cast<int32_t>(0xE0000000u);
constexpr int32_t kYaw80 = 0x071C0000;

bms::Entity record(bms::ItemType type, int32_t type_id, int32_t id, int16_t yaw) {
	bms::Entity e{};
	e.type = type;
	e.type_id = type_id;
	e.id = id;
	e.x = (300 << 16) | 0x1234;
	e.y = (400 << 16) | 0x0567;
	e.z = (20 << 16) | 0x0089;
	e.yaw = yaw;
	return e;
}

void with_def(w::World &world, w::EntityHandle h) {
	w::Entity *e = world.registry.get(h);
	e->item_type_index = 1;
	e->has_item_def = true;
}

// Promotion stores the placed word, the AI slot and the reset read it, and every load
// stream carries it.
int placed_rows_carry_the_word() {
	bms::File m{};
	m.organics.push_back(record(bms::ItemType::Organic, 0, 31, 100));
	m.items.push_back(record(bms::ItemType::Item, 164, 21, 100));
	m.items.push_back(record(bms::ItemType::Item, 164, 22, 135));
	m.buildings.push_back(record(bms::ItemType::Building, 700, 41, 100));
	m.markers.push_back(record(bms::ItemType::Marker, 6001, 51, 80));

	auto world = std::make_unique<w::World>();
	opennova::mission::promote_mission(m, *world, {});
	const w::EntityHandle organic = world->registry.find_by_net_id(31);
	const w::EntityHandle item100 = world->registry.find_by_net_id(21);
	const w::EntityHandle item135 = world->registry.find_by_net_id(22);
	const w::EntityHandle building = world->registry.find_by_net_id(41);
	const w::EntityHandle marker = world->registry.find_by_net_id(51);
	if (!expect(organic.valid() && item100.valid() && item135.valid() && building.valid() &&
	                    marker.valid(),
	            "promotion places each row"))
		return 1;
	if (!expect(world->registry.get(organic)->heading == kYaw100 &&
	                    world->registry.get(item100)->heading == kYaw100 &&
	                    world->registry.get(item135)->heading == kYaw135 &&
	                    world->registry.get(building)->heading == kYaw100 &&
	                    world->registry.get(marker)->heading == kYaw80,
	            "promotion stores each row's placed Yaw word"))
		return 1;
	w::AiEntity *ai = world->ai.for_handle(organic);
	if (!expect(ai != nullptr && ai->heading == kYaw100, "the AI slot starts at the word"))
		return 1;

	// The spawn-state reset reads the word into the body and the spawn backup.
	ai->heading = 0x12345678;
	w::Entity &o = *world->registry.get(organic);
	w::entity_reset_to_spawn_state(*world, world->ai, o);
	if (!expect(ai->heading == kYaw100 && ai->inf.target_heading == kYaw100 &&
	                    o.spawn_heading == kYaw100,
	            "the reset takes the word, not a rebuild"))
		return 1;

	for (const w::EntityHandle h : {organic, item100, item135, building, marker}) with_def(*world, h);
	world->registry.get(organic)->item_type = 3; // a person def: the 0x0C's full form

	// 0x0C
	const std::vector<uint8_t> c_wire =
			opennova::encode_organic_spawn_batch(ns::build_pool0_organic_batch(*world));
	opennova::OrganicSpawnBatch c;
	if (!expect(opennova::decode_organic_spawn_batch(c_wire.data(), c_wire.size(), c) &&
	                    c.records.size() == 1 && c.records[0].orientation == kYaw100,
	            "the 0x0C streams the organic's word"))
		return 1;
	// 0x0D
	const std::vector<uint8_t> d_wire =
			opennova::encode_pool_spawn_batch(ns::build_pool1_spawn_batch(*world));
	opennova::PoolSpawnBatch d;
	if (!expect(opennova::decode_pool_spawn_batch(d_wire.data(), d_wire.size(), d) &&
	                    d.records.size() == 2 && d.records[0].euler_z == kYaw100 &&
	                    d.records[1].euler_z == kYaw135,
	            "the 0x0D streams each item's word"))
		return 1;
	// 0x10
	const std::vector<uint8_t> s_wire =
			opennova::encode_static_entity_batch(ns::build_pool2_static_batch(*world));
	opennova::StaticEntityBatch s;
	if (!expect(opennova::decode_static_entity_batch(s_wire.data(), s_wire.size(), s) &&
	                    s.records.size() == 1 && s.records[0].euler_z == kYaw100,
	            "the 0x10 streams the building's word"))
		return 1;
	// 0x20
	const std::vector<uint8_t> p_wire =
			opennova::encode_pool3_sync_batch(ns::build_pool3_marker_batch(*world));
	opennova::Pool3SyncBatch p;
	if (!expect(opennova::decode_pool3_sync_batch(p_wire.data(), p_wire.size(), p) &&
	                    p.records.size() == 1 && p.records[0].movement_val == static_cast<uint32_t>(kYaw80),
	            "the 0x20 streams the marker's word"))
		return 1;
	// The 0x0A seam reads the same word.
	if (!expect(ns::snapshot_of(*world->registry.get(item100)).euler_z == kYaw100,
	            "the 0x0A snapshot reads the word"))
		return 1;
	std::printf("PASS placed_rows_carry_the_word\n");
	return 0;
}

// A placement copies the start marker's word onto the player; the 0x0F streams its high
// half (0xF8E4; the rebuild sent 0xF8E3).
int placement_streams_the_word() {
	auto world_storage = std::make_unique<w::World>();
	w::World &world = *world_storage;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 16);
	w::Entity start;
	start.kind = w::EntityKind::Marker;
	start.item_id = 6002; // the host player's start
	start.position.set_q16({(300 << 16) | 0x1234, (400 << 16) | 0x0567, 20 << 16});
	start.heading = w::placed_heading_bam(100);
	start.item_type_index = 1;
	start.has_item_def = true;
	if (!expect(world.registry.spawn_at(w::EntityHandle::make(3, 0), start).valid(),
	            "the start marker spawns"))
		return 1;

	ns::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
	                             inmatch::SocketMode::Socketless, /*host_key=*/0, &loopback);
	ctx.world = &world;
	inmatch::Server_InitNewRoundState(ctx);
	inmatch::Server_ProcessPendingPlayerSpawns(ctx, world);
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list[0];
	const w::Entity *player = world.registry.get(conn.link.owned_entity);
	const w::AiEntity *body = world.ai.for_handle(conn.link.owned_entity);
	if (!expect(player != nullptr && player->heading == kYaw100 && body != nullptr &&
	                    body->heading == kYaw100,
	            "the placement copies the marker's word onto the player and its body"))
		return 1;

	std::vector<inmatch::InitialStateMessage> emitted;
	const bool reached =
			inmatch::test::drive_host_own_burst_and_join(ctx, conn, /*now_tick=*/1, emitted);
	const std::vector<uint8_t> *load = nullptr;
	for (const inmatch::InitialStateMessage &msg : emitted)
		if (msg.tag == 0x0F) load = &msg.body;
	opennova::WorldStateLoad state;
	if (!expect(reached && load != nullptr &&
	                    opennova::decode_world_state_load(load->data(), load->size(), state) &&
	                    static_cast<uint16_t>(state.yaw) == 0xF8E4u,
	            "the 0x0F streams the word's high half"))
		return 1;
	std::printf("PASS placement_streams_the_word\n");
	return 0;
}

// The fire origin's carrier test compares the carrier's live Yaw / Pitch / Roll words
// with its savedLivePose words: a tilted hull that has not moved since its mover's stamp
// keeps the reach check (a far origin is refused, -17); a turn of its Yaw word skips it.
// The shifted whole-degree mirrors never equalled a stamped word, so ours skipped the
// check for every stamped carrier.
int fire_origin_reads_the_carrier_words() {
	auto world_storage = std::make_unique<w::World>();
	w::World &world = *world_storage;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	w::Entity hull;
	hull.kind = w::EntityKind::Item;
	hull.heading = kYaw100;
	hull.veh.yaw_seeded = true;
	hull.veh.yaw_bam = kYaw100;
	hull.veh.air_pitch_bam = 0x01000000; // a hull resting tilted
	const w::EntityHandle hull_handle = world.registry.spawn(1, hull);
	w::Entity *carrier = world.registry.get(hull_handle);
	w::stamp_saved_live_pose(*carrier);
	w::Entity rider;
	rider.kind = w::EntityKind::Organic;
	rider.flags = w::kEntityFlagPlayer;
	rider.ground_target = hull_handle;
	const w::Entity &owner = *world.registry.get(world.registry.spawn(0, rider));
	w::WeaponTableEntry weapon{};
	const w::FixedVec3 origin{10 * 65536, 0, 0}; // 10 u: past the 2 u reach
	if (!expect(w::weapon_fire_origin_status(world, owner, weapon, origin, false) == -17,
	            "an unmoved carrier keeps the reach check"))
		return 1;
	carrier->veh.yaw_bam = static_cast<int32_t>(static_cast<uint32_t>(kYaw100) + 0x10000u);
	if (!expect(w::weapon_fire_origin_status(world, owner, weapon, origin, false) == 0,
	            "a turned carrier skips it"))
		return 1;
	std::printf("PASS fire_origin_reads_the_carrier_words\n");
	return 0;
}

} // namespace

int main() {
	const int placed = placed_rows_carry_the_word();
	const int placement = placement_streams_the_word();
	const int fire = fire_origin_reads_the_carrier_words();
	for (const int r : {placed, placement, fire})
		if (r != 0) return r;
	return 0;
}
