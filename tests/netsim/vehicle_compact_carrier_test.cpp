// The §5.13 vehicle compact's carrier form and its vertical-velocity word, both
// sides of the wire [orig: Entity_SerializeVehicleState @0x460560].
//
// (a) HOST: the connection fan sends a vehicle resting on another entity (its
//     groundEntity, entity+0x28) as the carrier handle + CARRIER-LOCAL position
//     and heading — Entity_TransformWorldToLocal's out[3] = own - carrier — and a
//     free-standing vehicle as 0xFFFF + anchor-relative position + world heading
//     [orig: +0x28 load @0x460b4d, null test @0x460b56, handle @0x460ba1, the
//     transform @0x460bb6, local eulerZ @0x460c2a; the 0xFFFF leg @0x460c61].
//     The live tail's off-11 word is the vehicle's vertical velocity (entity+0xA0
//     slideDecay) compressed [orig: @0x460d5a..0x460d7b], never a "weapon X".
// (b) JOINER: the replica pipeline composes the parented form's position AND
//     heading against the carrier's live pose (Entity_TransformLocalToWorld's
//     out[3] = carrier[3] + local[3]) [orig: @0x4608ce / @0x43bd00 -> the
//     entity+576 store @0x4607f5], and lands the vertical velocity only from
//     records whose WIRE flags clear bit 0x02 (the dead-pose form lands 0)
//     [orig: @0x460910..0x46091e; the short form's 0 @0x460684].

#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/inmatch/loopback_channel.h>

#include "conn_fan_test_util.h"

#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <base/io/bam.h>
#include <runtime/world/angle.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr int64_t kBamPerDegree = 11930464; // 2^32 / 360
int32_t engine_heading_bam(int16_t mission_yaw) {
	return static_cast<int32_t>(static_cast<int64_t>(90 - mission_yaw) * kBamPerDegree);
}
int32_t fixed_of(double v) { return static_cast<int32_t>(v * 65536.0); }
double units_apart(int32_t ax, int32_t ay, int32_t az, int32_t bx, int32_t by, int32_t bz) {
	return std::sqrt(std::pow((ax - bx) / 65536.0, 2) + std::pow((ay - by) / 65536.0, 2) +
	                 std::pow((az - bz) / 65536.0, 2));
}
// Unsigned BAM distance, in 16.16-of-a-turn units of the wire's i16 heading.
uint32_t bam_distance(int32_t a, int32_t b) {
	const uint32_t d = uint32_t(a) - uint32_t(b);
	return d > 0x80000000u ? 0u - d : d;
}

w::PlayerSpawn player_spawn(w::Vec3 pos, int16_t yaw, uint16_t net_id) {
	w::PlayerSpawn s;
	s.position = pos;
	s.yaw = yaw;
	s.net_id = net_id;
	return s;
}

w::Entity vehicle_seed(uint16_t item_id, w::Vec3 pos, int16_t yaw) {
	w::Entity v;
	v.kind = w::EntityKind::Item;
	v.item_id = item_id;
	v.net_class_code = uint8_t(nw::EntityClass::Vehicle); // ai_function cveh/cbot
	v.health = 3000;
	v.health_max = 3000;
	v.position = pos;
	v.yaw = yaw;
	v.team = 1;
	return v;
}

