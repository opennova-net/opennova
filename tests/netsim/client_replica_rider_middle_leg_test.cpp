// D-NET-196 (L5-s14 row 2, the remainder's item 1): a joiner's seated pool-0 rider takes its
// carrier at its own visit in the World's pool-0 walk, not in ClientRuntime's leg. Retail's
// pool-1 walk moves the carrier ahead of the pool-0 walk, and the rider's own update poses
// it on the carrier at its mounted site, so until the rider's slot comes up it holds the
// pose its last tick left (the pose the proximity table copies). A rider below the own
// slot follows in the middle leg, ahead of the own body; one past it follows in the
// after-self leg, behind the own body. Without the own-body split nothing parks, and
// ClientRuntime's leg still recomposes every rider after the movers.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9, the
//  proximity build @0x4C240A, the pool-0 walk @0x4C2426..0x4C2472;
//  Entity_UpdateInfantryPlayerBody @0x4B40E0 - the site selector on the mount word +0x16C
//  @0x4B637C..0x4B638F, the kind-3 Entity_AttachToBoneAndUpdateTransform call @0x4B63C7,
//  every other kind's bone pose through the branch @0x4B64EF to the
//  Entity_GetBoneTransformAndOrientation call @0x4B6510]

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>
#include "../common/test_expect.h"

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;

constexpr uint16_t kHull = 0x1006;      // pool 1: the carrier, a chased hull
constexpr uint16_t kLowRider = 0x0003;  // pool 0, seated, below the own slot
constexpr uint16_t kSelf = 0x0005;      // the own row
constexpr uint16_t kHighRider = 0x0007; // pool 0, seated, past the own slot
constexpr uint16_t kHullType = 0x0248;
constexpr uint16_t kPlayerType = 0x14B9;

int &failures = test_expect::failures;
using test_expect::expect;

int32_t fixed_of(double v) { return static_cast<int32_t>(v * 65536.0); }

// The seat-local samples, as the record's compressed words decompress.
int32_t local_x(double units) {
	return nw::network_decompress_fixedpoint(nw::network_compress_fixedpoint(fixed_of(units)));
}

// One 0x0A from the anchor (100, 200, 10): the hull free-standing at hull_x units (y 200,
// z 10, Euler 0) and the own row free-standing, plus, when `riders` is set, the two riders
// seated on the hull (bone 1) at the local x +1 / -1.
std::vector<uint8_t> frame(double hull_x, bool riders) {
	nw::FrameUpdate fu;
	fu.flags2 = 0;
	fu.carried_handle = 0xFFFF;
	fu.health = 100;
	fu.anchor_x = fixed_of(100.0);
	fu.anchor_y = fixed_of(200.0);
	fu.anchor_z = fixed_of(10.0);
	nw::FrameUpdateRecord hull;
	hull.handle = kHull;
	hull.type_id = kHullType;
	hull.cls = nw::EntityClass::Vehicle;
	hull.vehicle.parent_slot_handle = 0xFFFF;
	hull.vehicle.pos_x_compressed = nw::network_compress_fixedpoint(fixed_of(hull_x - 100.0));
	hull.vehicle.pos_y_compressed = nw::network_compress_fixedpoint(0);
	hull.vehicle.pos_z_compressed = nw::network_compress_fixedpoint(0);
	hull.vehicle.euler_z = 0;
	hull.vehicle.flags_byte = 0; // the live form
	hull.vehicle.is_dead_pose = false;
	hull.vehicle.health_word = 100;
	fu.records.push_back(hull);
	const auto player = [](uint16_t handle, uint16_t carrier, int32_t x) {
		nw::FrameUpdateRecord rec;
		rec.handle = handle;
		rec.type_id = kPlayerType;
		rec.cls = nw::EntityClass::Player;
		rec.player.carrier_handle = carrier;
		rec.player.vehicle_bone = carrier != 0xFFFF ? 1 : 0;
		rec.player.pos_x_compressed = nw::network_compress_fixedpoint(x);
		rec.player.pos_y_compressed = nw::network_compress_fixedpoint(0);
		rec.player.pos_z_compressed = nw::network_compress_fixedpoint(0);
		rec.player.yaw_byte = 0x40;
		return rec;
	};
	fu.records.push_back(player(kSelf, 0xFFFF, fixed_of(30.0)));
	if (riders) {
		fu.records.push_back(player(kLowRider, kHull, fixed_of(1.0)));
		fu.records.push_back(player(kHighRider, kHull, fixed_of(-1.0)));
	}
	return nw::encode_frame_update(fu);
}

// The hull's pose words at one moment.
struct Pose {
	int32_t x = 0, y = 0, z = 0;
	int32_t heading = 0, pitch = 0, roll = 0;
};
Pose pose_of(const ns::ClientEntityState &row) {
	return Pose{row.x, row.y, row.z, row.heading_bam, row.pitch_bam, row.roll_bam};
}

