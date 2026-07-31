#include "env/nova_water_core.h"

#include <cmath>

using namespace godot;

void NovaWaterCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("update", "frame_counter"), &NovaWaterCore::update);
	ClassDB::bind_method(D_METHOD("get_color_rgba8"), &NovaWaterCore::get_color_rgba8);
	ClassDB::bind_method(D_METHOD("get_normal_rgba8"), &NovaWaterCore::get_normal_rgba8);
	ClassDB::bind_method(D_METHOD("get_texture_size"), &NovaWaterCore::get_texture_size);
	ClassDB::bind_method(
			D_METHOD("strip_set_view", "cam_transform", "cam_projection", "viewport_px", "fog_end_world"),
			&NovaWaterCore::strip_set_view);
	ClassDB::bind_method(
			D_METHOD("strip_build", "plane_height_world", "murk", "water_color_lit", "uv_scale",
					"uv_bias", "underwater", "nightvision"),
			&NovaWaterCore::strip_build);
	ClassDB::bind_method(D_METHOD("strip_positions"), &NovaWaterCore::strip_positions);
	ClassDB::bind_method(D_METHOD("strip_colors"), &NovaWaterCore::strip_colors);
	ClassDB::bind_method(D_METHOD("strip_speculars"), &NovaWaterCore::strip_speculars);
	ClassDB::bind_method(D_METHOD("strip_uv0"), &NovaWaterCore::strip_uv0);
	ClassDB::bind_method(D_METHOD("strip_custom0"), &NovaWaterCore::strip_custom0);
	ClassDB::bind_method(D_METHOD("strip_custom1"), &NovaWaterCore::strip_custom1);
	ClassDB::bind_method(D_METHOD("strip_custom2"), &NovaWaterCore::strip_custom2);
	ClassDB::bind_method(D_METHOD("strip_indices"), &NovaWaterCore::strip_indices);
}

void NovaWaterCore::update(int p_frame_counter) {
	opennova::env::water_noise_color_pixels(color_pixels, tables,
			static_cast<uint32_t>(p_frame_counter));
	opennova::env::water_noise_normal_pixels(normal_pixels, color_pixels);
}

namespace {

// libs/env packs A<<24|R<<16|G<<8|B (the D3D dword order); Godot RGBA8 wants
// R,G,B,A bytes.
PackedByteArray pixels_to_rgba8(const uint32_t *pixels, int count) {
	PackedByteArray bytes;
	bytes.resize(count * 4);
	uint8_t *write = bytes.ptrw();
	for (int i = 0; i < count; ++i) {
		const uint32_t pixel = pixels[i];
		write[i * 4 + 0] = static_cast<uint8_t>((pixel >> 16) & 0xFF);
		write[i * 4 + 1] = static_cast<uint8_t>((pixel >> 8) & 0xFF);
		write[i * 4 + 2] = static_cast<uint8_t>(pixel & 0xFF);
		write[i * 4 + 3] = static_cast<uint8_t>((pixel >> 24) & 0xFF);
	}
	return bytes;
}

} // namespace

PackedByteArray NovaWaterCore::get_color_rgba8() const {
	return pixels_to_rgba8(color_pixels,
			opennova::env::kWaterNoiseSize * opennova::env::kWaterNoiseSize);
}

PackedByteArray NovaWaterCore::get_normal_rgba8() const {
	return pixels_to_rgba8(normal_pixels,
			opennova::env::kWaterNoiseSize * opennova::env::kWaterNoiseSize);
}

int NovaWaterCore::get_texture_size() const {
	return opennova::env::kWaterNoiseSize;
}

// ---------------------------------------------------------------------------
// Water strip tessellation (env #29)