// (a) The host fan: carrier form for a deck-carried vehicle, free-standing form otherwise,
// the vertical velocity compressed into the live tail's first word.
bool run_fan_carrier_and_vertical_velocity() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 16);
	const w::EntityHandle host_h =
			w::spawn_remote_player(world, player_spawn({100.0f, 100.0f, 10.0f}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "host player spawned")) return false;

	// The deck (an LCAC-like hull) at its own pose; the carried vehicle rests on it
	// (groundEntity = deck, the link vehicle_contact.cpp maintains from the ground
	// raycast) with a distinct offset and heading; a third vehicle sits on terrain.
	const w::EntityHandle deck_h =
			world.registry.spawn(1, vehicle_seed(0x0510, {150.0f, 120.0f, 2.0f}, 30));
	w::Entity carried = vehicle_seed(0x050B, {152.5f, 121.25f, 3.0f}, 75);
	carried.ground_target = deck_h;
	carried.veh.slide_z = -501; // the helicopter init seed [orig: @0x4620f3]
	const w::EntityHandle carried_h = world.registry.spawn(1, carried);
	w::Entity loose = vehicle_seed(0x050B, {130.0f, 110.0f, 10.0f}, 90);
	loose.veh.slide_z = 3 * 65536; // 3.0 u/tick climb
	const w::EntityHandle loose_h = world.registry.spawn(1, loose);
	if (!expect(deck_h.valid() && carried_h.valid() && loose_h.valid(), "three vehicles spawned"))
		return false;

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel ch;
	conns.push_back(ns::Connection{&ch, ns::TransportMode::Loopback, host_h, 0});
	ns::test::emit_all(world, conns);
	ns::Datagram dg;
	if (!expect(ch.client_recv(dg), "0x0A frame dequeued")) return false;
	nw::FrameUpdate fu;
	const auto resolver = [](uint16_t tid) {
		return tid == 0x14B9 ? nw::EntityClass::Player : nw::EntityClass::Vehicle;
	};
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), resolver, fu),
	            "0x0A frame decodes")) return false;

	const nw::FrameUpdateRecord *rec_carried = nullptr, *rec_loose = nullptr;
	for (const nw::FrameUpdateRecord &rec : fu.records) {
		if (rec.cls != nw::EntityClass::Vehicle) continue;
		if (rec.handle == carried_h.packed) rec_carried = &rec;
		if (rec.handle == loose_h.packed) rec_loose = &rec;
	}
	if (!expect(rec_carried != nullptr && rec_loose != nullptr,
	            "both vehicles ride the frame")) return false;
	bool ok = true;

	// The carried vehicle: the deck's handle, then its pose lifted back through the
	// deck's pose reproduces the world pose (carrier-LOCAL position and heading).
	ok &= expect(rec_carried->vehicle.parent_slot_handle == deck_h.packed,
	             "a vehicle with a groundEntity sends that carrier's handle, not 0xFFFF");
	ok &= expect(!rec_carried->vehicle.is_dead_pose, "a live hull streams the 21-B form");
	{
		const int32_t deck_bam = engine_heading_bam(30);
		const nw::WorldPose lifted = nw::network_transform_local_to_world(
				nw::network_decompress_fixedpoint(rec_carried->vehicle.pos_x_compressed),
				nw::network_decompress_fixedpoint(rec_carried->vehicle.pos_y_compressed),
				nw::network_decompress_fixedpoint(rec_carried->vehicle.pos_z_compressed),
				fixed_of(150.0), fixed_of(120.0), fixed_of(2.0), uint32_t(deck_bam), 0u, 0u);
		const double err = units_apart(lifted.x, lifted.y, lifted.z, fixed_of(152.5),
		                               fixed_of(121.25), fixed_of(3.0));
		std::printf("[fan] carried pos round-trip error %.4f u\n", err);
		ok &= expect(err < 0.05, "the carried position is CARRIER-LOCAL (lifts back to world)");
		// Heading word = (own - carrier + 0x8000) >> 16: composing it back onto the
		// deck heading reproduces the hull's world heading within the i16 rounding.
		const int32_t composed = opennova::io::bam_add(
				deck_bam, static_cast<int32_t>(rec_carried->vehicle.euler_z) * 65536);
		ok &= expect(bam_distance(composed, engine_heading_bam(75)) <= 0x8000u,
		             "the carried euler_z is the carrier-LOCAL heading (own - carrier)");
		// Not the world heading: 75 deg mission is 15 deg engine, 30 deg mission is 60
		// deg engine -> the local word is -45 deg, nowhere near the world +15 deg.
		ok &= expect(bam_distance(static_cast<int32_t>(rec_carried->vehicle.euler_z) * 65536,
		                          engine_heading_bam(75)) > 0x10000000u,
		             "the carried euler_z is not the world heading");
	}
	ok &= expect(rec_carried->vehicle.vertical_velocity == nw::network_compress_fixedpoint(-501),
	             "off-11 carries the vehicle's vertical velocity (entity+0xA0), compressed");

	// The free-standing vehicle: 0xFFFF, anchor-relative position, world heading.
	ok &= expect(rec_loose->vehicle.parent_slot_handle == 0xFFFF,
	             "a vehicle without a groundEntity takes the 0xFFFF leg");
	{
		const int32_t wx = fu.anchor_x +
				nw::network_decompress_fixedpoint(rec_loose->vehicle.pos_x_compressed);
		const int32_t wy = fu.anchor_y +
				nw::network_decompress_fixedpoint(rec_loose->vehicle.pos_y_compressed);
		const int32_t wz = fu.anchor_z +
				nw::network_decompress_fixedpoint(rec_loose->vehicle.pos_z_compressed);
		ok &= expect(units_apart(wx, wy, wz, fixed_of(130.0), fixed_of(110.0), fixed_of(10.0)) < 0.05,
		             "the free-standing position is anchor-relative");
		ok &= expect(bam_distance(static_cast<int32_t>(rec_loose->vehicle.euler_z) * 65536,
		                          engine_heading_bam(90)) <= 0x8000u,
		             "the free-standing euler_z is the world heading");
	}
	ok &= expect(rec_loose->vehicle.vertical_velocity == nw::network_compress_fixedpoint(3 * 65536),
	             "the free-standing tail carries its vertical velocity too");
	return ok;
}

