// D-INF-35 (the vehicle-carrier half): a vehicle model's bone callback hands every bone
// slot the carrier's own orientation matrix, so a sitex / ctrlx / drvrx seat bone is the
// seat userpoint through that one Q22 matrix: the position words transformed, the heading
// the fpatan of the rotated direction words truncated toward zero, the pitch and roll the
// matrix's own eulers. The values below are minted by hand from that arithmetic.
// [orig: EntityDef_LoadModelsAndCallbacks @0x439F50 (BoneCallback_Simple for a vehicle
//  @0x43A072); BoneCallback_Simple @0x4E2600 (@0x4E267E..0x4E26B7);
//  Entity_GetBoneTransformAndOrientation @0x4B0C50 - Math_FixedPointTransformPoint22
//  @0x4B0D42, Math_TransformPointFixedPoint22 @0x4B0D64, the fpatan @0x4B0D74 and its
//  store @0x4B0D81, Math_FixedPointMatrixToEulerAngles @0x4B0DA1 (@0x4B0DB1 / @0x4B0DBB)]
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/mounted_pose.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/world.h>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_ctrl_catalog.h>

#include <cstdint>
#include <cstdio>
#include <climits>
#include <cstring>
#include <memory>

using namespace opennova::threedi;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                            \
	do {                                                                                    \
		if (!(c)) {                                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                        \
			++failures;                                                                     \
		}                                                                                   \
	} while (0)

