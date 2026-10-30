#include <runtime/hud/hud_viewport.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <base/io/bam.h> // kRadiansPerBamX87 (dbl_7C3608)
#include <base/io/fixed.h> // kQ22One (dbl_7C3600)
#include <runtime/renderer/aspect_ratio.h> // aspect_viewport_scale_y (flt_8409E8)
#include <runtime/renderer/object_lod.h> // object_lod_focal_pixels (the viewport's +0x40)
#include <runtime/world/occlusion_camera.h> // kViewFovWordRadians (dbl_7C3620)
#include <runtime/world/player_view.h> // PlayerCameraPose

namespace opennova::hud {

namespace {

constexpr int32_t kQ22Unit = 0x400000;

// A Q22 sine or cosine as the build truncates it [orig: _ftol2_sse over fsin / fcos * dbl_7C3600].
int32_t q22_trunc(double value) {
	return static_cast<int32_t>(value * io::kQ22One);
}

// Math_BuildFixedPointRotationMatrix_ByAxis: one axis' rotation, Q22, zero translation.
// [orig: Math_BuildFixedPointRotationMatrix_ByAxis @0x6135C0]
world::CollisionMatrix rotation_by_axis(int axis, int32_t cos_q22, int32_t sin_q22) {
	world::CollisionMatrix m;
	if (axis == 0) {
		m.m[0] = kQ22Unit;
		m.m[5] = cos_q22;
		m.m[6] = -sin_q22;
		m.m[9] = sin_q22;
		m.m[10] = cos_q22;
	} else if (axis == 1) {
		m.m[5] = kQ22Unit;
		m.m[0] = cos_q22;
		m.m[2] = -sin_q22;
		m.m[8] = sin_q22;
		m.m[10] = cos_q22;
	} else {
		m.m[10] = kQ22Unit;
		m.m[0] = cos_q22;
		m.m[1] = -sin_q22;
		m.m[4] = sin_q22;
		m.m[5] = cos_q22;
	}
	return m;
}

// Matrix_Multiply3x4_FixedPoint: a * b, each rotation dot product with one 0x200000 bias then
// >> 22, the translation a's rotation of b's translation plus a's own.
// [orig: Matrix_Multiply3x4_FixedPoint @0x613940]
world::CollisionMatrix multiply_3x4(const world::CollisionMatrix &a,
		const world::CollisionMatrix &b) {
	world::CollisionMatrix out;
	for (int row = 0; row < 3; ++row) {
		for (int col = 0; col < 4; ++col) {
			const int64_t sum = static_cast<int64_t>(b.m[4 + col]) * a.m[row * 4 + 1] +
					static_cast<int64_t>(b.m[col]) * a.m[row * 4] +
					static_cast<int64_t>(b.m[8 + col]) * a.m[row * 4 + 2] + 0x200000;
			uint32_t word = static_cast<uint32_t>(static_cast<uint64_t>(sum) >> 22);
			if (col == 3) word += static_cast<uint32_t>(a.m[row * 4 + 3]);
			out.m[row * 4 + col] = static_cast<int32_t>(word);
		}
	}
	return out;
}

// The projector's divide: `dividend` / `divisor` (a zero divisor taken as 1), a quotient past
// 32 bits saturating instead of faulting: the dividend's high word against half the divisor's
// magnitude; past it a negative dividend gives -0x7FFFFFFF whatever the divisor's sign, a
// non-negative one +0x7FFFFFFF over a non-negative divisor.
// [orig: HUD_ClipPointToFrustumAndProject @0x591F09..0x591F4A, every plane's divides]
int32_t clip_divide(int64_t dividend, int32_t divisor) {
	const int32_t div = divisor == 0 ? 1 : divisor;
	int32_t half = divisor >> 1;
	if (half < 0) half = -half;
	const int32_t high = static_cast<int32_t>(static_cast<uint64_t>(dividend) >> 32);
	if (high < 0) {
		if (high > -half) return static_cast<int32_t>(dividend / div);
		return -0x7FFFFFFF;
	}
	if (high < half) return static_cast<int32_t>(dividend / div);
	return div < 0 ? -0x7FFFFFFF : 0x7FFFFFFF;
}

// (a << 16) / b, the dividend the sign-extended 64-bit a * 65536 [orig: `shl eax, 10h;
// sar edx, 10h` @0x591F15..0x591F18].
int32_t clip_fixed_divide(int32_t a, int32_t b) {
	return clip_divide(static_cast<int64_t>(a) * 65536, b);
}

// The length the projector measures: the x87 square root of a^2 + b^2 (exact in its 64-bit
// mantissa), held at 2147418112.0, truncated: the integer square root's floor, capped.
// [orig: `fsqrt; fcom st(1)` against flt_7C19E0 = 2147418112.0 @0x591EEF..0x591F03]
int32_t clip_length(int32_t a, int32_t b) {
	const uint64_t n = static_cast<uint64_t>(static_cast<int64_t>(a) * a) +
			static_cast<uint64_t>(static_cast<int64_t>(b) * b);
	uint64_t r = static_cast<uint64_t>(std::sqrt(static_cast<double>(n)));
	while (r > 0 && r * r > n) --r;
	while ((r + 1) * (r + 1) <= n) ++r;
	return static_cast<int32_t>(std::min<uint64_t>(r, 0x7FFF0000u));
}

// A plane's signed distance: the Q22 dot with one 0x200000 bias, >> 22, plus the plane's own.
int32_t plane_dot(const std::array<int32_t, 4> &plane, const int32_t p[3]) {
	const int64_t sum = static_cast<int64_t>(p[1]) * plane[1] +
			static_cast<int64_t>(p[0]) * plane[0] + static_cast<int64_t>(p[2]) * plane[2] +
			0x200000;
	return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(sum) >> 22) +
			static_cast<uint32_t>(plane[3]));
}

