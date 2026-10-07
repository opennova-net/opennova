#pragma once

#include <cstdint>
#include <functional>
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

// A packed 0xRRGGBB colour as 0..1 floats.
inline Rgb packed_to_rgb01(uint32_t packed) {
	Rgb c;
	c.r = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
	c.g = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
	c.b = static_cast<float>(packed & 0xFF) / 255.0f;
	return c;
}

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

// The parse's scratch keyframe: where a TOD color line outside every tod_begin block
// lands (TimeOfDay_ParseProperty writes through the slot pointer, which sits on the
// scratch slot before the first block and after a tod_end). The load seeds it before the
// passes [orig: Environment_LoadTimeOfDayConfig @ 0x57db54..0x57db7e: fog and skyfog
// 0xC0C0FF, ground 0x202020, light 0x646440, sky 0x404064; the moon and the sky/cloud
// ramps are not seeded: zero on a first load, a previous load's leftovers are not kept],
// and once the .env parsed its colors become the twelve color blocks' parsed targets
// [orig: @ 0x57dce0..0x57dd57]. Those are the same values Environment_InitDefaults leaves
// in the blocks when the .env is skipped [orig: @ 0x57c03a..0x57c17d]. With no keyframe
// table nothing overwrites them (the per-tick compute is gated off, @ 0x57de8a): they are
// the colors a world without one runs on. `sun` is the light block.
inline Keyframe scratch_keyframe_defaults() {
	Keyframe scratch;
	scratch.sun = packed_to_rgb01(0x646440);
	scratch.sky = packed_to_rgb01(0x404064);
	scratch.ground = packed_to_rgb01(0x202020);
	scratch.fog = packed_to_rgb01(0xC0C0FF);
	scratch.skyfog = packed_to_rgb01(0xC0C0FF);
	return scratch;
}

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
	// The day's length in real minutes as the file writes it: the parse makes it the clock's
	// advance a tick, 0x18000000 / (3720 x max(minutes, 60)) [orig: TimeOfDay_ParseProperty
	// @ 0x57d0e4..0x57d118]. A mission's start sets the advance again from its own header (or
	// the session's) [orig: Game_StartMission @ 0x5253c3, @ 0x5253e2], so no mission runs on
	// it. Kept for the round trip; `tod_rate_set` says the file writes it.
	int tod_rate = 0;
	bool tod_rate_set = false;
	std::vector<Keyframe> keyframes;
	// The scratch keyframe after the parse (above): its seed, overwritten by the color
	// lines outside every block, each baked with the envscale read before it as the
	// parser packs it [orig: Color_ScaleRGBAndPack @ 0x57f890]. The targets a world with
	// no keyframe table runs on.
	Keyframe scratch = scratch_keyframe_defaults();
};

// The engine parses at most 16 TOD keyframes. A later tod_begin neither takes a slot nor
// moves the slot pointer, so its color lines land where the pointer already is: the 16th
// slot while that block is still open, the scratch keyframe after its tod_end
// [orig: TimeOfDay_ParseProperty @ 0x57c65b, tod_end's reset @ 0x57c6a3].
inline constexpr int kMaxTodKeyframes = 16;

// The HHMM clock's authored range; every time-of-day setter clamps into it.
inline constexpr int kTodTimeMax = 2359;
inline int clamp_tod_time(int time) {
	return time < 0 ? 0 : (time > kTodTimeMax ? kTodTimeMax : time);
}

Config make_default_config();

// Retail keeps negative authored values and clamps only the upper bound.
// [orig: TimeOfDay_ParseProperty water_murk @ 0x57cba9]
float clamp_water_murk_upper(float value);

bool load_env(std::istream &input, Config &out, std::string &error);
// The file from scratch (no stock writer exists; retail reads it through the parser above):
// every keyword the parser reads, a line each, CR LF, the keyframes in time order. Each value
// is written in the form the parser reads back as the Config holds it: a time as four HHMM
// digits, an atol keyword as a whole number, an atof keyword in the fewest digits that read
// back as the same float, a name quoted where it holds a separator. False, with `error`, for a
// name no line can carry (a '"', a control character).
bool save_env(std::ostream &output, const Config &cfg, std::string &error);