namespace {

constexpr int32_t kOne = 65536;

// A level carrier turned a quarter (Yaw word 0x40000000) at (10, 20, 3). The euler builder
// takes sin and cos of the word times retail's x87 radians-per-BAM double, truncated to
// 2^22 units: 4194303 and -201, so the matrix rows are (-201, -4194303, 0 | 10),
// (4194303, -201, 0 | 20), (0, 0, 4194304 | 3).
// [orig: Math_BuildFixedPointMatrixFromEulerAngles @0x613F40 (`fld dbl_7C3608` @0x613F5D)]
void test_a_seat_point_rides_the_carrier_matrix() {
	Entity carrier;
	carrier.heading = 0x40000000;
	carrier.position.set_q16({10 * kOne, 20 * kOne, 3 * kOne});
	const CollisionMatrix m = entity_placement_matrix(carrier);
	CHECK(m.m[0] == -201 && m.m[1] == -4194303 && m.m[4] == 4194303 && m.m[5] == -201 &&
			m.m[10] == 4194304);
	Seat seat;
	seat.type = SeatType::Passenger;
	seat.bone_index = 3;
	seat.bone_point[0] = kOne;           // (1, 2, 0.5)
	seat.bone_point[1] = 2 * kOne;
	seat.bone_point[2] = kOne / 2;
	seat.bone_direction[0] = 3 * kOne;   // (3, 4, 2): an elevated facing
	seat.bone_direction[1] = 4 * kOne;
	seat.bone_direction[2] = 2 * kOne;
	MountedPose pose;
	seat_bone_pose_on_carrier_matrix(carrier, seat, pose);
	// The point: x = (65536 * -201 + 131072 * -4194303 + 2^21) >> 22 = -131075 plus
	// 655360; y = (65536 * 4194303 + 131072 * -201 + 2^21) >> 22 = 65530 plus 1310720;
	// z = 32768 plus 196608.
	CHECK(pose.position.x.q16 == 524285);
	CHECK(pose.position.y.q16 == 1376250);
	CHECK(pose.position.z.q16 == 229376);
	// The direction rotated the same way (no translation): (-262153, 196595); the heading
	// is atan2(196595, -262153) times 2^31 / pi = 1707641595.29, truncated. The direction's
	// elevation does not reach it.
	CHECK(pose.heading == 1707641595);
	// Pitch and roll are the carrier matrix's: a level carrier's 0, whatever the
	// direction's elevation.
	CHECK(pose.pitch == 0);
	CHECK(pose.roll == 0);
}

// An unturned carrier (Yaw word 0) has the identity rotation: the point lands at the
// carrier's offset and the heading is the direction's own bearing.
void test_an_unturned_carrier_keeps_the_userpoint_bearing() {
	Entity carrier;
	carrier.heading = 0;
	carrier.position.set_q16({-5 * kOne, 7 * kOne, 0});
	Seat seat;
	seat.type = SeatType::Controller;
	seat.bone_index = 1;
	seat.bone_point[0] = -kOne;
	seat.bone_point[1] = kOne / 4;
	seat.bone_point[2] = 2 * kOne;
	seat.bone_direction[0] = 5 * kOne;
	seat.bone_direction[1] = -12 * kOne;
	MountedPose pose;
	seat_bone_pose_on_carrier_matrix(carrier, seat, pose);
	CHECK(pose.position.x.q16 == -6 * kOne);
	CHECK(pose.position.y.q16 == 7 * kOne + kOne / 4);
	CHECK(pose.position.z.q16 == 2 * kOne);
	// atan2(-12, 5) = -1.176005207095135 rad times 2^31 / pi = -803876323.467, truncated
	// toward zero.
	CHECK(pose.heading == -803876323);
	CHECK(pose.pitch == 0 && pose.roll == 0);
}

// The UseGun carry: the carrier's Q22 matrix as render floats in every bone slot, the bone
// pass, the userpoint's part back to Q22 / 16.16, the userpoint's position words through it
// and its own eulers; the userpoint's direction is never read. A model with no part
// animation hands back its inputs, so the bone is the carrier's matrix after the float
// round trip: a rotation entry below 2^24 survives it, a translation word of 2^24 or more
// keeps 24 significant bits (19088743 = 0x01234567 becomes 19088744, ties to even;
// -36984441 becomes -36984440). [orig: Entity_AttachToBoneAndUpdateTransform @0x5463D0 -
// Math_FixedPointToFloatMatrix4x4_Swizzled @0x5464CB (`fimul [ecx+0Ch]; fstp dword`
// @0x611122..0x611125), Model_TransformBoneMatrices @0x54652B (the no-animation return
// @0x58F27C), Math_FloatMatrixToFixedPoint22 @0x54653E, Math_FixedPointTransformPoint22
// @0x546564, Math_FixedPointMatrixToEulerAngles @0x54656F]
void test_a_usegun_bone_is_the_carrier_matrix_through_render_floats() {
	ThreediRenderObject part{};
	ThreediLod lod{};
	lod.render_objects = &part;
	lod.render_object_count = 1;
	ThreediUserPoint point{};
	std::strcpy(point.name, "UseGun");
	point.x = kOne;           // (1, 2, 0.5)
	point.y = 2 * kOne;
	point.z = kOne / 2;
	point.rot_x = 3 * kOne;   // a facing the carry never reads
	point.rot_y = 4 * kOne;
	point.rot_z = 2 * kOne;
	Threedi3di3 model{};
	model.lods = &lod;
	model.lod_count = 1;
	model.user_points = &point;
	model.user_point_count = 1;
	Entity carrier;
	carrier.heading = 0x40000000;
	carrier.position.set_q16({0x01234567, -0x02345679, 3 * kOne});
	Seat seat;
	seat.type = SeatType::Gunner;
	seat.bone_index = 1;
	seat.yaw_offset = 37;
	MountedPose pose;
	CHECK(resolve_usegun_seat_pose(model, carrier, seat, nullptr, 0, pose));
	// The rotation as in test_a_seat_point_rides_the_carrier_matrix (-131075, 65530, 32768)
	// plus the rounded translation (19088744, -36984440, 196608).
	CHECK(pose.position.x.q16 == 18957669);
	CHECK(pose.position.y.q16 == -36918910);
	CHECK(pose.position.z.q16 == 229376);
	// The eulers of that matrix: the yaw is atan2(-4194303, -201) times -2^31 / pi =
	// 1073774581.91, truncated; a level carrier's pitch and roll are 0.
	CHECK(pose.heading == 1073774581);
	CHECK(pose.pitch == 0 && pose.roll == 0);
	// resolve_model_mounted_pose takes a UseGun seat to the same carry.
	MountedPose routed;
	CHECK(resolve_model_mounted_pose(model, carrier, seat, nullptr, 0, routed));
	CHECK(routed.position.x.q16 == 18957669 && routed.heading == 1073774581);
	// A userpoint on a part the LOD does not hold resolves nothing.
	point.subobject_index = 1;
	CHECK(!resolve_usegun_seat_pose(model, carrier, seat, nullptr, 0, pose));
	// An addeweap frame is not the UseGun carry.
	point.subobject_index = 0;
	seat.attachment_frame = true;
	CHECK(!resolve_usegun_seat_pose(model, carrier, seat, nullptr, 0, pose));
}

// A UseGun userpoint on an animated part rides that part's bone: part 1 turns a half turn
// about the render Y axis (EWEAP_GUNYAW 0x10000 over a 0..8192 track: 2^21 units times
// 1.4980282e-6 is the float pi, cosf -1, sinf -8.74e-8, which truncates to 0 in Q22), so
// its bone on an unturned carrier at (10, 20, 3) is rows (-4194304, 0, 0 | 10),
// (0, -4194304, 0 | 20), (0, 0, 4194304 | 3); part 0 keeps the carrier's matrix.
// [orig: Model_TransformBoneMatrices @0x58E390 (the Euler arm @0x58E999..0x58EA7B, the
// parent pivot @0x58EACF..0x58EB36); PANM_SampleTrack @0x5B2270]
void test_a_usegun_on_an_animated_part_rides_its_bone() {
	ThreediRenderObject parts[2] = {};
	ThreediPartAnimation nodes[2];
	std::memset(nodes, 0, sizeof(nodes));
	nodes[0].subobject_index = 0;
	nodes[0].parent_subobject = 0;
	nodes[1].subobject_index = 1;
	nodes[1].parent_subobject = 0;
	nodes[1].flags = 2u << 8;            // Euler
	nodes[1].rotation_x.control = 113;   // the rotation_x track turns about render Y
	nodes[1].rotation_x.control_param = 0;
	nodes[1].rotation_x.end = 8192;
	ThreediControlRegister registers[1];
	std::memset(registers, 0, sizeof(registers));
	std::strcpy(registers[0].name, "EWEAP_GUNYAW");
	ThreediLod lod{};
	lod.render_objects = parts;
	lod.render_object_count = 2;
	lod.part_animations = nodes;
	lod.part_animation_count = 2;
	ThreediUserPoint point{};
	std::strcpy(point.name, "UseGun");
	point.x = kOne;
	point.y = 2 * kOne;
	point.z = kOne / 2;
	point.rot_x = 3 * kOne;
	point.rot_y = 4 * kOne;
	point.subobject_index = 1;
	Threedi3di3 model{};
	model.lods = &lod;
	model.lod_count = 1;
	model.user_points = &point;
	model.user_point_count = 1;
	model.ctrl.registers = registers;
	model.ctrl.count = 1;
	int32_t bus[THREEDI_CTRL_REGISTER_COUNT] = {};
	bus[THREEDI_CTRL_EWEAP_GUNYAW] = 0x10000;
	Entity carrier;
	carrier.heading = 0;
	carrier.position.set_q16({10 * kOne, 20 * kOne, 3 * kOne});
	Seat seat;
	seat.type = SeatType::Gunner;
	seat.bone_index = 1;
	MountedPose pose;
	CHECK(resolve_usegun_seat_pose(model, carrier, seat, bus, 0, pose));
	// x = (65536 * -4194304 + 2^21) >> 22 = -65536 plus 655360; y = -131072 plus 1310720;
	// z = 32768 plus 196608.
	CHECK(pose.position.x.q16 == 589824);
	CHECK(pose.position.y.q16 == 1179648);
	CHECK(pose.position.z.q16 == 229376);
	// atan2(0, -4194304) = pi times -2^31 / pi is -2^31: the Yaw word 0x80000000.
	CHECK(pose.heading == INT32_MIN);
	CHECK(pose.pitch == 0 && pose.roll == 0);
	// The same userpoint on part 0 takes the carrier's matrix.
	point.subobject_index = 0;
	CHECK(resolve_usegun_seat_pose(model, carrier, seat, bus, 0, pose));
	CHECK(pose.position.x.q16 == 11 * kOne && pose.position.y.q16 == 22 * kOne &&
			pose.position.z.q16 == 229376);
	CHECK(pose.heading == 0 && pose.pitch == 0 && pose.roll == 0);
}

// A non-vehicle carrier takes the same seat bone when its graphic model's collision
// block holds one object and its def lacks the EWeap bit (the stock chair, Chair01's
// sitex17): BoneCallback_Simple then hands its bone slots the carrier's matrix, so the
// seat is test_a_seat_point_rides_the_carrier_matrix's pose. Two objects, or the EWeap
// bit, bind no Simple callback: ours' seat frame then stands in (the seat-local point,
// here the carrier's origin).
// [orig: EntityDef_LoadModelsAndCallbacks @0x439F50 (def+0x5C == 1 @0x43A05A; the
//  collision block's +0x68 == 1 @0x43A060..0x43A066 and def+0x54 & 0x20 clear @0x43A06B
//  -> BoneCallback_Simple @0x43A072); Threedi_BuildCollisionModelFromChunks @0x5B3BF0
//  (the +0x68 store @0x5B3D3C)]
void test_a_one_object_carrier_takes_the_simple_seat_bone() {
	struct Case {
		uint32_t objects;
		uint32_t attrib;
		bool simple;
	};
	const Case cases[] = {{1, 0u, true}, {2, 0u, false}, {1, kItemAttribEweap, false}};
	for (const Case &c : cases) {
		auto world = std::make_unique<World>();
		Entity carrier;
		carrier.item_type = 9; // a decoration, no vehicle
		carrier.item_attrib = c.attrib;
		carrier.graphic_model_collision_objects = c.objects;
		carrier.heading = 0x40000000;
		carrier.position.set_q16({10 * kOne, 20 * kOne, 3 * kOne});
		Seat seat;
		seat.type = SeatType::Passenger;
		seat.bone_index = 3;
		seat.bone_point[0] = kOne;
		seat.bone_point[1] = 2 * kOne;
		seat.bone_point[2] = kOne / 2;
		seat.bone_direction[0] = 3 * kOne;
		seat.bone_direction[1] = 4 * kOne;
		seat.bone_direction[2] = 2 * kOne;
		Entity rider;
		rider.kind = EntityKind::Organic;
		const MountedPose pose = world->vehicles.pose_mounted_occupant(rider, carrier, seat);
		if (c.simple) {
			CHECK(pose.position.x.q16 == 524285);
			CHECK(pose.position.y.q16 == 1376250);
			CHECK(pose.position.z.q16 == 229376);
			CHECK(pose.heading == 1707641595);
		} else {
			CHECK(pose.position.x.q16 == 10 * kOne);
			CHECK(pose.position.y.q16 == 20 * kOne);
			CHECK(pose.position.z.q16 == 3 * kOne);
		}
	}
}

} // namespace

int main() {
	test_a_seat_point_rides_the_carrier_matrix();
	test_an_unturned_carrier_keeps_the_userpoint_bearing();
	test_a_usegun_bone_is_the_carrier_matrix_through_render_floats();
	test_a_usegun_on_an_animated_part_rides_its_bone();
	test_a_one_object_carrier_takes_the_simple_seat_bone();
	if (failures != 0) return 1;
	std::printf("seat_bone_pose: all passed\n");
	return 0;
}
