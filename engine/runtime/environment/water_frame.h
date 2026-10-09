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

// The witnessed water-height precedence (env #28, corrected by env #44): the
// mission header's override (attrib bit 0x1, explicit zero meaningful) > the
// .env's water_height > the .trn's. The time-of-day parse writes the .trn's
// line and then the .env's into the one global, half world units
// [orig: TimeOfDay_ParseProperty @ 0x57cb4e; Environment_LoadTimeOfDayConfig
// @ 0x57dbeb then @ 0x57dcbf], and the terrain's init after it stores only a
// height its caller flags in bit 31, which is the header's override alone
// [orig: Terrain_Init @ 0x60fcb1..0x60fcba; its one caller
// Game_LoadTerrainDuringConnect @ 0x520757..0x52076f passes the header's
// height | 0xFFFF0000 << 15, or 0]. An environment loaded through
// env::load_mission_env already holds the .trn's line under the .env's; the
// terrain rung stands for an owner whose environment did not read its .trn.
// A loaded terrain or environment is authoritative even when its encoded
// height is zero (clear stale state instead of drawing phantom water);
// standalone owners with no source retain `current`.
struct WaterHeightRungs {
	bool has_mission_override = false;
	float mission_override = 0.0f;
	// The map's terrain water height in world units (raw * kWaterHeightUnit); 0 for a
	// .trn with no water_height line or one of 0, either of which leaves
	// the parse's 0 [orig: Environment_InitDefaults @ 0x57c01e].
	float terrain_height = 0.0f;
	bool has_loaded_terrain = false;
};

// The rungs a mission's load fills [orig: Game_LoadTerrainDuringConnect
// @ 0x520710]: the header's attrib-gated override (half world units, as the
// header writes it) and the terrain's water height in world units, the
// terrain loaded or not.
WaterHeightRungs mission_water_rungs(const BmsEnvOverrides &overrides,
		float terrain_height, bool has_loaded_terrain);

// Which rung gave the plane its height: the header's override, the
// environment's water_height, the terrain's, or none (0 under a loaded
// terrain or environment, else `current` kept).
enum class WaterRung : uint8_t { Mission, Environment, Terrain, None };
struct ResolvedWaterHeight {
	float height = 0.0f;
	WaterRung rung = WaterRung::None;
};
ResolvedWaterHeight resolve_water_rung(const WaterHeightRungs &rungs,
		const EnvironmentState *env, float current);
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
// [orig: Terrain_RenderWorldScene @ 0x5c96c5..0x5c96fa ->
// Render_DrawViewportColorQuad @ 0x5c38e0]
uint8_t underwater_murk_overlay_alpha_byte(float murk);

// The camera-side gate of Render_WaterSurface's two calls
// [orig: Render_WaterSurface @ 0x5c32ed..0x5c330a]: the view-0 (above)
// call draws only while the camera is strictly above the plane
// (jle skip @ 0x5c330a) and the underwater view only while it is strictly
// below (jge skip @ 0x5c32fc); at exact equality neither side draws. The
// FrameFX bloom pass's nightvision redraw is the view-0 call
// [orig: FrameFX_RenderGlowSource @ 0x582a59..0x582a5d], so it shares the
// above gate and never runs underwater; it is not gated on g_WaterActive.
struct WaterSurfaceSides {
	bool above = false;
	bool underwater = false;
};

WaterSurfaceSides water_surface_sides(float eye_y, float water_height);

} // namespace opennova::env
