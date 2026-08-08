#include "env/env.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include <io/strutil.h>

namespace opennova::env {

namespace {

constexpr const char *NL = "\r\n";

using opennova::strutil::trim;

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
	// [orig: TimeOfDay_ParseProperty @ 0x57c590] writes RGB values into the
	// currently active TOD keyframe. fog_rgb also mirrors into skyfog_rgb while
	// skyfog still holds its 0xC0C0FF sentinel (@ 0x57c9b8); we model that with
	// a per-keyframe flag — tracked divergence #9 in docs/env/env-tod-re.md.
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

// Digit-positional HHMM sanitizer: hours clamp to 23, minutes to 59
// [orig: Environment_ParseTimeString @ 0x57c500].
int sanitize_hhmm(int value) {
	if (value < 0) {
		return 0;
	}
	const int hours = std::min(value / 100, 23);
	const int minutes = std::min(value % 100, 59);
	return hours * 100 + minutes;
}

// Quantize one channel the way the parser stores it: file byte scaled by
// envscale, truncated, clamped to 255 [orig: Color_ScaleRGBAndPack @ 0x57f890].
// (The original has no lower clamp and packs garbage for negative inputs; we
// clamp at 0 — tracked divergence #11 in docs/env/env-tod-re.md.)
int quantize_channel(float normalized, float envscale) {
	const int byte_value = clamp_int(static_cast<int>(normalized * 255.0f + 0.5f), 0, 255);
	const int scaled = static_cast<int>(static_cast<float>(byte_value) * envscale);
	return clamp_int(scaled, 0, 255);
}

// Integer channel lerp with round-half-up [orig: Color_InterpolateRGB888 @ 0x57c2f0].
int lerp_channel(int a, int b, int fraction_fp) {
	return static_cast<int>((static_cast<int64_t>(fraction_fp) * (b - a) + (static_cast<int64_t>(a) << 16) + 0x8000) >> 16);
}

Rgb lerp_rgb_quantized(const Rgb &a, const Rgb &b, int fraction_fp, float envscale) {
	Rgb out;
	out.r = static_cast<float>(lerp_channel(quantize_channel(a.r, envscale), quantize_channel(b.r, envscale), fraction_fp)) / 255.0f;
	out.g = static_cast<float>(lerp_channel(quantize_channel(a.g, envscale), quantize_channel(b.g, envscale), fraction_fp)) / 255.0f;
	out.b = static_cast<float>(lerp_channel(quantize_channel(a.b, envscale), quantize_channel(b.b, envscale), fraction_fp)) / 255.0f;
	return out;
}

} // namespace

int hhmm_to_hours_fp(float hhmm) {
	if (hhmm < 0.0f) {
		return 0;
	}
	const int whole = static_cast<int>(hhmm);
	int hours = whole / 100;
	float minutes = hhmm - static_cast<float>(hours * 100);
	if (hours > 23) {
		hours = 23;
	}
	if (minutes > 59.0f) {
		minutes = 59.0f;
	}
	// Integer-exact for whole minutes: (m << 16) / 60 truncates the same way.
	return (hours << 16) + static_cast<int>(minutes * (65536.0f / 60.0f));
}

Config make_default_config() {
	// Authoring template for "New Environment": Config{} carries the engine's
	// pre-parse defaults [orig: Environment_InitDefaults @ 0x57c010]; this
	// overrides them with friendly FULL_00-style values, written explicitly to
	// the file on save so the engine reads the authored intent.
	Config cfg;
	cfg.curtime = 1200;
	cfg.fog_level = 1000.0f;
	cfg.fog_type = 2;
	cfg.water_rgb = {56.0f / 255.0f, 59.0f / 255.0f, 39.0f / 255.0f};
	cfg.lightning_rgb = {85.0f / 255.0f, 85.0f / 255.0f, 90.0f / 255.0f};
	cfg.ceiling_rgb = {55.0f / 255.0f, 55.0f / 255.0f, 55.0f / 255.0f};
	cfg.floor_rgb = {25.0f / 255.0f, 25.0f / 255.0f, 25.0f / 255.0f};
	cfg.sky_speed = 15.0f;
	cfg.sky_height = 175.0f;
	cfg.sky_map1 = "Cloud01.pcx";
	cfg.sky_map2 = "Cloud01b.pcx";
	cfg.star_3di.clear();
	cfg.iris_percent = 15.0f;
	cfg.iris_center = 1.0f;
	cfg.advanced_clouds = 1;
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
	// Semantic port of the line callback [orig: TimeOfDay_ParseProperty @ 0x57c590],
	// invoked per tokenized line of .trn/.env. We do not emulate the fixed
	// globals; Config is the typed equivalent. envscale stays a stored field and
	// is applied at interpolation/engine-view time rather than baked into colors
	// at parse — tracked divergence #8 in docs/env/env-tod-re.md (equivalent for
	// files where envscale precedes all colors; the corpus sweep validates this).
	//
	// Load pipeline context (C6 grill, [orig: Environment_LoadTimeOfDayConfig
	// @ 0x57db30]): the retail loader runs this parser over the .trn/.env FIRST,
	// and overcast.def is NEVER a fallback — on .trn success it ALWAYS parses
	// additively into the overcast keyframe table (the keyframe count is not
	// reset between passes); a missing/failed .trn aborts the whole TOD load with
	// no .env pass, and a NULL map name goes straight to overcast.def. At runtime
	// the overcast blend cross-fades the .env-table colors against the overcast
	// table. This reimpl carries the .env table only — overcast blend 0 (clear
	// weather, pure .env) until weather scripting drives it; a future overcast
	// cross-fade must parse overcast.def additively per the order above.
	error.clear();
	out = Config();

	std::string line;
	bool in_tod = false;
	bool overflow_block = false;
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
			// The engine has no block-nesting state: tod_begin simply advances
			// to the next slot, so tod_end is optional and a new tod_begin
			// implicitly closes the open block (shipped FULL_03/FULL_05.ENV
			// rely on this) [orig: TimeOfDay_ParseProperty @ 0x57c647].
			if (in_tod) {
				if (overflow_block) {
					out.keyframes.back() = current;
				} else {
					out.keyframes.push_back(current);
				}
			}
			in_tod = true;
			skyfog_set = false;
			if (static_cast<int>(out.keyframes.size()) >= kMaxTodKeyframes) {
				// The engine's 17th tod_begin neither allocates a slot nor stores
				// its time; its color lines keep writing into slot 16
				// [orig: TimeOfDay_ParseProperty @ 0x57c65b].
				overflow_block = true;
				current = out.keyframes.back();
			} else {
				overflow_block = false;
				current = Keyframe();
				current.time = value.empty() ? 0 : sanitize_hhmm(std::atoi(value.c_str()));
			}
			continue;
		}
		if (key == "tod_end") {
			// A stray tod_end just resets the engine's slot pointer to scratch;
			// it is not an error [orig: TimeOfDay_ParseProperty @ 0x57c696].
			if (in_tod) {
				if (overflow_block) {
					out.keyframes.back() = current;
				} else {
					out.keyframes.push_back(current);
				}
				in_tod = false;
			}
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
			// [orig: TimeOfDay_ParseProperty @ 0x57d0b6] via Environment_ParseTimeString.
			out.curtime = sanitize_hhmm(std::atoi(value.c_str()));
		} else if (key == "fog_level") {
			// Integer parse (atol truncation), stored <<16 by the engine
			// [orig: TimeOfDay_ParseProperty @ 0x57cca3].
			out.fog_level = static_cast<float>(std::atol(value.c_str()));
		} else if (key == "fog_type") {
			out.fog_type = std::atoi(value.c_str());
		} else if (key == "terrain_rgb") {
			parse_rgb(value, out.terrain_rgb);
		} else if (key == "water_rgb") {
			parse_rgb(value, out.water_rgb);
		} else if (key == "water_height") {
			// Integer parse; engine stores <<15 (half world units in 16.16)
			// [orig: TimeOfDay_ParseProperty @ 0x57cb4e].
			out.water_height = static_cast<float>(std::atol(value.c_str()));
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
			// Integer parse; engine stores <<10 [orig: TimeOfDay_ParseProperty @ 0x57cbf1].
			out.sky_speed = static_cast<float>(std::atol(value.c_str()));
		} else if (key == "sky_height") {
			// Integer parse; engine stores <<16 [orig: TimeOfDay_ParseProperty @ 0x57cbc3].
			out.sky_height = static_cast<float>(std::atol(value.c_str()));
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
			// Engine clamps only the top (no lower bound)
			// [orig: TimeOfDay_ParseProperty @ 0x57cba9].
			out.water_murk = std::min(static_cast<float>(std::atof(value.c_str())), 0.99f);
		} else if (key == "advanced_clouds") {
			out.advanced_clouds = std::atoi(value.c_str());
		}
	}

	if (in_tod) {
		// An unterminated final block is still a counted slot in the engine.
		if (overflow_block) {
			out.keyframes.back() = current;
		} else {
			out.keyframes.push_back(current);
		}
	}

	// Stable, matching the engine's bubble sort (duplicate times keep file order)
	// [orig: Environment_SortAndSnapshotKeyframes @ 0x57c240].
	std::stable_sort(out.keyframes.begin(), out.keyframes.end(), [](const Keyframe &a, const Keyframe &b) {
		return a.time < b.time;
	});
	return true;
}