// The magnitude of a negative dot, held under 0x40000000 [orig: `cdq; xor; sub; cmp eax,
// 40000000h; jl` @0x591EBD..0x591ECC].
int32_t plane_depth(int32_t dot) {
	const uint32_t sign = dot < 0 ? 0xFFFFFFFFu : 0u;
	const int32_t magnitude = static_cast<int32_t>((static_cast<uint32_t>(dot) ^ sign) - sign);
	return magnitude >= 0x40000000 ? 0x3FFFFFFF : magnitude;
}

// A point behind the top or the bottom plane: the depth and the up axis over the plane's
// depth, the left axis by the new depth's reach over the old.
// [orig: @0x591ED3..0x59200D (the top), @0x59206B..0x5921A9 (the bottom)]
void pull_onto_vertical_plane(int32_t p[3], int32_t depth) {
	const int32_t old_reach = clip_length(p[0], p[2]);
	p[0] = clip_fixed_divide(p[0], depth);
	p[2] = clip_fixed_divide(p[2], depth);
	const int32_t reach = clip_length(p[0], p[1]);
	p[1] = clip_divide(static_cast<int64_t>(reach) * p[1], old_reach);
}

// A point behind the left or the right plane: the depth and the left axis over the plane's
// depth, the up axis by the new reach over the old.
// [orig: @0x592200..0x59233E (the left), @0x592395..0x5924D4 (the right)]
void pull_onto_horizontal_plane(int32_t p[3], int32_t depth) {
	const int32_t old_reach = clip_length(p[0], p[1]);
	p[0] = clip_fixed_divide(p[0], depth);
	p[1] = clip_fixed_divide(p[1], depth);
	const int32_t reach = clip_length(p[0], p[1]);
	p[2] = clip_divide(static_cast<int64_t>(reach) * p[2], old_reach);
}

} // namespace