// (c) The host fan reads the live dwords: a seeded hull's heading word is the
// rounded high half of its own BAM, never its whole-degree mirror, and a hull
// the death dispatch husked streams the 15-byte dead-pose form whose tail is its
// world roll then pitch, each rounded.
// [orig: Entity_SerializeVehicleState — eulerZ @0x460CEC..0x460D0A, the form
//  select @0x460D28, Roll @0x460D31, Pitch @0x460D52;
//  Entity_DispatchDeathCallback `or [edi+24h],edx` @0x493F63]
bool run_fan_full_precision_heading_and_wreck_pose() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 16);
	const w::EntityHandle host_h =
			w::spawn_remote_player(world, player_spawn({100.0f, 100.0f, 10.0f}, 0, 0xFFF0));
	w::Entity hull = vehicle_seed(0x050B, {130.0f, 110.0f, 10.0f}, 0);
	hull.veh.yaw_seeded = true;
	hull.veh.yaw_bam = 0x12345678;        // 25.6 deg engine heading, sub-degree
	hull.veh.air_pitch_bam = -0x0123ABCD; // the wreck's tilted attitude
	hull.veh.air_roll_bam = 0x09876543;
	hull.yaw = static_cast<int16_t>(std::lround(
			w::mission_yaw_deg_from_bam_heading(hull.veh.yaw_bam)));
	const w::EntityHandle hull_h = world.registry.spawn(1, hull);
	if (!expect(host_h.valid() && hull_h.valid(), "host and hull spawned")) return false;

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel ch;
	conns.push_back(ns::Connection{&ch, ns::TransportMode::Loopback, host_h, 0});
	const auto resolver = [](uint16_t tid) {
		return tid == 0x14B9 ? nw::EntityClass::Player : nw::EntityClass::Vehicle;
	};
	const auto hull_record = [&](nw::FrameUpdate &fu) -> const nw::FrameUpdateRecord * {
		ns::test::emit_all(world, conns);
		ns::Datagram dg;
		while (ch.client_recv(dg)) {
			if (dg.tag != nw::s2c::PER_FRAME_UPDATE) continue;
			fu = nw::FrameUpdate{};
			if (!nw::decode_frame_update(dg.body.data(), dg.body.size(), resolver, fu)) continue;
			for (const nw::FrameUpdateRecord &rec : fu.records)
				if (rec.cls == nw::EntityClass::Vehicle && rec.handle == hull_h.packed)
					return &rec;
		}
		return nullptr;
	};
	bool ok = true;
	nw::FrameUpdate live_frame;
	const nw::FrameUpdateRecord *live = hull_record(live_frame);
	ok &= expect(live != nullptr && !live->vehicle.is_dead_pose &&
	                     live->vehicle.euler_z == static_cast<int16_t>(0x1234),
	             "a seeded hull's heading word rounds its own BAM (0x12345678 -> 0x1234)");
	// The 0x0D spawn record carries the three dwords themselves and the 0x18
	// rebuild their truncated high words [orig: serialize_entity_pool_to_packet_0
	// @0x503B37, @0x503B53, @0x503B6F; serialize_object_to_buffer @0x505166 /
	// @0x505179].
	{
		const nw::PoolSpawnBatch batch = ns::build_pool1_spawn_batch(world);
		const nw::PoolSpawnRecord *spawn = nullptr;
		for (const nw::PoolSpawnRecord &r : batch.records)
			if (r.slot_id == hull_h.packed) spawn = &r;
		ok &= expect(spawn != nullptr && spawn->euler_z == 0x12345678 &&
		                     spawn->euler_x == -0x0123ABCD && spawn->euler_y == 0x09876543,
		             "the 0x0D spawn record carries the seeded hull's own BAM dwords");
		const nw::FullEntitySpawnRecord full =
				ns::build_full_entity_spawn(*world.registry.get(hull_h));
		ok &= expect(full.heading_hi == 0x1234 &&
		                     full.pitch_hi == static_cast<uint16_t>(uint32_t(-0x0123ABCD) >> 16),
		             "the 0x18 rebuild carries the same dwords' truncated high words");
	}

	w::entity_update_death_transforms(world, *world.registry.get(hull_h), /*silent=*/true);
	nw::FrameUpdate dead_frame;
	const nw::FrameUpdateRecord *dead = hull_record(dead_frame);
	ok &= expect(dead != nullptr && dead->vehicle.is_dead_pose &&
	                     (dead->vehicle.flags_byte & 0x06u) == 0x06u,
	             "a husked hull streams the dead-pose form with Flags 6");
	ok &= expect(dead != nullptr &&
	                     dead->vehicle.euler_y == static_cast<int16_t>(
	                             (uint32_t(0x09876543) + 0x8000u) >> 16) &&
	                     dead->vehicle.euler_x == static_cast<int16_t>(
	                             (uint32_t(-0x0123ABCD) + 0x8000u) >> 16),
	             "the dead-pose tail carries the wreck's rounded roll then pitch");
	return ok;
}

