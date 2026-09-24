// The per-frame water-plane state assembly — the engine half of the shell's
// water applier. The
// noise tables, PRNG, and screen-marched strip tessellation are
// env_water_render.h's (engine/formats/env); this header owns the height
// precedence ladder and the per-frame value block the applier feeds the strip
// march and shader. RE record: docs/env/env-tod-re.md (env #28/#29).
#pragma once

#include <cstdint>

#include <runtime/environment/environment_state.h>

namespace opennova::env {

// The witnessed water-height precedence (env #28): mission/BMS override
// (explicit zero meaningful) > TRN flagged nonzero >
// ENV <<15 half-world-units [orig: env parse @ 0x52073b, then Terrain_Init
// @ 0x60fcba overrides when flagged, then the BMS override @ 0x525371;
// TimeOfDay_ParseProperty @ 0x57cb4e stores the .env value in half world
// units]. A loaded terrain or environment is authoritative even when its encoded height is
// zero (clear stale state instead of drawing phantom water); standalone
// owners with no source retain `current`.
struct WaterHeightRungs {
	bool has_mission_override = false;
	float mission_override = 0.0f;
	// The map's terrain water height in world units (raw * 0.5); nonzero
	// proxies the bit-31 "has water" flag
	// [orig: Terrain_Init @ 0x60fcb1..0x60fcba].
	float terrain_height = 0.0f;
	bool has_loaded_terrain = false;
};

float resolve_water_height(const WaterHeightRungs &rungs,
		const EnvironmentState *env, float current);

// One frame of strip-march/shader inputs. Defaults are the shader-uniform
// stand-in values so owners without a loaded env still march strips.
struct WaterFrameInputs {
	float murk = 0.6f;
	float fog_end = 1000.0f;
	// Water renders lit: water_rgb x (light*0.707 + sky) x 2, saturating
	// [orig: Environment_UpdateWeatherTick @ 0x57f16b].
	Rgb lit{0.408f, 0.314f, 0.224f};
	bool env_loaded = false;
};

// murk_default is the node's fallback water alpha when no env document
// supplies water_murk.
WaterFrameInputs build_water_frame_inputs(const EnvironmentState *env,
		float murk_default);

// The full-viewport underwater murk scissor drawn after the scene/viewmodel
// and before HUD: alpha = 0x80 - trunc(murk * -96) = 128 + trunc(96*murk).
// [orig: Terrain_RenderSceneWithReflection @ 0x5c96c5..0x5c96fa ->
// Terrain_DrawScissorRect @ 0x5c38e0]
uint8_t underwater_murk_overlay_alpha_byte(float murk);

// The camera-side gate of render_water_surface's two calls
// [orig: render_water_surface @ 0x5c32ed..0x5c330a]: the view-0 (above)
// call draws only while the camera is strictly above the plane
// (jle skip @ 0x5c330a) and the underwater view only while it is strictly
// below (jge skip @ 0x5c32fc); at exact equality neither side draws. The
// FrameFX bloom pass's nightvision redraw is the view-0 call
// [orig: FrameFX_RenderBloomPass @ 0x582a59..0x582a5d], so it shares the
// above gate and never runs underwater; it is not gated on g_WaterActive.
struct WaterSurfaceSides {
	bool above = false;
	bool underwater = false;
};

WaterSurfaceSides water_surface_sides(float eye_y, float water_height);

} // namespace opennova::env
