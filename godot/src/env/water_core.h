#pragma once

#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <formats/env/env_water_render.h>

namespace godot {

// The per-frame water surface core of [orig: Render_WaterSurface @ 0x5c32c0, see docs/env/env-tod-re.md]:
// the animated 128x128 ridge color/alpha texture and its DuDv/normal
// derivative [orig: Water_GenerateNoiseTextures @ 0x5c0360, see docs/env/env-tod-re.md], plus the
// screen-marched strip tessellation of the detailed tier (env #29)
// [orig: Render_WaterStripDetailed @ 0x5c27d0, see docs/env/env-tod-re.md]. All math lives in engine/formats/env
// (env/env_water_render.h); this C++-only device helper (Water's member — its
// ClassDB row died with the ADR 0043 d10 env-core sweep; env_render_unit
// pins the vectors it served GUT) owns the static tables (built once with
// the witnessed init, from the boot PRNG state), the frame buffers, and the
// Godot<->render basis conversion for the strip view state. Water updates
// once per frame, blits the textures into ImageTextures, and rebuilds its
// ArrayMesh surface from the strip buffers. RE record: docs/env/env-tod-re.md
// "Water surface".
class WaterCore {
private:
	opennova::env::WaterNoiseTables tables = opennova::env::water_init_noise_tables();
	uint32_t color_pixels[opennova::env::kWaterNoiseSize * opennova::env::kWaterNoiseSize] = {};
	uint32_t normal_pixels[opennova::env::kWaterNoiseSize * opennova::env::kWaterNoiseSize] = {};

	// Strip-march state (env #29). The view block is rebuilt by
	// strip_set_view; the rows persist from the last strip_build for the
	// typed getters below.
	opennova::env::WaterStripView strip_view = {};
	opennova::env::WaterStripRows strip_rows;
	int strip_row_count = 0;
	// The built plane height quantized through the 16.16 fixed plane the
	// march ran at, so reconstructed positions match the lib's plane_y.
	float strip_plane_height = 0.0f;
	bool strip_view_set = false;

public:
	// Regenerates both textures for the given 62 Hz frame counter
	// [orig: called per frame from Render_WaterSurface @ 0x5c3326, see docs/env/env-tod-re.md].
	void update(int p_frame_counter);

	// RGBA8 bytes (128x128) for Image::create_from_data - the ridge
	// color/alpha texture and the DuDv/normal map from the last update().
	PackedByteArray get_color_rgba8() const;
	PackedByteArray get_normal_rgba8() const;

	int get_texture_size() const;

	// --- Water strip tessellation (env #29) ---
	// Fills the WaterStripView from the active Godot camera: the D3D
	// row-vector view matrix + its inverse, the projection scales, the
	// camera basis rows, the 16.16 camera position, and the viewport rect
	// (min 0,0 / max = px - 1 / center = px / 2). The camera crosses into the
	// render (d3d) basis through the util/axes.h x/z swap, so every row
	// output (uv0, the texm3x2 bases) carries retail's components.
	void strip_set_view(const Transform3D &p_cam_transform, const Projection &p_cam_projection,
			const Vector2i &p_viewport_px, float p_fog_end_world);

	// Runs the witnessed row march [orig: Render_WaterStripDetailed
	// @ 0x5c27d0] against the last strip_set_view; returns the row count
	// (3 vertices per row). p_water_color_lit is g_EnvWaterColorLit as a
	// Color (bytes / 255); p_depth_scale/p_depth_bias the WaterDepthCurve pair.
	int strip_build(float p_plane_height_world, float p_murk, const Color &p_water_color_lit,
			float p_depth_scale, float p_depth_bias, bool p_underwater, bool p_nightvision);

	// Godot-space world positions reconstructed per vertex from the
	// witnessed render-basis uv0 = world x/32, z/32 pair + the plane height
	// [orig: flt_7DBFAC @ 0x5c2899, see docs/env/env-tod-re.md], swapped back
	// into Godot axes.
	PackedVector3Array strip_positions() const;
	// Row diffuse / specular ARGB per vertex as raw bytes / 255 (no
	// color-space conversion) [orig: written @ 0x5c2f0a..0x5c2f2b, see docs/env/env-tod-re.md].
	PackedColorArray strip_colors() const;
	// The specular again as 4 floats per vertex (RGBA, raw bytes / 255) —
	// ARRAY_CUSTOM1 under the RGBA_FLOAT format only accepts a
	// PackedFloat32Array, and the shader consumes it as CUSTOM1: the ps.1.1
	// v1 register [orig: add r0.rgb, r0, v1 — the detail>=2 pixel shader
	// assembled in Water_InitSurfaceShaders @ 0x5c19b0].
	PackedFloat32Array strip_custom1() const;
	// The witnessed texcoord 0 pair = render-basis world x/32, z/32 (Godot
	// world z/32, x/32). Retail duplicates it into texcoord 3, so the color
	// noise and the DuDv map both sample it verbatim
	// (retail Render_WaterStripDetailed @ 0x5c2aec..0x5c2b00, the t3 copy
	// @ 0x5c3095..0x5c30bf).
	PackedVector2Array strip_uv0() const;
	// 4 floats per vertex: [depth (the clamped fog W, vertex +0x08), rhw
	// (+0x0C), screen U (t1 3rd comp), screen V (t2 3rd comp)] — the
	// projective depth pair plus the texm3x2 screen lookup for the env #30
	// reflection consumer.
	PackedFloat32Array strip_custom0() const;
	// 4 floats per vertex: [t1.x, t1.y, t2.x, t2.y] — the texm3x2
	// perturbation basis (camera right/forward xz under the witnessed rhw
	// scales [orig: rows @ 0x5c2f83..0x5c3067, see docs/env/env-tod-re.md]) as the mesh's ARRAY_CUSTOM2;
	// the rows' 3rd components (screen U/V) ride strip_custom0's zw. The
	// env #30 reflection consumer dots both against the DuDv sample.
	PackedFloat32Array strip_custom2() const;
	// PRIMITIVE_TRIANGLES indices unrolled from the witnessed <=5-row
	// triangle-strip batches through kWaterStripIndexTable
	// [orig: word_841328; batch walk @ 0x5c3164..0x5c329e, see docs/env/env-tod-re.md].
	PackedInt32Array strip_indices() const;
};

} // namespace godot
