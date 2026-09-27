// From-scratch authoring: no retail models, BADs, DEFs or imported metadata.
#include "../../apps/threedi_cli/weapon_timing.h"
#include <formats/def/def.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace threedi_cli;
namespace wa = opennova::world::weapon_action;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); ++failures; } } while (0)
WeaponTimingAction action(const char *role, const char *anim, double active = 0, double recovery = 0) {
	WeaponTimingAction a;
	std::strcpy(a.row.name, role);
	std::strcpy(a.row.anim, anim);
	a.active_seconds = active;
	a.recovery_seconds = recovery;
	return a;
}
std::string read(const std::filesystem::path &p) {
	std::ifstream f(p);
	return std::string(std::istreambuf_iterator<char>(f), {});
}
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) return 2;
	WeaponTimingRequest request;
	request.mode = "auto";
	request.cycle_seconds = 60.0 / 750;
	request.actions = {action("fire", "anim_wpn_fire"), action("reload", "anim_wpn_reload", 1.0)};
	WeaponTimingPlan plan;
	std::string error;
	CHECK(plan_weapon_timing(request, plan, error));
	CHECK(plan.cycle_ticks == 5 && plan.rows[0].delayend == 3);
	int shots = 0;
	for (const auto &e : plan.events) {
		if (e.kind == "shot") { CHECK(e.tick == shots * 5); ++shots; }
		if (e.scenario == wa::kReload && e.kind == "reload_ammo") CHECK(e.tick == 0);
	}
	CHECK(shots == 3);
	request.cycle_seconds = 8.0 / 62.5;
	CHECK(plan_weapon_timing(request, plan, error));
	CHECK(plan.cycle_ticks == 8 && plan.rows[0].delayend == 6);
	request.mode = "semi";
	CHECK(plan_weapon_timing(request, plan, error));
	CHECK(plan.cycle_ticks == 8 && plan.rows[0].delayend == 5);
	CHECK(plan.ready_tick == 7); // the next press is accepted the following tick
	request.mode = "burst";
	CHECK(plan_weapon_timing(request, plan, error));
	CHECK(plan.cycle_ticks == 8 && plan.rows[0].delayend == 6);
	shots = 0;
	for (const auto &e : plan.events) if (e.kind == "shot") ++shots;
	CHECK(shots == 3 && plan.ready_tick > 16);

	// A delayed Shot marker is placed against the visible clip pose, with
	// the native counter's non-advancing boundary tick accounted for.
	request.mode = "auto";
	request.actions[0].active_seconds = 0.1; // frame 3 at 30 fps
	request.cycle_seconds = 0.3;
	CHECK(plan_weapon_timing(request, plan, error));
	CHECK(plan.rows[0].delaystart == 7);
	bool found = false;
	for (const auto &e : plan.events) if (e.kind == "shot" && !found) {
		CHECK(e.tick == 7 && e.clip_ticks == 6);
		found = true;
	}
	CHECK(found);
	request.actions.push_back(action("switchto", "anim_wpn_switchto"));
	request.actions.push_back(action("switchfrom", "anim_wpn_switchfrom"));
	CHECK(plan_weapon_timing(request, plan, error));
	bool draw_ready = false, holster_complete = false;
	for (const auto &e : plan.events) {
		if (e.scenario == wa::kSwitchTo && e.kind == "ready") { CHECK(e.tick > 30); draw_ready = true; }
		if (e.scenario == wa::kSwitchFrom && e.kind == "switch_complete") { CHECK(e.clip_ticks > 0); holster_complete = true; }
	}
	CHECK(draw_ready && holster_complete);
	request.actions.back().active_seconds = 0.1;
	CHECK(!plan_weapon_timing(request, plan, error));
	request.actions.back().active_seconds = 0;
	request.cycle_seconds = 0.01;
	CHECK(!plan_weapon_timing(request, plan, error));
	request.cycle_seconds = 0.3;
	request.actions.push_back(request.actions[0]);
	CHECK(!plan_weapon_timing(request, plan, error));
	request.actions.pop_back();
	request.actions[0].active_seconds = std::numeric_limits<double>::quiet_NaN();
	CHECK(!plan_weapon_timing(request, plan, error));
	request.actions[0].active_seconds = 0.1;
	// OVERHEATED has no handler or anim state of its own to author.
	request.actions.push_back(action("overheated", "anim_wpn_idle"));
	CHECK(!plan_weapon_timing(request, plan, error));
	request.actions.pop_back();
	CHECK(plan_weapon_timing(request, plan, error));

	// The command emits actual parser-compatible ACTION rows, not a private
	// sidecar the runtime would need to learn to read.
	auto dir = std::filesystem::path(argv[1]) / "weapon-timing";
	std::filesystem::create_directories(dir);
	auto in = dir / "authored.txt", out = dir / "actions.txt";
	std::ofstream(in) << "weapon_timing 1\nmode auto\ncycle 0.08\n"
		"action fire anim_wpn_fire 0 0 \"\" GS_TEST Effect_Muzzle MFLASH01\n"
		"action reload anim_wpn_reload 1 0 \"\" \"\" \"\" \"\"\n";
	CHECK(cmd_weapon_timing(in.string().c_str(), out.string().c_str()) == 0);
	const auto text = read(out);
	const auto whole = "WEAPON \"WPN_AUTHORED\"\n" + text + "\nEND\n";
	opennova::def::DefWeaponsFile parsed{};
	CHECK(opennova::def::def_parse_weapons_memory(reinterpret_cast<const unsigned char *>(whole.data()), whole.size(), &parsed) == 0);
	CHECK(parsed.count == 1 && parsed.entries[0].actions_count == 3);
	if (parsed.count == 1 && parsed.entries[0].actions_count == 3) {
		auto &fire = parsed.entries[0].actions[0];
		CHECK(fire.delaystart == 0 && fire.delayend == 3);
		CHECK(std::strcmp(fire.soundsetend, "GS_TEST") == 0);
		CHECK(std::strcmp(fire.particleuserpoint, "MFLASH01") == 0);
	}
	opennova::def::def_free_weapons(&parsed);
	for (const char *bad : {
		"weapon_timing 1\nmode auto\ncycle nan\n",
		"weapon_timing 1\nmode auto\nmode semi\ncycle .1\n",
		"weapon_timing 1\nmode auto\ncycle .1\nunknown 2\n",
		"weapon_timing 1\nmode auto\ncycle .1\naction unknown key 0 0 x x x x\n",
		"weapon_timing 1\nmode auto\ncycle .1\naction fire key -1 0 x x x x\n"}) {
		std::ofstream(in) << bad;
		CHECK(cmd_weapon_timing(in.string().c_str(), out.string().c_str()) != 0);
		CHECK(read(out) == text); // failure preserves the previous output
	}
	return failures ? 1 : 0;
}
