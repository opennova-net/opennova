// weapon timing and weapon merge, from scratch: no retail models, BADs or
// imported metadata. The timing is measured by the engine's weapon FSM; the
// merge sets only the keys an edits file names, in a def the parser reads.
#include "../../apps/3di/weapon_timing.h"
#include "common/file_io.h"
#include "common/retail_paths.h"

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

// The lines of `text` split on '\n', each with its '\r'.
std::vector<std::string> lines_of(const std::string &text) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at < text.size()) {
		const size_t nl = text.find('\n', at);
		out.push_back(text.substr(at, nl == std::string::npos ? std::string::npos : nl + 1 - at));
		at = nl == std::string::npos ? text.size() : nl + 1;
	}
	return out;
}

const opennova::def::DefWeaponAction *def_action(const opennova::def::DefWeaponDef &w, const char *name) {
	for (size_t i = 0; i < w.actions_count; ++i)
		if (std::strcmp(w.actions[i].name, name) == 0) return &w.actions[i];
	return nullptr;
}
} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
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
	std::vector<WeaponEditEntry> edits;
	CHECK(parse_weapon_edits(text, edits, error));
	CHECK(edits.size() == 2);
	if (edits.size() == 2) {
		CHECK(edits[0].name == "WPN_AUTHORED" && edits[0].mode == "auto");
		CHECK(edits[0].pos_given && edits[0].pos[0] == "12.8" && edits[0].pos[1] == "0" && edits[0].pos[2] == "-51.2");
		CHECK(!edits[0].tpos_given);
		// Only ANIM and the delays, never FUNCTION, sounds or effects.
		for (const auto &k : edits[0].keys) CHECK(k.key == "anim" || k.key == "delaystart" || k.key == "delayend");
		CHECK(!contains(text, "function") && !contains(text, "sound") && !contains(text, "particle"));
		CHECK(contains(text, "action fire anim anim_wpn_fire\n") && contains(text, "action recoil anim anim_wpn_recoil\n"));
		CHECK(contains(text, "entry WPN_AUTHORED_SEMI semi\n"));
	}
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

	// weapon merge: a def in two styles, CRLF lines and the NUL tail retail's
	// file carries. WPN_AUTHORED quotes its blocks and keys in upper case,
	// names a FUNCTION the edits never touch, writes delayend as `delay`,
	// lacks a DELAYSTART and a RECOIL block; WPN_AUTHORED_SEMI opens a bare
	// `action reload` in lower case and carries no pos at all. WPN_OTHER's
	// RELOAD block writes its key with a comma and closes on `END // x`: the
	// retail tokenizer cuts both, so the parser binds the value and closes the
	// block [orig: Terrain_TokenizeConfigLine @ 0x53CB60; the whole-token
	// stricmp on "end" in ActionDef_ParseScriptLine @ 0x40251B].
	const std::string def =
		"// merge fixture\r\n"
		"weapon \"WPN_OTHER\"\r\n"
		"\tFLAGS\tAUTO\r\n"
		"\tACTION\t\"FIRE\"\r\n"
		"\tDELAYEND\t9\r\n"
		"\tEND\r\n"
		"\tACTION\t\"RELOAD\"\r\n"
		"\tDELAYEND,7\r\n"
		"\tEND // reload\r\n"
		"end\r\n"
		"\r\n"
		"weapon \"WPN_AUTHORED\"\r\n"
		"\tFLAGS\tAUTO\r\n"
		"\tFLAGS\tSIGHTED\r\n"
		"\tSIGHTS\tIRONS.TGA\t0\t0\t1024\t768\tblend\r\n"
		"\tpos\t\t10.0\t\t0.0\t\t-201.0\t\t0.0\t\t0.0\t\t1.0 // hip\r\n"
		"\r\n"
		"\tACTION\t\"FIRE\"\r\n"
		"\tDELAY\t\t\t5 \r\n"
		"\tSOUNDSETEND\t\tGS_AK47\r\n"
		"\tANIM\t\t\t\tANIM_WPN_FIRE\r\n"
		"\tFUNCTION\t\tWPN_STD_FIRE\r\n"
		"\tPARTICLE\t\tEffect_556cas_REV\r\n"
		"\tEND\r\n"
		"\r\n"
		"\tACTION\t\"RELOAD\"\r\n"
		"\tDELAYSTART\t\t196\r\n"
		"\tDELAYEND\t\tauto\r\n"
		"\tANIM\t\t\t\tANIM_WPN_RELOAD\r\n"
		"\tFUNCTION\t\tWPN_STD_RELOAD\r\n"
		"\tEND\r\n"
		"end\r\n"
		"\r\n"
		"weapon \"WPN_AUTHORED_SEMI\"\r\n"
		"\tflags sighted\r\n"
		"\taction reload\r\n"
		"\t\tdelayend auto\r\n"
		"\t\tfunction wpn_std_reload\r\n"
		"\tend\r\n"
		"end\r\n"
		"\r\n"
		"ammoclass_max_carry CLASS_AK47 300\r\n"
		"\r\n" + std::string(1, '\0');
	std::string merged;
	std::vector<std::string> notes;
	CHECK(merge_weapon_def(def, edits, merged, notes, error));
	if (!error.empty()) std::fprintf(stderr, "  merge: %s\n", error.c_str());
	// Exactly these bytes: the values the edits name rewritten in place (the
	// spacing and the trailing comment around them kept), a key a block
	// lacks added before its END in the block's own spelling, and a missing
	// block after the entry's last one. Five ticks a shot is a two-tick
	// fire recovery auto (shot, recovery, recoil, its one-tick recovery, the
	// deferred refire) and one tick semi (the press waits for idle).
	const std::string want =
		"// merge fixture\r\n"
		"weapon \"WPN_OTHER\"\r\n"
		"\tFLAGS\tAUTO\r\n"
		"\tACTION\t\"FIRE\"\r\n"
		"\tDELAYEND\t9\r\n"
		"\tEND\r\n"
		"\tACTION\t\"RELOAD\"\r\n"
		"\tDELAYEND,7\r\n"
		"\tEND // reload\r\n"
		"end\r\n"
		"\r\n"
		"weapon \"WPN_AUTHORED\"\r\n"
		"\tFLAGS\tAUTO\r\n"
		"\tFLAGS\tSIGHTED\r\n"
		"\tSIGHTS\tIRONS.TGA\t0\t0\t1024\t768\tblend\r\n"
		"\tpos\t\t12.8\t\t0\t\t-51.2\t\t0.0\t\t0.0\t\t1.0 // hip\r\n"
		"\r\n"
		"\tACTION\t\"FIRE\"\r\n"
		"\tDELAY\t\t\t2 \r\n"
		"\tSOUNDSETEND\t\tGS_AK47\r\n"
		"\tANIM\t\t\t\tanim_wpn_fire\r\n"
		"\tFUNCTION\t\tWPN_STD_FIRE\r\n"
		"\tPARTICLE\t\tEffect_556cas_REV\r\n"
		"\tDELAYSTART\t0\r\n"
		"\tEND\r\n"
		"\r\n"
		"\tACTION\t\"RELOAD\"\r\n"
		"\tDELAYSTART\t\t63\r\n"
		"\tDELAYEND\t\t0\r\n"
		"\tANIM\t\t\t\tanim_wpn_reload\r\n"
		"\tFUNCTION\t\tWPN_STD_RELOAD\r\n"
		"\tEND\r\n"
		"\r\n"
		"\tACTION\t\"RECOIL\"\r\n"
		"\tANIM\tanim_wpn_recoil\r\n"
		"\tDELAYSTART\t0\r\n"
		"\tDELAYEND\t1\r\n"
		"\tEND\r\n"
		"end\r\n"
		"\r\n"
		"weapon \"WPN_AUTHORED_SEMI\"\r\n"
		"\tflags sighted\r\n"
		"\tpos\t12.8\t0\t-51.2\t0\t0\t0\r\n"
		"\taction reload\r\n"
		"\t\tdelayend 0\r\n"
		"\t\tfunction wpn_std_reload\r\n"
		"\t\tanim\tanim_wpn_reload\r\n"
		"\t\tdelaystart\t63\r\n"
		"\tend\r\n"
		"\r\n"
		"\taction\t\"fire\"\r\n"
		"\t\tanim\tanim_wpn_fire\r\n"
		"\t\tdelaystart\t0\r\n"
		"\t\tdelayend\t1\r\n"
		"\tend\r\n"
		"\r\n"
		"\taction\t\"recoil\"\r\n"
		"\t\tanim\tanim_wpn_recoil\r\n"
		"\t\tdelaystart\t0\r\n"
		"\t\tdelayend\t1\r\n"
		"\tend\r\n"
		"end\r\n"
		"\r\n"
		"ammoclass_max_carry CLASS_AK47 300\r\n"
		"\r\n" + std::string(1, '\0');
	CHECK(merged == want);
	if (merged != want) {
		const auto got = lines_of(merged), expected = lines_of(want);
		for (size_t i = 0; i < std::max(got.size(), expected.size()); ++i) {
			const std::string g = i < got.size() ? got[i] : "<none>", w = i < expected.size() ? expected[i] : "<none>";
			if (g != w) std::fprintf(stderr, "  line %zu: got [%s] want [%s]\n", i + 1, g.c_str(), w.c_str());
		}
	}
	{
		opennova::def::DefWeaponsFile parsed{};
		CHECK(opennova::def::def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(merged.data()), merged.size(),
		                                              &parsed) == 0);
		CHECK(parsed.count == 3);
		if (parsed.count == 3) {
			const auto &other = parsed.entries[0];
			const auto &authored = parsed.entries[1];
			const auto &semi = parsed.entries[2];
			CHECK(def_action(other, "FIRE") && def_action(other, "FIRE")->delayend == 9);
			// `DELAYEND,7` binds and `END // reload` closes the block.
			CHECK(def_action(other, "RELOAD") && def_action(other, "RELOAD")->delayend == 7);
			CHECK(other.actions_count == 2);
			const auto *fire = def_action(authored, "FIRE");
			CHECK(fire && fire->delaystart == 0 && fire->delayend == 2 && std::strcmp(fire->anim, "anim_wpn_fire") == 0);
			CHECK(fire && std::strcmp(fire->function, "WPN_STD_FIRE") == 0 && std::strcmp(fire->soundsetend, "GS_AK47") == 0 &&
			      std::strcmp(fire->particle, "Effect_556cas_REV") == 0);
			const auto *reload = def_action(authored, "RELOAD");
			CHECK(reload && reload->delaystart == 63 && reload->delayend == 0 && std::strcmp(reload->function, "WPN_STD_RELOAD") == 0);
			const auto *recoil = def_action(authored, "RECOIL");
			CHECK(recoil && recoil->delaystart == 0 && recoil->delayend == 1 && std::strcmp(recoil->anim, "anim_wpn_recoil") == 0 &&
			      recoil->function[0] == '\0');
			CHECK(authored.pos[0] == 12.8f && authored.pos[1] == 0.0f && authored.pos[2] == -51.2f);
			CHECK(authored.pos_rotation_deg_q16[2] == 0x10000); // the cant is kept
			const auto *semi_reload = def_action(semi, "reload");
			CHECK(semi_reload && semi_reload->delaystart == 63 && semi_reload->delayend == 0 &&
			      std::strcmp(semi_reload->anim, "anim_wpn_reload") == 0);
			CHECK(def_action(semi, "fire") && def_action(semi, "fire")->delayend == 1 && def_action(semi, "recoil"));
			CHECK(semi.pos[0] == 12.8f && semi.pos[2] == -51.2f);
		}
		opennova::def::def_free_weapons(&parsed);
	}
	// The SIGHTS card note rides a tpos edit on a card-drawing entry.
	CHECK(notes.empty());
	edits[0].tpos_given = true;
	edits[0].tpos[0] = "-25.6";
	edits[0].tpos[1] = "5.12";
	edits[0].tpos[2] = "-38.4";
	CHECK(merge_weapon_def(def, edits, merged, notes, error));
	CHECK(notes.size() == 1 && contains(notes[0], "WPN_AUTHORED") && contains(notes[0], "IRONS.TGA"));
	// Of repeated blocks only the last is live (each re-initializes the one
	// row of its name): the merge sets the keys there and adds no block.
	{
		std::vector<WeaponEditEntry> twice_edits;
		CHECK(parse_weapon_edits("weapon_edits 1\nentry WPN_TWICE auto\naction fire delayend 3\n"
		                         "action fire delaystart 1\n", twice_edits, error));
		const std::string twice = "weapon \"WPN_TWICE\"\r\n\tFLAGS\tAUTO\r\n"
		                          "\tACTION\t\"FIRE\"\r\n\tDELAYEND\t5\r\n\tEND\r\n"
		                          "\tACTION\t\"FIRE\"\r\n\tDELAYEND\t7\r\n\tANIM\tANIM_WPN_FIRE\r\n"
		                          "\tFUNCTION\tWPN_STD_EMPTY\r\n\tEND\r\nend\r\n";
		CHECK(merge_weapon_def(twice, twice_edits, merged, notes, error));
		CHECK(merged == "weapon \"WPN_TWICE\"\r\n\tFLAGS\tAUTO\r\n"
		                "\tACTION\t\"FIRE\"\r\n\tDELAYEND\t5\r\n\tEND\r\n"
		                "\tACTION\t\"FIRE\"\r\n\tDELAYEND\t3\r\n\tANIM\tANIM_WPN_FIRE\r\n"
		                "\tFUNCTION\tWPN_STD_EMPTY\r\n\tDELAYSTART\t1\r\n\tEND\r\nend\r\n");
		// The kept FUNCTION binds another handler than the timing ran.
		CHECK(notes.size() == 1 && contains(notes[0], "FIRE block keeps FUNCTION wpn_std_empty"));
	}
	// A comment that cuts a value's token stays after the new value, and a
	// pos line short of six values gets its zeros ahead of its comment.
	{
		std::vector<WeaponEditEntry> cut_edits;
		CHECK(parse_weapon_edits("weapon_edits 1\nentry WPN_CUT auto\npos 1 2 3\naction fire delayend 3\n",
		                         cut_edits, error));
		const std::string cut = "weapon \"WPN_CUT\"\r\n\tFLAGS\tAUTO\r\n\tpos 10 20 30// hip\r\n"
		                        "\tACTION\t\"FIRE\"\r\n\tDELAYEND\t5// recoil\r\n\tEND\r\nend\r\n";
		CHECK(merge_weapon_def(cut, cut_edits, merged, notes, error));
		CHECK(merged == "weapon \"WPN_CUT\"\r\n\tFLAGS\tAUTO\r\n\tpos 1 2 3\t0\t0\t0// hip\r\n"
		                "\tACTION\t\"FIRE\"\r\n\tDELAYEND\t3// recoil\r\n\tEND\r\nend\r\n");
		// The game ends lines only at CR LF, so a def with a lone LF is refused.
		std::string lf;
		for (char c : cut)
			if (c != '\r') lf += c;
		CHECK(!merge_weapon_def(lf, cut_edits, merged, notes, error) && contains(error, "CR LF"));
		// An END that carries a comment still closes the entry (the retail
		// tokenizer cuts the comment before the key compare), so the merge
		// reads it back and keeps the comment on its line.
		std::string commented_end = cut;
		commented_end.replace(commented_end.rfind("end\r\n"), 5, "end // WPN_CUT\r\n");
		CHECK(merge_weapon_def(commented_end, cut_edits, merged, notes, error));
		CHECK(merged == "weapon \"WPN_CUT\"\r\n\tFLAGS\tAUTO\r\n\tpos 1 2 3\t0\t0\t0// hip\r\n"
		                "\tACTION\t\"FIRE\"\r\n\tDELAYEND\t3// recoil\r\n\tEND\r\nend // WPN_CUT\r\n");
	}
	// A final `end` with no CR LF: the game's tail leg drops the line's last
	// byte, so it reads `en` and never closes the entry, which the merge
	// refuses; a trailing byte after the `end` (here a space) is what the tail
	// leg drops, and that entry closes. [orig: File_ParseASCIIFile @ 0x53D8E9 /
	// @ 0x53D8EC]
	{
		std::vector<WeaponEditEntry> tail_edits;
		CHECK(parse_weapon_edits("weapon_edits 1\nentry WPN_TAIL auto\naction fire delayend 3\n", tail_edits, error));
		const std::string tail = "weapon \"WPN_TAIL\"\r\n\tFLAGS\tAUTO\r\n\tACTION\t\"FIRE\"\r\n\tDELAYEND\t5\r\n"
		                         "\tEND\r\nend";
		CHECK(!merge_weapon_def(tail, tail_edits, merged, notes, error) && contains(error, "WPN_TAIL has no `end`"));
		CHECK(merge_weapon_def(tail + " ", tail_edits, merged, notes, error));
		CHECK(merged == "weapon \"WPN_TAIL\"\r\n\tFLAGS\tAUTO\r\n\tACTION\t\"FIRE\"\r\n\tDELAYEND\t3\r\n"
		                "\tEND\r\nend ");
	}
	// An entry the text never closes is an entry with no `end`, not a missing
	// one: the game allocates its slot at the `weapon` line.
	// [orig: WeaponDefs_ParseLineCallback @ 0x5436D3..0x543737]
	{
		std::vector<WeaponEditEntry> open_edits;
		CHECK(parse_weapon_edits("weapon_edits 1\nentry WPN_OPEN auto\naction fire delayend 3\n", open_edits, error));
		const std::string open = "weapon \"WPN_OPEN\"\r\n\tFLAGS\tAUTO\r\n\tACTION\t\"FIRE\"\r\n\tEND\r\n";
		CHECK(!merge_weapon_def(open, open_edits, merged, notes, error) && contains(error, "WPN_OPEN has no `end`"));
		// A `weapon` line inside that open entry ends the game's walk, so the
		// entry it names is none of the game's. [orig: WeaponDefs_ParseLineCallback
		// @ 0x5436AD..0x5436D2]
		std::vector<WeaponEditEntry> next_edits;
		CHECK(parse_weapon_edits("weapon_edits 1\nentry WPN_NEXT auto\naction fire delayend 3\n", next_edits, error));
		const std::string next = open + "weapon \"WPN_NEXT\"\r\n\tFLAGS\tAUTO\r\nend\r\n";
		CHECK(!merge_weapon_def(next, next_edits, merged, notes, error) &&
		      contains(error, "stops reading the weapon.def at line 5"));
	}
	// A pos line inside a dead ACTION block (an earlier block of a name a later
	// one replaces) is forwarded to the action parser, which reads no pos, so
	// it is no view line: the merge leaves it and adds the entry's own pos
	// ahead of its first ACTION line. [orig: WeaponDefs_ParseLineCallback, the
	// in-block forward @ 0x54388D, the block flag @ 0x54393B / @ 0x543790]
	{
		std::vector<WeaponEditEntry> dead_edits;
		CHECK(parse_weapon_edits("weapon_edits 1\nentry WPN_DEAD auto\npos 12.8 0 -51.2\naction fire delayend 3\n",
		                         dead_edits, error));
		const std::string dead = "weapon \"WPN_DEAD\"\r\n\tFLAGS\tAUTO\r\n"
		                         "\tACTION\t\"FIRE\"\r\n\tpos\t1\t2\t3\t0\t0\t0\r\n\tDELAYEND\t5\r\n\tEND\r\n"
		                         "\tACTION\t\"FIRE\"\r\n\tDELAYEND\t7\r\n\tEND\r\nend\r\n";
		CHECK(merge_weapon_def(dead, dead_edits, merged, notes, error));
		CHECK(merged == "weapon \"WPN_DEAD\"\r\n\tFLAGS\tAUTO\r\n\tpos\t12.8\t0\t-51.2\t0\t0\t0\r\n"
		                "\tACTION\t\"FIRE\"\r\n\tpos\t1\t2\t3\t0\t0\t0\r\n\tDELAYEND\t5\r\n\tEND\r\n"
		                "\tACTION\t\"FIRE\"\r\n\tDELAYEND\t3\r\n\tEND\r\nend\r\n");
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
		CHECK(parsed.ammo_class_carries_count == 2);
		if (parsed.ammo_class_carries_count == 2) {
			CHECK(std::strcmp(parsed.ammo_class_carries[0].name, "CLASS_TESTX") == 0);
			CHECK(parsed.ammo_class_carries[0].cap == 300);
			CHECK(std::strcmp(parsed.ammo_class_carries[1].name, "CLASS_TESTY") == 0);
			CHECK(parsed.ammo_class_carries[1].cap == 40);
		}
		const opennova::world::WeaponTable table = opennova::world::build_weapon_table(parsed);
		const int x = table.ammo_class_id_of("CLASS_TESTX");
		const int y = table.ammo_class_id_of("CLASS_TESTY");
		CHECK(x > 0 && table.ammo_class_caps[static_cast<size_t>(x)] == 300);
		CHECK(y > 0 && table.ammo_class_caps[static_cast<size_t>(y)] == 40);
		opennova::def::def_free_weapons(&parsed);
	}
	// Refusals: an entry the def lacks, a mode its FLAGS contradict, an
	// encrypted def, and edits that do not read.
	auto wrong = edits;
	wrong[1].name = "WPN_MISSING";
	CHECK(!merge_weapon_def(def, wrong, merged, notes, error) && contains(error, "no entry WPN_MISSING") && merged.empty());
	wrong = edits;
	wrong[0].mode = "semi";
	CHECK(!merge_weapon_def(def, wrong, merged, notes, error) && contains(error, "fires auto"));
	CHECK(!merge_weapon_def(std::string("SCR\x01", 4) + def, edits, merged, notes, error) && contains(error, "encrypted"));
	for (const char *broken : {
		"weapon_edits 2\nentry WPN_X auto\n",
		"weapon_edits 1\npos 1 2 3\n",
		"weapon_edits 1\nentry WPN_X auto\naction fire function wpn_std_idle\n",
		"weapon_edits 1\nentry WPN_X auto\naction fire delayend -1\n",
		"weapon_edits 1\nentry WPN_X auto\naction fire delayend 99999999999\n",
		"weapon_edits 1\nentry WPN_X auto\naction fire delayend 2\naction fire delayend 3\n",
		"weapon_edits 1\nentry WPN_X auto\nentry wpn_x semi\n"}) {
		std::vector<WeaponEditEntry> e;
		CHECK(!parse_weapon_edits(broken, e, error));
	}

	// The command end to end, and its refusal leaves the output alone.
	const auto def_path = dir / "weapon.def", merged_path = dir / "merged.def";
	std::ofstream(def_path, std::ios::binary) << def;
	std::filesystem::remove(merged_path);
	CHECK(cmd_weapon_merge(def_path.string().c_str(), out.string().c_str(), merged_path.string().c_str()) == 0);
	CHECK(std::filesystem::exists(merged_path));
	const auto missing_edits = dir / "missing.txt";
	std::ofstream(missing_edits) << "weapon_edits 1\nentry WPN_MISSING auto\naction fire delayend 2\n";
	const std::string kept = read(merged_path);
	CHECK(cmd_weapon_merge(def_path.string().c_str(), missing_edits.string().c_str(), merged_path.string().c_str()) != 0);
	CHECK(read(merged_path) == kept);

	// The shipped weapon.def: the AK pair shares one clip set, and a merge
	// touches only the lines of their blocks it names.
	const std::string shipped = retail::reference_fixture("def/weapon.def");
	if (shipped.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS fixtures/def/weapon.def (the shipped weapon.def merge)");
	} else {
		const std::string retail_def = read(shipped);
		std::vector<WeaponEditEntry> ak;
		CHECK(parse_weapon_edits("weapon_edits 1\nentry WPN_AK47AUTO auto\naction fire delayend 4\n"
		                         "action reload delaystart 200\nentry WPN_AK47 semi\naction fire delayend 3\n",
		                         ak, error));
		CHECK(merge_weapon_def(retail_def, ak, merged, notes, error));
		const auto before = lines_of(retail_def), after = lines_of(merged);
		CHECK(before.size() == after.size());
		size_t differing = 0;
		for (size_t i = 0; i < before.size() && i < after.size(); ++i) differing += before[i] != after[i];
		CHECK(differing <= 3);
	}
	return failures ? 1 : 0;
}