void NovaWaterCore::strip_set_view(const Transform3D &p_cam_transform,
		const Projection &p_cam_projection, const Vector2i &p_viewport_px,
		float p_fog_end_world) {
	// Godot world axes coincide COMPONENTWISE with the render (d3d) basis:
	// both are (-engY, engZ, engX) of the engine axes [orig:
	// Math_FixedPointToFloat3_YNegated @ 0x611210] — the same identity
	// nova_star_field.cpp serves its godot-space buffers under, and the one
	// nova_water.gd already relies on when it feeds get_water_uv_state with
	// godot cam x/z. Positions and directions therefore carry over UNCHANGED;
	// only the matrix conventions differ (D3D row-vector v' = v * M vs
	// Godot's column-vector transforms).
	//
	// D3D view matrix [orig: viewMatrix @ 0xA7845C, consumed row-vector by
	// Math_TransformPoint4ByMatrix4x4_Float @ 0x612e80]: the camera's
	// world-basis vectors sit in the COLUMNS (0 right / 1 up / 2 forward) and
	// row 3 carries -dot(axis, eye). A Godot camera looks along -basis.z, so
	// the D3D forward column is -basis.z; right/up carry over unchanged
	// (matching the witnessed screen mapping s = center +/- clip/(2w) *
	// extent [orig: @ 0x5c0cf1..0x5c0d25]: +view-x lands right of center,
	// +view-y above it).
	const Basis &basis = p_cam_transform.basis;
	const Vector3 right = basis.get_column(0);
	const Vector3 up = basis.get_column(1);
	const Vector3 forward = -basis.get_column(2);
	const Vector3 eye = p_cam_transform.origin;

	float *view = strip_view.view;
	view[0] = static_cast<float>(right.x);
	view[1] = static_cast<float>(up.x);
	view[2] = static_cast<float>(forward.x);
	view[3] = 0.0f;
	view[4] = static_cast<float>(right.y);
	view[5] = static_cast<float>(up.y);
	view[6] = static_cast<float>(forward.y);
	view[7] = 0.0f;
	view[8] = static_cast<float>(right.z);
	view[9] = static_cast<float>(up.z);
	view[10] = static_cast<float>(forward.z);
	view[11] = 0.0f;
	view[12] = static_cast<float>(-right.dot(eye));
	view[13] = static_cast<float>(-up.dot(eye));
	view[14] = static_cast<float>(-forward.dot(eye));
	view[15] = 1.0f;

	// The inverse the march unprojects through. Retail inverts the cached
	// view matrix numerically per pass [orig: Math_InvertMatrix4x4_Float_
	// ToStatic @ 0x611960]; for the rigid camera transform that inverse IS
	// the transposed rotation with the eye in row 3 — built analytically
	// here from the same source data.
	float *inv = strip_view.view_inv;
	inv[0] = static_cast<float>(right.x);
	inv[1] = static_cast<float>(right.y);
	inv[2] = static_cast<float>(right.z);
	inv[3] = 0.0f;
	inv[4] = static_cast<float>(up.x);
	inv[5] = static_cast<float>(up.y);
	inv[6] = static_cast<float>(up.z);
	inv[7] = 0.0f;
	inv[8] = static_cast<float>(forward.x);
	inv[9] = static_cast<float>(forward.y);
	inv[10] = static_cast<float>(forward.z);
	inv[11] = 0.0f;
	inv[12] = static_cast<float>(eye.x);
	inv[13] = static_cast<float>(eye.y);
	inv[14] = static_cast<float>(eye.z);
	inv[15] = 1.0f;

	// Projection [orig: mat @ 0x2721980; m11 read @ 0x2721994].
	// Preserve the complete shell matrix so the screen march also serves
	// orthographic and off-center frustum cameras. Godot is column-vector
	// while the strip core is row-vector, but both layouts index a coefficient
	// as [input][output], so flattening columns is direct. Only the view-Z
	// input changes sign: Godot looks down -Z, while the D3D/render view above
	// measures +forward.
	for (int input = 0; input < 4; ++input) {
		const float input_sign = input == 2 ? -1.0f : 1.0f;
		const Vector4 &column = p_cam_projection.columns[input];
		strip_view.proj[input * 4 + 0] = static_cast<float>(column.x) * input_sign;
		strip_view.proj[input * 4 + 1] = static_cast<float>(column.y) * input_sign;
		strip_view.proj[input * 4 + 2] = static_cast<float>(column.z) * input_sign;
		strip_view.proj[input * 4 + 3] = static_cast<float>(column.w) * input_sign;
	}

	// Camera world-basis rows for the texm3x2 bump rows [orig: flt_27219C0
	// row 0 (right) / row 2 (forward), Math_CopyVec3Row0/2 @ 0x611fb0 /
	// @ 0x611f70] — godot == render componentwise again.
	strip_view.cam_right[0] = static_cast<float>(right.x);
	strip_view.cam_right[1] = static_cast<float>(right.y);
	strip_view.cam_right[2] = static_cast<float>(right.z);
	strip_view.cam_forward[0] = static_cast<float>(forward.x);
	strip_view.cam_forward[1] = static_cast<float>(forward.y);
	strip_view.cam_forward[2] = static_cast<float>(forward.z);

	// Camera position as 16.16, like the camera block the originals fild
	// [orig: 0xA78364 (eng X = render z) / 0xA78368 (eng Y, negated =
	// render x) / 0xA7836C (eng Z = render y)].
	strip_view.cam_x_fp = static_cast<int32_t>(std::lround(static_cast<double>(eye.x) * 65536.0));
	strip_view.cam_y_fp = static_cast<int32_t>(std::lround(static_cast<double>(eye.y) * 65536.0));
	strip_view.cam_z_fp = static_cast<int32_t>(std::lround(static_cast<double>(eye.z) * 65536.0));

	// Viewport rect + center, pixels [orig: 0xA78384..0xA783A8]: min 0,
	// max = px - 1 (the clip rect's right/bottom edges are max + 1 = px),
	// center = px / 2.
	strip_view.vp_min_x = 0;
	strip_view.vp_min_y = 0;
	strip_view.vp_max_x = p_viewport_px.x - 1;
	strip_view.vp_max_y = p_viewport_px.y - 1;
	strip_view.vp_center_x = p_viewport_px.x / 2;
	strip_view.vp_center_y = p_viewport_px.y / 2;

	// The pass fog end, 16.16 [orig: Environment_GetFogEndDistance @ 0x57e3e0,
	// fetched with the underwater flag @ 0x5c28a2]; clamped to one fp unit —
	// the row colors integer-divide by it.
	int32_t fog_end_fp = static_cast<int32_t>(std::lround(static_cast<double>(p_fog_end_world) * 65536.0));
	if (fog_end_fp < 1) {
		fog_end_fp = 1;
	}
	strip_view.fog_end_fp = fog_end_fp;
	strip_view_set = true;
}

