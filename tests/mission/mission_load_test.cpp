// The mission load's executor (runtime/mission/mission_load.h), ungated: every leg records its
// name, so each arm's order is pinned as the engine runs it, with no device behind the legs.
//   - a single player's or a listen host's fresh session: the create ahead of every load, the
//     head (begin, update, effect system, window), the head's terrain and its pump, the objects
//     with their two pumps, the runtime, the freeze, the device stages, phase B past them, the
//     load end; the loading screen at each stage's checkpoint, the session's periodic update
//     past each (formerly GUT's game_world_test "reports only real stage checkpoints");
//   - a dedicated host as opennova-serve runs it, its session's legs alone: phase A, the freeze
//     ahead of the render arm's pump and phase B, then the load end (before, the server froze its
//     page past phase B);
//   - a wire-header joiner's S2C 0x11 head and its S2C 0x1A load past it: no create, no host
//     head, the mission start with the runtime, the pool-0 reload, the streamed world, the first
//     0x2F's leg, the streamed types' legs ahead of the freeze;
//   - a failed create, a failed terrain gate and a failed runtime stop where they fail.
#include <runtime/mission/mission_load.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova::mission;

namespace {

int failures = 0;

bool check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
	return ok;
}

using Trace = std::vector<std::string>;

void dump(const Trace &trace) {
	for (const std::string &row : trace)
		std::fprintf(stderr, "  %s\n", row.c_str());
}

// Every leg records itself; the failing ones answer the error the test names.
struct Recorder {
	Trace trace;
	MissionLoadRole role = MissionLoadRole::Authority;
	bool deferred = true;
	int create_error = 0;
	int gate_error = 0;
	int runtime_error = 0;
	bool streamed_world = true;
	bool device = true; // false: the session's legs alone (a headless host)

	MissionLoadLegs legs() {
		MissionLoadLegs out;
		MissionLoadSessionLegs &s = out.session;
		const auto leg = [this](const char *name) {
			return [this, name]() { trace.push_back(name); };
		};
		s.create_session = [this]() {
			trace.push_back("create_session");
			return create_error;
		};
		s.begin_host_load = leg("begin_host_load");
		s.open_host_load_window = leg("open_host_load_window");
		s.pump_host_load_stage = leg("pump_host_load_stage");
		s.mission_start_update = leg("mission_start_update");
		s.loading_step = leg("loading_step");
		s.start_runtime = [this]() {
			trace.push_back("start_runtime");
			return runtime_error;
		};
		s.role = [this]() { return role; };
		s.mission_start_deferred = [this]() { return deferred; };
		s.freeze_model_list = leg("freeze_model_list");
		s.reload_pool0_rows = leg("reload_pool0_rows");
		s.first_loadout = leg("first_loadout");
		s.settle_streamed_types = leg("settle_streamed_types");
		s.pump_render_arm = leg("pump_render_arm");
		s.mission_start_boundary = [this]() {
			trace.push_back("mission_start_boundary");
			return 0;
		};
		s.weapon_profile = leg("weapon_profile");
		s.play = leg("play");
		s.end_host_load = leg("end_host_load");
		if (!device)
			return out;

		MissionLoadDeviceLegs &d = out.device;
		d.present = [this](int percent) { trace.push_back("present " + std::to_string(percent)); };
		d.begin_timeline = leg("begin_timeline");
		d.span = [this](const char *name) { trace.push_back(std::string("[") + name); };
		d.end_span = leg("]");
		d.finish_timeline = leg("finish_timeline");
		d.copy_start_options = leg("copy_start_options");
		d.head_effect_system = leg("head_effect_system");
		d.terrain_gate = [this]() {
			trace.push_back("terrain_gate");
			return gate_error;
		};
		d.head_begin = leg("head_begin");
		d.environment = leg("environment");
		d.terrain = [this]() {
			trace.push_back("terrain");
			return 0;
		};
		d.avatar_models = leg("avatar_models");
		d.mission_root = [this](bool keep) {
			trace.push_back(keep ? "mission_root keep" : "mission_root");
		};
		d.place_objects = leg("place_objects");
		d.music_context = [this](bool open) {
			trace.push_back(open ? "music open" : "music closed");
		};
		d.runtime_started = leg("runtime_started");
		d.drop_created_session = leg("drop_created_session");
		d.unload_failed_runtime = leg("unload_failed_runtime");
		d.warm_model_definitions = leg("warm_model_definitions");
		d.mission_audio = leg("mission_audio");
		d.place_streamed_world = [this]() {
			trace.push_back("place_streamed_world");
			return streamed_world;
		};
		d.celestial_models = [this](bool dedicated) {
			trace.push_back(dedicated ? "celestial reduced" : "celestial full");
		};
		d.hud_overlay_init = leg("hud_overlay_init");
		d.start_effect_world = leg("start_effect_world");
		d.terrain_textures = [this](bool dedicated) {
			trace.push_back(dedicated ? "terrain_textures reduced" : "terrain_textures full");
		};
		d.sky_maps = [this](bool) { trace.push_back("sky_maps"); };
		d.resolve_model_materials = leg("resolve_model_materials");
		d.hud_textures = [this](bool full) {
			trace.push_back(full ? "hud_textures full" : "hud_textures reduced");
		};
		d.frame_fx_textures = [this](bool) { trace.push_back("frame_fx_textures"); };
		d.effects_warm = [this](bool spawn) {
			trace.push_back(spawn ? "effects_warm spawn" : "effects_warm");
		};
		d.spawn_loadout = leg("spawn_loadout");
		d.script_effects = leg("script_effects");
		d.world_ready = leg("world_ready");
		d.water_enabled = leg("water_enabled");
		d.world_loaded = leg("world_loaded");
		d.post_load_frames = leg("post_load_frames");
		return out;
	}

