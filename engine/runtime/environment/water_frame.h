// The per-frame water-plane state assembly — the engine half of the shell's
// water applier. The
// noise tables, PRNG, and screen-marched strip tessellation are
// env_water_render.h's (engine/formats/env); this header owns the height
// precedence ladder and the per-frame value block the applier feeds the strip
// march and shader. RE record: docs/env/env-tod-re.md (env #28/#29).
#pragma once

#include <cstdint>
#include <optional>

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
// The same rungs from the header's override already in world units (none:
// not flagged), as the live Water node holds it.
WaterHeightRungs water_height_rungs(std::optional<float> mission_override,
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

// A mission's water plane as its load leaves it, with no plane before it:
// the header's flagged override [orig: Terrain_Init @ 0x60fcb1..0x60fcba,
// which stores only a height its caller flags in bit 31, the header's], else
// the environment's water_height line, else the terrain's, else none (0). The
// environment's line is the one global every pass of the time-of-day load
// writes after Environment_InitDefaults zeroed it [orig: Environment_InitDefaults
// memset @ 0x57c01e over g_EnvWaterHeightFixed; TimeOfDay_ParseProperty
// @ 0x57cb45..0x57cb6a, atol << 15]: the .trn's, then overcast.def's, then
// the .env's; a .env that is not there returns before its pass [orig:
// Environment_LoadTimeOfDayConfig @ 0x57dca3] and the earlier passes' line
// stands. So `environment` is the config that load made
// (env::load_mission_env_config, or env::load_mission_env over the three
// texts) whether or not it read the .env, and the plane has no terrain rung:
// the global has no terrain writer, a .trn's water_height being a time-of-day
// line, the first pass's (env #46). The host boot, the editor's ground facts
// and its environment uses resolve it here alike.
ResolvedWaterHeight mission_water_rung(const BmsEnvOverrides &overrides, const Config &environment);

// One frame of strip-march/shader inputs. Defaults are the shader-uniform
// stand-in values so owners without a loaded env still march strips.
struct WaterFrameInputs {
	float murk = 0.6f;
	// The smoothed fog distance (g_EnvFogDistance): the scene far plane and
	// the strip's depth curve read it.
	float fog_end = 1000.0f;
	// The strip march's fog end per side, the device fog range
	// Environment_GetFogEndDistance serves the pass: above the plane the
	// smoothed distance attenuated by the overcast blend [orig:
	// Environment_GetFogEndDistance @ 0x57e426], under it the murk
	// visibility curve [orig: Environment_GetFogEndDistance @ 0x57e40d].
	// Without a loaded environment both sides march the dry one.
	float pass_fog_end_above = 1000.0f;
	float pass_fog_end_underwater = 1000.0f;
	// Water renders lit: water_rgb x (light*0.707 + sky) x 2, saturating
	// [orig: Environment_UpdateWeatherTick @ 0x57f16b].
	Rgb lit{0.408f, 0.314f, 0.224f};
	bool env_loaded = false;
};

// murk_default is the node's fallback water alpha when no env document
// supplies water_murk.
WaterFrameInputs build_water_frame_inputs(const EnvironmentState *env,
		float murk_default);

// The fog end one side's strip march takes (WaterFrameInputs carries both).
constexpr float water_pass_fog_end(const WaterFrameInputs &inputs, bool underwater) {
	return underwater ? inputs.pass_fog_end_underwater : inputs.pass_fog_end_above;
}

// The full-viewport underwater murk scissor drawn after the scene/viewmodel
// and before HUD: alpha = 0x80 - trunc(murk * -96) = 128 + trunc(96*murk).
// [orig: Terrain_RenderWorldScene @ 0x5c96c5..0x5c96fa ->
// Render_DrawViewportColorQuad @ 0x5c38e0]
uint8_t underwater_murk_overlay_alpha_byte(float murk);

// The no-water sentinel belongs to the water's own draws alone: a zero
// g_EnvWaterHeightFixed skips the surface [orig: Render_WaterSurface
// @ 0x5c32e0..0x5c32e7], the mirror render and with it the mirror's clip arm
// (the main scene's and the weapon Inset's prerender alike) [orig:
// Render_MainScene @ 0x5c125e..0x5c1268, the return @ 0x5c19a5] and the glint
// [orig: Terrain_RenderWorldScene @ 0x5c96b5..0x5c96be, ahead of
// Environment_UpdateSunGlare's call @ 0x5c96c0]. The waterline itself stands
// at zero: the transparent split is stored from the height with no test
// (renderer/render_order.h transparent_queue_for), and every eye-side test
// compares the eye against the height the same way
// (EnvironmentState::classify_render_eye).
constexpr bool water_draws_at_height(float water_height) {
	return water_height != 0.0f;
}

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

// One scene's water passes this display frame, from the eye's side of the
// plane and the scene's pass gate (g_WaterActive, renderer quadtree.h
// water_pass_active): the beauty pass's strip on the eye's side while the gate
// holds [orig: Terrain_RenderWaterPass @ 0x610640 -> Render_WaterSurface
// @ 0x5c32e0 (the sides @ 0x5c32ed..0x5c330a)], the bloom pass's
// nightvision redraw above the plane whatever the gate [orig:
// FrameFX_RenderGlowSource @ 0x582a59..0x582a5d, Render_WaterSurface(0, 1)],
// and the mirror prerender ahead of every scene pass, on the gate alone: not
// the eye's side and not the scene's water letter [orig: Render_TerrainScene
// @ 0x610cd9..0x610ce0 -> Water_ReflectionPrerender @ 0x5c2780], so the
// mirror the redraw samples renders under a skipping label too (env #30,
// docs/env/env-tod-re.md).
struct WaterFramePasses {
	WaterSurfaceSides sides{};
	bool beauty = false;
	bool night_vision = false;
	bool refresh_mirror = false;
	// Every Render_WaterSurface call regenerates the noise pair at the frame's
	// counter [orig: Render_WaterSurface @ 0x5c3326 -> Water_GenerateNoiseTextures
	// @ 0x5C0360]; a frame with no call leaves it.
	constexpr bool any() const { return beauty || night_vision; }
};
WaterFramePasses water_frame_passes(float eye_y, float water_height, bool water_pass_active);

// The weapon Inset pass's water (the witnesses: water_mirror.h
// inset_mirror_source, water_pass_latch above): its mirror renders while its
// own g_WaterActive holds, and its strip past its own water letter on the
// eye's side; the Inset pass runs no bloom redraw.
WaterFramePasses inset_water_frame_passes(float eye_y, float water_height,
		bool water_pass_active, bool view_water_letter);

// The noise pair's frame counter: the world's entity-update count once a world
// feeds it (the counter retail's generator reads [orig:
// Water_GenerateNoiseTextures @ 0x5C0360, its counter @ 0x5C0366]), else the
// water's own count of rendered water frames. The pair is a pure function of
// the counter, so every call of a frame (the beauty strip, the redraw, the
// Inset's strip) reads the one regeneration at that counter.
class WaterNoiseClock {
public:
	// The world's entity-update count; from here the clock no longer counts
	// rendered frames.
	void feed(uint32_t counter) {
		counter_ = counter;
		fed_ = true;
	}
	// A display frame that renders water: an unfed clock counts it.
	void advance_rendered_frame() {
		if (!fed_) {
			++counter_;
		}
	}
	// True once per counter value: the pair regenerates at counter().
	bool take_regeneration() {
		if (generated_valid_ && generated_ == counter_) {
			return false;
		}
		generated_ = counter_;
		generated_valid_ = true;
		return true;
	}
	// The pair's tables were made again: the next call regenerates.
	void invalidate() { generated_valid_ = false; }
	uint32_t counter() const { return counter_; }

private:
	uint32_t counter_ = 0;
	uint32_t generated_ = 0;
	bool fed_ = false;
	bool generated_valid_ = false;
};

// The water pass's latch, the Blink water word g_WaterActive reads
// [orig: g_WaterPassBlinkLatch @ 0x31BC910]. A
// scene's two water passes run only past its water letter (Terrain_RenderWorldScene @ 0x5c93a0:
// the label @0x5c93b8..0x5c93cd ahead of the Terrain_RenderWaterPass calls @0x5c95dc /
// @0x5c95ea), and each stores the g_BlinkWaterVisible that scene's collect left
// (Terrain_RenderWaterPass @ 0x610640: the store @0x610666, or @0x610671 with no water active).
// Terrain_SetupViewAndLighting @ 0x60fe40 then forces g_WaterActive on it (the test @0x60ff20,
// the store @0x60ff33). It is one word for every scene of a frame: the main scene's
// g_WaterActive (its Render_TerrainScene, the call @0x5ca504) reads what the last scene's water
// pass left; the weapon Inset pass's own (its Render_TerrainScene, the call @0x5c99b7) reads
// the main collect's of the same frame (Terrain_RenderWorldScene, the call @0x5ca8ec); and the
// Inset's collect (the call @0x5c9de9) leaves its own for the next frame's main scene. A scene
// whose letter skips its water passes leaves the latch as it was.
constexpr bool water_pass_latch(bool latch, bool water_pass_runs, bool blink_water_visible) {
	return water_pass_runs ? blink_water_visible : latch;
}

} // namespace opennova::env
