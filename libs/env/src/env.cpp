#include "env/env.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace opennova::env {

namespace {

constexpr const char *NL = "\r\n";

std::string trim(const std::string &value) {
	size_t begin = 0;
	while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
		++begin;
	}
	size_t end = value.size();
	while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
		--end;
	}
	return value.substr(begin, end - begin);
}

std::string unquote(const std::string &value) {
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
		return value.substr(1, value.size() - 2);
	}
	return value;
}

int clamp_int(int value, int min_value, int max_value) {
	return std::max(min_value, std::min(max_value, value));
}

float clamp_float(float value, float min_value, float max_value) {
	return std::max(min_value, std::min(max_value, value));
}

bool parse_rgb(const std::string &text, Rgb &out, float scale = 1.0f) {
	int r = 0;
	int g = 0;
	int b = 0;
	if (std::sscanf(text.c_str(), "%d,%d,%d", &r, &g, &b) != 3) {
		return false;
	}
	out.r = clamp_float(static_cast<float>(r) * scale / 255.0f, 0.0f, 1.0f);
	out.g = clamp_float(static_cast<float>(g) * scale / 255.0f, 0.0f, 1.0f);
	out.b = clamp_float(static_cast<float>(b) * scale / 255.0f, 0.0f, 1.0f);
	return true;
}

std::string rgb_to_string(const Rgb &color) {
	const int r = clamp_int(static_cast<int>(color.r * 255.0f + 0.5f), 0, 255);
	const int g = clamp_int(static_cast<int>(color.g * 255.0f + 0.5f), 0, 255);
	const int b = clamp_int(static_cast<int>(color.b * 255.0f + 0.5f), 0, 255);
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%d,%d,%d", r, g, b);
	return buffer;
}

std::string number_to_string(float value) {
	std::ostringstream output;
	output << std::setprecision(6) << std::defaultfloat << value;
	return output.str();
}

std::string format_tod_time(int time) {
	const int normalized = clamp_int(time, 0, 2359);
	std::ostringstream output;
	output << std::setw(4) << std::setfill('0') << normalized;
	return output.str();
}

void assign_tod_color(Keyframe &keyframe, const std::string &key, const std::string &value, bool &skyfog_set) {
	// sub_53E3F0 writes RGB values into the currently active TOD keyframe.
	// fog_rgb also mirrors into skyfog_rgb until an explicit skyfog_rgb appears
	// (engine_spec_env.md §8.1, sub_53E3F0@0x53e805).
	if (key == "sun_rgb") {
		parse_rgb(value, keyframe.sun);
	} else if (key == "ground_rgb" || key == "ambient_rgb") {
		parse_rgb(value, keyframe.ground);
	} else if (key == "fog_rgb") {
		parse_rgb(value, keyframe.fog);
		if (!skyfog_set) {
			keyframe.skyfog = keyframe.fog;
		}
	} else if (key == "sky_rgb") {
		parse_rgb(value, keyframe.sky);
	} else if (key == "moon_rgb") {
		parse_rgb(value, keyframe.moon);
	} else if (key == "skyfog_rgb") {
		parse_rgb(value, keyframe.skyfog);
		skyfog_set = true;
	} else if (key == "skybase_rgb") {
		parse_rgb(value, keyframe.skybase);
	} else if (key == "skybright_rgb") {
		parse_rgb(value, keyframe.skybright);
	} else if (key == "skyhighlight_rgb") {
		parse_rgb(value, keyframe.skyhighlight);
	} else if (key == "cloudbase_rgb") {
		parse_rgb(value, keyframe.cloudbase);
	} else if (key == "cloudhighlight_rgb") {
		parse_rgb(value, keyframe.cloudhighlight);
	} else if (key == "cloudedge_rgb") {
		parse_rgb(value, keyframe.cloudedge);
	}
}

Rgb lerp_rgb(const Rgb &a, const Rgb &b, float t) {
	return {
		a.r + (b.r - a.r) * t,
		a.g + (b.g - a.g) * t,
		a.b + (b.b - a.b) * t,
	};
}

