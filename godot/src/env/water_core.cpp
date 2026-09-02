#include "env/water_core.h"

#include <cmath>
#include <base/io/fixed.h>

using namespace godot;

void WaterCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("update", "frame_counter"), &WaterCore::update);
	ClassDB::bind_method(D_METHOD("get_color_rgba8"), &WaterCore::get_color_rgba8);
	ClassDB::bind_method(D_METHOD("get_normal_rgba8"), &WaterCore::get_normal_rgba8);
}

void WaterCore::update(int p_frame_counter) {
	opennova::env::water_noise_color_pixels(color_pixels, tables,
			static_cast<uint32_t>(p_frame_counter));
	opennova::env::water_noise_normal_pixels(normal_pixels, color_pixels);
}

namespace {

// engine/formats/env packs A<<24|R<<16|G<<8|B (the D3D dword order); Godot RGBA8 wants
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

PackedByteArray WaterCore::get_color_rgba8() const {
	return pixels_to_rgba8(color_pixels,
			opennova::env::kWaterNoiseSize * opennova::env::kWaterNoiseSize);
}

PackedByteArray WaterCore::get_normal_rgba8() const {
	return pixels_to_rgba8(normal_pixels,
			opennova::env::kWaterNoiseSize * opennova::env::kWaterNoiseSize);
}

int WaterCore::get_texture_size() const {
	return opennova::env::kWaterNoiseSize;
}

// ---------------------------------------------------------------------------
// Water strip tessellation (env #29)

void WaterCore::strip_set_view(const Transform3D &p_cam_transform,
		const Projection &p_cam_projection, const Vector2i &p_viewport_px,
		float p_fog_end_world) {
	// Godot world axes coincide COMPONENTWISE with the render (d3d) basis, so
	// positions and directions carry over unchanged; the matrix conventions
	// (and the -basis.z forward of a Godot camera) are the engine builder's
	// business (env_water_render.h water_strip_view_from_camera).
	const Basis &basis = p_cam_transform.basis;
	const Vector3 right_v = basis.get_column(0);
	const Vector3 up_v = basis.get_column(1);
	const Vector3 forward_v = -basis.get_column(2);
	const Vector3 eye_v = p_cam_transform.origin;
	const float right[3] = {static_cast<float>(right_v.x), static_cast<float>(right_v.y), static_cast<float>(right_v.z)};
	const float up[3] = {static_cast<float>(up_v.x), static_cast<float>(up_v.y), static_cast<float>(up_v.z)};
	const float forward[3] = {static_cast<float>(forward_v.x), static_cast<float>(forward_v.y), static_cast<float>(forward_v.z)};
	const float eye[3] = {static_cast<float>(eye_v.x), static_cast<float>(eye_v.y), static_cast<float>(eye_v.z)};
	float proj_columns[16];
	for (int input = 0; input < 4; ++input) {
		const Vector4 &column = p_cam_projection.columns[input];
		proj_columns[input * 4 + 0] = static_cast<float>(column.x);
		proj_columns[input * 4 + 1] = static_cast<float>(column.y);
		proj_columns[input * 4 + 2] = static_cast<float>(column.z);
		proj_columns[input * 4 + 3] = static_cast<float>(column.w);
	}
	opennova::env::water_strip_view_from_camera(right, up, forward, eye, proj_columns,
			p_viewport_px.x, p_viewport_px.y, p_fog_end_world, strip_view);
	strip_view_set = true;
}

int WaterCore::strip_build(float p_plane_height_world, float p_murk,
		const Color &p_water_color_lit, float p_uv_scale, float p_uv_bias,
		bool p_underwater, bool p_nightvision) {
	if (!strip_view_set) {
		strip_row_count = 0;
		return 0;
	}
	opennova::env::WaterStripParams params;
	// Env_WaterHeightFixed is 16.16 render y (== godot y).
	params.plane_height_fp =
			opennova::io::float_to_fp16_16_round_sat(p_plane_height_world);
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

// engine/formats/env packs A<<24|R<<16|G<<8|B (the D3D dword order); raw bytes / 255,
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

PackedVector3Array WaterCore::strip_positions() const {
	// World positions recovered as (uv0 * 32, plane height): uv0 is the
	// unprojected world x/z * 0.03125 [orig: flt_7DBFAC @ 0x5c2899, see docs/env/env-tod-re.md] in the
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

PackedColorArray WaterCore::strip_colors() const {
	// The row-constant diffuse, written to all 3 row vertices
	// [orig: @ 0x5c2f0a..0x5c2f2b, see docs/env/env-tod-re.md].
	return packed_argb_to_colors(strip_rows.diffuse);
}

PackedFloat32Array WaterCore::strip_custom1() const {
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

PackedVector2Array WaterCore::strip_uv0() const {
	PackedVector2Array out;
	const int count = strip_row_count * 3;
	out.resize(count);
	Vector2 *write = out.ptrw();
	for (int i = 0; i < count; ++i) {
		write[i] = Vector2(strip_rows.uv0[i * 2], strip_rows.uv0[i * 2 + 1]);
	}
	return out;
}

PackedFloat32Array WaterCore::strip_custom0() const {
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

PackedFloat32Array WaterCore::strip_custom2() const {
	// 4 floats per vertex: [t1.x, t1.y, t2.x, t2.y] — the texm3x2
	// perturbation basis, t1 = (right.x, right.z) * (-min(rhw, 0.05)/2),
	// t2 = (fwd.x, fwd.z) * (-5*min(rhw, 0.05))
	// [orig: rows @ 0x5c2f83..0x5c3067; consumed by texm3x2pad t1, t0_bx2 /
	// texm3x2tex t2, t0_bx2 — Water_InitSurfaceShaders @ 0x5c19b0, see docs/env/env-tod-re.md]. The 3rd
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

PackedInt32Array WaterCore::strip_indices() const {
	// The witnessed batch submits unrolled to PRIMITIVE_TRIANGLES: each
	// <=5-row window locks 8n-10 vertices and draws a TRIANGLESTRIP through
	// the first 8n-10 entries of the static index table; global vertex =
	// 3*first_row + entry [orig: word_841328; batch walk @ 0x5c3164..
	// 0x5c329e, see docs/env/env-tod-re.md]. The strip->list unroll alternates winding per triangle and
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
			// anyway — cull NONE [orig: pass flags 0x400000 @ 0x5c340d, see docs/env/env-tod-re.md]).
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
