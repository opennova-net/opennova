#pragma once

// Portable precipitation (rain/snow streak) compilation — the styled half of
// the weather-particle split: the SIM owns the drop pool
// (runtime/environment/precipitation.h, fed by the World's weather tick and
// the kernel's per-render update); this module owns the witnessed per-drop
// triangle build an embedding renderer uploads verbatim. No Godot or
// simulator dependencies.
//
// [orig: render_weather_trail_particles @ 0x5dee10 — gated on
// Env_RainPctCurrent > 48; camera velocity = the camera's delta since the
// drawer's last call under the same camera mode, else zero (clamped to 0.2),
// the fall vector = the accumulated per-tick
// fall since the last draw (clamped to 0.1, then zeroed @ 0x5deee8); snow:
// trail = camera up x 0.05, right = camera right x 0.025; rain: trail =
// (0, 0.1, 0) + velocity - fall, right = camera right x 0.01, width
// 1 + dist x 0.1, length 1 + dist x 0.05; per slot with z >= floor one
// triangle {pos + trail x length (uv 0.5, 0); pos - right x width (0, 1);
// pos + right x width (1, 1)}, diffuse Env_TerrainLightCombined | 0xFF000000,
// 768-vertex TRIANGLELIST batches under an identity world matrix; textures
// eraindrp.tga / jsnwflk.tga (WeatherParticle_LoadTextures @ 0x5de840), the
// one-texture mode word 0x651 (SRCALPHA / INVSRCALPHA, alpha MODULATE, color
// MODULATE2X) with pass flags 0x10500000 (lighting off, z-write off, cull
// none, no fog, no alpha test).]

#include <runtime/environment/precipitation.h>

#include <cstdint>
#include <vector>

namespace opennova::renderer {

// The camera-side inputs in the RENDER (Godot) frame: mission (x, y, z)
// 16.16 maps to (x, z, -y) / 65536 [orig: Math_FixedPointToFloat3_YNegated
// @ 0x611210 — the same axis swap under the D3D basis].
struct PrecipitationCamera {
	int32_t position_q16[3] = {0, 0, 0}; // mission frame 16.16
	float right[3] = {1.0f, 0.0f, 0.0f}; // render frame, unit
	float up[3] = {0.0f, 1.0f, 0.0f};    // render frame, unit
	// The scene's camera mode, the drawer's second argument
	// [orig: g_CameraMode pushed @ 0x5c969a..0x5c96a4].
	int32_t mode = 0;
};

// The drawer's call-to-call memory: the last call's camera mode and camera
// position (the position's delta is the rain velocity term)
// [orig: dword_2C05A14 (the mode), dword_2C05A18..20 (the position)]. Retail
// keeps it in zero-initialized data only the drawer writes, never reset, so
// the first call at mode 0 measures its velocity from the origin.
// Every scene pass calls the drawer at its own camera, so while the weapon
// Inset renders its call reads the main pass's camera of the same frame and
// the next main call reads the Inset's (runtime/renderer/scene_overlay.h
// kInsetOverlayOrder carries the witness); one state serves both calls.
struct PrecipitationDrawState {
	int32_t last_mode = 0;
	int32_t last_camera_q16[3] = {0, 0, 0};
};

// One compiled frame: `drops` triangles, 3 vertices each, interleaved
// {x, y, z, u, v} (stride 5) in the render frame; one diffuse color for all.
struct PrecipitationDrawFrame {
	std::vector<float> vertices;
	int drops = 0;
	uint32_t color_argb = 0xFF000000u; // Env_TerrainLightCombined | 0xFF000000
	bool snow = false;                 // selects the texture (jsnwflk vs eraindrp)

	void clear() {
		vertices.clear();
		drops = 0;
	}
};

// The witnessed drop-streak build over the first active_count(rain) slots.
// Consumes and ZEROES the field's fall accumulator (the drawer's read-then-
// clear) and advances the draw state's camera memory. An un-raining field
// (rain_pct_q16 <= 48) compiles nothing and touches neither: the drawer
// returns before any of it [orig: @ 0x5dee27..0x5dee48].
void compile_precipitation_frame(env::PrecipitationField &field,
		int32_t rain_pct_q16, uint32_t precipitation_kind,
		uint32_t terrain_light_combined_rgb, const PrecipitationCamera &camera,
		PrecipitationDrawState &state, PrecipitationDrawFrame &out);

// The velocity / fall clamps [orig: @ 0x5df097 / @ 0x5df0ea].
inline constexpr float kPrecipitationVelocityClamp = 0.2f;
inline constexpr float kPrecipitationFallClamp = 0.1f;
// The retail batch size (the presenter may upload larger batches; the split
// is a device limit, not a look) [orig: @ 0x5df347].
inline constexpr int kPrecipitationBatchVertices = 768;

} // namespace opennova::renderer