Rgb scale_rgb(const Rgb &color, float scale) {
	return {
		clamp_float(color.r * scale, 0.0f, 1.0f),
		clamp_float(color.g * scale, 0.0f, 1.0f),
		clamp_float(color.b * scale, 0.0f, 1.0f),
	};
}

} // namespace

Config make_default_config() {
	// Terrain_SetDefaultEnvironmentValues@0x53E030 resets the environment before
	// file parsing. These defaults are authoring-friendly values based on FULL_00,
	// with the same field coverage so new documents export as valid .env files.
	Config cfg;
	cfg.keyframes = {
		{0, {}, {14.0f / 255.0f, 29.0f / 255.0f, 45.0f / 255.0f}, {1.0f / 255.0f, 2.0f / 255.0f, 6.0f / 255.0f}, {35.0f / 255.0f, 36.0f / 255.0f, 59.0f / 255.0f}, {47.0f / 255.0f, 66.0f / 255.0f, 86.0f / 255.0f}, {1.0f / 255.0f, 2.0f / 255.0f, 6.0f / 255.0f}},
		{1200, {170.0f / 255.0f, 170.0f / 255.0f, 167.0f / 255.0f}, {49.0f / 255.0f, 55.0f / 255.0f, 46.0f / 255.0f}, {77.0f / 255.0f, 91.0f / 255.0f, 138.0f / 255.0f}, {84.0f / 255.0f, 88.0f / 255.0f, 89.0f / 255.0f}, {}, {77.0f / 255.0f, 91.0f / 255.0f, 138.0f / 255.0f}},
		{2359, {}, {14.0f / 255.0f, 29.0f / 255.0f, 44.0f / 255.0f}, {1.0f / 255.0f, 2.0f / 255.0f, 6.0f / 255.0f}, {35.0f / 255.0f, 36.0f / 255.0f, 59.0f / 255.0f}, {47.0f / 255.0f, 66.0f / 255.0f, 86.0f / 255.0f}, {1.0f / 255.0f, 2.0f / 255.0f, 6.0f / 255.0f}},
	};
	for (Keyframe &keyframe : cfg.keyframes) {
		keyframe.skybase = keyframe.sky;
		keyframe.skybright = keyframe.sky;
		keyframe.skyhighlight = keyframe.sun;
		keyframe.cloudbase = keyframe.sky;
		keyframe.cloudhighlight = keyframe.ground;
		keyframe.cloudedge = keyframe.ground;
	}
	return cfg;
}

