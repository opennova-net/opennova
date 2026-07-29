#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
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

#include <env/env_water_render.h>

namespace godot {

// The per-frame water surface core of [orig: render_water_surface @ 0x5c32c0]:
// the animated 128x128 ridge color/alpha texture and its DuDv/normal
// derivative [orig: Water_GenerateNoiseTextures @ 0x5c0360], plus the
// screen-marched strip tessellation of the detailed tier (env #29)
// [orig: render_water_strip_detailed @ 0x5c27d0]. All math lives in libs/env
// (env/env_water_render.h); this binding owns the static tables (built once with
// the witnessed init, from the boot PRNG state), the frame buffers, and the
// Godot<->render basis conversion for the strip view state. NovaWater updates
// once per frame, blits the textures into ImageTextures, and rebuilds its
// ArrayMesh surface from the strip buffers. RE record: docs/env/env-tod-re.md
// "Water surface".
class NovaWaterCore : public RefCounted {
	GDCLASS(NovaWaterCore, RefCounted)

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

protected:
	static void _bind_methods();

public:
	// Regenerates both textures for the given 62 Hz frame counter
	// [orig: called per frame from render_water_surface @ 0x5c3326].
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
	// (min 0,0 / max = px - 1 / center = px / 2). Godot world axes coincide
	// componentwise with the render (d3d) basis — see the .cpp.
	void strip_set_view(const Transform3D &p_cam_transform, const Projection &p_cam_projection,
			const Vector2i &p_viewport_px, float p_fog_end_world);

	// Runs the witnessed row march [orig: render_water_strip_detailed
	// @ 0x5c27d0] against the last strip_set_view; returns the row count
	// (3 vertices per row). p_water_color_lit is Env_WaterColorLit as a
	// Color (bytes / 255); p_uv_scale/p_uv_bias the WaterUvState pair.
	int strip_build(float p_plane_height_world, float p_murk, const Color &p_water_color_lit,
			float p_uv_scale, float p_uv_bias, bool p_underwater, bool p_nightvision);

	// Godot-space world positions reconstructed per vertex from the
	// witnessed uv0 = world x/32, z/32 pair + the plane height
	// [orig: flt_7DBFAC @ 0x5c2899].
	PackedVector3Array strip_positions() const;
	// Row diffuse / specular ARGB per vertex as raw bytes / 255 (no
	// color-space conversion) [orig: written @ 0x5c2f0a..0x5c2f2b].
	PackedColorArray strip_colors() const;
	PackedColorArray strip_speculars() const;
	// The specular again as 4 floats per vertex (RGBA, raw bytes / 255) —
	// ARRAY_CUSTOM1 under the RGBA_FLOAT format only accepts a
	// PackedFloat32Array, and the shader consumes it as CUSTOM1: the ps.1.1
	// v1 register [orig: add r0.rgb, r0, v1 — the detail>=2 pixel shader
	// assembled in Water_InitSurfaceShaders @ 0x5c19b0].
	PackedFloat32Array strip_custom1() const;
	// The witnessed world/32 texcoord 0 pair, carried for parity/debug.
	PackedVector2Array strip_uv0() const;
	// 4 floats per vertex: [depth (the clamped fog W, vertex +0x08), rhw
	// (+0x0C), screen U (t1 3rd comp), screen V (t2 3rd comp)] — the
	// projective depth pair plus the texm3x2 screen lookup for the env #30
	// reflection consumer.
	PackedFloat32Array strip_custom0() const;
	// 4 floats per vertex: [t1.x, t1.y, t2.x, t2.y] — the texm3x2
	// perturbation basis (camera right/forward xz under the witnessed rhw
	// scales [orig: rows @ 0x5c2f83..0x5c3067]) as the mesh's ARRAY_CUSTOM2;
	// the rows' 3rd components (screen U/V) ride strip_custom0's zw. The
	// env #30 reflection consumer dots both against the DuDv sample.
	PackedFloat32Array strip_custom2() const;
	// PRIMITIVE_TRIANGLES indices unrolled from the witnessed <=5-row
	// triangle-strip batches through kWaterStripIndexTable
	// [orig: word_841328; batch walk @ 0x5c3164..0x5c329e].
	PackedInt32Array strip_indices() const;
};

} // namespace godot