	std::vector<int> presents() const {
		std::vector<int> out;
		for (const std::string &row : trace)
			if (row.rfind("present ", 0) == 0)
				out.push_back(std::stoi(row.substr(8)));
		return out;
	}
};

bool expect_trace(const Trace &got, const Trace &want, const char *what) {
	if (check(got == want, what))
		return true;
	std::fprintf(stderr, "  got:\n");
	dump(got);
	std::fprintf(stderr, "  want:\n");
	dump(want);
	return false;
}

// The mission textures stage and the tail past the runtime on the full arm, a host's deferred
// mission start inside it.
const Trace kFullArmTail = {
	"present 60",
	"loading_step",
	"[effects",
	"start_effect_world",
	"]",
	"pump_render_arm",
	"present 70",
	"loading_step",
	"terrain_textures full",
	"[sky_maps",
	"sky_maps",
	"]",
	"[model_materials",
	"resolve_model_materials",
	"]",
	"[hud_textures",
	"hud_textures full",
	"]",
	"[frame_fx_textures",
	"frame_fx_textures",
	"]",
	"[effects_warm",
	"effects_warm spawn",
	"]",
};

const Trace kLoadEnd = {
	"[script_effects",
	"script_effects",
	"]",
	"[load_end",
	"end_host_load",
	"]",
	"[finish",
	"present 80",
	"loading_step",
	"]",
	"world_ready",
	"[water",
	"water_enabled",
	"]",
	"[world_loaded",
	"present 90",
	"world_loaded",
	"]",
	"finish_timeline",
	"post_load_frames",
};

Trace joined(std::initializer_list<Trace> parts) {
	Trace out;
	for (const Trace &part : parts)
		out.insert(out.end(), part.begin(), part.end());
	return out;
}

void fresh_authority_load() {
	Recorder r;
	MissionLoadArm arm;
	arm.fresh_authority = true;
	const MissionLoadLegs legs = r.legs();
	check(run_mission_load(arm, legs) == 0, "fresh session: the load runs");
	const Trace want = joined({
			{ "create_session", "begin_host_load", "mission_start_update", "begin_timeline",
					"copy_start_options", "[effect_system", "head_effect_system", "]",
					"open_host_load_window",
					// the head
					"terrain_gate", "begin_timeline", "head_begin", "present 10", "[environment",
					"environment", "]", "present 20", "[terrain", "terrain", "]",
					"pump_host_load_stage",
					// the objects
					"present 30", "loading_step", "[objects", "mission_root",
					"pump_host_load_stage", "pump_host_load_stage", "place_objects", "]",
					// the runtime: the mission start left to phase B past the device stages
					"present 40", "loading_step", "[runtime", "music open", "start_runtime",
					"runtime_started", "]", "freeze_model_list", "present 50", "loading_step",
					"[audio", "mission_audio", "]", "[celestial", "celestial full", "]",
					"[hud_init", "hud_overlay_init", "]" },
			kFullArmTail,
			{ "[mission_start", "mission_start_boundary", "weapon_profile", "spawn_loadout", "play",
					"]" },
			kLoadEnd,
	});
	expect_trace(r.trace, want, "fresh session: the load plan's order");
	// The loading screen at each real stage's start, then world ready (GUT's former
	// test_mission_load_reports_only_real_stage_checkpoints; the entry presents 0).
	check(r.presents() == std::vector<int>({ 10, 20, 30, 40, 50, 60, 70, 80, 90 }),
			"fresh session: the stage checkpoints in order");
}

// opennova-serve's legs: the session's alone, phase A in the runtime's start, the freeze ahead
// of the render arm's pump and phase B.
void headless_dedicated_host_load() {
	Recorder r;
	r.device = false;
	r.role = MissionLoadRole::DedicatedHost;
	MissionLoadArm arm;
	arm.dedicated = true;
	const MissionLoadLegs legs = r.legs();
	check(run_mission_load(arm, legs) == 0, "dedicated host: the load runs");
	const Trace want = {
		"begin_host_load",
		"mission_start_update",
		"open_host_load_window",
		"pump_host_load_stage",
		"loading_step",
		"pump_host_load_stage",
		"pump_host_load_stage",
		"loading_step",
		"start_runtime",
		"freeze_model_list",
		"loading_step",
		"loading_step",
		"pump_render_arm",
		"loading_step",
		"mission_start_boundary",
		"weapon_profile",
		"play",
		"end_host_load",
		"loading_step",
	};
	expect_trace(r.trace, want, "dedicated host: the session's legs in the load plan's order");
}