int NovaWaterCore::strip_build(float p_plane_height_world, float p_murk,
		const Color &p_water_color_lit, float p_uv_scale, float p_uv_bias,
		bool p_underwater, bool p_nightvision) {
	if (!strip_view_set) {
		strip_row_count = 0;
		return 0;
	}
	opennova::env::WaterStripParams params;
	// Env_WaterHeightFixed is 16.16 render y (== godot y).
	params.plane_height_fp =
			static_cast<int32_t>(std::lround(static_cast<double>(p_plane_height_world) * 65536.0));
	params.underwater_view = p_underwater;
	params.nightvision = p_nightvision;
	params.water_murk = p_murk;
	// Env_WaterColorLit @ 0x26c6804 is packed 0x00RRGGBB bytes.
	params.water_color_lit = p_water_color_lit.to_argb32() & 0x00FFFFFFu;
	params.uv_scale = p_uv_scale;
	params.uv_bias = p_uv_bias;
	// Quantize through the fixed plane so reconstructed positions sit on the
	// exact plane_y the march ran at (2^-16 is float-exact).
	strip_plane_height = static_cast<float>(params.plane_height_fp) * (1.0f / 65536.0f);
	strip_row_count = opennova::env::water_build_strip_rows(strip_view, params, strip_rows);
	return strip_row_count;
}