bool save_env(std::ostream &output, const Config &cfg, std::string &error) {
	// No stock writer exists in the engine; the editor emits a normalized
	// stock-style text file using the keyword vocabulary parsed by
	// [orig: TimeOfDay_ParseProperty @ 0x57c590] and CRLF line endings used by
	// shipped assets. (enviro_name is an authoring extension; the engine ignores
	// unknown keywords.)
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
	std::stable_sort(keyframes.begin(), keyframes.end(), [](const Keyframe &a, const Keyframe &b) {
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
	// Engine-faithful per-tick interpolation. Parameter space is 16.16 HOURS
	// (a day = 0x180000), bracketing and fraction are integer math
	// [orig: Environment_FindKeyframeSegment @ 0x57dd80], the channel lerp is the
	// byte lerp with +0x8000 rounding [orig: Color_InterpolateRGB888 @ 0x57c2f0],
	// and fractions above 63356 snap to 1.0 — an original source typo (65536
	// transposed) preserved for parity [orig: Environment_LerpKeyframeSet @ 0x57c3c6].
	TodState state;
	if (keyframes.empty()) {
		return state;
	}

	std::vector<Keyframe> sorted = keyframes;
	std::stable_sort(sorted.begin(), sorted.end(), [](const Keyframe &a, const Keyframe &b) {
		return a.time < b.time;
	});

	constexpr int kDay = 0x180000; // 24.0 hours in 16.16
	int t = hhmm_to_hours_fp(time);
	t %= kDay;

	std::vector<int> times(sorted.size());
	for (size_t i = 0; i < sorted.size(); ++i) {
		times[i] = hhmm_to_hours_fp(static_cast<float>(sorted[i].time));
	}

	size_t lo = sorted.size() - 1;
	size_t hi = 0;
	if (t >= times.front()) {
		for (size_t i = 0; i < sorted.size(); ++i) {
			if (times[i] <= t) {
				lo = i;
				hi = (i + 1) % sorted.size();
			}
		}
	}

	int duration = times[hi] - times[lo];
	if (duration < 0) {
		duration += kDay;
	}
	int fraction = 0;
	if (duration != 0) {
		int elapsed = t - times[lo];
		if (elapsed < 0) {
			elapsed += kDay;
		}
		fraction = static_cast<int>((static_cast<int64_t>(elapsed) << 16) / duration);
	}
	if (fraction < 0) {
		fraction = 0;
	} else if (fraction > 63356) {
		fraction = 0x10000;
	}

	const Keyframe &a = sorted[lo];
	const Keyframe &b = sorted[hi];
	state.sun = lerp_rgb_quantized(a.sun, b.sun, fraction, envscale);
	state.ground = lerp_rgb_quantized(a.ground, b.ground, fraction, envscale);
	state.fog = lerp_rgb_quantized(a.fog, b.fog, fraction, envscale);
	state.sky = lerp_rgb_quantized(a.sky, b.sky, fraction, envscale);
	state.moon = lerp_rgb_quantized(a.moon, b.moon, fraction, envscale);
	state.skyfog = lerp_rgb_quantized(a.skyfog, b.skyfog, fraction, envscale);
	state.skybase = lerp_rgb_quantized(a.skybase, b.skybase, fraction, envscale);
	state.skybright = lerp_rgb_quantized(a.skybright, b.skybright, fraction, envscale);
	state.skyhighlight = lerp_rgb_quantized(a.skyhighlight, b.skyhighlight, fraction, envscale);
	state.cloudbase = lerp_rgb_quantized(a.cloudbase, b.cloudbase, fraction, envscale);
	state.cloudhighlight = lerp_rgb_quantized(a.cloudhighlight, b.cloudhighlight, fraction, envscale);
	state.cloudedge = lerp_rgb_quantized(a.cloudedge, b.cloudedge, fraction, envscale);
	return state;
}

Vec3 compute_sun_direction(float tod_time) {
	// [orig: Environment_ComputeSunDirection @ 0x57d6d0] — angle = 2*pi * t/24h
	// in HOURS space (the original reads curtime's high word, 1/256h steps and
	// computes in 16.16 fixed; we keep floats — sub-1e-4 quantization divergence,
	// documented in docs/env/env-tod-re.md). Fixed vector (0.9397 sin, 0.342,
	// -0.9397 cos) swizzled by the float getter to (-0.342, -0.9397 cos, +0.9397 sin).
	const float hours = static_cast<float>(hhmm_to_hours_fp(tod_time)) / 65536.0f;
	const float angle = 2.0f * 3.14159265358979323846f * hours / 24.0f;
	Vec3 dir;
	dir.x = -22414.0f / 65536.0f;
	dir.y = -(61583.0f / 65536.0f) * std::cos(angle);
	dir.z = (61583.0f / 65536.0f) * std::sin(angle);
	// The getter exposes the fixed tuple directly; terrain byte-packs it without
	// another normalization [orig: Environment_GetLightDirectionFloat @ 0x57d870,
	// tuple copy @ 0x57d8be..0x57d8cd; PolyTrn_RenderTile @ 0x60e1d9..0x60e331].
	return dir;
}

Vec3 compute_moon_direction(float tod_time) {
	// [orig: Terrain_ComputeMoonDirection @ 0x57d760] — sun phase + pi, constants
	// 56755/65536 = 0.866 and Y = 0x8000 = 0.5, hours space as for the sun.
	const float hours = static_cast<float>(hhmm_to_hours_fp(tod_time)) / 65536.0f;
	const float angle = 2.0f * 3.14159265358979323846f * hours / 24.0f + 3.14159265358979323846f;
	Vec3 dir;
	dir.x = -32768.0f / 65536.0f;
	dir.y = -(56755.0f / 65536.0f) * std::cos(angle);
	dir.z = (56755.0f / 65536.0f) * std::sin(angle);
	// The active getter copies this selected moon tuple through the same
	// direct path [orig: Environment_GetLightDirectionFloat @ 0x57d870].
	return dir;
}

} // namespace opennova::env
