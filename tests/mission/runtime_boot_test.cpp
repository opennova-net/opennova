// S9 (ADR 0028): the mission boot policy — the first ctest lock on the boot
// ORDER. Pins run_mission_boot's exact step sequence and gates (role, missing
// inputs, the load abort, the ammo-gated AI-weapon seed), the mission-text
// fallback rule, and the .aip profile-speed resolution (parse, dedup order,
// trim/lowercase, unauthored-row exclusion).

#include <mission/runtime_boot.h>

#include <aip/aip.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace ms = opennova::mission;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

ms::BootFileSource source_over(
		const std::map<std::string, std::string> *files) {
	ms::BootFileSource s;
	s.has_file = [files](const std::string &name) {
		return files->find(name) != files->end();
	};
	s.read_file = [files](const std::string &name, std::vector<uint8_t> &out) {
		const auto it = files->find(name);
		if (it == files->end()) return false;
		out.assign(it->second.begin(), it->second.end());
		return true;
	};
	return s;
}

struct StepRecorder {
	std::vector<std::string> calls;
	bool load_ok = true;
	bool ammo_ok = true;

	ms::BootSteps steps() {
		ms::BootSteps s;
		s.install_seat_specs = [this] { calls.push_back("seat_specs"); };
		s.install_ai_profiles = [this] { calls.push_back("aip"); };
		s.install_terrain_til = [this] { calls.push_back("til"); };
		s.install_mission_text = [this] { calls.push_back("text"); };
		s.load_mission = [this] {
			calls.push_back("load");
			return load_ok;
		};
		s.install_terrain_field = [this] { calls.push_back("terrain"); };
		s.install_sound_profiles = [this] { calls.push_back("sndprof"); };
		s.install_infantry_anim = [this] { calls.push_back("adm"); };
		s.install_wac = [this] { calls.push_back("wac"); };
		s.spawn_local_player = [this] { calls.push_back("spawn"); };
		s.resolve_infantry_adm = [this] { calls.push_back("adm_ids"); };
		s.resolve_item_traits = [this] { calls.push_back("traits"); };
		s.install_asset_root = [this] { calls.push_back("asset_root"); };
		s.resolve_collision = [this] { calls.push_back("collision"); };
		s.occlusion_init = [this] { calls.push_back("occlusion"); };
		s.load_weapon_table = [this] { calls.push_back("weapons"); };
		s.load_ammo_table = [this] {
			calls.push_back("ammo");
			return ammo_ok;
		};
		s.resolve_ai_weapons = [this] { calls.push_back("ai_weapons"); };
		return s;
	}
};

bool dump_on_fail(const StepRecorder &r, const char *label) {
	std::fprintf(stderr, "  %s:", label);
	for (const std::string &c : r.calls) std::fprintf(stderr, " %s", c.c_str());
	std::fprintf(stderr, "\n");
	return false;
}

// The full-inputs SP boot runs every step in the exact order.
bool run_full_order() {
	StepRecorder r;
	ms::BootParams p;
	p.playable = true;
	p.has_resource_root = true;
	p.has_item_db = true;
	p.has_terrain = true;
	p.has_terrain_til = true;
	p.has_wac = true;
	const ms::BootAbort abort = ms::run_mission_boot(p, r.steps());
	const std::vector<std::string> expected = {
			"seat_specs", "aip", "til", "text", "load", "terrain", "sndprof",
			"adm", "wac", "spawn", "adm_ids", "traits", "asset_root",
			"collision", "occlusion", "weapons", "ammo", "ai_weapons"};
	if (!expect(abort == ms::BootAbort::kNone, "full: boots clean")) return false;
	if (r.calls != expected) {
		expect(false, "full: exact step order");
		return dump_on_fail(r, "got");
	}
	return true;
}