namespace {

// libs/env packs A<<24|R<<16|G<<8|B (the D3D dword order); raw bytes / 255,
// no color-space conversion — the strips feed the COLOR attribute of a
// gamma-space shader (D-RMAT-7).
PackedColorArray packed_argb_to_colors(const std::vector<uint32_t> &packed) {
	PackedColorArray out;
	out.resize(static_cast<int64_t>(packed.size()));
	Color *write = out.ptrw();
	for (size_t i = 0; i < packed.size(); ++i) {
		const uint32_t argb = packed[i];
		write[i] = Color(
				static_cast<float>((argb >> 16) & 0xFFu) / 255.0f,
				static_cast<float>((argb >> 8) & 0xFFu) / 255.0f,
				static_cast<float>(argb & 0xFFu) / 255.0f,
				static_cast<float>((argb >> 24) & 0xFFu) / 255.0f);
	}
	return out;
}

} // namespace

PackedVector3Array NovaWaterCore::strip_positions() const {
	// World positions recovered as (uv0 * 32, plane height): uv0 is the
	// unprojected world x/z * 0.03125 [orig: flt_7DBFAC @ 0x5c2899] in the
	// render basis, which maps to godot axes unchanged (see strip_set_view).
	PackedVector3Array out;
	const int count = strip_row_count * 3;
	out.resize(count);
	Vector3 *write = out.ptrw();
	for (int i = 0; i < count; ++i) {
		write[i] = Vector3(strip_rows.uv0[i * 2] * 32.0f, strip_plane_height,
				strip_rows.uv0[i * 2 + 1] * 32.0f);
	}
	return out;
}

PackedColorArray NovaWaterCore::strip_colors() const {
	// The row-constant diffuse, written to all 3 row vertices
	// [orig: @ 0x5c2f0a..0x5c2f2b].
	return packed_argb_to_colors(strip_rows.diffuse);
}

PackedColorArray NovaWaterCore::strip_speculars() const {
	// The row-constant specular (WaterColorLit RGB under the distance alpha)
	// [orig: @ 0x5c2eb5..0x5c2ef4].
	return packed_argb_to_colors(strip_rows.specular);
}

PackedFloat32Array NovaWaterCore::strip_custom1() const {
	// The specular as the mesh's ARRAY_CUSTOM1 payload (RGBA_FLOAT custom
	// arrays only accept PackedFloat32Array): 4 floats per vertex, raw bytes
	// / 255, no color-space conversion — the ps.1.1 v1 register the shader
	// adds after the x2/x4 modulates [orig: add r0.rgb, r0, v1 —
	// Water_InitSurfaceShaders @ 0x5c19b0; row values @ 0x5c2eb5..0x5c2ef4].
	PackedFloat32Array out;
	const int count = strip_row_count * 3;
	out.resize(count * 4);
	float *write = out.ptrw();
	for (int i = 0; i < count; ++i) {
		const uint32_t argb = strip_rows.specular[i];
		write[i * 4 + 0] = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
		write[i * 4 + 1] = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
		write[i * 4 + 2] = static_cast<float>(argb & 0xFFu) / 255.0f;
		write[i * 4 + 3] = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
	}
	return out;
}

PackedVector2Array NovaWaterCore::strip_uv0() const {
	PackedVector2Array out;
	const int count = strip_row_count * 3;
	out.resize(count);
	Vector2 *write = out.ptrw();
	for (int i = 0; i < count; ++i) {
		write[i] = Vector2(strip_rows.uv0[i * 2], strip_rows.uv0[i * 2 + 1]);
	}
	return out;
}