// What the parser makes of one line's key and value, for a caller that reads a file's lines
// itself (the editor's source findings): the HHMM time a token reads as, by position
// [orig: Environment_ParseTimeString @ 0x57c500]; whether a key (lower case) is a color line
// the slot pointer takes, one the parser reads at all, and one whose color the envscale read
// before it scales [orig: TimeOfDay_ParseProperty @ 0x57c590: every *_rgb arm but terrain_rgb
// packs through Color_ScaleRGBAndPack]; whether a name can be written on a line (no '"', no
// control character).
int parse_tod_time(const char *text);
bool is_tod_color_key(const std::string &key);
bool is_env_key(const std::string &key);
bool is_envscaled_key(const std::string &key);
bool env_name_writable(const std::string &name);

// The overcast keyframes' file, parsed after the terrain's on every time-of-day load where it
// exists [orig: Environment_LoadTimeOfDayConfig @ 0x57db30, the name @ 0x57dc0c, the exists check
// @ 0x57dc23].
inline constexpr const char *kOvercastFile = "overcast.def";

// The texts a mission's time-of-day load reads, in its order; each null where its file is not
// there. `terrain` null is the load with no map name, which goes straight to overcast.def [orig:
// Environment_LoadTimeOfDayConfig @ 0x57db98]; a map whose .trn is missing returns before every
// pass [orig: @ 0x57dbca..0x57dbcf], and with it the terrain's load and the mission's [orig:
// Terrain_LoadEnvironmentConfig @ 0x610a24..0x610a40, its colour map, detail map and polydata
// unnamed; Game_LoadTerrainDuringConnect @ 0x520745], which a caller refuses before it reads an
// environment.
struct MissionEnvTexts {
	const std::string *terrain = nullptr;     // the mission's <terrain>.trn
	const std::string *overcast = nullptr;    // kOvercastFile
	const std::string *environment = nullptr; // the mission's <environment>.env
};

// What the load makes: the mission's environment, and the overcast table its overcast cross-fades
// toward (its keyframes, and the envscale they are read under: divergence #8).
struct MissionEnv {
	Config config;
	Config overcast;
	bool environment = false; // the .env parsed
};

// A mission's environment as the mission load makes it [orig: Environment_LoadTimeOfDayConfig
// @ 0x57db30]. The load resets every field to the pre-parse defaults first [orig:
// Terrain_LoadEnvironmentConfig @ 0x610947 -> Environment_InitDefaults @ 0x57c010], then runs the
// keyword parser over three files in turn, one set of globals under all three: the terrain's .trn
// [orig: @ 0x57dbeb], overcast.def after it into the same keyframe table, its slot count not reset
// [orig: @ 0x57dc3b], that table the overcast table [orig: Environment_SortAndSnapshotKeyframes into
// g_EnvTrnSnapshotTable @ 0x57dc48], then the slot pointer back on the scratch keyframe and the count
// at 0 [orig: @ 0x57dc56..0x57dc5c] for the .env [orig: @ 0x57dcbf], whose keyframes are the
// mission's [orig: @ 0x57dd5c]. So a keyword a file writes is the mission's until a later file writes
// it: the .env's over the terrain's (a .trn's water_rgb and water_murk, which every shipped terrain
// writes, stand where its .env writes none), the envscale read last scaling what follows it [orig:
// g_EnvParseEnvScale set once @ 0x57db49]. The parser reads every line of the three: the hook the
// terrain's load installs ahead of it returns 0 on each [orig: TimeOfDay_ParseProperty @ 0x57c5ac;
// Terrain_LoadEnvironmentConfig @ 0x6109ad pushes Terrain_ParseConfigCallback @ 0x60f330, whose every
// exit is 0]. A .env that does not exist, or does not parse, is skipped before it seeds a color or
// snapshots a keyframe [orig: the FileExists check @ 0x57dca3, the parse's @ 0x57dcbf], and the
// mission starts all the same [orig: Game_LoadTerrainDuringConnect @ 0x520710 reads no outcome of
// it]: on the earlier passes' globals, with no keyframe and the scratch keyframe unseeded. False
// when the .env was skipped.
bool load_mission_env(const MissionEnvTexts &texts, MissionEnv &out);