bool load_env(std::istream &input, Config &out, std::string &error) {
	// Semantic port of the line callback sub_53E3F0@0x53E3F0, invoked by
	// sub_53FA70 after the generic line parser sub_501FB0 tokenizes .trn/.env.
	// We do not emulate the fixed globals; Config is the typed equivalent.
	error.clear();
	out = Config();

	std::string line;
	bool in_tod = false;
	bool skyfog_set = false;
	Keyframe current;
	int line_number = 0;

	while (std::getline(input, line)) {
		++line_number;
		const size_t comment = line.find(';');
		if (comment != std::string::npos) {
			line = line.substr(0, comment);
		}
		line = trim(line);
		if (line.empty()) {
			continue;
		}

		std::istringstream line_input(line);
		std::string key;
		line_input >> key;

		std::string value;
		std::getline(line_input, value);
		value = trim(value);

		if (key == "tod_begin") {
			if (in_tod) {
				error = "Nested tod_begin at line " + std::to_string(line_number);
				return false;
			}
			in_tod = true;
			skyfog_set = false;
			current = Keyframe();
			current.time = value.empty() ? 0 : clamp_int(std::atoi(value.c_str()), 0, 2359);
			continue;
		}
		if (key == "tod_end") {
			if (!in_tod) {
				error = "tod_end without tod_begin at line " + std::to_string(line_number);
				return false;
			}
			out.keyframes.push_back(current);
			in_tod = false;
			continue;
		}

		if (in_tod) {
			if (!value.empty()) {
				assign_tod_color(current, key, value, skyfog_set);
			}
			continue;
		}

		const std::string text = unquote(value);
		if (key == "enviro_name") {
			out.name = text;
		} else if (key == "timeofday") {
			out.timeofday = text;
		} else if (key == "envscale") {
			out.envscale = static_cast<float>(std::atof(value.c_str()));
		} else if (key == "curtime") {
			out.curtime = clamp_int(std::atoi(value.c_str()), 0, 2359);
		} else if (key == "fog_level") {
			out.fog_level = static_cast<float>(std::atof(value.c_str()));
		} else if (key == "fog_type") {
			out.fog_type = std::atoi(value.c_str());
		} else if (key == "terrain_rgb") {
			parse_rgb(value, out.terrain_rgb);
		} else if (key == "water_rgb") {
			parse_rgb(value, out.water_rgb);
		} else if (key == "water_height") {
			out.water_height = static_cast<float>(std::atof(value.c_str()));
			out.water_height_set = true;
		} else if (key == "cloud_rgb") {
			parse_rgb(value, out.cloud_rgb);
		} else if (key == "vertex_rgb") {
			parse_rgb(value, out.vertex_rgb);
		} else if (key == "lightning_rgb") {
			parse_rgb(value, out.lightning_rgb);
		} else if (key == "ceiling_rgb") {
			parse_rgb(value, out.ceiling_rgb);
		} else if (key == "floor_rgb") {
			parse_rgb(value, out.floor_rgb);
		} else if (key == "sky_speed") {
			out.sky_speed = static_cast<float>(std::atof(value.c_str()));
		} else if (key == "sky_height") {
			out.sky_height = static_cast<float>(std::atof(value.c_str()));
		} else if (key == "sky_map1") {
			out.sky_map1 = text;
		} else if (key == "sky_map2") {
			out.sky_map2 = text;
		} else if (key == "sun_3di") {
			out.sun_3di = text;
		} else if (key == "moon_3di") {
			out.moon_3di = text;
		} else if (key == "glare_3di") {
			out.glare_3di = text;
		} else if (key == "star_3di") {
			out.star_3di = text;
		} else if (key == "iris_percent") {
			out.iris_percent = static_cast<float>(std::atof(value.c_str()));
		} else if (key == "iris_center") {
			out.iris_center = static_cast<float>(std::atof(value.c_str()));
		} else if (key == "water_murk") {
			out.water_murk = clamp_float(static_cast<float>(std::atof(value.c_str())), 0.0f, 0.99f);
		} else if (key == "advanced_clouds") {
			out.advanced_clouds = std::atoi(value.c_str());
		}
	}

	if (in_tod) {
		error = "Unclosed tod_begin";
		return false;
	}

	std::sort(out.keyframes.begin(), out.keyframes.end(), [](const Keyframe &a, const Keyframe &b) {
		return a.time < b.time;
	});
	// Matches the post-parse TOD sort performed by sub_53FA70@0x53fc2e-0x53fca8.
	return true;
}