// The rider's pose on the hull's pose.
bool on_hull(const ns::ClientEntityState &rider, const Pose &hull, int32_t lx) {
	const nw::WorldPose posed = nw::network_transform_local_to_world(lx, 0, 0, hull.x, hull.y,
			hull.z, static_cast<uint32_t>(hull.heading), static_cast<uint32_t>(hull.pitch),
			static_cast<uint32_t>(hull.roll));
	return rider.x == posed.x && rider.y == posed.y && rider.z == posed.z;
}

struct View {
	ns::ClientReplicaPipeline view;
	explicit View(bool split) {
		view.set_remote_motion_mode(true);
		view.set_self_body_split(split);
		view.set_item_class_resolver([](uint16_t type)
				-> ns::ClientReplicaPipeline::ItemClassResolution {
			if (type == kHullType) return nw::EntityClass::Vehicle;
			if (type == kPlayerType) return nw::EntityClass::Player;
			return std::nullopt;
		});
		// The spawn stream's rows, typed (a compact record never creates one).
		view.state().upsert(kHull).type_id = kHullType;
		for (const uint16_t handle : {kLowRider, kSelf, kHighRider})
			view.state().upsert(handle).type_id = kPlayerType;
		// The hull at 50 with its two riders, settled over three full ticks.
		for (int i = 0; i < 3; ++i) {
			view.apply(0x0A, frame(50.0, true));
			tick();
		}
	}
	void tick() {
		view.tick_remote_motion(kSelf);
		view.tick_remote_motion_through_self();
		view.tick_remote_motion_after_self();
	}
	const ns::ClientEntityState &row(uint16_t handle) { return *view.state().find(handle); }
	bool rows_present() {
		return view.state().find(kHull) != nullptr && view.state().find(kLowRider) != nullptr &&
				view.state().find(kHighRider) != nullptr && view.state().find(kSelf) != nullptr;
	}
};

void test_a_parked_rider_follows_its_carrier_at_its_own_visit() {
	View v(/*split=*/true);
	if (!expect(v.rows_present(), "the hull, the own row and both riders are decoded")) return;
	const int32_t low_local = local_x(1.0);
	const int32_t high_local = local_x(-1.0);
	expect(v.row(kLowRider).net_seat_valid && v.row(kHighRider).net_seat_valid,
			"both riders hold a resolved seat sample");
	const Pose hull_before = pose_of(v.row(kHull));
	expect(on_hull(v.row(kLowRider), hull_before, low_local) &&
					on_hull(v.row(kHighRider), hull_before, high_local),
			"both riders sit on the hull as the settled ticks left it");
	// The hull's next record moves it 10 units: past the snap distance, so the pool-1
	// visit in ClientRuntime's leg lands it on the sample. The riders send no record.
	v.view.apply(0x0A, frame(60.0, false));
	v.view.tick_remote_motion(kSelf);
	const Pose hull_after = pose_of(v.row(kHull));
	expect(hull_after.x != hull_before.x, "the hull moved in ClientRuntime's leg");
	expect(on_hull(v.row(kLowRider), hull_before, low_local),
			"the low rider waits for its slot: the pose its last tick left");
	expect(on_hull(v.row(kHighRider), hull_before, high_local),
			"the high rider waits for its slot: the pose its last tick left");
	// The World's entity pass: the middle leg ahead of its pool-0 walk.
	v.view.tick_remote_motion_through_self();
	expect(on_hull(v.row(kLowRider), hull_after, low_local),
			"the low rider takes the moved hull at its own visit, ahead of the own body");
	expect(on_hull(v.row(kHighRider), hull_before, high_local),
			"the high rider still waits past the own body's slot");
	v.view.tick_remote_motion_after_self();
	expect(on_hull(v.row(kHighRider), hull_after, high_local),
			"the high rider takes the moved hull at its own visit, behind the own body");
}

void test_without_the_split_the_client_leg_recomposes_every_rider() {
	View v(/*split=*/false);
	if (!expect(v.rows_present(), "the hull, the own row and both riders are decoded")) return;
	v.view.apply(0x0A, frame(60.0, false));
	v.view.tick_remote_motion(kSelf);
	const Pose hull = pose_of(v.row(kHull));
	expect(hull.x != fixed_of(50.0), "the hull moved in ClientRuntime's leg");
	expect(on_hull(v.row(kLowRider), hull, local_x(1.0)) &&
					on_hull(v.row(kHighRider), hull, local_x(-1.0)),
			"no row parks: the leg's post-mover recompose poses both riders on the moved hull");
}

} // namespace

int main() {
	test_a_parked_rider_follows_its_carrier_at_its_own_visit();
	test_without_the_split_the_client_leg_recomposes_every_rider();
	if (failures != 0) {
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("client_replica_rider_middle_leg: ok\n");
	return 0;
}