PackedFloat32Array NovaWaterCore::strip_custom0() const {
	// 4 floats per vertex: [depth (+0x08), rhw (+0x0C), screen U (t1's 3rd
	// component @ 0x5c2fd0..), screen V (t2's 3rd component @ 0x5c301e..,
	// V-flipped on the underwater pass)] — the witnessed projective depth
	// pair plus the texm3x2 screen lookup for the env #30 reflection
	// consumer.
	PackedFloat32Array out;
	const int count = strip_row_count * 3;
	out.resize(count * 4);
	float *write = out.ptrw();
	for (int i = 0; i < count; ++i) {
		write[i * 4 + 0] = strip_rows.depth[i];
		write[i * 4 + 1] = strip_rows.rhw[i];
		write[i * 4 + 2] = strip_rows.t1[i * 3 + 2];
		write[i * 4 + 3] = strip_rows.t2[i * 3 + 2];
	}
	return out;
}

PackedFloat32Array NovaWaterCore::strip_custom2() const {
	// 4 floats per vertex: [t1.x, t1.y, t2.x, t2.y] — the texm3x2
	// perturbation basis, t1 = (right.x, right.z) * (-min(rhw, 0.05)/2),
	// t2 = (fwd.x, fwd.z) * (-5*min(rhw, 0.05))
	// [orig: rows @ 0x5c2f83..0x5c3067; consumed by texm3x2pad t1, t0_bx2 /
	// texm3x2tex t2, t0_bx2 — Water_InitSurfaceShaders @ 0x5c19b0]. The 3rd
	// components (screen U/V) ride strip_custom0's zw; the env #30 shader
	// reassembles the full rows from both attributes.
	PackedFloat32Array out;
	const int count = strip_row_count * 3;
	out.resize(count * 4);
	float *write = out.ptrw();
	for (int i = 0; i < count; ++i) {
		write[i * 4 + 0] = strip_rows.t1[i * 3 + 0];
		write[i * 4 + 1] = strip_rows.t1[i * 3 + 1];
		write[i * 4 + 2] = strip_rows.t2[i * 3 + 0];
		write[i * 4 + 3] = strip_rows.t2[i * 3 + 1];
	}
	return out;
}

PackedInt32Array NovaWaterCore::strip_indices() const {
	// The witnessed batch submits unrolled to PRIMITIVE_TRIANGLES: each
	// <=5-row window locks 8n-10 vertices and draws a TRIANGLESTRIP through
	// the first 8n-10 entries of the static index table; global vertex =
	// 3*first_row + entry [orig: word_841328; batch walk @ 0x5c3164..
	// 0x5c329e]. The strip->list unroll alternates winding per triangle and
	// SKIPS the {2,6}-style degenerate stitches (any two indices equal) —
	// they only existed to join row pairs inside one strip call.
	PackedInt32Array out;
	const std::vector<opennova::env::WaterStripBatch> batches =
			opennova::env::water_strip_batches(strip_row_count);
	for (const opennova::env::WaterStripBatch &batch : batches) {
		const int32_t base = batch.first_row * 3;
		for (int tri = 0; tri + 2 < batch.vertex_count; ++tri) {
			const int32_t i0 = opennova::env::kWaterStripIndexTable[tri];
			const int32_t i1 = opennova::env::kWaterStripIndexTable[tri + 1];
			const int32_t i2 = opennova::env::kWaterStripIndexTable[tri + 2];
			if (i0 == i1 || i1 == i2 || i0 == i2) {
				continue;
			}
			// D3D strip parity: odd triangles swap the first two vertices to
			// keep a consistent facing (the water passes are two-sided
			// anyway — cull NONE [orig: pass flags 0x400000 @ 0x5c340d]).
			if ((tri & 1) != 0) {
				out.push_back(base + i1);
				out.push_back(base + i0);
			} else {
				out.push_back(base + i0);
				out.push_back(base + i1);
			}
			out.push_back(base + i2);
		}
	}
	return out;
}