// Gates: a rootless boot keeps only the root-free steps; a joiner never
// spawns; a missing item db drops trait/collision resolution.
bool run_gates() {
	{
		StepRecorder r;
		ms::BootParams p;
		p.playable = true; // no root/db/placer/terrain/til/wac
		(void)ms::run_mission_boot(p, r.steps());
		const std::vector<std::string> expected = {"text", "load", "spawn"};
		if (r.calls != expected) {
			expect(false, "rootless: only the root-free steps");
			return dump_on_fail(r, "got");
		}
	}
	{
		StepRecorder r;
		ms::BootParams p;
		p.is_joiner = true;
		p.playable = true; // playable joiner still never spawns here
		p.has_resource_root = true;
		p.has_item_db = true;
		(void)ms::run_mission_boot(p, r.steps());
		for (const std::string &c : r.calls)
			if (!expect(c != "spawn", "joiner: L spawns on the name-match, not here"))
				return false;
	}
	{
		StepRecorder r;
		ms::BootParams p;
		p.has_item_db = true; // db without root: collision resolves from the
		                      // sim cache; only the root-fed asset_root skips
		(void)ms::run_mission_boot(p, r.steps());
		bool saw_collision = false, saw_occlusion = false;
		for (const std::string &c : r.calls) {
			saw_collision = saw_collision || c == "collision";
			saw_occlusion = saw_occlusion || c == "occlusion";
			if (!expect(c != "asset_root", "rootless db: no asset_root install"))
				return false;
		}
		if (!expect(saw_collision && saw_occlusion,
				"rootless db: collision + occlusion still run"))
			return dump_on_fail(r, "got");
	}
	return true;
}

// The load abort: nothing after a failed load runs.
bool run_load_abort() {
	StepRecorder r;
	r.load_ok = false;
	ms::BootParams p;
	p.playable = true;
	p.has_resource_root = true;
	p.has_item_db = true;
	const ms::BootAbort abort = ms::run_mission_boot(p, r.steps());
	if (!expect(abort == ms::BootAbort::kLoadFailed, "abort: reported")) return false;
	if (!expect(!r.calls.empty() && r.calls.back() == "load",
			"abort: load is the last step run"))
		return dump_on_fail(r, "got");
	return true;
}

// The AI-weapon seed runs only against a loaded ammo table.
bool run_ammo_gate() {
	StepRecorder r;
	r.ammo_ok = false;
	ms::BootParams p;
	p.has_resource_root = true;
	p.has_item_db = true;
	(void)ms::run_mission_boot(p, r.steps());
	for (const std::string &c : r.calls)
		if (!expect(c != "ai_weapons", "ammo gate: no seed without the table"))
			return false;
	return true;
}

// <mission>.bin when present; medmssn.bin only when it does not exist.
bool run_text_fallback() {
	std::map<std::string, std::string> files;
	files["00trg.bin"] = "MISSIONTEXT";
	files["medmssn.bin"] = "FALLBACK!";
	const ms::BootFileSource src = source_over(&files);
	std::vector<uint8_t> out;
	if (!expect(ms::resolve_mission_text(src, "00trg", out) ==
					ms::MissionTextSource::kMission && out.size() == 11,
			"text: <mission>.bin wins"))
		return false;
	if (!expect(ms::resolve_mission_text(src, "other", out) ==
					ms::MissionTextSource::kFallback && out.size() == 9,
			"text: medmssn fallback when absent"))
		return false;
	if (!expect(ms::resolve_mission_text(src, "", out) ==
					ms::MissionTextSource::kFallback,
			"text: empty basename goes straight to the fallback"))
		return false;
	std::map<std::string, std::string> none;
	const ms::BootFileSource empty_src = source_over(&none);
	if (!expect(ms::resolve_mission_text(empty_src, "00trg", out) ==
					ms::MissionTextSource::kNone && out.empty(),
			"text: neither file -> none"))
		return false;
	return true;
}