constexpr uint16_t kDeckHandle = 0x1002;
constexpr uint16_t kHullHandle = 0x1003;
constexpr uint16_t kVehType = 0x0248;

nw::FrameUpdate frame(int32_t ax, int32_t ay, int32_t az) {
	nw::FrameUpdate fu;
	fu.flags2 = 0;
	fu.mount_handle = 0xFFFF;
	fu.health = 100;
	fu.anchor_x = ax;
	fu.anchor_y = ay;
	fu.anchor_z = az;
	return fu;
}

nw::FrameUpdateRecord vehicle_record(uint16_t handle, uint16_t parent, int32_t lx, int32_t ly,
                                     int32_t lz, int16_t euler_z, uint8_t flags,
                                     uint16_t vertical_velocity) {
	nw::FrameUpdateRecord r;
	r.handle = handle;
	r.type_id = kVehType;
	r.cls = nw::EntityClass::Vehicle;
	r.vehicle.parent_slot_handle = parent;
	r.vehicle.pos_x_compressed = nw::network_compress_fixedpoint(lx);
	r.vehicle.pos_y_compressed = nw::network_compress_fixedpoint(ly);
	r.vehicle.pos_z_compressed = nw::network_compress_fixedpoint(lz);
	r.vehicle.euler_z = euler_z;
	r.vehicle.flags_byte = flags;
	r.vehicle.is_dead_pose = (flags & nw::kVehicleCompactFlagDeadPose) != 0;
	r.vehicle.health_word = 100;
	r.vehicle.vertical_velocity = vertical_velocity;
	return r;
}

