// The infantry compact record's ground carrier (D-NET-898).
//
// Retail's org1 record writer picks its carrier the way the player record does: the
// ridden vehicle (+0x16C) when mounted, else the entity the body stands on (+0x28, the
// resolver's ground probe hit: a deck, a building, a body). With a carrier the record
// writes seat bone 0 for an unmounted body, the carrier's handle and the position local
// to it, and the yaw byte of the target heading local to it. A soldier its vehicle's
// destruction kills is detached with +0x28 kept, so its first record still names the
// hull. [orig: NetPacket_SerializeInfantryEntityState @0x4C0320 - `mov ebp, [edi+28h]`
// @0x4C06F7, the +0x16C override @0x4C06EF..0x4C0708, the bone byte 0
// @0x4C071F..0x4C072E, the carrier test @0x4C0735..0x4C073E, the target-heading swap
// @0x4C07C9..0x4C07CD, Entity_TransformWorldToLocal @0x4C07E3]
//
// Drives the production world -> snapshot -> connection fan -> codec path.

#include "conn_fan_test_util.h"

#include <net/npwire/ingame_decode.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/replication/connection_fan.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include "../common/test_expect.h"

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;
namespace w = opennova::world;

using test_expect::expect;

constexpr int32_t fx(double units) {
	return static_cast<int32_t>(units * 65536.0);
}

constexpr uint16_t kVehicleType = 0x1004;

// One soldier standing at (102, 200.5, 11) and an unrotated hull at (100, 200, 10);
// `stand_on_hull` sets the soldier's ground entity (+0x28) to the hull. Fills `out` with
// the soldier's decoded record; false when it is absent.
bool emit_soldier_record(bool stand_on_hull, nw::FrameUpdateRecord &out,
                         uint16_t &hull_handle) {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 8);
	world->registry.configure_pool(1, 8);

	w::Entity hull;
	hull.kind = w::EntityKind::Item;
	hull.has_item_def = true;
	hull.item_type_index = 1;
	hull.item_id = kVehicleType;
	hull.position = {100.0f, 200.0f, 10.0f};
	hull.heading = w::placed_heading_bam(90); // engine heading 0: the identity rotation
	hull.health = 3000;
	hull.health_max = 3000;
	hull.net_class_code = static_cast<uint8_t>(nw::EntityClass::Vehicle);
	const w::EntityHandle hh = world->registry.spawn_from(1, 0, hull);
	if (!expect(hh.valid(), "hull spawned")) return false;
	hull_handle = hh.packed;

	w::Entity soldier;
	soldier.kind = w::EntityKind::Organic;
	soldier.item_id = 0x2000;
	soldier.net_class_code = static_cast<uint8_t>(nw::EntityClass::Infantry);
	soldier.position = {102.0f, 200.5f, 11.0f};
	soldier.heading = w::placed_heading_bam(90);
	const w::EntityHandle sh = world->registry.spawn(0, soldier);
	if (!expect(sh.valid(), "soldier spawned")) return false;
	world->ai.attach(sh);
	w::AiEntity *ae = world->ai.for_handle(sh);
	if (!expect(ae != nullptr, "soldier AI row attached")) return false;
	ae->inf.target_heading = 0x10000000; // the yaw byte's source (+0x1A8)
	w::Entity *live = world->registry.get(sh);
	if (!expect(live != nullptr && !live->mounted, "soldier resolvable and unmounted"))
		return false;
	if (stand_on_hull) live->ground_target = hh;

	ns::LoopbackChannel channel;
	std::vector<ns::Connection> conns;
	conns.push_back(ns::Connection{&channel, ns::TransportMode::Loopback, sh, 0});
	ns::test::emit_all(*world, conns);
	ns::Datagram dg;
	if (!expect(channel.client_recv(dg), "0x0A frame dequeued")) return false;
	auto classify = [](uint16_t type_id) {
		return type_id == kVehicleType ? nw::EntityClass::Vehicle : nw::EntityClass::Infantry;
	};
	nw::FrameUpdate frame;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), classify, frame),
	            "0x0A frame decodes")) return false;
	for (const nw::FrameUpdateRecord &record : frame.records) {
		if (record.handle == sh.packed) {
			out = record;
			return true;
		}
	}
	return expect(false, "soldier record present");
}

// An unmounted soldier standing on the hull: bone 0, the hull, the pose local to it.
bool test_a_soldier_on_a_hull_streams_carrier_local() {
	nw::FrameUpdateRecord rec;
	uint16_t hull = 0;
	if (!emit_soldier_record(/*stand_on_hull=*/true, rec, hull)) return false;
	bool ok = expect(rec.infantry.seat_bone_idx == 0, "an unmounted body writes bone 0");
	ok &= expect(rec.infantry.vehicle_slot_handle == hull,
	             "the record names the entity the body stands on (+0x28)");
	ok &= expect(nw::network_decompress_fixedpoint(rec.infantry.pos_x_compressed) == fx(2.0) &&
	                     nw::network_decompress_fixedpoint(rec.infantry.pos_y_compressed) ==
	                             fx(0.5) &&
	                     nw::network_decompress_fixedpoint(rec.infantry.pos_z_compressed) ==
	                             fx(1.0),
	             "the position is local to the carrier");
	// The target heading 0x10000000 less the carrier's 0, rounded high byte.
	ok &= expect(rec.infantry.yaw_byte == 0x10, "the yaw byte is the local target heading");
	return ok;
}

// The same soldier on the terrain (+0x28 null): the free-standing form.
bool test_a_soldier_on_the_terrain_streams_free() {
	nw::FrameUpdateRecord rec;
	uint16_t hull = 0;
	if (!emit_soldier_record(/*stand_on_hull=*/false, rec, hull)) return false;
	bool ok = expect(rec.infantry.seat_bone_idx == 0, "bone 0 on the terrain");
	ok &= expect(rec.infantry.vehicle_slot_handle == 0xFFFF, "no carrier on the terrain");
	return ok;
}

} // namespace

int main() {
	bool ok = true;
	ok &= test_a_soldier_on_a_hull_streams_carrier_local();
	ok &= test_a_soldier_on_the_terrain_streams_free();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
