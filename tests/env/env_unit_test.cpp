#include <formats/env/env.h>
#include <formats/env/tod_clock.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float actual, float expected, float epsilon = 0.0001f) {
	return std::fabs(actual - expected) <= epsilon;
}

std::string fixture_path() {
#ifdef OPENNOVA_SOURCE_DIR
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/env/synth_full.env";
#else
	return "fixtures/env/synth_full.env";
#endif
}

} // namespace

int main() {
	// The mission TOD clock (env/tod_clock.h): the witnessed exact-integer
	// rate 0x18000000/(3720*minutes) [orig: @0x57d108], the 60-minute day
	// floor [orig: @0x57d170], and the Q8.8 -> day-wrapped 8.24 widening
	// [orig: @0x525371].
	bool clock_ok = true;
	clock_ok &= expect(opennova::env::kTodDayFixed24 == 0x18000000,
			"the 8.24 day span is the witnessed 0x18000000 dividend");
	clock_ok &= expect(opennova::env::tod_advance_per_tick(1440) ==
					0x18000000 / (3720 * 1440),
			"the default 1440-minute day uses the exact integer rate");
	clock_ok &= expect(opennova::env::tod_advance_per_tick(10) ==
					opennova::env::tod_advance_per_tick(60),
			"day lengths below 60 minutes clamp to the retail floor");
	// A mission's day length of 0 is a clock that stands [orig: Environment_SetTodAdvanceRate
	// @0x57d176, @0x57d196]; an .env's tod_rate of 0 takes the floor (its arm has no zero test,
	// @0x57d0fe), and with neither the clock runs at Environment_InitDefaults' 75 (@0x57c22f).
	clock_ok &= expect(opennova::env::tod_advance_per_tick(0) == 0,
			"a mission day length of 0 stands the clock");
	clock_ok &= expect(opennova::env::tod_rate_advance_per_tick(0) ==
					opennova::env::tod_advance_per_tick(60),
			"an .env tod_rate of 0 takes the 60-minute floor");
	clock_ok &= expect(opennova::env::kTodDefaultAdvancePerTick == 75,
			"the engine's default advance is 75");
	// Q8.8 12.00 -> 12h in 8.24; 25.5h wraps to 1.5h.
	clock_ok &= expect(opennova::env::tod_start_fixed24(12 << 8) == 12 << 24,
			"the Q8.8 start hour widens by 16 bits");
	clock_ok &= expect(opennova::env::tod_start_fixed24((25 << 8) | 0x80) ==
					(1 << 24) + (1 << 23),
			"a start hour past 24 wraps into the day");
	// Midnight wraps day-modulo (the floor-divided rate drifts a few 8.24
	// units per authored day — retail's own exact-integer behavior, kept).
	const int32_t rate = opennova::env::tod_advance_per_tick(60);
	const int32_t before_midnight = opennova::env::kTodDayFixed24 - rate;
	clock_ok &= expect(
			opennova::env::tod_advance(before_midnight, 2, rate) == rate,
			"the accumulator wraps across midnight, never overflows");
	if (!clock_ok) return 1;

	// A mission's .env that is not there is skipped and the mission starts on the
	// pre-parse defaults with no keyframe [orig: Environment_LoadTimeOfDayConfig @ 0x57dca3;
	// Environment_InitDefaults @ 0x57c010]; one that is there parses over them.
	{
		using opennova::env::MissionEnv;
		using opennova::env::MissionEnvTexts;
		MissionEnv loaded;
		loaded.config.fog_level = 1.0f;
		loaded.config.keyframes.resize(3);
		const bool skipped = !opennova::env::load_mission_env(MissionEnvTexts{}, loaded);
		const opennova::env::Config &mission = loaded.config;
		const opennova::env::Config defaults;
		if (!expect(skipped && !loaded.environment && mission.keyframes.empty() && near(mission.fog_level, 1024.0f) &&
		                    mission.fog_type == 1 && mission.sky_map1 == "cld_day1.pcx" &&
		                    mission.sun_3di == "msun.3di" && mission.curtime == defaults.curtime &&
		                    near(mission.water_murk, 0.8f) && near(mission.iris_percent, 50.0f) &&
		                    loaded.overcast.keyframes.empty(),
		            "a missing mission .env leaves the engine defaults and no keyframe"))
			return 1;
		const std::string empty;
		MissionEnvTexts texts;
		texts.environment = &empty;
		if (!expect(opennova::env::load_mission_env(texts, loaded) && loaded.environment &&
		                    loaded.config.keyframes.empty() && near(loaded.config.fog_level, 1024.0f),
		            "an empty mission .env parses to the same defaults"))
			return 1;
		const std::string authored = "fog_level 640\r\nfog_type 2\r\n";
		texts.environment = &authored;
		if (!expect(opennova::env::load_mission_env(texts, loaded) && near(loaded.config.fog_level, 640.0f) &&
		                    loaded.config.fog_type == 2 && loaded.config.sky_map1 == "cld_day1.pcx",
		            "a mission .env parses over the defaults"))
			return 1;
	}

	// The terrain's pass (env #43): the parser reads the .trn first, then overcast.def into the
	// same table, then the .env, one set of globals under all three [orig:
	// Environment_LoadTimeOfDayConfig @ 0x57db30, @ 0x57dbeb, @ 0x57dc3b, @ 0x57dcbf]. A shipped
	// terrain's water_rgb and water_murk stand where its .env writes neither; the .env's win where
	// it writes them; the envscale read last scales what follows it; the .trn's keyframes and
	// overcast.def's are the overcast table, the .env's the mission's; the terrain's own keys
	// (polytrn_*, terrain_creator, its foliage blocks) are no keyword of the parser.
	{
		using opennova::env::MissionEnv;
		using opennova::env::MissionEnvTexts;
		const std::string trn = "terrain_name     \"Dvxi5\"\r\nterrain_creator  \"Brophy\"\r\n"
		                        "water_height     21         ;default, if zero will take from mission\r\n"
		                        "polytrn_colormap         Dvxi5_c.tga\r\n"
		                        "water_rgb \t\t 108,81,48\r\nwater_murk  \t\t .3\r\n"
		                        "foliage\r\n  graphic mveg5.3di\r\n  match 254\r\nend\r\n";
		const std::string env = "fog_level 640\r\nwater_rgb 56,59,39\r\n"
		                        "tod_begin 1200\r\n  sun_rgb 10,20,30\r\ntod_end\r\n";
		MissionEnv loaded;
		MissionEnvTexts texts;
		texts.terrain = &trn;
		texts.environment = &env;
		if (!expect(opennova::env::load_mission_env(texts, loaded) && near(loaded.config.water_murk, 0.3f) &&
		                    near(loaded.config.water_rgb.r, 56.0f / 255.0f) &&
		                    near(loaded.config.water_rgb.b, 39.0f / 255.0f) && loaded.config.water_height_set &&
		                    near(loaded.config.water_height, 21.0f) && near(loaded.config.fog_level, 640.0f) &&
		                    loaded.config.keyframes.size() == 1 && loaded.config.name == "Untitled",
		            "the .trn's murk and height stand under a .env that writes neither; its water_rgb is the .env's"))
			return 1;
		const std::string env_writes = "water_murk 0.6\r\nwater_height 30\r\n";
		texts.environment = &env_writes;
		if (!expect(opennova::env::load_mission_env(texts, loaded) && near(loaded.config.water_murk, 0.6f) &&
		                    near(loaded.config.water_height, 30.0f) && near(loaded.config.water_rgb.r, 108.0f / 255.0f),
		            "the .env's murk and height are over the .trn's; the .trn's water_rgb stands"))
			return 1;
		const std::string pinned_murk = "water_murk 1.0\r\n";
		texts.terrain = &pinned_murk;
		texts.environment = nullptr;
		if (!expect(!opennova::env::load_mission_env(texts, loaded) && !loaded.environment &&
		                    near(loaded.config.water_murk, 0.99f) && loaded.config.keyframes.empty(),
		            "a missing .env leaves the .trn's murk (clamped at 0.99) over the defaults"))
			return 1;
		// The overcast table: the .trn's blocks then overcast.def's, one table, sorted; the .env's
		// own table starts empty; the .trn's envscale scales the .env's colours where the .env
		// writes none.
		const std::string trn_blocks = "envscale 2\r\ntod_begin 1800\r\n sun_rgb 1,1,1\r\ntod_end\r\n";
		const std::string overcast = "tod_begin 0600\r\n sun_rgb 2,2,2\r\ntod_end\r\n";
		const std::string env_table = "tod_begin 1200\r\n sun_rgb 100,100,100\r\ntod_end\r\n";
		texts.terrain = &trn_blocks;
		texts.overcast = &overcast;
		texts.environment = &env_table;
		if (!expect(opennova::env::load_mission_env(texts, loaded) && loaded.overcast.keyframes.size() == 2 &&
		                    loaded.overcast.keyframes[0].time == 600 && loaded.overcast.keyframes[1].time == 1800 &&
		                    near(loaded.overcast.envscale, 2.0f) && loaded.config.keyframes.size() == 1 &&
		                    loaded.config.keyframes[0].time == 1200 && near(loaded.config.envscale, 2.0f),
		            "the overcast table is the .trn's and overcast.def's blocks; the .env's own"))
			return 1;
		// A naked colour line in the .trn lands on the scratch keyframe the .env's pass goes on
		// over, seeded once a load; with the .env skipped the blocks keep their defaults.
		const std::string trn_naked = "sky_rgb 1,2,3\r\n";
		texts.terrain = &trn_naked;
		texts.overcast = nullptr;
		const std::string env_naked = "sun_rgb 4,5,6\r\n";
		texts.environment = &env_naked;
		if (!expect(opennova::env::load_mission_env(texts, loaded) &&
		                    near(loaded.config.scratch.sky.b, 3.0f / 255.0f) &&
		                    near(loaded.config.scratch.sun.b, 6.0f / 255.0f),
		            "the scratch keyframe carries the .trn's naked colour into the .env's pass"))
			return 1;
		texts.environment = nullptr;
		const opennova::env::Keyframe seed = opennova::env::scratch_keyframe_defaults();
		if (!expect(!opennova::env::load_mission_env(texts, loaded) &&
		                    near(loaded.config.scratch.sky.b, seed.sky.b),
		            "a skipped .env leaves the scratch's seed"))
			return 1;
		// The reader's names: the .trn and the .env as named, overcast.def by kOvercastFile.
		std::vector<std::string> asked;
		const opennova::env::EnvTextReader read = [&](const std::string &name, std::string &text) {
			asked.push_back(name);
			if (name == "Dvxi5.trn") text = trn;
			else if (name == "full_00.env") text = env;
			else return false;
			return true;
		};
		if (!expect(opennova::env::read_mission_env(read, "Dvxi5.trn", "full_00.env", loaded) &&
		                    asked.size() == 3 && asked[0] == "Dvxi5.trn" &&
		                    asked[1] == opennova::env::kOvercastFile && asked[2] == "full_00.env" &&
		                    near(loaded.config.water_murk, 0.3f),
		            "the reader reads the .trn, overcast.def and the .env in the load's order"))
			return 1;
		asked.clear();
		if (!expect(!opennova::env::read_mission_env(read, "", "", loaded) && asked.size() == 1 &&
		                    near(loaded.config.water_murk, 0.8f),
		            "no names: overcast.def alone, the defaults standing"))
			return 1;
	}

	// The lines and tokens are the shared retail walk's: a CR LF pair ends a line and an LF
	// alone does not, so an LF-only file is one line whose first token is its key; keys
	// compare without case; a value is a token, numbers the CRT's atol and atof (a `d`
	// exponent); a time token reads by position; a color's blue is token 3, which a short
	// line reads where an earlier, longer line left it; and a non-color key inside a block
	// reads as it does outside one. [orig: Environment_LoadTimeOfDayConfig @ 0x57db30 ->
	// File_ParseASCIIFile @ 0x53d810; TimeOfDay_ParseProperty @ 0x57c590, the stricmp of
	// tokens[1] and atol of tokens[2..4]; Environment_ParseTimeString @ 0x57c500]
	{
		opennova::env::Config lf;
		std::string err;
		std::istringstream lf_text("fog_level 640\nfog_type 2\n");
		if (!expect(opennova::env::load_env(lf_text, lf, err) && near(lf.fog_level, 640.0f) &&
		                    lf.fog_type == opennova::env::Config().fog_type,
		            "an LF-only file is one line"))
			return 1;
		opennova::env::Config walked;
		std::istringstream text("enviro_name \"Named\" // a note\r\nENVSCALE 1d1\r\nFOG_TYPE 3\r\n"
		                        "curtime 30\r\nnote aaaaaaaaaaaaaaa b 99\r\n"
		                        "tod_begin 1200\r\n    sun_rgb 1,2\r\n    water_murk 0.5\r\ntod_end\r\n");
		if (!expect(opennova::env::load_env(text, walked, err) && walked.name == "Named" &&
		                    near(walked.envscale, 10.0f) && walked.fog_type == 3 && walked.curtime == 0 &&
		                    walked.keyframes.size() == 1 && walked.keyframes[0].time == 1200 &&
		                    near(walked.keyframes[0].sun.g, 2.0f / 255.0f) &&
		                    near(walked.keyframes[0].sun.b, 99.0f / 255.0f) && near(walked.water_murk, 0.5f),
		            "the retail walk's tokens, keys, numbers, times, stale blue and in-block globals"))
			return 1;
	}

	// The scratch keyframe (env #39): a color line outside every tod_begin block lands there,
	// packed with the envscale read before it [orig: Color_ScaleRGBAndPack @ 0x57f890], fog
	// mirroring into the seeded skyfog; the writer puts those lines ahead of the envscale so
	// they read back as they were, and writes none for the seed.
	{
		opennova::env::Config naked;
		std::string err;
		std::istringstream lines("envscale 0.5\r\nsky_height 175\r\nsky_rgb 10,20,30\r\nfog_rgb 40,50,60\r\n"
		                         "tod_begin 1200\r\n    sun_rgb 1,1,1\r\ntod_end\r\n");
		if (!expect(opennova::env::load_env(lines, naked, err) && naked.keyframes.size() == 1,
		            "naked color lines parse beside a block"))
			return 1;
		const opennova::env::Keyframe seed = opennova::env::scratch_keyframe_defaults();
		if (!expect(near(naked.scratch.sky.r, 5.0f / 255.0f) && near(naked.scratch.sky.b, 15.0f / 255.0f) &&
		                    near(naked.scratch.fog.g, 25.0f / 255.0f) && near(naked.scratch.skyfog.g, 25.0f / 255.0f) &&
		                    near(naked.scratch.sun.r, seed.sun.r) && near(naked.keyframes[0].sun.r, 1.0f / 255.0f),
		            "naked lines are baked into the scratch keyframe; the block keeps its own"))
			return 1;
		std::ostringstream saved;
		if (!expect(opennova::env::save_env(saved, naked, err), "save with a scratch keyframe")) return 1;
		const std::string text = saved.str();
		if (!expect(text.find("sky_rgb 5,10,15") != std::string::npos &&
		                    text.find("sky_rgb 5,10,15") < text.find("envscale"),
		            "the scratch lines precede the envscale line"))
			return 1;
		opennova::env::Config back;
		std::istringstream again(text);
		std::ostringstream resaved;
		if (!expect(opennova::env::load_env(again, back, err) && opennova::env::save_env(resaved, back, err) &&
		                    resaved.str() == text && near(back.scratch.sky.b, 15.0f / 255.0f) &&
		                    near(back.scratch.skyfog.g, 25.0f / 255.0f),
		            "a saved scratch keyframe reads back and saves the same"))
			return 1;
		// The engine's raw-200 sky_height default has no file form (the keyword reads whole
		// units, atol << 16): a config holding it writes no sky_height line, so it reads back
		// as the default, where "sky_height 0.00305176" read back as 0.
		{
			std::ostringstream first;
			opennova::env::Config reread;
			if (!expect(opennova::env::save_env(first, opennova::env::Config(), err), "save the defaults")) return 1;
			std::istringstream again_in(first.str());
			std::ostringstream second;
			if (!expect(opennova::env::load_env(again_in, reread, err) &&
			                    reread.sky_height == opennova::env::Config().sky_height &&
			                    opennova::env::save_env(second, reread, err) && second.str() == first.str(),
			            "the default sky_height survives a save"))
				return 1;
		}
		std::ostringstream plain;
		if (!expect(opennova::env::save_env(plain, opennova::env::Config(), err) &&
		                    plain.str().find("_rgb 192,192,255") == std::string::npos &&
		                    plain.str().find("sky_rgb") == std::string::npos,
		            "the seed writes no scratch line"))
			return 1;
	}

	std::ifstream fixture(fixture_path(), std::ios::binary);
	if (!fixture) {
		std::fprintf(stderr, "FAIL: cannot open %s\n", fixture_path().c_str());
		return 1;
	}

	opennova::env::Config loaded;
	std::string error;
	if (!opennova::env::load_env(fixture, loaded, error)) {
		std::fprintf(stderr, "FAIL: load_env failed: %s\n", error.c_str());
		return 1;
	}

	if (!expect(loaded.name == "Synth_Full", "env name should parse")) return 1;
	if (!expect(loaded.timeofday == "Day", "timeofday should parse")) return 1;
	if (!expect(loaded.curtime == 1200, "curtime should parse")) return 1;
	if (!expect(near(loaded.fog_level, 1000.0f), "fog_level should parse")) return 1;
	if (!expect(loaded.fog_type == 2, "fog_type should parse")) return 1;
	if (!expect(!loaded.water_height_set, "synth_full does not author water_height")) return 1;
	if (!expect(loaded.sky_map1 == "Cloud01.pcx", "sky_map1 should parse")) return 1;
	if (!expect(loaded.sky_map2 == "Cloud01b.pcx", "sky_map2 should parse")) return 1;
	if (!expect(loaded.star_3di.empty(), "blank star_3di should parse as empty")) return 1;
	if (!expect(loaded.keyframes.size() == 10, "synth_full should have 10 TOD keyframes")) return 1;
	if (!expect(loaded.keyframes.front().time == 200, "first TOD keyframe should be 0200")) return 1;
	if (!expect(loaded.keyframes.back().time == 2359, "last TOD keyframe should be 2359")) return 1;
	if (!expect(near(loaded.vertex_rgb.r, 128.0f / 255.0f), "vertex_rgb should parse")) return 1;

	const opennova::env::TodState noon = opennova::env::interpolate_tod(loaded.keyframes, 1200.0f, loaded.envscale);
	if (!expect(near(noon.sun.r, 170.0f / 255.0f), "TOD exact noon sun should match keyframe")) return 1;
	if (!expect(near(noon.fog.b, 140.0f / 255.0f), "TOD exact noon fog should match keyframe")) return 1;

	// Integer channel lerp rounds half up: 1300 sits exactly between 1200 and
	// 1400 in HOURS space; sun.r 170 -> 165 lands on 168 (not 167.5).
	// [orig: Color_InterpolateRGB888 @ 0x57c2f0]
	const opennova::env::TodState between = opennova::env::interpolate_tod(loaded.keyframes, 1300.0f, loaded.envscale);
	if (!expect(near(between.sun.r, 168.0f / 255.0f), "TOD interpolation should blend between 1200 and 1400")) return 1;

	const opennova::env::TodState wrap = opennova::env::interpolate_tod(loaded.keyframes, 100.0f, loaded.envscale);
	if (!expect(wrap.moon.r > 0.0f, "TOD interpolation should wrap across midnight")) return 1;

	// The float getters expose retail's fixed tuples directly; normalizing here
	// perturbs the bytes packed later by the terrain and sky lighting paths.
	{
		const opennova::env::Vec3 sun = opennova::env::compute_sun_direction(1200.0f);
		const opennova::env::Vec3 moon = opennova::env::compute_moon_direction(0.0f);
		if (!expect(near(sun.x, -22414.0f / 65536.0f, 1.0e-7f), "sun fixed x tuple should stay unnormalized")) return 1;
		if (!expect(near(sun.y, 61583.0f / 65536.0f, 1.0e-7f), "sun fixed magnitude should stay unnormalized")) return 1;
		if (!expect(near(moon.x, -32768.0f / 65536.0f, 1.0e-7f), "moon fixed x tuple should stay unnormalized")) return 1;
		if (!expect(near(moon.y, 56755.0f / 65536.0f, 1.0e-7f), "moon fixed magnitude should stay unnormalized")) return 1;
	}

	// Interpolation runs in HOURS space, not HHMM: 1230 is halfway between 1200
	// and 1300 (12.5h), not 30% of the way. [orig: Environment_ParseTimeString @ 0x57c500]
	{
		std::vector<opennova::env::Keyframe> hours_frames(2);
		hours_frames[0].time = 1200;
		hours_frames[0].sun = {0.0f, 0.0f, 0.0f};
		hours_frames[1].time = 1300;
		hours_frames[1].sun = {200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f};
		const opennova::env::TodState half = opennova::env::interpolate_tod(hours_frames, 1230.0f, 1.0f);
		if (!expect(near(half.sun.r, 100.0f / 255.0f), "1230 should be the hours-space midpoint of 1200..1300")) return 1;

		// Fractions above 63356/65536 snap to 1.0 (original source typo kept for
		// parity): 12:59 of a one-hour segment yields 64443 > 63356 -> endpoint.
		// [orig: Environment_LerpKeyframeSet @ 0x57c3c6]
		const opennova::env::TodState snap = opennova::env::interpolate_tod(hours_frames, 1259.0f, 1.0f);
		if (!expect(near(snap.sun.r, 200.0f / 255.0f), "fraction above 63356 should snap to the end keyframe")) return 1;
	}

	// envscale quantizes at the byte level with truncation before the lerp.
	// [orig: Color_ScaleRGBAndPack @ 0x57f890]
	{
		std::vector<opennova::env::Keyframe> scale_frames(1);
		scale_frames[0].time = 0;
		scale_frames[0].sun = {170.0f / 255.0f, 170.0f / 255.0f, 170.0f / 255.0f};
		const opennova::env::TodState scaled = opennova::env::interpolate_tod(scale_frames, 0.0f, 1.1f);
		if (!expect(near(scaled.sun.r, 187.0f / 255.0f), "envscale should truncate at the byte level (170*1.1 = 187)")) return 1;
	}

	// The engine caps TOD keyframes at 16. A 17th tod_begin takes no slot, stores no time and
	// leaves the slot pointer where it is: after the 16th block's tod_end that is the scratch
	// keyframe, so the 17th block's colors land there, packed; with the 16th block still open
	// they bleed into its slot. [orig: TimeOfDay_ParseProperty @ 0x57c65b; tod_end's reset
	// @ 0x57c6a3]
	for (const bool closed : {true, false}) {
		std::ostringstream many;
		for (int i = 0; i < 17; ++i) {
			many << "tod_begin " << (100 * (i % 24)) << "\r\n";
			many << "    sun_rgb " << (i + 1) << "," << (i + 1) << "," << (i + 1) << "\r\n";
			if (closed || i < 15) many << "tod_end\r\n";
		}
		std::istringstream many_input(many.str());
		opennova::env::Config many_cfg;
		error.clear();
		if (!opennova::env::load_env(many_input, many_cfg, error)) {
			std::fprintf(stderr, "FAIL: 17-block env should parse: %s\n", error.c_str());
			return 1;
		}
		if (!expect(many_cfg.keyframes.size() == 16, "keyframes should cap at 16")) return 1;
		const opennova::env::Keyframe *sixteenth = nullptr;
		for (const opennova::env::Keyframe &kf : many_cfg.keyframes)
			if (kf.time == 1500) sixteenth = &kf;
		if (!expect(sixteenth != nullptr, "the 16th block keeps its time")) return 1;
		if (closed) {
			if (!expect(near(sixteenth->sun.r, 16.0f / 255.0f) && near(many_cfg.scratch.sun.r, 17.0f / 255.0f),
			            "after the 16th block's tod_end, the 17th block's colors land on the scratch keyframe"))
				return 1;
		} else if (!expect(near(sixteenth->sun.r, 17.0f / 255.0f) &&
		                           near(many_cfg.scratch.sun.r, opennova::env::scratch_keyframe_defaults().sun.r),
		                   "with the 16th block open, the 17th block's colors bleed into its slot")) {
			return 1;
		}
	}

	// tod_rate, the day's length in minutes, is read and written back as written
	// [orig: TimeOfDay_ParseProperty @ 0x57d0e4].
	{
		std::istringstream rate_input("tod_rate 45\r\n");
		opennova::env::Config rate_cfg;
		std::ostringstream rate_saved;
		if (!expect(opennova::env::load_env(rate_input, rate_cfg, error) && rate_cfg.tod_rate_set &&
		                    rate_cfg.tod_rate == 45 && opennova::env::save_env(rate_saved, rate_cfg, error) &&
		                    rate_saved.str().find("tod_rate 45\r\n") != std::string::npos,
		            "tod_rate reads and writes back"))
			return 1;
		std::ostringstream none;
		if (!expect(opennova::env::save_env(none, opennova::env::Config(), error) &&
		                    none.str().find("tod_rate") == std::string::npos,
		            "a file that writes no tod_rate gets none"))
			return 1;
	}

	// Each value is written in the form the parser reads back as held: a time before 01:00 as
	// four digits (a short token reads 00:00), an atol keyword as a whole number however large,
	// an atof keyword in as many digits as its float needs, a name holding a separator quoted;
	// a name holding a '"' has no line form. [orig: Environment_ParseTimeString @ 0x57c500;
	// TimeOfDay_ParseProperty @ 0x57c590]
	{
		opennova::env::Config forms;
		forms.curtime = 30;
		forms.fog_level = 1500000.0f;
		forms.sky_speed = -7.0f;
		forms.water_height = 2000000.0f;
		forms.water_height_set = true;
		forms.iris_center = 0.123456789f;
		forms.envscale = 1.1f;
		forms.sky_map1 = "my clouds.pcx";
		forms.sun_3di = "sun;1.3di";
		std::ostringstream written;
		if (!expect(opennova::env::save_env(written, forms, error), "save the value forms")) return 1;
		const std::string text = written.str();
		std::istringstream back_in(text);
		opennova::env::Config back;
		if (!expect(opennova::env::load_env(back_in, back, error) && back.curtime == 30 &&
		                    back.fog_level == 1500000.0f && back.sky_speed == -7.0f &&
		                    back.water_height == 2000000.0f && back.iris_center == forms.iris_center &&
		                    back.envscale == forms.envscale && back.sky_map1 == "my clouds.pcx" &&
		                    back.sun_3di == "sun;1.3di",
		            "every value form reads back as held"))
			return 1;
		if (!expect(text.find("curtime 0030\r\n") != std::string::npos &&
		                    text.find("fog_level 1500000\r\n") != std::string::npos &&
		                    text.find("envscale 1.1\r\n") != std::string::npos &&
		                    text.find("sky_map1 \"my clouds.pcx\"\r\n") != std::string::npos,
		            "the forms are the plain ones"))
			return 1;
		forms.moon_3di = "a\"b.3di";
		std::ostringstream refused;
		if (!expect(!opennova::env::save_env(refused, forms, error) && !error.empty(),
		            "a name holding a '\"' is refused"))
			return 1;
	}

	// curtime sanitizes digits positionally (hours <= 23, minutes <= 59).
	// [orig: Environment_ParseTimeString @ 0x57c500]
	{
		std::istringstream curtime_input("curtime 1290\r\n");
		opennova::env::Config curtime_cfg;
		error.clear();
		if (!opennova::env::load_env(curtime_input, curtime_cfg, error)) {
			std::fprintf(stderr, "FAIL: curtime env should parse: %s\n", error.c_str());
			return 1;
		}
		if (!expect(curtime_cfg.curtime == 1259, "curtime 1290 should sanitize to 1259")) return 1;
	}

	// Engine pre-parse defaults (what an .env omitting keywords means).
	// [orig: Environment_InitDefaults @ 0x57c010]
	{
		const opennova::env::Config engine_defaults;
		if (!expect(engine_defaults.sky_map1 == "cld_day1.pcx", "default sky_map1 is cld_day1.pcx")) return 1;
		if (!expect(engine_defaults.star_3di == "mstar.3di", "default star_3di is mstar.3di")) return 1;
		if (!expect(engine_defaults.fog_type == 1, "default fog_type is 1")) return 1;
		if (!expect(near(engine_defaults.fog_level, 1024.0f), "default fog_level is 1024")) return 1;
		if (!expect(near(engine_defaults.iris_percent, 50.0f), "default iris_percent is 50")) return 1;
		if (!expect(engine_defaults.advanced_clouds == 0, "default advanced_clouds is 0")) return 1;
		if (!expect(engine_defaults.curtime == 1500, "default curtime is 15:00")) return 1;
	}

	std::ostringstream saved_stream;
	error.clear();
	if (!opennova::env::save_env(saved_stream, loaded, error)) {
		std::fprintf(stderr, "FAIL: save_env failed: %s\n", error.c_str());
		return 1;
	}
	const std::string saved = saved_stream.str();
	if (!expect(saved.find("\r\n") != std::string::npos, "writer should use CRLF")) return 1;
	for (size_t i = 0; i < saved.size(); ++i) {
		if (saved[i] == '\n' && (i == 0 || saved[i - 1] != '\r')) {
			std::fprintf(stderr, "FAIL: writer produced bare LF at byte %zu\n", i);
			return 1;
		}
	}
	if (!expect(saved.find("tod_begin 0200\r\n") != std::string::npos, "writer should zero-pad TOD times")) return 1;

	opennova::env::Config reparsed;
	std::istringstream saved_input(saved);
	error.clear();
	if (!opennova::env::load_env(saved_input, reparsed, error)) {
		std::fprintf(stderr, "FAIL: reparsing saved env failed: %s\n", error.c_str());
		return 1;
	}
	if (!expect(reparsed.name == loaded.name, "name should roundtrip semantically")) return 1;
	if (!expect(reparsed.keyframes.size() == loaded.keyframes.size(), "keyframe count should roundtrip semantically")) return 1;
	if (!expect(reparsed.keyframes[5].time == 1200, "keyframe times should roundtrip semantically")) return 1;
	if (!expect(near(reparsed.keyframes[5].skyfog.b, loaded.keyframes[5].skyfog.b), "keyframe colors should roundtrip semantically")) return 1;

	const opennova::env::Config defaults = opennova::env::make_default_config();
	std::ostringstream default_stream;
	if (!opennova::env::save_env(default_stream, defaults, error)) {
		std::fprintf(stderr, "FAIL: save_env defaults failed: %s\n", error.c_str());
		return 1;
	}
	std::istringstream default_input(default_stream.str());
	opennova::env::Config default_reparsed;
	if (!opennova::env::load_env(default_input, default_reparsed, error)) {
		std::fprintf(stderr, "FAIL: default env should be reloadable: %s\n", error.c_str());
		return 1;
	}
	if (!expect(!default_reparsed.keyframes.empty(), "default env should include TOD keyframes")) return 1;

	std::istringstream water_input("water_height 32\r\nwater_murk 1.25\r\n");
	opennova::env::Config water_cfg;
	error.clear();
	if (!opennova::env::load_env(water_input, water_cfg, error)) {
		std::fprintf(stderr, "FAIL: water env should parse: %s\n", error.c_str());
		return 1;
	}
	if (!expect(water_cfg.water_height_set, "water_height should record authored presence")) return 1;
	if (!expect(near(water_cfg.water_height, 32.0f), "water_height should parse as world units")) return 1;
	if (!expect(near(water_cfg.water_murk, 0.99f), "water_murk should clamp at 0.99")) return 1;
	if (!expect(near(opennova::env::clamp_water_murk_upper(-0.5f), -0.5f),
	            "water_murk normalization should preserve negative authored values")) return 1;
	if (!expect(near(opennova::env::clamp_water_murk_upper(1.5f), 0.99f),
	            "water_murk normalization should clamp only the upper bound")) return 1;

	std::ostringstream water_saved;
	if (!opennova::env::save_env(water_saved, water_cfg, error)) {
		std::fprintf(stderr, "FAIL: save_env water failed: %s\n", error.c_str());
		return 1;
	}
	if (!expect(water_saved.str().find("water_height 32\r\n") != std::string::npos,
	            "writer should preserve authored water_height")) return 1;

	// The keyframe segment search [orig: Environment_FindKeyframeSegment @ 0x57dd80]: the last keyframe at or before
	// the time, below the first the last (the 24 h wrap), toward the next; the fraction elapsed over the length.
	{
		using opennova::env::find_keyframe_segment;
		using opennova::env::hhmm_to_hours_fp;
		using opennova::env::KeyframeSegment;
		const std::vector<int> times = {hhmm_to_hours_fp(600.0f), hhmm_to_hours_fp(1200.0f), hhmm_to_hours_fp(1800.0f)};
		const KeyframeSegment none = find_keyframe_segment({}, 0);
		if (!expect(none.lo == -1 && none.hi == -1 && none.fraction_fp == 0, "no keyframe, no segment")) return 1;
		const KeyframeSegment mid = find_keyframe_segment(times, hhmm_to_hours_fp(900.0f));
		if (!expect(mid.lo == 0 && mid.hi == 1 && mid.fraction_fp == 0x8000 && mid.elapsed_fp == 3 << 16 &&
		            mid.length_fp == 6 << 16, "09:00 is halfway from 06:00 to 12:00")) return 1;
		const KeyframeSegment at = find_keyframe_segment(times, hhmm_to_hours_fp(1200.0f));
		if (!expect(at.lo == 1 && at.hi == 2 && at.fraction_fp == 0, "a keyframe's own time starts its segment")) return 1;
		// Past the last toward the first, and below the first from the last: the night's 12 h segment.
		const KeyframeSegment late = find_keyframe_segment(times, hhmm_to_hours_fp(2100.0f));
		if (!expect(late.lo == 2 && late.hi == 0 && late.length_fp == 12 << 16 && late.fraction_fp == 0x4000,
		            "21:00 is a quarter of the way from 18:00 to 06:00")) return 1;
		const KeyframeSegment early = find_keyframe_segment(times, hhmm_to_hours_fp(300.0f));
		if (!expect(early.lo == 2 && early.hi == 0 && early.elapsed_fp == 9 << 16 && early.fraction_fp == 0xC000,
		            "03:00 wraps below the first keyframe")) return 1;
		// One keyframe, or a segment of no length: no fraction.
		const KeyframeSegment one = find_keyframe_segment({hhmm_to_hours_fp(1200.0f)}, hhmm_to_hours_fp(1500.0f));
		if (!expect(one.lo == 0 && one.hi == 0 && one.length_fp == 0 && one.fraction_fp == 0, "one keyframe holds all day"))
			return 1;
		// The time taken mod a day.
		if (!expect(find_keyframe_segment(times, hhmm_to_hours_fp(900.0f) + opennova::env::kTodDayHoursFp).fraction_fp ==
		                    0x8000, "a time past a day wraps")) return 1;
	}

	// The tick's colors [orig: Environment_ComputeTimeOfDayColors @ 0x57de40]: the environment's keyframes, cross-faded
	// by the overcast blend toward the overcast table's, toward black with none (env #41).
	{
		opennova::env::Config day;
		day.keyframes.resize(1);
		day.keyframes[0].time = 1200;
		day.keyframes[0].sun = {200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f};
		opennova::env::Config grey = day;
		grey.keyframes[0].sun = {100.0f / 255.0f, 100.0f / 255.0f, 100.0f / 255.0f};
		const opennova::env::TodState clear = opennova::env::tod_colors(day, &grey, 1200.0f, 0);
		if (!expect(near(clear.sun.r, 200.0f / 255.0f), "no overcast, the environment's colors")) return 1;
		const opennova::env::TodState half = opennova::env::tod_colors(day, &grey, 1200.0f, 0x8000);
		if (!expect(near(half.sun.r, 150.0f / 255.0f), "half overcast, halfway to the overcast table")) return 1;
		const opennova::env::TodState dark = opennova::env::tod_colors(day, nullptr, 1200.0f, 0x8000);
		if (!expect(near(dark.sun.r, 100.0f / 255.0f), "no overcast table, halfway to black")) return 1;
		const opennova::env::TodState same = opennova::env::blend_tod_states(
				opennova::env::interpolate_tod(day.keyframes, 1200.0f, day.envscale),
				opennova::env::interpolate_tod(grey.keyframes, 1200.0f, grey.envscale), 0x8000);
		if (!expect(near(same.sun.r, half.sun.r), "the same as interpolate then blend")) return 1;
	}

	// The clock a .env's own lines leave: curtime's truncating 16.16 hours << 8 [orig: TimeOfDay_ParseProperty
	// @ 0x57d0d0], tod_rate's advance, else the engine's default.
	{
		opennova::env::Config clock_cfg;
		clock_cfg.curtime = 1210;
		const opennova::env::TodFileClock clock = opennova::env::tod_file_clock(clock_cfg);
		if (!expect(clock.start_fixed24 == 204122624u, "curtime 1210 starts the clock at (12<<16) + 655360/60, << 8"))
			return 1;
		if (!expect(clock.advance_per_tick == uint32_t(opennova::env::kTodDefaultAdvancePerTick),
		            "no tod_rate, the engine's default advance")) return 1;
		clock_cfg.tod_rate = 30;
		clock_cfg.tod_rate_set = true;
		if (!expect(opennova::env::tod_file_clock(clock_cfg).advance_per_tick ==
		                    uint32_t(opennova::env::tod_rate_advance_per_tick(30)),
		            "tod_rate's advance, the 60-minute floor")) return 1;
	}

	// The keyword store shifts and the ranges they leave.
	if (!expect(opennova::env::env_store_max(opennova::env::kFogLevelStoreShift) == 32767 &&
	            opennova::env::env_store_min(opennova::env::kWaterHeightStoreShift) == -65536 &&
	            opennova::env::env_store_max(opennova::env::kSkySpeedStoreShift) == 2097151,
	            "the store shifts' ranges")) return 1;

	std::printf("OK: env parse/write/interpolation\n");
	return 0;
}