// (b) The joiner pipeline: the parented form composes position AND heading; the
// vertical velocity lands under the wire-flag bit-0x02 gate.
bool run_pipeline_parented_form() {
	ns::ClientReplicaPipeline view;
	view.set_remote_motion_mode(true);
	view.set_item_class_resolver([](uint16_t type) -> ns::ClientReplicaPipeline::ItemClassResolution {
		if (type == kVehType) return nw::EntityClass::Vehicle;
		return std::nullopt;
	});

	const int32_t deck_x = fixed_of(100.0), deck_y = fixed_of(200.0), deck_z = fixed_of(10.0);
	const int32_t deck_heading = 0x20000000; // 45 deg
	{
		nw::PoolSpawnBatch batch;
		nw::PoolSpawnRecord deck;
		deck.slot_id = kDeckHandle;
		deck.item_type_id = kVehType;
		deck.entity_name = "LCAC";
		deck.pos_x = deck_x;
		deck.pos_y = deck_y;
		deck.pos_z = deck_z;
		deck.euler_z = deck_heading;
		batch.records.push_back(deck);
		nw::PoolSpawnRecord hull;
		hull.slot_id = kHullHandle;
		hull.item_type_id = kVehType;
		hull.entity_name = "Drivable Dune Buggy";
		hull.pos_x = deck_x;
		hull.pos_y = deck_y;
		hull.pos_z = deck_z + fixed_of(1.0);
		hull.euler_z = 0;
		batch.records.push_back(hull);
		view.apply(nw::s2c::POOL_SPAWN, nw::encode_pool_spawn_batch(batch));
	}
	// Rows are re-found after every apply: the view owns them by value.
	const auto deck_row = [&]() { return view.state().find(kDeckHandle); };
	const auto hull_row = [&]() { return view.state().find(kHullHandle); };
	if (!expect(deck_row() != nullptr && hull_row() != nullptr, "both 0x0D rows decode"))
		return false;
	bool ok = true;

	// Frame 1: the deck free-standing at its spawn pose; the hull parented to it with
	// a (2, 0.5, 1) local offset and a +22.5 deg LOCAL heading.
	const int32_t lx = fixed_of(2.0), ly = fixed_of(0.5), lz = fixed_of(1.0);
	const int16_t local_hi = 0x1000; // 0x10000000 = 22.5 deg
	const uint16_t vz_wire = nw::network_compress_fixedpoint(-501);
	{
		nw::FrameUpdate fu = frame(deck_x, deck_y, deck_z);
		fu.records.push_back(vehicle_record(kDeckHandle, 0xFFFF, 0, 0, 0,
		                                    int16_t(deck_heading >> 16), 0, 0));
		fu.records.push_back(vehicle_record(kHullHandle, kDeckHandle, lx, ly, lz, local_hi, 0,
		                                    vz_wire));
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	}
	const ns::ClientEntityState *deck = deck_row();
	const ns::ClientEntityState *hull = hull_row();
	ok &= expect(hull->carrier_handle == kDeckHandle && hull->net_seat_valid,
	             "the hull row carries the deck as its per-record carrier");
	{
		const nw::WorldPose posed = nw::network_transform_local_to_world(
				lx, ly, lz, deck->x, deck->y, deck->z, uint32_t(deck->heading_bam),
				uint32_t(deck->pitch_bam), uint32_t(deck->roll_bam));
		const double err = units_apart(hull->x, hull->y, hull->z, posed.x, posed.y, posed.z);
		std::printf("[pipeline] parented pos error %.4f u\n", err);
		ok &= expect(err < 0.05, "the parented position composes against the deck pose");
	}
	const int32_t expected_heading =
			opennova::io::bam_add(deck->heading_bam, static_cast<int32_t>(local_hi) * 65536);
	ok &= expect(hull->heading_bam == expected_heading,
	             "the parented heading is carrier + local, not the raw wire euler");
	ok &= expect(hull->heading_bam != static_cast<int32_t>(local_hi) * 65536,
	             "the raw local euler never lands as the world heading");
	ok &= expect(hull->yaw_byte == uint8_t(uint32_t(expected_heading) >> 24),
	             "the coarse yaw byte follows the composed heading");
	ok &= expect(hull->net_smooth_heading == expected_heading,
	             "the staged heading slot stays coherent with the landed pose");
	ok &= expect(hull->vehicle_vertical_velocity == nw::network_decompress_fixedpoint(vz_wire) &&
	                     hull->vehicle_vertical_velocity_pending,
	             "a live record (wire bit 0x02 clear) lands the vertical velocity");

	// The joiner's mover consumed the landing; a wire-dead live-form record (bit 0x02
	// set) must leave the register alone [orig: `and esi,2; jnz` @0x460911..0x460918].
	view.state().find(kHullHandle)->vehicle_vertical_velocity_pending = false;
	{
		nw::FrameUpdate fu = frame(deck_x, deck_y, deck_z);
		fu.records.push_back(vehicle_record(kHullHandle, kDeckHandle, lx, ly, lz, local_hi, 0x02,
		                                    nw::network_compress_fixedpoint(7 * 65536)));
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	}
	hull = hull_row();
	ok &= expect(hull->vehicle_vertical_velocity == nw::network_decompress_fixedpoint(vz_wire) &&
	                     !hull->vehicle_vertical_velocity_pending,
	             "a wire-dead record (bit 0x02) does not land the vertical velocity");

	// The dead-pose short form (bit 0x04, bit 0x02 clear) lands 0 [orig: @0x460684].
	{
		nw::FrameUpdate fu = frame(deck_x, deck_y, deck_z);
		fu.records.push_back(vehicle_record(kHullHandle, kDeckHandle, lx, ly, lz, local_hi,
		                                    nw::kVehicleCompactFlagDeadPose, 0));
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	}
	hull = hull_row();
	ok &= expect(hull->vehicle_vertical_velocity == 0 && hull->vehicle_vertical_velocity_pending,
	             "the dead-pose form lands a zero vertical velocity");

	// The per-tick recompose follows the deck's LIVE heading: turn the deck and let the
	// chase run; the hull's world heading tracks deck + local every tick.
	{
		nw::FrameUpdate fu = frame(deck_x, deck_y, deck_z);
		fu.records.push_back(vehicle_record(kDeckHandle, 0xFFFF, 0, 0, 0, 0x6000, 0, 0));
		fu.records.push_back(vehicle_record(kHullHandle, kDeckHandle, lx, ly, lz, local_hi, 0,
		                                    vz_wire));
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	}
	bool tracked = true;
	for (int t = 0; t < 64; ++t) {
		view.tick_remote_motion(/*self_handle=*/0xFFFF);
		deck = deck_row();
		hull = hull_row();
		tracked = tracked &&
				hull->heading_bam == opennova::io::bam_add(deck->heading_bam,
				                                           static_cast<int32_t>(local_hi) * 65536);
	}
	ok &= expect(tracked, "the carried hull's heading recomposes onto the deck's live heading");
	ok &= expect(deck_row()->heading_bam != deck_heading, "the deck actually turned");
	return ok;
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_fan_carrier_and_vertical_velocity();
	ok &= run_fan_full_precision_heading_and_wreck_pose();
	ok &= run_pipeline_parented_form();
	if (!ok || failures != 0) {
		std::fprintf(stderr, "vehicle_compact_carrier_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("vehicle_compact_carrier_test: OK\n");
	return 0;
}