// A dedicated host's device arm: no effect system at the head, the music closed, the reduced
// arm's textures, no warm.
void dedicated_device_arm() {
	Recorder r;
	r.role = MissionLoadRole::DedicatedHost;
	MissionLoadArm arm;
	arm.dedicated = true;
	arm.fresh_authority = true;
	const MissionLoadLegs legs = r.legs();
	check(run_mission_load(arm, legs) == 0, "dedicated arm: the load runs");
	const auto has = [&](const char *row) {
		for (const std::string &t : r.trace)
			if (t == row)
				return true;
		return false;
	};
	check(!has("head_effect_system"), "dedicated arm: no effect system at the head");
	check(has("music closed") && !has("music open"), "dedicated arm: the music context closed");
	check(has("celestial reduced") && has("terrain_textures reduced") &&
					has("hud_textures reduced"),
			"dedicated arm: the reduced arm's stages");
	check(!has("effects_warm") && !has("effects_warm spawn"), "dedicated arm: no warm");
}

void joiner_head_and_load() {
	Recorder r;
	r.role = MissionLoadRole::Joiner;
	r.deferred = false;
	const MissionLoadLegs legs = r.legs();
	check(run_join_head_load(legs) == 0, "joiner: the S2C 0x11 head runs");
	expect_trace(r.trace,
			{ "copy_start_options", "loading_step", "terrain_gate", "begin_timeline", "head_begin",
					"present 10", "[environment", "environment", "]", "present 20", "[terrain",
					"terrain", "]", "loading_step", "[avatars", "avatar_models", "]",
					"finish_timeline" },
			"joiner: the S2C 0x11 head's order");

	r.trace.clear();
	MissionLoadArm arm;
	arm.wire_header_join = true;
	arm.head_loaded = true;
	arm.effects_preloaded = true;
	check(run_mission_load(arm, legs) == 0, "joiner: the S2C 0x1A load runs");
	const Trace want = joined({
			{ "begin_timeline", "present 30", "loading_step", "[objects", "mission_root keep",
					"place_objects", "]", "present 40", "loading_step", "[runtime", "music open",
					"start_runtime",
					// a joiner's mission start runs with its runtime's
					"mission_start_boundary", "weapon_profile", "spawn_loadout", "play",
					"runtime_started", "]", "present 50", "loading_step", "[audio", "mission_audio",
					"]", "[pool0_channels", "reload_pool0_rows", "]", "[streamed_world",
					"place_streamed_world", "]", "[celestial", "celestial full", "]", "[hud_init",
					"hud_overlay_init", "]", "[first_leg", "first_loadout", "]", "[model_list",
					"warm_model_definitions", "settle_streamed_types", "freeze_model_list", "]",
					"present 60", "loading_step", "[effects", "start_effect_world", "]",
					"pump_render_arm", "present 70", "loading_step", "terrain_textures full",
					"[sky_maps", "sky_maps", "]", "[model_materials", "resolve_model_materials",
					"]", "[hud_textures", "hud_textures full", "]", "[frame_fx_textures",
					"frame_fx_textures", "]", "[effects_warm", "effects_warm", "]" },
			kLoadEnd,
	});
	expect_trace(r.trace, want, "joiner: the S2C 0x1A load's order past the head");
}

void failures_stop_where_they_fail() {
	{
		Recorder r;
		r.create_error = 7;
		MissionLoadArm arm;
		arm.fresh_authority = true;
		check(run_mission_load(arm, r.legs()) == 7, "failed create: its error");
		expect_trace(
				r.trace, { "create_session", "finish_timeline" }, "failed create: nothing loads");
	}
	{
		Recorder r;
		r.gate_error = 5;
		MissionLoadArm arm;
		arm.fresh_authority = true;
		check(run_mission_load(arm, r.legs()) == 5, "failed gate: its error");
		check(r.trace.size() >= 3 && r.trace[r.trace.size() - 3] == "terrain_gate" &&
						r.trace[r.trace.size() - 2] == "finish_timeline" &&
						r.trace.back() == "drop_created_session",
				"failed gate: the timeline finished, the created session dropped");
	}
	{
		Recorder r;
		r.runtime_error = 3;
		MissionLoadArm arm;
		check(run_mission_load(arm, r.legs()) == 3, "failed runtime: its error");
		const Trace tail(r.trace.end() - 4, r.trace.end());
		expect_trace(tail, { "start_runtime", "]", "finish_timeline", "unload_failed_runtime" },
				"failed runtime: the span closed, the timeline finished, the world unloaded");
	}
}

} // namespace

int main() {
	fresh_authority_load();
	headless_dedicated_host_load();
	dedicated_device_arm();
	joiner_head_and_load();
	failures_stop_where_they_fail();
	if (failures == 0)
		std::printf("mission_load: all passed\n");
	return failures == 0 ? 0 : 1;
}
