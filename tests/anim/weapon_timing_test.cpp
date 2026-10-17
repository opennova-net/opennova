// weapon timing, from scratch: no retail models, BADs or imported metadata.
// The timing is measured by the engine's weapon FSM and written as the keys to
// set, and no others.
#include "../../apps/3di/weapon_timing.h"
#include "common/file_io.h"

#include <formats/def/def.h>
#include <formats/threedi/threedi_build.h>
#include <runtime/renderer/fp_viewmodel_spec.h>
#include <runtime/world/player_view.h>
#include <runtime/world/weapon_table_build.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace opennova::threedi_cli;
namespace wa = opennova::world::weapon_action;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); ++failures; } } while (0)

WeaponTimingAction action(int id, double active = 0, double recovery = 0) {
	WeaponTimingAction a;
	a.action = id;
	a.active_seconds = active;
	a.recovery_seconds = recovery;
	return a;
}

WeaponTimingEntry entry(const char *name, const char *mode, double cycle) { return {name, mode, cycle}; }

std::string read(const std::filesystem::path &p) { return test_io::read_file_text(p.string()); }

bool contains(const std::string &s, const char *what) { return s.find(what) != std::string::npos; }

const WeaponTimingRow *row(const WeaponTimingEntryPlan &plan, int id) {
	for (const auto &r : plan.rows)
		if (r.action == id) return &r;
	return nullptr;
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 2) return 2;
	WeaponTimingRequest request;
	request.actions = {action(wa::kFire), action(wa::kReload, 1.0)};
	request.entries = {entry("WPN_TEST", "auto", 60.0 / 750)};
	WeaponTimingPlan plan;
	std::string error;
	CHECK(plan_weapon_timing(request, plan, error));
	CHECK(plan.entries.size() == 1);
	// ANIM comes from the action's own slot; the recoil nobody authored is a
	// zero window whose ANIM the entry keeps.
	const auto &auto_plan = plan.entries[0];
	CHECK(auto_plan.cycle_ticks == 5 && row(auto_plan, wa::kFire)->delayend == 3);
	CHECK(row(auto_plan, wa::kFire)->anim == "anim_wpn_fire");
	CHECK(row(auto_plan, wa::kReload)->anim == "anim_wpn_reload" && row(auto_plan, wa::kReload)->delaystart == 63);
	CHECK(row(auto_plan, wa::kRecoil) && row(auto_plan, wa::kRecoil)->anim.empty() &&
	      row(auto_plan, wa::kRecoil)->delaystart == 0 && row(auto_plan, wa::kRecoil)->delayend == 0);
	int shots = 0;
	for (const auto &e : auto_plan.events) {
		if (e.kind == "shot") { CHECK(e.tick == shots * 5); ++shots; }
		if (e.scenario == wa::kReload && e.kind == "reload_ammo") CHECK(e.tick == 0);
	}
	CHECK(shots == 3);
	// The fire clip shows until the next shot restarts it: two ticks of pose
	// under a three-tick recovery (the counter-zero tick does not step it).
	bool fire_shown = false;
	for (const auto &s : auto_plan.shown)
		if (s.action == wa::kFire && s.scenario == wa::kFire) { CHECK(s.clip_ticks == 2); fire_shown = true; }
	CHECK(fire_shown);

	// One clip set, several entries: each is solved in its own mode.
	request.entries = {entry("WPN_TEST", "auto", 8.0 / 62.5), entry("WPN_TEST_SEMI", "semi", 8.0 / 62.5),
	                   entry("WPN_TEST_BURST", "burst", 8.0 / 62.5)};
	CHECK(plan_weapon_timing(request, plan, error));
	CHECK(plan.entries.size() == 3);
	if (plan.entries.size() == 3) {
		CHECK(plan.entries[0].cycle_ticks == 8 && row(plan.entries[0], wa::kFire)->delayend == 6);
		CHECK(plan.entries[1].cycle_ticks == 8 && row(plan.entries[1], wa::kFire)->delayend == 5);
		CHECK(plan.entries[1].ready_tick == 7); // the next press is accepted the following tick
		CHECK(plan.entries[2].cycle_ticks == 8 && row(plan.entries[2], wa::kFire)->delayend == 6);
		shots = 0;
		for (const auto &e : plan.entries[2].events) if (e.kind == "shot") ++shots;
		CHECK(shots == 3 && plan.entries[2].ready_tick > 16);
	}

	// A delayed Shot marker is placed against the visible clip pose, with
	// the native counter's non-advancing boundary tick accounted for.
	request.entries = {entry("WPN_TEST", "auto", 0.3)};
	request.actions[0].active_seconds = 0.1; // frame 3 at 30 fps
	CHECK(plan_weapon_timing(request, plan, error));
	CHECK(row(plan.entries[0], wa::kFire)->delaystart == 7);
	bool found = false;
	for (const auto &e : plan.entries[0].events) if (e.kind == "shot" && !found) {
		CHECK(e.tick == 7 && e.clip_ticks == 6);
		found = true;
	}
	CHECK(found);
	request.actions.push_back(action(wa::kSwitchTo));
	request.actions.push_back(action(wa::kSwitchFrom));
	CHECK(plan_weapon_timing(request, plan, error));
	bool draw_ready = false, holster_complete = false;
	for (const auto &e : plan.entries[0].events) {
		if (e.scenario == wa::kSwitchTo && e.kind == "ready") { CHECK(e.tick > 30); draw_ready = true; }
		if (e.scenario == wa::kSwitchFrom && e.kind == "switch_complete") { CHECK(e.clip_ticks > 0); holster_complete = true; }
	}
	CHECK(draw_ready && holster_complete);
	// A draw timed by its clip names the idle handler, which runs on its
	// delays: ready one second in, where the switch timer's draw is ~31 ticks.
	{
		auto timed = request;
		for (auto &a : timed.actions)
			if (a.action == wa::kSwitchTo) a.active_seconds = 1.0;
		WeaponTimingPlan p;
		CHECK(plan_weapon_timing(timed, p, error));
		const WeaponTimingRow *draw = row(p.entries[0], wa::kSwitchTo);
		CHECK(draw != nullptr && draw->delaystart == 63 && draw->function == "wpn_std_idle");
		int ready = -1;
		for (const auto &e : p.entries[0].events)
			if (e.scenario == wa::kSwitchTo && e.kind == "ready" && ready < 0) ready = e.tick;
		CHECK(ready >= 63 && ready <= 65);
		CHECK(contains(weapon_edits_text(p), "action switchto function wpn_std_idle\n"));
	}

	// Refusals, each with the reason an author can act on.
	const auto refused = [&](const WeaponTimingRequest &r, const char *why) {
		WeaponTimingPlan p;
		std::string e;
		const bool ok = plan_weapon_timing(r, p, e);
		if (ok || !contains(e, why)) std::fprintf(stderr, "  refusal '%s' gave '%s'\n", why, e.c_str());
		return !ok && contains(e, why);
	};
	auto bad = request;
	bad.actions.back().active_seconds = 0.1;
	CHECK(refused(bad, "fixed switch timer"));
	bad = request;
	bad.entries[0].cycle_seconds = 0.01; // one tick: past the FSM's two
	CHECK(refused(bad, "at most every 2 ticks (1875 rounds a minute)"));
	bad.entries[0].mode = "semi";
	CHECK(refused(bad, "at most every 3 ticks (1250 rounds a minute)"));
	bad = request;
	bad.entries[0].cycle_seconds = 0.05; // 3 ticks, under the 7-tick Shot marker
	CHECK(refused(bad, "need at least"));
	bad = request;
	bad.actions.push_back(bad.actions[0]);
	CHECK(refused(bad, "twice"));
	bad = request;
	bad.actions[0].active_seconds = std::numeric_limits<double>::quiet_NaN();
	CHECK(refused(bad, "finite"));
	bad = request;
	bad.actions.push_back(action(wa::kOverheated));
	CHECK(refused(bad, "no anim slot"));
	bad = request;
	bad.actions[0].recovery_seconds = 0.1;
	CHECK(refused(bad, "shot period"));
	bad = request;
	bad.entries.clear();
	CHECK(refused(bad, "at least one weapon.def entry"));
	bad = request;
	bad.entries.push_back(entry("wpn_test", "semi", 0.3));
	CHECK(refused(bad, "named twice"));
	bad = request;
	bad.entries[0].name = "WPN_A_NAME_LONGER_THAN_THIRTY_ONE";
	CHECK(refused(bad, "1 to 31"));
	bad = request;
	bad.entries[0].mode = "fast";
	CHECK(refused(bad, "semi, auto or burst"));
	// Auto fire re-arms in the recoil's recovery: an active recoil with none
	// fires once a press.
	bad = request;
	bad.actions.push_back(action(wa::kRecoil, 0.05, 0.0));
	CHECK(refused(bad, "needs a recovery of at least one tick"));
	bad.entries[0].mode = "semi"; // semi presses again from idle
	CHECK(plan_weapon_timing(bad, plan, error));

	// The eyes: pos = -eye * 256 in the view frame, which is the model's
	// mission axes. Through the engine's own mapping the eye lands on the
	// camera: the model point at the eye plus the def's view offset is the
	// camera origin, and the rig carries a model point onto the camera
	// exactly as the view offset is carried.
	request.pos.given = true;
	request.pos.metres[0] = -0.05;
	request.pos.metres[1] = 0.0;
	request.pos.metres[2] = 0.2;
	request.tpos.given = true;
	request.tpos.metres[0] = 0.1;
	request.tpos.metres[1] = -0.02;
	request.tpos.metres[2] = 0.15;
	CHECK(plan_weapon_timing(request, plan, error));
	CHECK(plan.pos_given && plan.tpos_given);
	CHECK(plan.pos_units[0] == 12.8 && plan.pos_units[1] == 0.0 && plan.pos_units[2] == -51.2);
	CHECK(plan.tpos_units[0] == -25.6 && plan.tpos_units[1] == 5.12 && plan.tpos_units[2] == -38.4);
	{
		opennova::world::PlayerViewState view{};
		const float pos[3] = {static_cast<float>(plan.pos_units[0]), static_cast<float>(plan.pos_units[1]),
		                      static_cast<float>(plan.pos_units[2])};
		float offset[3];
		opennova::world::player_view_bias_view_units(view, false, pos, offset);
		const float eye_plus_offset[3] = {static_cast<float>(request.pos.metres[0]) + offset[0],
		                                  static_cast<float>(request.pos.metres[1]) + offset[1],
		                                  static_cast<float>(request.pos.metres[2]) + offset[2]};
		float camera[3];
		opennova::renderer::viewmodel_camera_local_from_view(eye_plus_offset, camera);
		CHECK(std::fabs(camera[0]) < 1e-6f && std::fabs(camera[1]) < 1e-6f && std::fabs(camera[2]) < 1e-6f);
		// The rig: mission -> presentation, then its yaw about up.
		const double m[3] = {0.3, -0.7, 1.1};
		const auto p = opennova::threedi::threedi_mission_to_presentation({m[0], m[1], m[2]});
		const double yaw = opennova::renderer::kViewmodelRigYawDeg * 3.14159265358979323846 / 180.0;
		const double rig[3] = {p.x * std::cos(yaw) + p.z * std::sin(yaw), p.y,
		                       -p.x * std::sin(yaw) + p.z * std::cos(yaw)};
		const float view_point[3] = {static_cast<float>(m[0]), static_cast<float>(m[1]), static_cast<float>(m[2])};
		float mapped[3];
		opennova::renderer::viewmodel_camera_local_from_view(view_point, mapped);
		for (int i = 0; i < 3; ++i) CHECK(std::fabs(rig[i] - mapped[i]) < 1e-6);
	}

	// The command: a request in, the edits file out, the JSON on stdout.
	auto dir = std::filesystem::path(argv[1]) / "weapon-timing";
	std::filesystem::create_directories(dir);
	auto in = dir / "authored.txt", out = dir / "edits.txt";
	std::ofstream(in) << "weapon_timing 2\n"
		"# the Blender add-on's request\n"
		"action fire 0 0\n"
		"action reload 1 0\n"
		"action recoil 0 0.016\n"
		"entry WPN_AUTHORED auto 0.08\n"
		"entry WPN_AUTHORED_SEMI semi 0.08\n"
		"view pos -0.05 0 0.2\n";
	CHECK(cmd_weapon_timing(in.string().c_str(), out.string().c_str()) == 0);
	const auto text = read(out);
	CHECK(text.rfind("weapon_edits 1\n", 0) == 0);
	CHECK(contains(text, "\nentry WPN_AUTHORED auto\n") && contains(text, "\nentry WPN_AUTHORED_SEMI semi\n"));
	CHECK(contains(text, "\npos 12.8 0 -51.2\n") && !contains(text, "\ntpos "));
	// Only ANIM and the delays, never FUNCTION, sounds or effects.
	for (size_t at = text.find("\naction "); at != std::string::npos; at = text.find("\naction ", at + 1)) {
		const size_t key = text.find(' ', text.find(' ', at + 1) + 1) + 1;
		const std::string name = text.substr(key, text.find(' ', key) - key);
		CHECK(name == "anim" || name == "delaystart" || name == "delayend");
	}
	CHECK(!contains(text, "function") && !contains(text, "sound") && !contains(text, "particle"));
	CHECK(contains(text, "action fire anim anim_wpn_fire\n") && contains(text, "action recoil anim anim_wpn_recoil\n"));
	for (const char *broken : {
		"weapon_timing 1\naction fire 0 0\nentry WPN_X auto 0.1\n",
		"weapon_timing 2\naction fire 0 0\n",
		"weapon_timing 2\naction fire 0 0\nentry WPN_X auto nan\n",
		"weapon_timing 2\naction fire 0 0\nentry WPN_X auto 0.1\nunknown 2\n",
		"weapon_timing 2\naction unknown 0 0\nentry WPN_X auto 0.1\n",
		"weapon_timing 2\naction fire -1 0\nentry WPN_X auto 0.1\n",
		"weapon_timing 2\naction fire 0 0\nentry WPN_X auto 0.1\nview pos 0 0 0\nview pos 0 0 1\n",
		"weapon_timing 2\naction fire 0 0\nentry WPN_X auto 0.1\nview eye 0 0 0\n"}) {
		std::ofstream(in) << broken;
		CHECK(cmd_weapon_timing(in.string().c_str(), out.string().c_str()) != 0);
		CHECK(read(out) == text); // failure preserves the previous output
	}

	// `ammoclass_max_carry` reads its class and abs(atol) of its value from the
	// tokens, so the comma form and the quoted key both bind and a negative cap
	// is its magnitude. [orig: WeaponDefs_ParseLineCallback @ 0x5437F2, tokens[2]
	// @ 0x5437FE, abs(atol(tokens[3])) @ 0x543862..0x543873]
	{
		const std::string carry = "ammoclass_max_carry,CLASS_TESTX,-300\r\n"
		                          "\"ammoclass_max_carry\" CLASS_TESTY 40\r\n"
		                          "weapon \"WPN_CARRY\"\r\nend\r\n";
		opennova::def::DefWeaponsFile parsed{};
		CHECK(opennova::def::def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(carry.data()), carry.size(),
		                                              &parsed) == 0);
		CHECK(parsed.ammo_classes_count == 2);
		if (parsed.ammo_classes_count == 2) {
			CHECK(std::strcmp(parsed.ammo_classes[0].name, "CLASS_TESTX") == 0);
			CHECK(parsed.ammo_classes[0].max_carry == 300);
			CHECK(std::strcmp(parsed.ammo_classes[1].name, "CLASS_TESTY") == 0);
			CHECK(parsed.ammo_classes[1].max_carry == 40);
		}
		const opennova::world::WeaponTable table = opennova::world::build_weapon_table(parsed);
		const int x = table.ammo_class_id_of("CLASS_TESTX");
		const int y = table.ammo_class_id_of("CLASS_TESTY");
		CHECK(x > 0 && table.ammo_class_caps[static_cast<size_t>(x)] == 300);
		CHECK(y > 0 && table.ammo_class_caps[static_cast<size_t>(y)] == 40);
		opennova::def::def_free_weapons(&parsed);
	}
	return failures ? 1 : 0;
}
