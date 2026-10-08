#include <formats/env/env.h>
#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/fixed.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <sstream>

#include <base/io/strutil.h>

namespace opennova::env {

namespace {

constexpr const char *NL = "\r\n";


int clamp_int(int value, int min_value, int max_value) {
	return std::max(min_value, std::min(max_value, value));
}

float clamp_float(float value, float min_value, float max_value) {
	return std::max(min_value, std::min(max_value, value));
}

// A color line's three channels: atol of tokens 1..3, whatever the line's
// count, the blue a stale slot on a short line (io::ConfigTokens::slot)
// [orig: TimeOfDay_ParseProperty @ 0x57c590, j__atol of tokens[2], [3] and [4]
// in every *_rgb arm].
void token_rgb(const io::ConfigTokens &tokens, int rgb[3]) {
	for (int i = 0; i < 3; ++i) rgb[i] = io::retail_atol(tokens.token(1 + i));
}

void parse_rgb(const io::ConfigTokens &tokens, Rgb &out, float scale = 1.0f) {
	int rgb[3];
	token_rgb(tokens, rgb);
	out.r = clamp_float(static_cast<float>(rgb[0]) * scale / 255.0f, 0.0f, 1.0f);
	out.g = clamp_float(static_cast<float>(rgb[1]) * scale / 255.0f, 0.0f, 1.0f);
	out.b = clamp_float(static_cast<float>(rgb[2]) * scale / 255.0f, 0.0f, 1.0f);
}

std::string rgb_to_string(const Rgb &color) {
	const int r = clamp_int(static_cast<int>(color.r * 255.0f + 0.5f), 0, 255);
	const int g = clamp_int(static_cast<int>(color.g * 255.0f + 0.5f), 0, 255);
	const int b = clamp_int(static_cast<int>(color.b * 255.0f + 0.5f), 0, 255);
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%d,%d,%d", r, g, b);
	return buffer;
}

// An atof keyword's value (envscale, iris_percent, iris_center, water_murk): the fewest
// significant digits, from six, that the parser's atof reads back as the same float
// [orig: TimeOfDay_ParseProperty @ 0x57c5d8, its atof of tokens[2]].
std::string number_to_string(float value) {
	std::string text;
	for (int digits = 6; digits <= 9; ++digits) {
		std::ostringstream output;
		output << std::setprecision(digits) << std::defaultfloat << value;
		text = output.str();
		if (static_cast<float>(io::retail_atof(text.c_str())) == value) break;
	}
	return text;
}

// An atol keyword's value (fog_level, sky_height, sky_speed, water_height): the whole number
// the parser's atol reads from the value, saturating at the 32-bit long [orig:
// TimeOfDay_ParseProperty @ 0x57ccbf, j__atol of tokens[2]].
std::string whole_to_string(float value) {
	double whole = std::trunc(static_cast<double>(value));
	if (!(whole >= -2147483648.0)) whole = -2147483648.0;
	if (whole > 2147483647.0) whole = 2147483647.0;
	return std::to_string(static_cast<long long>(whole));
}

std::string format_tod_time(int time) {
	const int normalized = clamp_tod_time(time);
	std::ostringstream output;
	output << std::setw(4) << std::setfill('0') << normalized;
	return output.str();
}

// A name as a value token: quoted where the tokenizer would cut it (a space, a comma or a tab
// ends a token, a ';' or "//" outside quotes cuts the line); '"' toggles quoting and ends a
// token, so a name holding one has no line form [orig: File_ParseASCIIFile @ 0x53d810, the
// tokenizer's delimiters @0x53CC33..0x53CC4C and quote @0x53CC4E..0x53CC70].
std::string name_token(const std::string &name) {
	const bool quoted = name.find_first_of(" ,\t;") != std::string::npos || name.find("//") != std::string::npos;
	return quoted ? "\"" + name + "\"" : name;
}

// The parser's color pack: each byte times the envscale read so far, truncated, clamped
// to 255 [orig: Color_ScaleRGBAndPack @ 0x57f890] (and at 0: the original packs garbage
// for a negative byte, divergence #11).
void parse_rgb_baked(const io::ConfigTokens &tokens, Rgb &out, float envscale) {
	int rgb[3];
	token_rgb(tokens, rgb);
	float *channels[3] = {&out.r, &out.g, &out.b};
	for (int i = 0; i < 3; ++i) {
		const int baked = clamp_int(static_cast<int>(static_cast<float>(rgb[i]) * envscale), 0, 255);
		*channels[i] = static_cast<float>(baked) / 255.0f;
	}
}

// `baked`: the keyframe is the scratch slot, whose colors are kept as the parser packs them
// (`envscale` applied here); otherwise a timed keyframe, kept as authored (envscale applied
// at interpolation, divergence #8).
void assign_tod_color(Keyframe &keyframe, const std::string &key, const io::ConfigTokens &value,
                      bool &skyfog_set, bool baked = false, float envscale = 1.0f) {
	// [orig: TimeOfDay_ParseProperty @ 0x57c590] writes RGB values into the
	// currently active TOD keyframe. fog_rgb also mirrors into skyfog_rgb while
	// skyfog still holds its 0xC0C0FF sentinel (@ 0x57c9b8); we model that with
	// a per-keyframe flag — tracked divergence #9 in docs/env/env-tod-re.md.
	const auto parse = [baked, envscale](const io::ConfigTokens &tokens, Rgb &out) {
		if (baked) parse_rgb_baked(tokens, out, envscale);
		else parse_rgb(tokens, out);
	};
	if (key == "sun_rgb") {
		parse(value, keyframe.sun);
	} else if (key == "ground_rgb" || key == "ambient_rgb") {
		parse(value, keyframe.ground);
	} else if (key == "fog_rgb") {
		parse(value, keyframe.fog);
		if (!skyfog_set) {
			keyframe.skyfog = keyframe.fog;
		}
	} else if (key == "sky_rgb") {
		parse(value, keyframe.sky);
	} else if (key == "moon_rgb") {
		parse(value, keyframe.moon);
	} else if (key == "skyfog_rgb") {
		parse(value, keyframe.skyfog);
		skyfog_set = true;
	} else if (key == "skybase_rgb") {
		parse(value, keyframe.skybase);
	} else if (key == "skybright_rgb") {
		parse(value, keyframe.skybright);
	} else if (key == "skyhighlight_rgb") {
		parse(value, keyframe.skyhighlight);
	} else if (key == "cloudbase_rgb") {
		parse(value, keyframe.cloudbase);
	} else if (key == "cloudhighlight_rgb") {
		parse(value, keyframe.cloudhighlight);
	} else if (key == "cloudedge_rgb") {
		parse(value, keyframe.cloudedge);
	}
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

// A time token as HHMM, read by position: the last two characters are the
// minutes, the one or two before them the hours, each digit's byte less '0';
// a token shorter than three characters reads 0; hours clamp to 23 and
// minutes to 59 from above [orig: Environment_ParseTimeString @ 0x57c500].
int parse_tod_time(const char *text) {
	const size_t len = std::strlen(text);
	if (len < 3) return 0;
	const auto byte = [text](size_t i) { return static_cast<int>(static_cast<signed char>(text[i])); };
	int minutes = byte(len - 1) + 10 * byte(len - 2) - 528;
	int hours = byte(len - 3) - '0';
	if (len >= 4) hours += 10 * (byte(len - 4) - '0');
	if (minutes > 59) minutes = 59;
	if (hours > 23) hours = 23;
	return hours * 100 + minutes;
}

bool is_tod_color_key(const std::string &key) {
	return key == "sun_rgb" || key == "ground_rgb" || key == "ambient_rgb" || key == "fog_rgb" ||
			key == "sky_rgb" || key == "moon_rgb" || key == "skyfog_rgb" || key == "skybase_rgb" ||
			key == "skybright_rgb" || key == "skyhighlight_rgb" || key == "cloudbase_rgb" ||
			key == "cloudhighlight_rgb" || key == "cloudedge_rgb";
}

// The keys the parser reads [orig: TimeOfDay_ParseProperty @ 0x57c590, each stricmp of
// tokens[1]], with enviro_name and vertex_rgb, which this reader keeps for the round trip
// (retail has no arm for either: it skips both lines).
bool is_env_key(const std::string &key) {
	static const char *const kKeys[] = {
		"envscale", "iris_percent", "iris_center", "tod_begin", "tod_end", "lightning_rgb", "cloud_rgb",
		"ceiling_rgb", "floor_rgb", "terrain_rgb", "water_rgb", "water_height", "water_murk", "sky_height",
		"sky_speed", "sky_map1", "sky_map2", "fog_level", "fog_type", "sun_3di", "moon_3di", "glare_3di",
		"star_3di", "timeofday", "advanced_clouds", "curtime", "tod_rate", "enviro_name", "vertex_rgb",
	};
	if (is_tod_color_key(key)) return true;
	for (const char *known : kKeys)
		if (key == known) return true;
	return false;
}

bool is_envscaled_key(const std::string &key) {
	return (is_tod_color_key(key) || key == "lightning_rgb" || key == "cloud_rgb" || key == "ceiling_rgb" ||
	        key == "floor_rgb" || key == "water_rgb");
}

bool env_name_writable(const std::string &name) {
	for (const unsigned char c : name)
		if (c < 0x20 || c == 0x7F || c == '"') return false;
	return true;
}

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
	return (hours << 16) + static_cast<int>(minutes * (io::kFp16One / 60.0f));
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

float clamp_water_murk_upper(float value) {
	return std::min(value, 0.99f);
}

namespace {

// The parser's state across the passes of one load. The slot pointer: a keyframe of the pass's table by
// index, or -1 for the scratch keyframe, where it sits as the load begins, as the .env's pass begins and
// after every tod_end [orig: g_EnvTodCurrentSlotPtr, set to the scratch @ 0x57db88, @ 0x57dc56 and by
// tod_end @ 0x57c6a3]; between the .trn's pass and overcast.def's nothing moves it. Each keyframe's fog
// mirrors into its skyfog until a skyfog line sets it (divergence #9: a flag per keyframe for the 0xC0C0FF
// sentinel test @ 0x57c9b8); the scratch's sentinel is seeded once a load [orig: @ 0x57db54..0x57db59].
struct TodParse {
	int slot = -1;
	std::vector<bool> skyfog_set;
	bool scratch_skyfog_set = false; // the scratch's skyfog no longer holds its 0xC0C0FF seed
};

// Stable, matching the engine's bubble sort (duplicate times keep file order)
// [orig: Environment_SortAndSnapshotKeyframes @ 0x57c240].
void sort_keyframes(std::vector<Keyframe> &keyframes) {
	std::stable_sort(keyframes.begin(), keyframes.end(), [](const Keyframe &a, const Keyframe &b) {
		return a.time < b.time;
	});
}

// One file's pass of the keyword parser over `out`: the globals it writes stand over what an earlier pass
// wrote, and its keyframes append to out.keyframes, the pass's slot table [orig: TimeOfDay_ParseProperty
// @ 0x57c590].
void parse_tod_pass(const std::string &text, Config &out, TodParse &parse) {
	// Semantic port of the line callback [orig: TimeOfDay_ParseProperty @ 0x57c590],
	// invoked per tokenized line of .trn/overcast.def/.env. We do not emulate the fixed
	// globals; Config is the typed equivalent. envscale stays a stored field and
	// is applied at interpolation/engine-view time rather than baked into colors
	// at parse — tracked divergence #8 in docs/env/env-tod-re.md (equivalent for
	// files where envscale precedes all colors; the corpus sweep validates this).
	//
	// The lines and tokens are the shared retail walk's (io::for_each_config_line:
	// a CR LF pair ends a line and nothing else does, an unterminated last line
	// loses its final byte, `;` or `//` outside quotes cuts a line, and space,
	// comma or tab separates tokens) [orig: Environment_LoadTimeOfDayConfig
	// @ 0x57db30 -> File_ParseASCIIFile @ 0x53d810, the walks @ 0x57dbeb /
	// @ 0x57dc3b / @ 0x57dcbf]. A key compares without case and reads its values
	// by token, numbers through the CRT's atol and atof (io::retail_atol,
	// io::retail_atof), whatever the line's count [orig: TimeOfDay_ParseProperty
	// @ 0x57c590, the stricmp of tokens[1] in every arm]. A color line is a
	// color line inside a block or out of one, and every other key reads the
	// same either way: the parser keeps no block state but the slot pointer.
	int &slot = parse.slot;
	std::vector<bool> &skyfog_set = parse.skyfog_set;
	bool &scratch_skyfog_set = parse.scratch_skyfog_set;

	io::for_each_config_line(text.data(), text.size(), [&](const io::ConfigTokens &tokens) {
		const std::string key = strutil::to_lower(tokens.tokens[0]);
		const char *value = tokens.token(1);

		if (key == "tod_begin") {
			// The engine has no block-nesting state: tod_begin takes the next slot and points
			// there, so tod_end is optional and a new tod_begin simply moves on (shipped
			// FULL_03/FULL_05.ENV rely on this). With all 16 slots taken it neither takes a
			// slot, stores its time nor moves the pointer: its color lines land where the
			// pointer is, the 16th slot while that block is open, the scratch after a tod_end
			// [orig: TimeOfDay_ParseProperty @ 0x57c647, the count test @ 0x57c65b].
			if (static_cast<int>(out.keyframes.size()) < kMaxTodKeyframes) {
				Keyframe keyframe;
				keyframe.time = parse_tod_time(value);
				out.keyframes.push_back(keyframe);
				skyfog_set.push_back(false);
				slot = static_cast<int>(out.keyframes.size()) - 1;
			}
			return;
		}
		if (key == "tod_end") {
			// The pointer back on the scratch keyframe; a stray tod_end is no error
			// [orig: TimeOfDay_ParseProperty @ 0x57c696].
			slot = -1;
			return;
		}

		if (is_tod_color_key(key)) {
			if (slot >= 0) {
				bool mirrored = skyfog_set[static_cast<size_t>(slot)];
				assign_tod_color(out.keyframes[static_cast<size_t>(slot)], key, tokens, mirrored);
				skyfog_set[static_cast<size_t>(slot)] = mirrored;
			} else {
				// On the scratch keyframe the color lands packed with the envscale read so
				// far [orig: TimeOfDay_ParseProperty @ 0x57c590 through g_EnvTodCurrentSlotPtr;
				// Color_ScaleRGBAndPack @ 0x57f890].
				assign_tod_color(out.scratch, key, tokens, scratch_skyfog_set, true, out.envscale);
			}
			return;
		}

		if (key == "enviro_name") {
			out.name = value;
		} else if (key == "timeofday") {
			out.timeofday = value;
		} else if (key == "envscale") {
			out.envscale = static_cast<float>(io::retail_atof(value));
		} else if (key == "curtime") {
			// [orig: TimeOfDay_ParseProperty @ 0x57d0b6] via Environment_ParseTimeString.
			out.curtime = parse_tod_time(value);
		} else if (key == "fog_level") {
			// Integer parse (atol truncation), stored <<16 by the engine
			// [orig: TimeOfDay_ParseProperty @ 0x57cca3].
			out.fog_level = static_cast<float>(io::retail_atol(value));
		} else if (key == "fog_type") {
			out.fog_type = io::retail_atol(value);
		} else if (key == "terrain_rgb") {
			parse_rgb(tokens, out.terrain_rgb);
		} else if (key == "water_rgb") {
			parse_rgb(tokens, out.water_rgb);
		} else if (key == "water_height") {
			// Integer parse; engine stores <<15 (half world units in 16.16)
			// [orig: TimeOfDay_ParseProperty @ 0x57cb4e].
			out.water_height = static_cast<float>(io::retail_atol(value));
			out.water_height_set = true;
		} else if (key == "cloud_rgb") {
			parse_rgb(tokens, out.cloud_rgb);
		} else if (key == "vertex_rgb") {
			parse_rgb(tokens, out.vertex_rgb);
		} else if (key == "lightning_rgb") {
			parse_rgb(tokens, out.lightning_rgb);
		} else if (key == "ceiling_rgb") {
			parse_rgb(tokens, out.ceiling_rgb);
		} else if (key == "floor_rgb") {
			parse_rgb(tokens, out.floor_rgb);
		} else if (key == "sky_speed") {
			// Integer parse; engine stores <<10 [orig: TimeOfDay_ParseProperty @ 0x57cbf1].
			out.sky_speed = static_cast<float>(io::retail_atol(value));
		} else if (key == "sky_height") {
			// Integer parse; engine stores <<16 [orig: TimeOfDay_ParseProperty @ 0x57cbc3].
			out.sky_height = static_cast<float>(io::retail_atol(value));
		} else if (key == "sky_map1") {
			out.sky_map1 = value;
		} else if (key == "sky_map2") {
			out.sky_map2 = value;
		} else if (key == "sun_3di") {
			out.sun_3di = value;
		} else if (key == "moon_3di") {
			out.moon_3di = value;
		} else if (key == "glare_3di") {
			out.glare_3di = value;
		} else if (key == "star_3di") {
			out.star_3di = value;
		} else if (key == "iris_percent") {
			out.iris_percent = static_cast<float>(io::retail_atof(value));
		} else if (key == "iris_center") {
			out.iris_center = static_cast<float>(io::retail_atof(value));
		} else if (key == "water_murk") {
			out.water_murk = clamp_water_murk_upper(static_cast<float>(io::retail_atof(value)));
		} else if (key == "advanced_clouds") {
			out.advanced_clouds = io::retail_atol(value);
		} else if (key == "tod_rate") {
			// The day's length in minutes, kept as written: the engine makes it the clock's
			// advance a tick on the spot [orig: TimeOfDay_ParseProperty @ 0x57d0f9..0x57d118].
			out.tod_rate = io::retail_atol(value);
			out.tod_rate_set = true;
		}
	});
}

} // namespace

bool load_env(std::istream &input, Config &out, std::string &error) {
	// One file alone over the pre-parse defaults (a document's read, the authoring tools'): a
	// mission's load runs the same parser over its three files (load_mission_env).
	error.clear();
	out = Config();
	const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	TodParse parse;
	parse_tod_pass(text, out, parse);
	sort_keyframes(out.keyframes);
	return true;
}

bool save_env(std::ostream &output, const Config &cfg, std::string &error) {
	// No stock writer exists in the engine; this emits a normalized stock-style text file from
	// scratch in the keyword vocabulary parsed by [orig: TimeOfDay_ParseProperty @ 0x57c590]
	// and the CRLF line endings of shipped assets, each value in the form that parser reads back
	// as the Config holds it (env.h). (enviro_name is an authoring extension; the engine skips
	// a keyword it has no arm for.)
	error.clear();
	for (const std::string *name : {&cfg.name, &cfg.timeofday, &cfg.sky_map1, &cfg.sky_map2, &cfg.sun_3di,
	                                &cfg.moon_3di, &cfg.glare_3di, &cfg.star_3di})
		if (!env_name_writable(*name)) {
			error = "The name '" + *name + "' holds a '\"' or a control character, which no line can carry.";
			return false;
		}
	if (static_cast<int>(cfg.keyframes.size()) > kMaxTodKeyframes) {
		error = "The game reads 16 keyframes at most.";
		return false;
	}

	output << "enviro_name \"" << cfg.name << "\"" << NL;
	output << NL;
	output << "timeofday \"" << cfg.timeofday << "\"" << NL;
	output << NL;
	// The scratch keyframe's colors that are not its seed, as color lines outside every
	// block. They precede the envscale line: they are kept packed (envscale already
	// applied), so they must read back at envscale 1.
	{
		const Keyframe seed = scratch_keyframe_defaults();
		const Keyframe &scratch = cfg.scratch;
		bool any = false;
		const auto line = [&](const char *key, const Rgb &value, const Rgb &seeded, bool force = false) {
			if (!force && rgb_to_string(value) == rgb_to_string(seeded)) return;
			output << key << " " << rgb_to_string(value) << NL;
			any = true;
		};
		line("sun_rgb", scratch.sun, seed.sun);
		line("moon_rgb", scratch.moon, seed.moon);
		line("sky_rgb", scratch.sky, seed.sky);
		line("ground_rgb", scratch.ground, seed.ground);
		// skyfog before fog, both when either moved: a fog line mirrors into a skyfog that
		// still holds its seed (@ 0x57c9b8), so the pair reads back as written.
		const bool fog_moved = rgb_to_string(scratch.fog) != rgb_to_string(seed.fog) ||
				rgb_to_string(scratch.skyfog) != rgb_to_string(seed.skyfog);
		line("skyfog_rgb", scratch.skyfog, seed.skyfog, fog_moved);
		line("fog_rgb", scratch.fog, seed.fog, fog_moved);
		line("skybase_rgb", scratch.skybase, seed.skybase);
		line("skybright_rgb", scratch.skybright, seed.skybright);
		line("skyhighlight_rgb", scratch.skyhighlight, seed.skyhighlight);
		line("cloudbase_rgb", scratch.cloudbase, seed.cloudbase);
		line("cloudhighlight_rgb", scratch.cloudhighlight, seed.cloudhighlight);
		line("cloudedge_rgb", scratch.cloudedge, seed.cloudedge);
		if (any) output << NL;
	}
	output << "envscale " << number_to_string(cfg.envscale) << NL;
	output << NL;
	output << "iris_percent " << number_to_string(cfg.iris_percent) << NL;
	output << "iris_center " << number_to_string(cfg.iris_center) << NL;
	output << NL;
	output << "water_rgb " << rgb_to_string(cfg.water_rgb) << NL;
	if (cfg.water_height_set) {
		output << "water_height " << whole_to_string(cfg.water_height) << NL;
	}
	output << NL;
	output << "sky_map1 " << name_token(cfg.sky_map1) << NL;
	output << "sky_map2 " << name_token(cfg.sky_map2) << NL;
	// The keyword reads whole units (atol, stored << 16 [orig: TimeOfDay_ParseProperty
	// @ 0x57cbc3]), so the engine's raw-200 default (~0.003 units [orig:
	// Environment_InitDefaults @ 0x57c1ab]) has no file form: left unwritten, it reads back
	// as itself, where any line would read back as 0.
	if (cfg.sky_height != Config().sky_height) {
		output << "sky_height " << whole_to_string(cfg.sky_height) << NL;
	}
	output << "sky_speed " << whole_to_string(cfg.sky_speed) << NL;
	output << NL;
	output << "fog_level " << whole_to_string(cfg.fog_level) << NL;
	output << "fog_type " << cfg.fog_type << NL;
	output << NL;
	output << "cloud_rgb " << rgb_to_string(cfg.cloud_rgb) << NL;
	output << "terrain_rgb " << rgb_to_string(cfg.terrain_rgb) << NL;
	output << "vertex_rgb " << rgb_to_string(cfg.vertex_rgb) << NL;
	output << NL;
	output << "lightning_rgb " << rgb_to_string(cfg.lightning_rgb) << NL;
	output << "sun_3di " << name_token(cfg.sun_3di) << NL;
	output << "moon_3di " << name_token(cfg.moon_3di) << NL;
	output << "glare_3di " << name_token(cfg.glare_3di) << NL;
	output << "star_3di " << name_token(cfg.star_3di) << NL;
	output << NL;
	output << "ceiling_rgb " << rgb_to_string(cfg.ceiling_rgb) << NL;
	output << "floor_rgb " << rgb_to_string(cfg.floor_rgb) << NL;
	output << NL;
	// Four HHMM digits: the time token reads by position, so a time before 01:00 written short
	// ("30") would read as 00:00 [orig: Environment_ParseTimeString @ 0x57c500, a token shorter
	// than three characters reads 0].
	output << "curtime " << format_tod_time(cfg.curtime) << NL;
	if (cfg.tod_rate_set) output << "tod_rate " << cfg.tod_rate << NL;
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

namespace {

Rgb lerp_rgb_bytes(const Rgb &a, const Rgb &b, int fraction_fp) {
	// Both inputs are already-quantized engine bytes (envscale applied at
	// interpolation), so the lerp runs at scale 1.
	return lerp_rgb_quantized(a, b, fraction_fp, 1.0f);
}

} // namespace

TodState blend_tod_states(const TodState &env_state, const TodState &overcast_state,
		int overcast_blend_fp) {
	// [orig: Environment_LerpKeyframeSet @ 0x57c3b0] — fractions above 63356
	// snap to 1.0 (the transposed-65536 typo), negatives to 0.
	int fraction = overcast_blend_fp;
	if (fraction < 0) {
		fraction = 0;
	} else if (fraction > 63356) {
		fraction = 0x10000;
	}
	if (fraction == 0) {
		return env_state;
	}
	TodState out;
	out.sun = lerp_rgb_bytes(env_state.sun, overcast_state.sun, fraction);
	out.ground = lerp_rgb_bytes(env_state.ground, overcast_state.ground, fraction);
	out.fog = lerp_rgb_bytes(env_state.fog, overcast_state.fog, fraction);
	out.sky = lerp_rgb_bytes(env_state.sky, overcast_state.sky, fraction);
	out.moon = lerp_rgb_bytes(env_state.moon, overcast_state.moon, fraction);
	out.skyfog = lerp_rgb_bytes(env_state.skyfog, overcast_state.skyfog, fraction);
	out.skybase = lerp_rgb_bytes(env_state.skybase, overcast_state.skybase, fraction);
	out.skybright = lerp_rgb_bytes(env_state.skybright, overcast_state.skybright, fraction);
	out.skyhighlight = lerp_rgb_bytes(env_state.skyhighlight, overcast_state.skyhighlight, fraction);
	out.cloudbase = lerp_rgb_bytes(env_state.cloudbase, overcast_state.cloudbase, fraction);
	out.cloudhighlight = lerp_rgb_bytes(env_state.cloudhighlight, overcast_state.cloudhighlight, fraction);
	out.cloudedge = lerp_rgb_bytes(env_state.cloudedge, overcast_state.cloudedge, fraction);
	return out;
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
	const float hours = static_cast<float>(hhmm_to_hours_fp(tod_time)) / io::kFp16One;
	const float angle = 2.0f * 3.14159265358979323846f * hours / 24.0f;
	Vec3 dir;
	dir.x = -22414.0f / io::kFp16One;
	dir.y = -(61583.0f / io::kFp16One) * std::cos(angle);
	dir.z = (61583.0f / io::kFp16One) * std::sin(angle);
	// The getter exposes the fixed tuple directly; terrain byte-packs it without
	// another normalization [orig: Environment_GetLightDirectionFloat @ 0x57d870,
	// tuple copy @ 0x57d8be..0x57d8cd; PolyTrn_RenderTile @ 0x60e1d9..0x60e331].
	return dir;
}

Vec3 compute_moon_direction(float tod_time) {
	// [orig: Terrain_ComputeMoonDirection @ 0x57d760] — sun phase + pi, constants
	// 56755/65536 = 0.866 and Y = 0x8000 = 0.5, hours space as for the sun.
	const float hours = static_cast<float>(hhmm_to_hours_fp(tod_time)) / io::kFp16One;
	const float angle = 2.0f * 3.14159265358979323846f * hours / 24.0f + 3.14159265358979323846f;
	Vec3 dir;
	dir.x = -32768.0f / io::kFp16One;
	dir.y = -(56755.0f / io::kFp16One) * std::cos(angle);
	dir.z = (56755.0f / io::kFp16One) * std::sin(angle);
	// The active getter copies this selected moon tuple through the same
	// direct path [orig: Environment_GetLightDirectionFloat @ 0x57d870].
	return dir;
}

bool load_mission_env(const MissionEnvTexts &texts, MissionEnv &out) {
	// The defaults the load reset first, and the parse state it seeds once [orig:
	// Environment_InitDefaults @ 0x57c010; Environment_LoadTimeOfDayConfig @ 0x57db44..0x57db8e].
	out = MissionEnv();
	Config &config = out.config;
	TodParse parse;
	// The terrain's pass, then overcast.def's into the same table [orig: Environment_LoadTimeOfDayConfig
	// @ 0x57dbeb, @ 0x57dc3b], that table the overcast table [orig: @ 0x57dc48].
	if (texts.terrain != nullptr) parse_tod_pass(*texts.terrain, config, parse);
	if (texts.overcast != nullptr) parse_tod_pass(*texts.overcast, config, parse);
	sort_keyframes(config.keyframes);
	out.overcast.keyframes = std::move(config.keyframes);
	out.overcast.envscale = config.envscale;
	// The pointer back on the scratch and the count at 0 for the .env [orig: @ 0x57dc56..0x57dc5c].
	config.keyframes.clear();
	parse.slot = -1;
	parse.skyfog_set.clear();
	if (texts.environment == nullptr) {
		// Skipped before its seeding: the color blocks keep Environment_InitDefaults' targets
		// [orig: @ 0x57dca3, the seeding @ 0x57dce0 not reached; Environment_InitDefaults
		// @ 0x57c03a..0x57c17d], the scratch's seed.
		config.scratch = scratch_keyframe_defaults();
		return false;
	}
	parse_tod_pass(*texts.environment, config, parse);
	sort_keyframes(config.keyframes);
	out.environment = true;
	return true;
}

bool read_mission_env(const EnvTextReader &read, const std::string &terrain_file,
		const std::string &environment_file, MissionEnv &out) {
	std::string terrain, overcast, environment;
	MissionEnvTexts texts;
	if (!terrain_file.empty() && read(terrain_file, terrain)) texts.terrain = &terrain;
	if (read(kOvercastFile, overcast)) texts.overcast = &overcast;
	if (!environment_file.empty() && read(environment_file, environment)) texts.environment = &environment;
	return load_mission_env(texts, out);
}

} // namespace opennova::env
