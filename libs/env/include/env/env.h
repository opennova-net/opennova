#pragma once

#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace opennova::env {

// Engine: Jointops.exe (retail JO:CA) environment subsystem. RE record:
// docs/env/env-tod-re.md. Native equivalents:
//   [orig: Environment_InitDefaults @ 0x57c010]            (Config defaults)
//   [orig: TimeOfDay_ParseProperty @ 0x57c590]             (.trn/.env keyword callback)
//   [orig: Environment_ParseTimeString @ 0x57c500]         (HHMM -> 16.16 hours)
//   [orig: Environment_SortAndSnapshotKeyframes @ 0x57c240] (stable sort + snapshot)
//   [orig: Environment_FindKeyframeSegment @ 0x57dd80]     (bracketing, 24h wrap)
//   [orig: Environment_ComputeTimeOfDayColors @ 0x57de40]  (per-tick TOD interpolation)
//   [orig: Environment_ComputeSunDirection @ 0x57d6d0]
//   [orig: Terrain_ComputeMoonDirection @ 0x57d760]
struct Rgb {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
};

struct Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct Keyframe {
	int time = 0;
	Rgb sun;
	Rgb ground;
	Rgb fog;
	Rgb sky;
	Rgb moon;
	Rgb skyfog;
	Rgb skybase;
	Rgb skybright;
	Rgb skyhighlight;
	Rgb cloudbase;
	Rgb cloudhighlight;
	Rgb cloudedge;
};

struct TodState {
	Rgb sun;
	Rgb ground;
	Rgb fog;
	Rgb sky;
	Rgb moon;
	Rgb skyfog;
	Rgb skybase;
	Rgb skybright;
	Rgb skyhighlight;
	Rgb cloudbase;
	Rgb cloudhighlight;
	Rgb cloudedge;
};

// Field defaults mirror the engine's pre-parse state [orig: Environment_InitDefaults
// @ 0x57c010]: this is what an .env that omits a keyword means to the engine.
// (sky_height keeps the engine's raw-200 quirk, ~0.003 units; every shipped file
// sets sky_height. make_default_config() overrides fields for authoring.)
struct Config {
	std::string name = "Untitled"; // authoring extension; retail JO has no enviro_name keyword
	std::string timeofday = "Day";
	float envscale = 1.0f;
	int curtime = 1500;
	float fog_level = 1024.0f;
	int fog_type = 1;
	Rgb terrain_rgb = {1.0f, 1.0f, 1.0f};
	Rgb water_rgb = {104.0f / 255.0f, 80.0f / 255.0f, 57.0f / 255.0f};
	float water_height = 0.0f;
	bool water_height_set = false;
	Rgb cloud_rgb = {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f};
	Rgb vertex_rgb = {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f}; // parsed for round-trip; retail JO ignores it
	Rgb lightning_rgb = {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f};
	Rgb ceiling_rgb = {51.0f / 255.0f, 54.0f / 255.0f, 64.0f / 255.0f};
	Rgb floor_rgb = {46.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f};
	float sky_speed = 0.0f;
	float sky_height = 200.0f / 65536.0f;
	std::string sky_map1 = "cld_day1.pcx";
	std::string sky_map2 = "cld_day1b.pcx";
	std::string sun_3di = "msun.3di";
	std::string moon_3di = "fmoon4.3di";
	std::string glare_3di = "mglare.3di";
	std::string star_3di = "mstar.3di";
	float iris_percent = 50.0f;
	float iris_center = 1.25f;
	float water_murk = 0.8f;
	int advanced_clouds = 0;
	std::vector<Keyframe> keyframes;
};

// The engine parses at most 16 TOD keyframes; later tod_begin blocks bleed their
// colors into the 16th slot [orig: TimeOfDay_ParseProperty @ 0x57c65b].
inline constexpr int kMaxTodKeyframes = 16;

Config make_default_config();

bool load_env(std::istream &input, Config &out, std::string &error);
bool save_env(std::ostream &output, const Config &cfg, std::string &error);

// HHMM (digits clamped positionally: hours <= 23, minutes <= 59) to 16.16
// fixed-point hours [orig: Environment_ParseTimeString @ 0x57c500].
int hhmm_to_hours_fp(float hhmm);

// Interpolates in 16.16 HOURS space with the engine's integer math: byte
// quantization (x envscale, truncated, clamped <= 255) before the lerp,
// per-channel (t*(b-a) + (a<<16) + 0x8000) >> 16 rounding, truncating segment
// fraction, 24h wrap, and the t > 63356 full-snap quirk.
// [orig: Environment_ComputeTimeOfDayColors @ 0x57de40 + helpers]
TodState interpolate_tod(const std::vector<Keyframe> &keyframes, float time, float envscale = 1.0f);

Vec3 compute_sun_direction(float tod_time);
Vec3 compute_moon_direction(float tod_time);

} // namespace opennova::env
