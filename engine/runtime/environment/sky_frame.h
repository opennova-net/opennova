// The per-frame sky-dome state assembly — the engine half of the shell's
// dome applier. The dome
// geometry itself is env::build_sky_dome_mesh (engine/formats/env, built once
// at the reference height per env #20's ratified fold); this header owns the
// per-frame value block the applier pushes into the dome shader.
// Engine equivalents: [orig: SkyDome_BuildMesh @ 0x578db0] and
// [orig: Render_Skybox @ 0x579080], fed by
// [orig: Environment_ComputeTimeOfDayColors @ 0x57de40] interpolated TOD
// colors; see docs/env/env-tod-re.md.
#pragma once

#include <runtime/environment/environment_state.h>
#include <runtime/environment/weather_runtime.h>

namespace opennova::env {

// One frame of dome shader inputs.
struct SkyFrameState {
	bool loaded = false;
	// Flat cloud-color dome: the original applies the pass-1 NULL-texture
	// effect with material ambient = cloud_rgb against D3DRS_AMBIENT
	// 0xFFFFFF — the only render path that reads cloud_rgb
	// [orig: Render_Skybox @ 0x579b42..0x579bb6].
	bool flat_pass = false;
	Rgb flat_color;
	// Retail unpacks these six packed ENV blocks at 2/255, without clamping
	// the uploaded constants [orig: Color_UnpackToFloat4 @ 0x578985; six call
	// sites @ 0x579312..0x57936f], dimmed under the first-person NVG view and
	// overwritten white in the thermal view (sky_frame.cpp carries both
	// witnesses). Fog does not use this helper: D3DRS_FOGCOLOR consumes its
	// packed byte color.
	Rgb sky_base;
	Rgb sky_bright;
	Rgb sky_highlight;
	Rgb cloud_base;
	Rgb cloud_highlight;
	Rgb cloud_edge;
	// Pass 1 is always sun-driven; the cloud pass follows the active light,
	// moon at night [orig: Render_Skybox @ 0x579287
	// Terrain_GetSunDirectionAsFloat vs @ 0x579291
	// Environment_GetLightDirectionFloat].
	Vec3 sun_dir{};
	Vec3 light_dir{};
	// The sky pass temporarily swaps the device fog color from world fog to
	// the post-horizon-blend, doubled skyfog block (0x808080 in the thermal
	// view), then restores world fog [orig: sky fog wrapper SkyDome_RenderWithSkyfog
	// @ 0x579cb0].
	Rgb skyfog_color;
	float fog_end = 1000.0f;
	// Witnessed defaults when no environment backs the dome.
	float sky_speed = 15.0f;
	float sky_height = 175.0f;
	// No clear colour: the sky pass swaps only the device fog colour. The
	// whole frame is cleared BEFORE it (EnvironmentState::frame_clear_color_for)
	// [orig: Render_ProcessMainSceneFrame @ 0x5ca771..0x5ca7bf, then
	//  SkyDome_RenderWithSkyfog @ 0x5ca81a].
};

SkyFrameState build_sky_frame(const EnvironmentState &env);

// The dome follows the camera in xz and rides at HALF the camera height
// [orig: Render_Skybox @ 0x5790d0 — world translation z = camHeight >> 1].
inline Vec3 sky_dome_anchor(const Vec3 &cam_pos) {
	return Vec3{cam_pos.x, cam_pos.y * 0.5f, cam_pos.z};
}

// Standalone fallback for owners with no weather runtime: a private core
// ticked for its cloud scroll only at the same fixed 62 Hz cadence — the
// integer scroll math has ONE home either way. Rendering may run at any
// refresh rate.
struct ScrollFallback {
	WeatherCore core;
	double tick_credit = 0.0;

	void advance(double delta, float sky_speed);
};

} // namespace opennova::env