// .aip parse (engine/formats/aip): the witnessed GROUND key set — speeds,
// tabs, case-insensitive keys, junk lines, the type gate, and the weapon
// blocks with their exact conversions [orig: AIProfile_ParseProperty
// @0x45de70].
bool run_aip_parse() {
	const std::string text =
			"; comment line\n"
			"type GROUND\n"
			"PATROL_speed\t5\r\n"
			"combat_speed 12 trailing junk\n"
			"unrelated 99\n"
			"aim_skill 9\n"
			"react_time 2\n"
			"primary_weap 50cal\n"
			"primary_ammo 200\n"
			"primary_rate 0.5\n"
			"primary_fov 45\n"
			"primary_range 300\n"
			"primary_facing 180\n"
			"primary_flags WEAPON_TURRET WEAPON_SLOW\n"
			"secondary_flags WEAPON_PITCHLOCKED_MINUS45\n"
			"COMBAT_FLAGS FOLLOW_WP RC_FIRE\n";
	std::vector<uint8_t> bytes(text.begin(), text.end());
	const opennova::aip::Profile row =
			opennova::aip::parse_profile(bytes.data(), bytes.size());
	if (!expect(row.type == 2, "aip: type GROUND")) return false;
	if (!expect(row.patrol_speed == 5, "aip: patrol via tab + mixed case")) return false;
	if (!expect(row.combat_speed == 12, "aip: combat, extra tokens ignored")) return false;
	if (!expect(row.aim_skill == 4, "aip: aim_skill clamps to 4")) return false;
	if (!expect(row.react_ticks == 125, "aip: react_time 2s -> 125 ticks (x62.5)"))
		return false;
	if (!expect(row.primary.weapon == "50cal", "aip: primary_weap name kept")) return false;
	if (!expect(row.primary.ammo == 200, "aip: primary_ammo atol")) return false;
	if (!expect(row.primary.rate_ticks == 31, "aip: primary_rate 0.5s -> 31 (chop)"))
		return false;
	// 45 deg * 11930464 = 536870880 = 0x1FFFFFE0.
	if (!expect(row.primary.cone_bam == 536870880, "aip: primary_fov deg->BAM")) return false;
	if (!expect(row.primary.range == 300 << 16, "aip: primary_range <<16")) return false;
	// 180 deg * 11930464 = 2147483520.
	if (!expect(row.primary.facing_bam == 2147483520, "aip: primary_facing deg->BAM"))
		return false;
	if (!expect(row.primary.flags == (opennova::aip::kWeaponTurret | opennova::aip::kWeaponSlow),
				"aip: primary_flags tokens"))
		return false;
	if (!expect(row.secondary.flags == opennova::aip::kWeaponPitchLockedMinus45,
				"aip: secondary_flags token"))
		return false;
	if (!expect(row.combat_flags == 0x81u, "aip: COMBAT_FLAGS FOLLOW_WP|RC_FIRE")) return false;

	// Keys BEFORE a type line (or with no type at all) are ignored — the
	// dispatch is type-gated exactly like retail's +16 branch.
	const std::string untyped = "patrol_speed 7\n";
	std::vector<uint8_t> ub(untyped.begin(), untyped.end());
	const opennova::aip::Profile none =
			opennova::aip::parse_profile(ub.data(), ub.size());
	if (!expect(none.type == 0 && none.patrol_speed == -1,
				"aip: untyped file parses nothing"))
		return false;

	// ORGANIC parses nothing beyond type [orig: the type-3 early return].
	const std::string organic = "type ORGANIC\npatrol_speed 7\n";
	std::vector<uint8_t> ob(organic.begin(), organic.end());
	const opennova::aip::Profile org =
			opennova::aip::parse_profile(ob.data(), ob.size());
	return expect(org.type == 3 && org.patrol_speed == -1,
			"aip: ORGANIC keys ignored");
}

// The mission-profile resolve: entity-walk order (markers first), first
// occurrence wins, absent .aip skips, keyless .aip contributes no row,
// name2 trims + lowercases.
bool run_aip_resolve() {
	bms::File mission{};
	auto with_profile = [](const char *name2) {
		bms::Entity e{};
		std::memset(e.name2, 0, sizeof(e.name2));
		std::strncpy(e.name2, name2, sizeof(e.name2) - 1);
		return e;
	};
	mission.markers.push_back(with_profile("Helo1"));   // first occurrence
	mission.items.push_back(with_profile("helo1 "));    // dup (trim + case)
	mission.items.push_back(with_profile("truck2"));
	mission.organics.push_back(with_profile("nofile")); // no .aip
	mission.organics.push_back(with_profile("empty"));  // keyless .aip

	std::map<std::string, std::string> files;
	files["helo1.aip"] = "type GROUND\npatrol_speed 7\ncombat_speed 9\n";
	files["truck2.aip"] = "type GROUND\ncombat_speed 3\n";
	files["empty.aip"] = "nothing_relevant 1\n";
	const ms::BootFileSource src = source_over(&files);

	const std::vector<ms::PromoteOptions::AiProfileRow> rows =
			ms::resolve_ai_profiles(src, mission);
	if (!expect(rows.size() == 2, "resolve: two authored rows")) return false;
	if (!expect(rows[0].profile == "helo1" && rows[0].data.patrol_speed == 7 &&
					rows[0].data.combat_speed == 9,
			"resolve: helo1 parsed once, first occurrence"))
		return false;
	return expect(rows[1].profile == "truck2" && rows[1].data.patrol_speed == -1 &&
					rows[1].data.combat_speed == 3,
			"resolve: truck2 keeps unauthored patrol");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_full_order();
	ok &= run_gates();
	ok &= run_load_abort();
	ok &= run_ammo_gate();
	ok &= run_text_fallback();
	ok &= run_aip_parse();
	ok &= run_aip_resolve();
	if (!ok) return 1;
	std::printf("runtime_boot_test: OK\n");
	return 0;
}
