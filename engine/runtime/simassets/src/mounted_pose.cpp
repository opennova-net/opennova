// The mounted-pose resolver — the adapter's model-bound body moved onto the
// sim's own parse (ADR 0028): the same userpoint re-anchor, live-PANM carry,
// attachment look-at frame, and mission-euler extraction, in native rows math
// (the Godot Transform3D round trip contributed no semantics — its basis
// conversions are reproduced here explicitly).
#include "simassets/mounted_pose.h"

#include <threedi/threedi_panm_pose.h>
#include <world/angle.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace opennova::simassets {

namespace {

constexpr double kHalfPi = 1.57079632679489661923;
constexpr double kRadiansPerDegree = 3.14159265358979323846 / 180.0;

struct V3 {
	double x = 0.0, y = 0.0, z = 0.0;
};

// Row-major 3x3, column-vector convention (Godot Basis rows).
struct M3 {
	double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

V3 v3_sub_scaled(const V3 &a, const V3 &b, double s) {
	return V3{a.x - b.x * s, a.y - b.y * s, a.z - b.z * s};
}

double v3_dot(const V3 &a, const V3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

V3 v3_cross(const V3 &a, const V3 &b) {
	return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x};
}

double v3_length_sq(const V3 &v) { return v3_dot(v, v); }

bool v3_normalize(V3 &v) {
	const double len = std::sqrt(v3_length_sq(v));
	if (len == 0.0) return false;
	v.x /= len;
	v.y /= len;
	v.z /= len;
	return true;
}

bool v3_finite(const V3 &v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

M3 m3_mul(const M3 &a, const M3 &b) {
	M3 o;
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] +
					a.m[r][2] * b.m[2][c];
	return o;
}

M3 m3_transposed(const M3 &a) {
	M3 o;
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			o.m[r][c] = a.m[c][r];
	return o;
}

double m3_det(const M3 &a) {
	return a.m[0][0] * (a.m[1][1] * a.m[2][2] - a.m[2][1] * a.m[1][2]) -
			a.m[1][0] * (a.m[0][1] * a.m[2][2] - a.m[2][1] * a.m[0][2]) +
			a.m[2][0] * (a.m[0][1] * a.m[1][2] - a.m[1][1] * a.m[0][2]);
}

bool m3_inverse(const M3 &a, M3 &o) {
	const double det = m3_det(a);
	if (det == 0.0) return false;
	const double inv = 1.0 / det;
	o.m[0][0] = (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) * inv;
	o.m[0][1] = (a.m[0][2] * a.m[2][1] - a.m[0][1] * a.m[2][2]) * inv;
	o.m[0][2] = (a.m[0][1] * a.m[1][2] - a.m[0][2] * a.m[1][1]) * inv;
	o.m[1][0] = (a.m[1][2] * a.m[2][0] - a.m[1][0] * a.m[2][2]) * inv;
	o.m[1][1] = (a.m[0][0] * a.m[2][2] - a.m[0][2] * a.m[2][0]) * inv;
	o.m[1][2] = (a.m[0][2] * a.m[1][0] - a.m[0][0] * a.m[1][2]) * inv;
	o.m[2][0] = (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]) * inv;
	o.m[2][1] = (a.m[0][1] * a.m[2][0] - a.m[0][0] * a.m[2][1]) * inv;
	o.m[2][2] = (a.m[0][0] * a.m[1][1] - a.m[0][1] * a.m[1][0]) * inv;
	return true;
}

V3 m3_xform(const M3 &a, const V3 &v) {
	return V3{a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
			a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
			a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}

bool m3_finite(const M3 &a) {
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			if (!std::isfinite(a.m[r][c])) return false;
	return true;
}

V3 m3_column(const M3 &a, int c) {
	return V3{a.m[0][c], a.m[1][c], a.m[2][c]};
}

void m3_set_column(M3 &a, int c, const V3 &v) {
	a.m[0][c] = v.x;
	a.m[1][c] = v.y;
	a.m[2][c] = v.z;
}

// Column Gram-Schmidt — Godot Basis::orthonormalized().
void m3_orthonormalize(M3 &a) {
	V3 x = m3_column(a, 0);
	V3 y = m3_column(a, 1);
	V3 z = m3_column(a, 2);
	v3_normalize(x);
	y = v3_sub_scaled(y, x, v3_dot(x, y));
	v3_normalize(y);
	z = v3_sub_scaled(z, x, v3_dot(x, z));
	z = v3_sub_scaled(z, y, v3_dot(y, z));
	v3_normalize(z);
	m3_set_column(a, 0, x);
	m3_set_column(a, 1, y);
	m3_set_column(a, 2, z);
}

M3 m3_axis_angle_y(double angle) {
	const double c = std::cos(angle), s = std::sin(angle);
	M3 o;
	o.m[0][0] = c;
	o.m[0][2] = s;
	o.m[2][0] = -s;
	o.m[2][2] = c;
	return o;
}

M3 m3_axis_angle_z(double angle) {
	const double c = std::cos(angle), s = std::sin(angle);
	M3 o;
	o.m[0][0] = c;
	o.m[0][1] = -s;
	o.m[1][0] = s;
	o.m[1][1] = c;
	return o;
}

M3 m3_axis_angle_x(double angle) {
	const double c = std::cos(angle), s = std::sin(angle);
	M3 o;
	o.m[1][1] = c;
	o.m[1][2] = -s;
	o.m[2][1] = s;
	o.m[2][2] = c;
	return o;
}

// The adapter's godot_model_basis_from_mission_euler: the ordinary mission
// euler into the model world frame (RotY(90 - yaw) * RotZ(pitch) * RotX(roll)
// * RotY(+90), the trailing term the .3di model-forward correction).
M3 model_basis_from_mission_euler(double pitch_deg, double yaw_deg,
		double roll_deg) {
	return m3_mul(
			m3_mul(m3_mul(m3_axis_angle_y((90.0 - yaw_deg) * kRadiansPerDegree),
						  m3_axis_angle_z(pitch_deg * kRadiansPerDegree)),
					m3_axis_angle_x(roll_deg * kRadiansPerDegree)),
			m3_axis_angle_y(kHalfPi));
}

struct Affine {
	M3 basis;
	V3 origin;
};

V3 affine_xform(const Affine &t, const V3 &v) {
	const V3 r = m3_xform(t.basis, v);
	return V3{r.x + t.origin.x, r.y + t.origin.y, r.z + t.origin.z};
}

bool affine_inverse(const Affine &t, Affine &o) {
	if (!m3_inverse(t.basis, o.basis)) return false;
	const V3 r = m3_xform(o.basis, t.origin);
	o.origin = V3{-r.x, -r.y, -r.z};
	return true;
}

// The evaluator's row-major render matrix into the model-world affine — the
// exact panm_matrix_to_transform mapping (the (-x, y, z) mirror + row-vector
// transpose).
Affine affine_from_panm_matrix(const ThreediMatrix4x4 &m) {
	Affine t;
	t.basis.m[0][0] = m.m[0];
	t.basis.m[0][1] = -m.m[4];
	t.basis.m[0][2] = -m.m[8];
	t.basis.m[1][0] = -m.m[1];
	t.basis.m[1][1] = m.m[5];
	t.basis.m[1][2] = m.m[9];
	t.basis.m[2][0] = -m.m[2];
	t.basis.m[2][1] = m.m[6];
	t.basis.m[2][2] = m.m[10];
	t.origin = V3{-static_cast<double>(m.m[12]), static_cast<double>(m.m[13]),
			static_cast<double>(m.m[14])};
	return t;
}

} // namespace

bool resolve_model_mounted_pose(const Threedi3di3 &model,
		const world::Entity &carrier, const world::Seat &seat,
		const int32_t *ctrl_values, uint32_t time_ms,
		world::MountedPose &out) {
	if (seat.type != world::SeatType::Gunner || seat.bone_index == 0)
		return false;
	const int userpoint_index = static_cast<int>(seat.bone_index) - 1;
	if (userpoint_index < 0 || model.user_points == nullptr ||
			static_cast<size_t>(userpoint_index) >= model.user_point_count)
		return false;
	const ThreediUserPoint &up =
			model.user_points[static_cast<size_t>(userpoint_index)];
	const int part_index = up.subobject_index;
	// The decode swizzle + render X-mirror: authored 16.16 -> the model world
	// frame the PANM evaluator poses (the adapter's get_user_point_info space).
	const V3 authored_model_position{
			static_cast<double>(up.y) / 65536.0,
			static_cast<double>(up.z) / 65536.0,
			static_cast<double>(up.x) / 65536.0};
	const V3 authored_model_direction{
			static_cast<double>(up.rot_y) / 65536.0,
			static_cast<double>(up.rot_z) / 65536.0,
			static_cast<double>(up.rot_x) / 65536.0};
	if (part_index < 0 || !v3_finite(authored_model_position)) return false;

	constexpr int lod_index = 0;
	std::vector<ThreediMatrix4x4> rest_parts;
	std::vector<ThreediMatrix4x4> live_parts;
	if (!threedi_panm_pose_parts(model, lod_index, 0u, nullptr, rest_parts,
				nullptr) ||
			!threedi_panm_pose_parts(model, lod_index, time_ms, ctrl_values,
					live_parts, nullptr))
		return false;
	if (static_cast<size_t>(part_index) >= rest_parts.size() ||
			static_cast<size_t>(part_index) >= live_parts.size())
		return false;
	const Affine rest_part = affine_from_panm_matrix(
			rest_parts[static_cast<size_t>(part_index)]);
	const Affine live_part = affine_from_panm_matrix(
			live_parts[static_cast<size_t>(part_index)]);
	if (!v3_finite(rest_part.origin) || !m3_finite(rest_part.basis) ||
			!v3_finite(live_part.origin) || !m3_finite(live_part.basis) ||
			std::fabs(m3_det(rest_part.basis)) < 1.0e-8)
		return false;

	Affine rest_inverse;
	if (!affine_inverse(rest_part, rest_inverse)) return false;
	const V3 point_in_part = affine_xform(rest_inverse, authored_model_position);
	const V3 live_model_position = affine_xform(live_part, point_in_part);
	const M3 carrier_basis = model_basis_from_mission_euler(
			static_cast<double>(carrier.pitch), static_cast<double>(carrier.yaw),
			static_cast<double>(carrier.roll));
	if (!v3_finite(live_model_position) || !m3_finite(carrier_basis) ||
			std::fabs(m3_det(carrier_basis)) < 1.0e-8)
		return false;
	const Affine carrier_world{carrier_basis,
			V3{static_cast<double>(carrier.position.x),
					static_cast<double>(carrier.position.z),
					-static_cast<double>(carrier.position.y)}};
	const V3 live_world_position = affine_xform(carrier_world, live_model_position);
	if (!v3_finite(live_world_position)) return false;
	out.position = world::Vec3{static_cast<float>(live_world_position.x),
			static_cast<float>(-live_world_position.z),
			static_cast<float>(live_world_position.y)};

	M3 live_basis;
	if (seat.attachment_frame &&
			v3_finite(authored_model_direction) &&
			v3_length_sq(authored_model_direction) > 1.0e-8) {
		// An addeweap child owns the complete EWeap userpoint frame. Build the
		// same direction look-at frame retail multiplies through the live
		// bone: forward = direction; right = (forward.z, 0, -forward.x);
		// up = forward x right. Retail's result is a row-vector render matrix,
		// so transpose and conjugate by the loader's X mirror before composing
		// in the model world frame; the rest-bone inverse then makes that
		// authored frame part-local; the live bone carries both position and
		// orientation through PANM.
		// [orig: build_bone_attachment_matrix @ 0x56C630;
		//  build_direction_look_at_matrix @ 0x612C90]
		V3 forward = authored_model_direction;
		v3_normalize(forward);
		V3 right{forward.z, 0.0, -forward.x};
		if (v3_length_sq(right) <= 1.0e-8)
			right = V3{1.0, 0.0, 0.0};
		else
			v3_normalize(right);
		V3 fup = v3_cross(forward, right);
		if (v3_length_sq(fup) <= 1.0e-8) return false;
		v3_normalize(fup);
		// The adapter built this Basis from column AXES (right, up, forward).
		M3 retail_frame;
		m3_set_column(retail_frame, 0, right);
		m3_set_column(retail_frame, 1, fup);
		m3_set_column(retail_frame, 2, forward);
		M3 x_flip;
		x_flip.m[0][0] = -1.0;
		const M3 authored_basis =
				m3_mul(m3_mul(x_flip, m3_transposed(retail_frame)), x_flip);
		M3 rest_basis_inverse;
		if (!m3_inverse(rest_part.basis, rest_basis_inverse)) return false;
		const M3 attachment_in_part = m3_mul(rest_basis_inverse, authored_basis);
		live_basis = m3_mul(m3_mul(carrier_basis, live_part.basis),
				attachment_in_part);
	} else {
		const double baseline_yaw = seat.attachment_frame
				? static_cast<double>(carrier.yaw + seat.yaw_offset)
				: seat.type == world::SeatType::Gunner
				? static_cast<double>(carrier.yaw - seat.yaw_offset)
				: static_cast<double>(carrier.yaw + seat.yaw_offset);
		const M3 baseline_basis = model_basis_from_mission_euler(
				static_cast<double>(carrier.pitch), baseline_yaw,
				static_cast<double>(carrier.roll));
		M3 rest_basis_inverse;
		if (!m3_inverse(rest_part.basis, rest_basis_inverse)) return false;
		const M3 part_delta = m3_mul(live_part.basis, rest_basis_inverse);
		M3 carrier_inverse;
		if (!m3_inverse(carrier_basis, carrier_inverse)) return false;
		live_basis = m3_mul(
				m3_mul(m3_mul(carrier_basis, part_delta), carrier_inverse),
				baseline_basis);
	}
	if (!m3_finite(live_basis) || std::fabs(m3_det(live_basis)) < 1.0e-8)
		return false;
	m3_orthonormalize(live_basis);
	const M3 euler_basis = m3_mul(live_basis, m3_axis_angle_y(-kHalfPi));
	const double pitch_rad = std::asin(
			std::clamp(euler_basis.m[1][0], -1.0, 1.0));
	if (std::fabs(std::cos(pitch_rad)) < 1.0e-6) return false;
	const double heading_rad =
			std::atan2(-euler_basis.m[2][0], euler_basis.m[0][0]);
	const double roll_rad =
			std::atan2(-euler_basis.m[1][2], euler_basis.m[1][1]);
	const double yaw_deg = world::normalize_mission_yaw_deg(
			90.0 - heading_rad / kRadiansPerDegree);
	const double pitch_deg = pitch_rad / kRadiansPerDegree;
	const double roll_deg = roll_rad / kRadiansPerDegree;
	if (!std::isfinite(yaw_deg) || !std::isfinite(pitch_deg) ||
			!std::isfinite(roll_deg))
		return false;
	out.yaw = static_cast<int16_t>(std::lround(yaw_deg));
	out.pitch = static_cast<int16_t>(std::lround(pitch_deg));
	out.roll = static_cast<int16_t>(std::lround(roll_deg));
	return true;
}

void compose_mounted_pose_controls(
        uint32_t carrier_item_attrib, const MountedPoseControlSources &sources,
        int32_t (&r_ctrl)[THREEDI_CTRL_REGISTER_COUNT]) {
    if ((carrier_item_attrib & 0x1000u) == 0)
        r_ctrl[THREEDI_CTRL_VEHICLE_SPECIAL1] = sources.part_anim_phase0;
    r_ctrl[THREEDI_CTRL_VEHICLE_SPECIAL2] = sources.part_anim_phase1;
    if (sources.has_heat_glow)
        r_ctrl[THREEDI_CTRL_HEAT_GLOW] = sources.heat_glow;
    if (sources.has_emplaced) {
        r_ctrl[THREEDI_CTRL_EWEAP_GUNYAW] =
                static_cast<int32_t>(sources.emplaced_gun_yaw);
        r_ctrl[THREEDI_CTRL_EWEAP_GUNPITCH] =
                static_cast<int32_t>(sources.emplaced_gun_pitch);
    }
}

uint32_t mounted_pose_time_ms(uint32_t logic_tick, int64_t override_ms) {
    return override_ms >= 0 ? static_cast<uint32_t>(override_ms)
                            : logic_tick * 16u;
}

} // namespace opennova::simassets
