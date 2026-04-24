#pragma once

#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace opennova::env {

// Engine: jodemo.exe environment subsystem documented in docs/engine_spec_env.md.
// Native equivalents:
//   - Terrain_SetDefaultEnvironmentValues@0x53E030
//   - sub_53E3F0@0x53E3F0 (.env/.trn keyword callback)
//   - sub_53FA70@0x53FA70 (load, sort, snapshot)
//   - sub_53FCC0@0x53FCC0 (per-frame TOD color interpolation)
//   - Terrain_CalcSunDirection@0x53F5D0
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

struct Config {
	std::string name = "Untitled";
	std::string timeofday = "Day";
	float envscale = 1.0f;
	int curtime = 1200;
	float fog_level = 1000.0f;
	int fog_type = 2;
	Rgb terrain_rgb = {1.0f, 1.0f, 1.0f};
	Rgb water_rgb = {56.0f / 255.0f, 59.0f / 255.0f, 39.0f / 255.0f};
	Rgb cloud_rgb = {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f};
	Rgb vertex_rgb = {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f};
	Rgb lightning_rgb = {85.0f / 255.0f, 85.0f / 255.0f, 90.0f / 255.0f};
	Rgb ceiling_rgb = {55.0f / 255.0f, 55.0f / 255.0f, 55.0f / 255.0f};
	Rgb floor_rgb = {25.0f / 255.0f, 25.0f / 255.0f, 25.0f / 255.0f};
	float sky_speed = 15.0f;
	float sky_height = 175.0f;
	std::string sky_map1 = "Cloud01.pcx";
	std::string sky_map2 = "Cloud01b.pcx";
	std::string sun_3di = "msun.3di";
	std::string moon_3di = "fmoon4.3di";
	std::string glare_3di = "mglare.3di";
	std::string star_3di;
	float iris_percent = 15.0f;
	float iris_center = 1.0f;
	float water_murk = 0.8f;
	int advanced_clouds = 1;
	std::vector<Keyframe> keyframes;
};

Config make_default_config();

bool load_env(std::istream &input, Config &out, std::string &error);
bool save_env(std::ostream &output, const Config &cfg, std::string &error);

TodState interpolate_tod(const std::vector<Keyframe> &keyframes, float time, float envscale = 1.0f);

Vec3 compute_sun_direction(float tod_time);
Vec3 compute_moon_direction(float tod_time);

} // namespace opennova::env
