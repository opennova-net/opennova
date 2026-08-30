// The typed environment record the embedder pushes into the F3 Environment
// window (ADR 0042 d6: records in, typed requests out). A plain value the
// embedder builds from the weather home (world::WeatherState), the mission
// document, the occlusion blink flags and the local view, snapshotted here so
// the window never reaches into a live World. The rows are the retail
// environment debug page's [orig: Debug_DrawEnvironmentValues @ 0x4ef000 —
// "Script & Env Values": Env/Trn, Loc, Reverb, Blink, Fogtype, Fogdist,
// ColorFade, SunFade, MoonLight, Fog/SkyFog/Cloud/Sun/Lightning/Sky/Ground/
// Ceiling/Floor block currents, FOV, SkyHeight, SkySpeed, OutDoor, InDoor,
// Gain, Iris, Rain %, Overcast %, Complexity, DCB]. An invalid snapshot clears
// the window (the world unloaded).
#pragma once

#include <cstdint>
#include <string>

namespace opennova::devtools {

struct EnvironmentSnapshot {
	bool valid = false;
	uint64_t logic_tick = 0;
	std::string env_name;   // Bms_EnvironmentName
	std::string trn_name;   // Bms_MapBaseName
	// g_LocalPlayerBlinkFlags — bits 2/4/8/0x10/0x20 print V/S/W/L/O.
	uint32_t blink_flags = 0;
	int32_t fog_type = 0;
	int32_t fog_dist_metres = 0;    // Env_FogDistCurrent hi word
	int32_t fog_target_metres = 0;
	int32_t color_fade_seconds = 0; // (Env_ColorFadeTicks + 31) / 62
	int32_t sun_fade_pct = 0;       // Env_SunDimPctCurrent hi word
	bool night = false;             // Env_IsNightPhase
	// The block CURRENT render colors, packed 0x00RRGGBB.
	uint32_t fog_rgb = 0;
	uint32_t skyfog_rgb = 0;
	uint32_t cloud_rgb = 0;
	uint32_t sun_rgb = 0;
	uint32_t lightning_rgb = 0;
	uint32_t sky_rgb = 0;
	uint32_t ground_rgb = 0;
	uint32_t ceiling_rgb = 0;
	uint32_t floor_rgb = 0;
	uint32_t outdoor_rgb = 0;       // Env_TerrainLightCombined
	uint32_t indoor_rgb = 0;        // Env_CeilingFloorBlend
	uint32_t gain_rgb = 0;          // Env_ModulatorBlock
	uint32_t iris_rgb = 0;          // Env_Modulator2Block
	int32_t fov_degrees = 0;
	int32_t sky_height_metres = 0;  // Env_SkyHeightCurrent hi word
	int32_t sky_speed = 0;          // Env_CloudScrollRate >> 10
	int32_t rain_pct = 0;           // (100 * Env_RainPctCurrent) >> 16
	int32_t rain_target_pct = 0;
	int32_t overcast_pct = 0;       // (100 * Env_OvercastBlend) >> 16
	int32_t overcast_target_pct = 0;
	int32_t complexity = 0;         // g_ProxCandidateArenaUsed
	// The live weather state beyond the retail page (the control strip's
	// seeds): minute of day, quake ticks, precipitation kind, wind scale,
	// lightning timers.
	int32_t minute_of_day = 0;
	int32_t quake_ticks = 0;
	int32_t precipitation_kind = 0;
	int32_t wind_scale = 0;
	int32_t lightning_timer_a = 0;
	int32_t lightning_timer_b = 0;
	int32_t lightning_level = 0;
	bool authority = false;         // the commands land (not a joiner)
	bool tod_keyframed = true;      // a keyframe table re-snaps the five keyframed color rows
};

}  // namespace opennova::devtools