// The same load over a reader of the files by name (`read` false: no such file): `terrain_file`
// (empty: no map name) and `environment_file` (empty: none) as the caller names them, overcast.def by
// kOvercastFile.
using EnvTextReader = std::function<bool(const std::string &name, std::string &text)>;
bool read_mission_env(const EnvTextReader &read, const std::string &terrain_file,
		const std::string &environment_file, MissionEnv &out);

// HHMM (digits clamped positionally: hours <= 23, minutes <= 59) to 16.16
// fixed-point hours [orig: Environment_ParseTimeString @ 0x57c500].
int hhmm_to_hours_fp(float hhmm);

// Interpolates in 16.16 HOURS space with the engine's integer math: byte
// quantization (x envscale, truncated, clamped <= 255) before the lerp,
// per-channel (t*(b-a) + (a<<16) + 0x8000) >> 16 rounding, truncating segment
// fraction, 24h wrap, and the t > 63356 full-snap quirk.
// [orig: Environment_ComputeTimeOfDayColors @ 0x57de40 + helpers]
TodState interpolate_tod(const std::vector<Keyframe> &keyframes, float time, float envscale = 1.0f);

// The overcast cross-fade: the final TOD colors interpolated between the .env
// snapshot and the .trn/overcast.def snapshot by the weather's overcast blend
// (16.16, clamped to 0x10000) — the same per-byte lerp with the 63356 snap
// quirk [orig: Environment_ComputeTimeOfDayColors @ 0x57de40 ->
// Environment_LerpKeyframeSet @ 0x57c3b0 over (env, trn, clamp(g_EnvOvercastBlend))].
TodState blend_tod_states(const TodState &env_state, const TodState &overcast_state,
		int overcast_blend_fp);

Vec3 compute_sun_direction(float tod_time);
Vec3 compute_moon_direction(float tod_time);
// ---------------------------------------------------------------------------
// BMS mission overrides [orig: Game_LoadTerrainDuringConnect @ 0x520710]
//                       [orig: Game_StartMission @ 0x525371..0x525399]

struct BmsEnvOverrides {
	bool has_water_height = false; // attrib bit 0x1
	float water_height = 0.0f;     // file s16, world half-units (engine <<15)
	bool has_fog_level = false;    // attrib bit 0x2
	float fog_level = 0.0f;        // world units
	bool has_fog_color = false;    // attrib bit 0x4
	Rgb fog_color;
	bool has_water_color = false; // any RGB byte nonzero
	Rgb water_color;
	bool has_water_murk = false; // murk byte nonzero
	float water_murk = 0.0f;     // byte * 0.01
	bool has_start_time = false; // local play only
	int start_time = 0;          // HHMM
};

// Applies the override layer onto a parsed Config (the engine mutates its
// globals; we mutate a copy so the base file stays authoritative).
void apply_bms_overrides(Config &config, const BmsEnvOverrides &overrides);

// The override layer a mission header authors: the water height (attrib bit
// 0x1; file s16 half-units), the fog distance (0x2), the fog color (0x4, bytes
// -> 0..1), and the ungated water color (any byte nonzero) and murk (byte
// nonzero, * 0.01). start_time is local-play state the caller adds.
// [orig: Game_LoadTerrainDuringConnect @ 0x520710 + Game_StartMission @ 0x525371]
BmsEnvOverrides bms_env_overrides_from_header(uint32_t attrib_flags, int water_override,
                                              int fog_override, const int fog_color[3],
                                              const int water_color[3], int water_murk);

} // namespace opennova::env