HudViewport hud_viewport_build(const HudViewBlock &block, float scale_y, int32_t surface_w,
		int32_t surface_h) {
	HudViewport out;
	// The focal: half the rect's width over the tangent of half the fov word, rounded half up
	// [orig: @0x410FC0..0x410FF7].
	const double half_h = static_cast<double>(block.fov_q16 >> 1) * world::kViewFovWordRadians;
	const int32_t width = block.right - block.left + 1;
	const int32_t height = block.bottom - block.top + 1;
	const int32_t focal =
			renderer::object_lod_focal_pixels(static_cast<float>(width), std::tan(half_h));
	const int32_t cos_h = q22_trunc(std::cos(half_h));
	const int32_t sin_h = q22_trunc(std::sin(half_h));
	// The vertical half-angle: the rect's half-height (bottom - top + 2) >> 1 over the focal
	// [orig: @0x411022..0x411056].
	const double half_v = focal != 0
			? std::atan(static_cast<double>((block.bottom - block.top + 2) >> 1) / focal)
			: 0.0;
	const int32_t cos_v = q22_trunc(std::cos(half_v));
	const int32_t sin_v = q22_trunc(std::sin(half_v));
	// [orig: +0x7C (sin_h, -cos_h, 0, 0), +0x8C (sin_h, cos_h, 0, 0), +0x9C (sin_v, 0, -cos_v,
	//  0), +0xAC (sin_v, 0, cos_v, 0), +0x78 0x800 @0x41106E..0x4110F2]
	out.planes[0] = { sin_v, 0, -cos_v, 0 }; // top, flag 4
	out.planes[1] = { sin_v, 0, cos_v, 0 }; // bottom, flag 8
	out.planes[2] = { sin_h, -cos_h, 0, 0 }; // left, flag 1
	out.planes[3] = { sin_h, cos_h, 0, 0 }; // right, flag 2
	out.near_q16 = 0x800;
	// The centre: the rect's origin plus half its sides, 16.16 [orig: @0x411114..0x411131].
	out.center_x_q16 = static_cast<int32_t>(
			static_cast<uint32_t>(block.left + (width >> 1)) << 16);
	out.center_y_q16 = static_cast<int32_t>(
			static_cast<uint32_t>(block.top + (height >> 1)) << 16);
	out.focal_q16 = static_cast<int32_t>(static_cast<uint32_t>(focal) << 16);
	// The fixed view matrix: T(-eye), then the yaw's, the pitch's and the roll's rotation,
	// each axis built from (cos, -sin) [orig: the angles' sines and cosines @0x41115C..0x4111ED,
	// Math_InitFixedPointTranslationMatrix4x4 @0x411259, the products @0x41126B..0x411298].
	const auto angle_rotation = [](int axis, int32_t bam) {
		const double radians = static_cast<double>(bam) * io::kRadiansPerBamX87;
		return rotation_by_axis(axis, q22_trunc(std::cos(radians)), -q22_trunc(std::sin(radians)));
	};
	world::CollisionMatrix view;
	view.m[0] = view.m[5] = view.m[10] = kQ22Unit;
	view.m[3] = -block.pos_q16[0];
	view.m[7] = -block.pos_q16[1];
	view.m[11] = -block.pos_q16[2];
	view = multiply_3x4(angle_rotation(2, block.yaw_bam), view);
	view = multiply_3x4(angle_rotation(1, block.pitch_bam), view);
	view = multiply_3x4(angle_rotation(0, block.roll_bam), view);
	out.view = view;
	out.surface_w = surface_w;
	out.surface_h = surface_h;
	out.scale_y = scale_y;
	out.view_pos_q16 = block.pos_q16;
	out.valid = width > 0 && height > 0 && focal > 0;
	return out;
}