bool save_env(std::ostream &output, const Config &cfg, std::string &error) {
	// No stock writer has been identified in the runtime path; the editor emits a
	// normalized stock-style text file using the same keyword vocabulary parsed by
	// sub_53E3F0 and CRLF line endings used by captured assets.
	error.clear();

	output << "enviro_name \"" << cfg.name << "\"" << NL;
	output << NL;
	output << "timeofday \"" << cfg.timeofday << "\"" << NL;
	output << NL;
	output << "envscale " << number_to_string(cfg.envscale) << NL;
	output << NL;
	output << "iris_percent " << number_to_string(cfg.iris_percent) << NL;
	output << "iris_center " << number_to_string(cfg.iris_center) << NL;
	output << NL;
	output << "water_rgb " << rgb_to_string(cfg.water_rgb) << NL;
	if (cfg.water_height_set) {
		output << "water_height " << number_to_string(cfg.water_height) << NL;
	}
	output << NL;
	output << "sky_map1 " << cfg.sky_map1 << NL;
	output << "sky_map2 " << cfg.sky_map2 << NL;
	output << "sky_height " << number_to_string(cfg.sky_height) << NL;
	output << "sky_speed " << number_to_string(cfg.sky_speed) << NL;
	output << NL;
	output << "fog_level " << number_to_string(cfg.fog_level) << NL;
	output << "fog_type " << cfg.fog_type << NL;
	output << NL;
	output << "cloud_rgb " << rgb_to_string(cfg.cloud_rgb) << NL;
	output << "terrain_rgb " << rgb_to_string(cfg.terrain_rgb) << NL;
	output << "vertex_rgb " << rgb_to_string(cfg.vertex_rgb) << NL;
	output << NL;
	output << "lightning_rgb " << rgb_to_string(cfg.lightning_rgb) << NL;
	output << "sun_3di " << cfg.sun_3di << NL;
	output << "moon_3di " << cfg.moon_3di << NL;
	output << "glare_3di " << cfg.glare_3di << NL;
	output << "star_3di " << cfg.star_3di << NL;
	output << NL;
	output << "ceiling_rgb " << rgb_to_string(cfg.ceiling_rgb) << NL;
	output << "floor_rgb " << rgb_to_string(cfg.floor_rgb) << NL;
	output << NL;
	output << "curtime " << cfg.curtime << NL;
	output << NL;
	output << "water_murk " << number_to_string(cfg.water_murk) << NL;
	output << "advanced_clouds " << cfg.advanced_clouds << NL;

	std::vector<Keyframe> keyframes = cfg.keyframes;
	std::sort(keyframes.begin(), keyframes.end(), [](const Keyframe &a, const Keyframe &b) {
		return a.time < b.time;
	});

	for (const Keyframe &keyframe : keyframes) {
		output << NL;
		output << "tod_begin " << format_tod_time(keyframe.time) << NL;
		output << "    sun_rgb " << rgb_to_string(keyframe.sun) << NL;
		output << "    moon_rgb " << rgb_to_string(keyframe.moon) << NL;
		output << "    sky_rgb " << rgb_to_string(keyframe.sky) << NL;
		output << "    ground_rgb " << rgb_to_string(keyframe.ground) << NL;
		output << "    skyfog_rgb " << rgb_to_string(keyframe.skyfog) << NL;
		output << "    fog_rgb " << rgb_to_string(keyframe.fog) << NL;
		output << "    skybase_rgb " << rgb_to_string(keyframe.skybase) << NL;
		output << "    skybright_rgb " << rgb_to_string(keyframe.skybright) << NL;
		output << "    skyhighlight_rgb " << rgb_to_string(keyframe.skyhighlight) << NL;
		output << "    cloudbase_rgb " << rgb_to_string(keyframe.cloudbase) << NL;
		output << "    cloudhighlight_rgb " << rgb_to_string(keyframe.cloudhighlight) << NL;
		output << "    cloudedge_rgb " << rgb_to_string(keyframe.cloudedge) << NL;
		output << "tod_end" << NL;
	}

	if (!output.good()) {
		error = "Write error for ENV";
		return false;
	}
	return true;
}

