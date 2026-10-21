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

#include <cstdint>
#include <cstdio>

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

} // namespace

int main() {
	test_a_seat_point_rides_the_carrier_matrix();
	test_an_unturned_carrier_keeps_the_userpoint_bearing();
	if (failures != 0) return 1;
	std::printf("seat_bone_pose: all passed\n");
	return 0;
}