HudViewport hud_main_viewport(const world::PlayerCameraPose &camera,
		const world::PlayerCameraPose &view_block, float fov_h_deg, int aspect_mode,
		int32_t surface_w, int32_t surface_h) {
	if (surface_w <= 0 || surface_h <= 0) return HudViewport{};
	HudViewBlock block;
	for (int i = 0; i < 3; ++i) block.pos_q16[static_cast<size_t>(i)] = camera.pos_q16[i];
	block.yaw_bam = camera.yaw_bam;
	block.pitch_bam = camera.pitch_bam;
	block.roll_bam = camera.roll_bam;
	block.fov_q16 = static_cast<int32_t>(std::lround(static_cast<double>(fov_h_deg) * 65536.0));
	block.left = 0;
	block.top = 0;
	block.right = surface_w - 1;
	block.bottom = surface_h - 1;
	HudViewport out = hud_viewport_build(block,
			renderer::aspect_viewport_scale_y(aspect_mode, static_cast<float>(surface_w),
					static_cast<float>(surface_h)),
			surface_w, surface_h);
	for (int i = 0; i < 3; ++i) out.view_pos_q16[static_cast<size_t>(i)] = view_block.pos_q16[i];
	return out;
}

uint8_t hud_clip_point_to_frustum_and_project(const HudViewport &vp, int32_t point[3]) {
	// The up axis over the stretch: stored through a float, divided, truncated
	// [orig: `fild [esi+8]; fstp` @0x591E5B..0x591E66, Render_GetViewportScaleY, `fdivr`
	//  @0x591E6E, _ftol2_sse @0x591E71].
	// A quotient past 32 bits takes the conversion's integer indefinite, 0x80000000.
	const double up = static_cast<double>(static_cast<float>(point[2])) /
			static_cast<double>(vp.scale_y);
	point[2] = up >= -2147483648.0 && up < 2147483648.0 ? static_cast<int32_t>(up) : INT32_MIN;
	uint8_t flags = 0;
	int32_t dot = plane_dot(vp.planes[0], point);
	if (dot < 0) {
		pull_onto_vertical_plane(point, plane_depth(dot));
		flags = 4;
	}
	dot = plane_dot(vp.planes[1], point);
	if (dot < 0) {
		pull_onto_vertical_plane(point, plane_depth(dot));
		flags |= 8;
	}
	dot = plane_dot(vp.planes[2], point);
	if (dot < 0) {
		pull_onto_horizontal_plane(point, plane_depth(dot));
		flags |= 1;
	}
	dot = plane_dot(vp.planes[3], point);
	if (dot < 0) {
		pull_onto_horizontal_plane(point, plane_depth(dot));
		flags |= 2;
	}
	if (point[0] < vp.near_q16) {
		flags |= 0x10;
	} else {
		// The focal over the depth, 16.16 (0x7FFFFFFF where the divide would overflow), then
		// each lateral axis through it into whole pixels off the centre [orig:
		//  @0x5924F2..0x592570].
		int32_t scale = 0x7FFFFFFF;
		if (point[0] > (vp.focal_q16 >> 15) && vp.focal_q16 >= 0)
			scale = static_cast<int32_t>((static_cast<int64_t>(vp.focal_q16) * 65536) / point[0]);
		const auto offset = [scale](int32_t axis) {
			const int64_t product = static_cast<int64_t>(scale) * axis + 0x8000;
			return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 16);
		};
		const int32_t y = static_cast<int32_t>(
				static_cast<uint32_t>(vp.center_y_q16) - offset(point[2])) >> 16;
		const int32_t x = static_cast<int32_t>(
				static_cast<uint32_t>(vp.center_x_q16) - offset(point[1])) >> 16;
		point[0] = x;
		point[1] = y;
	}
	if (flags & 1) point[0] = 0;
	if (flags & 2) point[0] = vp.surface_w;
	if (flags & 4) point[1] = 0;
	if (flags & 8) point[1] = vp.surface_h;
	return flags;
}

HudProjectedPoint hud_project_world_point(const HudViewport &viewport,
		const std::array<int32_t, 3> &world_q16) {
	HudProjectedPoint out;
	if (!viewport.valid) return out;
	int32_t point[3];
	viewport.view.transform_point(world_q16.data(), point);
	out.depth_q16 = point[0];
	out.clip = hud_clip_point_to_frustum_and_project(viewport, point);
	out.x = static_cast<float>(point[0]);
	out.y = static_cast<float>(point[1]);
	out.valid = true;
	return out;
}

} // namespace opennova::hud