TodState interpolate_tod(const std::vector<Keyframe> &keyframes, float time, float envscale) {
	// Per-frame color interpolation equivalent to sub_53FCC0@0x53FCC0.
	// The original reads dword_FF375C after sub_53FA70 sorts/snapshots keyframes;
	// this pure function keeps the same bracketing and midnight wrap behavior.
	TodState state;
	if (keyframes.empty()) {
		return state;
	}
	if (keyframes.size() == 1) {
		const Keyframe &keyframe = keyframes.front();
		state.sun = scale_rgb(keyframe.sun, envscale);
		state.ground = scale_rgb(keyframe.ground, envscale);
		state.fog = scale_rgb(keyframe.fog, envscale);
		state.sky = scale_rgb(keyframe.sky, envscale);
		state.moon = scale_rgb(keyframe.moon, envscale);
		state.skyfog = scale_rgb(keyframe.skyfog, envscale);
		state.skybase = scale_rgb(keyframe.skybase, envscale);
		state.skybright = scale_rgb(keyframe.skybright, envscale);
		state.skyhighlight = scale_rgb(keyframe.skyhighlight, envscale);
		state.cloudbase = scale_rgb(keyframe.cloudbase, envscale);
		state.cloudhighlight = scale_rgb(keyframe.cloudhighlight, envscale);
		state.cloudedge = scale_rgb(keyframe.cloudedge, envscale);
		return state;
	}

	std::vector<Keyframe> sorted = keyframes;
	std::sort(sorted.begin(), sorted.end(), [](const Keyframe &a, const Keyframe &b) {
		return a.time < b.time;
	});

	int t = static_cast<int>(std::floor(time));
	while (t < 0) {
		t += 2400;
	}
	t %= 2400;

	size_t lo = sorted.size() - 1;
	size_t hi = 0;
	for (size_t i = 0; i < sorted.size(); ++i) {
		if (sorted[i].time <= t) {
			lo = i;
		}
		if (sorted[i].time > t) {
			hi = i;
			break;
		}
	}

	const Keyframe &a = sorted[lo];
	const Keyframe &b = sorted[hi];
	float range = static_cast<float>(b.time - a.time);
	if (range <= 0.0f) {
		range = static_cast<float>(2400 - a.time + b.time);
	}
	float elapsed = static_cast<float>(t - a.time);
	if (elapsed < 0.0f) {
		elapsed += 2400.0f;
	}
	const float fraction = range > 0.0f ? clamp_float(elapsed / range, 0.0f, 1.0f) : 0.0f;

	state.sun = scale_rgb(lerp_rgb(a.sun, b.sun, fraction), envscale);
	state.ground = scale_rgb(lerp_rgb(a.ground, b.ground, fraction), envscale);
	state.fog = scale_rgb(lerp_rgb(a.fog, b.fog, fraction), envscale);
	state.sky = scale_rgb(lerp_rgb(a.sky, b.sky, fraction), envscale);
	state.moon = scale_rgb(lerp_rgb(a.moon, b.moon, fraction), envscale);
	state.skyfog = scale_rgb(lerp_rgb(a.skyfog, b.skyfog, fraction), envscale);
	state.skybase = scale_rgb(lerp_rgb(a.skybase, b.skybase, fraction), envscale);
	state.skybright = scale_rgb(lerp_rgb(a.skybright, b.skybright, fraction), envscale);
	state.skyhighlight = scale_rgb(lerp_rgb(a.skyhighlight, b.skyhighlight, fraction), envscale);
	state.cloudbase = scale_rgb(lerp_rgb(a.cloudbase, b.cloudbase, fraction), envscale);
	state.cloudhighlight = scale_rgb(lerp_rgb(a.cloudhighlight, b.cloudhighlight, fraction), envscale);
	state.cloudedge = scale_rgb(lerp_rgb(a.cloudedge, b.cloudedge, fraction), envscale);
	return state;
}

Vec3 compute_sun_direction(float tod_time) {
	// Terrain_CalcSunDirection@0x53F5D0 analogue. The constants are the
	// normalized fixed-point direction components documented for the engine path.
	const float angle = 2.0f * 3.14159265358979323846f * tod_time / 2400.0f;
	Vec3 dir;
	dir.x = -22414.0f / 65536.0f;
	dir.y = -(61583.0f / 65536.0f) * std::cos(angle);
	dir.z = (61583.0f / 65536.0f) * std::sin(angle);
	const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
	if (len > 0.0f) {
		dir.x /= len;
		dir.y /= len;
		dir.z /= len;
	}
	return dir;
}

Vec3 compute_moon_direction(float tod_time) {
	// Moon direction mirrors the sun phase by 180 degrees, matching the runtime
	// sky/TOD consumer described alongside Terrain_CalcSunDirection@0x53F5D0.
	const float angle = 2.0f * 3.14159265358979323846f * tod_time / 2400.0f + 3.14159265358979323846f;
	Vec3 dir;
	dir.x = -32768.0f / 65536.0f;
	dir.y = -(56755.0f / 65536.0f) * std::cos(angle);
	dir.z = (56755.0f / 65536.0f) * std::sin(angle);
	const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
	if (len > 0.0f) {
		dir.x /= len;
		dir.y /= len;
		dir.z /= len;
	}
	return dir;
}

} // namespace opennova::env
