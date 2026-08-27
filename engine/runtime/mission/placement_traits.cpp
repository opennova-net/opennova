#include <runtime/mission/placement_traits.h>

#include <cmath>

namespace opennova::mission {

namespace {

struct Mat3 {
	// Column-major: m[c][r].
	double m[3][3];
};

Mat3 mul(const Mat3 &a, const Mat3 &b) {
	Mat3 out{};
	for (int c = 0; c < 3; ++c) {
		for (int r = 0; r < 3; ++r) {
			out.m[c][r] = a.m[0][r] * b.m[c][0] + a.m[1][r] * b.m[c][1] +
					a.m[2][r] * b.m[c][2];
		}
	}
	return out;
}

Mat3 rot_x(double a) {
	const double c = std::cos(a);
	const double s = std::sin(a);
	return Mat3{ { { 1, 0, 0 }, { 0, c, s }, { 0, -s, c } } };
}

Mat3 rot_y(double a) {
	const double c = std::cos(a);
	const double s = std::sin(a);
	return Mat3{ { { c, 0, -s }, { 0, 1, 0 }, { s, 0, c } } };
}

Mat3 rot_z(double a) {
	const double c = std::cos(a);
	const double s = std::sin(a);
	return Mat3{ { { c, s, 0 }, { -s, c, 0 }, { 0, 0, 1 } } };
}

constexpr double kDegToRad = 0.017453292519943295;

} // namespace

// The engine builds Rz(90-yaw) * Ry(-pitch) * Rx(roll) in its Z-up world
// [orig: Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40, via
// Entity_UpdateOrientationMatrix @ 0x43b440]; conjugated into the Y-up
// presentation basis (see the header) that is
// RotY(90-yaw) * RotZ(pitch) * RotX(roll) * RotY(90).
PlacementBasis bms_to_presentation_basis(float pitch_deg, float yaw_deg,
		float roll_deg) {
	const double pitch = pitch_deg * kDegToRad;
	const double yaw = yaw_deg * kDegToRad;
	const double roll = roll_deg * kDegToRad;
	const Mat3 m = mul(mul(mul(rot_y(90.0 * kDegToRad - yaw), rot_z(pitch)),
								   rot_x(roll)),
			rot_y(90.0 * kDegToRad));
	PlacementBasis out;
	out.x = PlacementVec3{ float(m.m[0][0]), float(m.m[0][1]),
		float(m.m[0][2]) };
	out.y = PlacementVec3{ float(m.m[1][0]), float(m.m[1][1]),
		float(m.m[1][2]) };
	out.z = PlacementVec3{ float(m.m[2][0]), float(m.m[2][1]),
		float(m.m[2][2]) };
	return out;
}

} // namespace opennova::mission
