// S9 (ADR 0028): the mission boot's file-resolution rules — the mission-text
// fallback rule, the .aip profile-speed resolution (parse, dedup order,
// trim/lowercase, unauthored-row exclusion) and the authority's placed tiles
// (<mission>.til, else the terrain's polytrn_tileinfo). The boot ORDER itself is pinned
// through MissionKernel::boot_trace in mission_kernel_test (ADR 0043 slice E9).

#include <runtime/mission/runtime_boot.h>
#include <runtime/terrain_query/terrain_field_build.h>

#include <formats/aip/aip.h>
#include <formats/mission/bms.h>
#include <formats/til/til_io.h>

#include "../common/boot_file_source.h"

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

using test_boot::source_over;

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

// The .aip walk is the shared one [orig: AIProfile_LoadOrFind -> File_ParseASCIIFile
// @0x45FE45 -> AIProfile_ParseProperty @0x45de70]: a comma separates, `//` ends the
// line, a key with no value reads "" (atol 0) rather than being skipped, and the
// game's atof takes a `d` exponent.
bool run_aip_walk() {
	const std::string text =
			"type GROUND\r\n"
			"rank,7 // seven\r\n"
			"radio_distance 300\r\n"
			"radio_distance\r\n"
			"react_time 1d1\r\n";
	const auto prof = opennova::aip::parse_profile(
			reinterpret_cast<const uint8_t *>(text.data()), text.size());
	return expect(prof.type == 2 && prof.rank == 7 && prof.radio_distance == 0 &&
						prof.react_ticks == 625,
			"aip: the shared walk (comma, comment, bare key, d exponent)");
}

// .aip parse (engine/formats/aip): the witnessed GROUND key set — speeds,
// tabs, case-insensitive keys, junk lines, the type gate, and the weapon
// blocks with their exact conversions [orig: AIProfile_ParseProperty
// @0x45de70].
bool run_aip_parse() {
	const std::string text =
			"; comment line\r\n"
			"type GROUND\r\n"
			"PATROL_speed\t5\r\n"
			"combat_speed 12 trailing junk\r\n"
			"unrelated 99\r\n"
			"aim_skill 9\r\n"
			"react_time 2\r\n"
			"primary_weap 50cal\r\n"
			"primary_ammo 200\r\n"
			"primary_rate 0.5\r\n"
			"primary_fov 45\r\n"
			"primary_range 300\r\n"
			"primary_facing 180\r\n"
			"primary_flags WEAPON_TURRET WEAPON_SLOW\r\n"
			"secondary_flags WEAPON_PITCHLOCKED_MINUS45\r\n"
			"COMBAT_FLAGS FOLLOW_WP RC_FIRE\r\n";
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
	const std::string untyped = "patrol_speed 7\r\n";
	std::vector<uint8_t> ub(untyped.begin(), untyped.end());
	const opennova::aip::Profile none =
			opennova::aip::parse_profile(ub.data(), ub.size());
	if (!expect(none.type == 0 && none.patrol_speed == -1,
				"aip: untyped file parses nothing"))
		return false;

	// ORGANIC parses nothing beyond type [orig: the type-3 early return].
	const std::string organic = "type ORGANIC\r\npatrol_speed 7\r\n";
	std::vector<uint8_t> ob(organic.begin(), organic.end());
	const opennova::aip::Profile org =
			opennova::aip::parse_profile(ob.data(), ob.size());
	if (!expect(org.type == 3 && org.patrol_speed == -1,
			"aip: ORGANIC keys ignored"))
		return false;

	// The HELO (type 1) flight set: flight keys land in the helo_* rows
	// (+200..+236) in retail's PARSED forms — km/h speeds as 16.16 units per
	// tick (atof * 1000 * 4.444444444444444e-06 * 65536 = x65536/225, chopped),
	// climbs atof * 0.016 * 65536, altitudes atol<<16, min_agl atof*65536 — the
	// SHARED keys (view/radar) still apply, and the GROUND rows stay untouched.
	// [orig: the type-1 arms of AIProfile_ParseProperty @0x45f684..0x45f9eb]
	const std::string helo =
			"type HELO\r\n"
			"patrol_speed 30\r\n"
			"patrol_altitude 40\r\n"
			"patrol_climb 8\r\n"
			"combat_speed 45\r\n"
			"combat_altitude 60\r\n"
			"combat_climb 12\r\n"
			"turn_rate 45\r\n"
			"accel_time 3\r\n"
			"use_waypoint_z 1\r\n"
			"min_agl 15\r\n"
			"min_speed 10\r\n"
			"view_dist 400\r\n";
	const std::string extended = helo +
			"subtype plane\r\nflight_skill 9\r\ndefault_state helo_combat\r\n"
			"hunt_flags MAINTAIN_SPEED\r\nhunt_limit 1.5\r\nalert RED\r\nrank 7\r\n"
			"radio_distance 300\r\nradio_delay 10\r\ncheck_six_rate 2\r\ntarget_eval_rate 3\r\n"
			"evade_flags COUNTER ATEAM RC_FIRE\r\n";
	const auto extra = opennova::aip::parse_profile(
			reinterpret_cast<const uint8_t *>(extended.data()), extended.size());
	if (!expect(extra.subtype == 2 && extra.drive_skill == 4 && extra.default_state == 8,
				"aip: aircraft class, skill clamp and default state name"))
		return false;
	if (!expect(extra.hunt_flags == 1 && extra.hunt_limit == 93 && extra.alert == 2 &&
						extra.rank == 7,
				"aip: flight hunt, alert and rank fields"))
		return false;
	if (!expect(extra.radio_distance == 300 && extra.radio_delay == 10 &&
						extra.check_six_rate == 1310 && extra.target_eval_rate == 1966 &&
						extra.evade_flags == 0x10,
				"aip: radio/evaluation conversions and evade-only flags"))
		return false;
	const std::string ground_extra =
			"type GROUND\r\ndefault_state GROUND_FOLLOWWP\r\npatrol_speed 2.5\r\n"
			"combat_speed 5.75\r\nturn_rate 30\r\naccel_time 2\r\ndrive_skill -2\r\nview_fov 360\r\n";
	const auto gp = opennova::aip::parse_profile(
			reinterpret_cast<const uint8_t *>(ground_extra.data()), ground_extra.size());
	if (!expect(gp.default_state == 17 && gp.has_ground_patrol_speed &&
						gp.ground_patrol_speed == 728 && gp.has_ground_combat_speed &&
						gp.ground_combat_speed == 1674 && gp.drive_skill == 0,
				"aip: literal ground state table and fractional speeds"))
		return false;
	if (!expect(gp.turn_rate_bam_tick == 11930464 * 30 / 62 && gp.accel_ticks == 124 &&
						gp.view_fov_bam == -256,
				"aip: ground steering/acceleration and full-turn BAM wrap"))
		return false;
	std::vector<uint8_t> hb(helo.begin(), helo.end());
	const opennova::aip::Profile hp =
			opennova::aip::parse_profile(hb.data(), hb.size());
	if (!expect(hp.type == 1, "aip: type HELO")) return false;
	if (!expect(hp.helo_patrol_speed == 8738 && hp.helo_combat_speed == 13107,
			"aip: helo speeds 30/45 km/h -> 8738/13107 (x65536/225, chop)"))
		return false;
	if (!expect(hp.patrol_speed == -1 && hp.combat_speed == -1,
			"aip: the GROUND speed rows stay untouched for a helo"))
		return false;
	if (!expect(hp.helo_patrol_altitude == (40 << 16) &&
					hp.helo_combat_altitude == (60 << 16),
			"aip: altitudes atol<<16"))
		return false;
	if (!expect(hp.helo_patrol_climb == 8388 && hp.helo_combat_climb == 12582 &&
					hp.min_speed == 2912,
			"aip: climbs 8/12 -> 8388/12582 (x0.016x65536), min_speed 10 km/h -> 2912"))
		return false;
	if (!expect(hp.turn_rate_bam_tick == 11930464 * 45 / 62,
			"aip: turn_rate BAM/tick formula"))
		return false;
	if (!expect(hp.accel_ticks == 62 * 3, "aip: accel_time 62x")) return false;
	if (!expect(hp.use_waypoint_z == 1 && hp.min_agl == (15 << 16),
			"aip: use_waypoint_z + min_agl"))
		return false;
	return expect(hp.view_dist == (400 << 16),
			"aip: shared keys apply to HELO profiles too");
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
	files["helo1.aip"] = "type GROUND\r\npatrol_speed 7\r\ncombat_speed 9\r\n";
	files["truck2.aip"] = "type GROUND\r\ncombat_speed 3\r\n";
	files["empty.aip"] = "nothing_relevant 1\r\n";
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

// The resolve loads the FALLBACK profiles too: a placed item with no
// ai_textfile takes its class row's default ("helo1" for the helicopter
// family, the def's default_aip then "helo1" for the vehicle family), so the
// promote's brain seed finds a row for a bare-placed vehicle exactly as
// retail's AI init does [orig: Entity_InitHelicopterAIFromDef @0x4683C0
// @0x4684c9; Entity_InitVehicleAIFromDef @0x4686C0 @0x4687c1/@0x4687d3].
bool run_aip_fallback() {
	bms::File mission{};
	auto item = [](int32_t type_id) {
		bms::Entity e{};
		e.type = bms::ItemType::Item;
		e.type_id = type_id;
		std::memset(e.name2, 0, sizeof(e.name2));
		return e;
	};
	mission.items.push_back(item(2010)); // helicopter class, nameless
	mission.items.push_back(item(1237)); // vehicle class, def default_aip
	mission.items.push_back(item(9));    // no AI class row
	bms::Entity soldier{};
	soldier.type = bms::ItemType::Organic;
	std::memset(soldier.name2, 0, sizeof(soldier.name2));
	mission.organics.push_back(soldier); // nameless organic: no fallback

	std::map<std::string, std::string> files;
	files["helo1.aip"] = "type HELO\r\npatrol_speed 30\r\n";
	files["d_5ton.aip"] = "type GROUND\r\ncombat_speed 4\r\n";
	const ms::BootFileSource src = source_over(&files);
	const auto defaults = [](int32_t type_id) {
		ms::PromoteOptions::AiProfileDefaults d;
		if (type_id == 2010) { d.known = true; d.helicopter_init = true; }
		if (type_id == 1237) { d.known = true; d.default_aip = "D_5ton"; }
		return d;
	};
	const std::vector<ms::PromoteOptions::AiProfileRow> rows =
			ms::resolve_ai_profiles(src, mission, defaults);
	if (!expect(rows.size() == 2, "fallback: helo1 + d_5ton rows, nothing else"))
		return false;
	if (!expect(rows[0].profile == "helo1" && rows[0].data.type == 1,
				"fallback: the helicopter row is helo1, type HELO"))
		return false;
	if (!expect(rows[1].profile == "d_5ton" && rows[1].data.combat_speed == 4,
				"fallback: the vehicle row is the def's default_aip"))
		return false;
	// Without an embedder answer nothing is loaded for nameless records.
	const std::vector<ms::PromoteOptions::AiProfileRow> bare =
			ms::resolve_ai_profiles(src, mission);
	return expect(bare.empty(), "fallback: no class answer -> no fallback rows");
}

// A .til of `count` placed tiles, each at a distinct cell.
std::string til_of(int count) {
	TilFile til;
	for (int i = 0; i < count; ++i) {
		TilOverlayEntry tile;
		tile.x_fixed = (4 + 8 * i) << 16;
		tile.z_fixed = -(4 << 16);
		tile.tile_index = static_cast<uint8_t>(i);
		til.entries.push_back(tile);
	}
	std::vector<uint8_t> out;
	std::string error;
	if (!save_til(til, out, error)) std::fprintf(stderr, "save_til: %s\n", error.c_str());
	return std::string(out.begin(), out.end());
}

std::string bytes_string(const std::vector<uint8_t> &bytes) {
	return std::string(bytes.begin(), bytes.end());
}

// The authority's placed tiles: <mission>.til by the name cut at its FIRST '.',
// else the terrain configuration's polytrn_tileinfo, its extension forced from
// its first '.', each taken only when the load takes it (16 bytes or more that
// open with the til0 magic). [orig: PolyTrn_LoadTerrainConfig @0x60e6c9..0x60e6e5;
// Terrain_LoadTileInfoFile @0x60a740 (-1 @0x60a78b, @0x60a7ba); Terrain_Init
// @0x60fcfd/@0x60fd0c; Path_ReplaceOrAppendExtension's first-dot scan @0x53c7c4]
bool run_placed_tiles() {
	bms::File mission;
	std::snprintf(mission.header.terrain, 16, "%s", "synthtrn");
	std::snprintf(mission.header.environment, sizeof(mission.header.environment), "%s", "synthenv");
	const std::string trn =
			"polytrn_colormap synth_c.tga\r\n"
			"polytrn_detailmap synth_d.tga\r\n"
			"polytrn_polydata synth.cpt\r\n"
			"polytrn_tileinfo Own.v1.dat\r\n";
	std::map<std::string, std::string> files;
	files["op.til"] = til_of(1);
	files["op.v2.til"] = til_of(2);
	files["synthtrn.trn"] = trn;
	files["Own.til"] = til_of(3);
	const ms::BootFileSource src = source_over(&files);
	std::vector<uint8_t> out;
	bool ok = true;

	// The mission's own, by its first-dot base: op.v2.bms reads op.til.
	ok &= expect(ms::read_placed_tiles(src, "op.v2.bms", mission, out) == "op.til" &&
					bytes_string(out) == files["op.til"],
			"tiles: <mission>.til by the first-dot base");

	// None of the mission's: the terrain's own, Own.v1.dat read as Own.til.
	ok &= expect(ms::read_placed_tiles(src, "other.bms", mission, out) == "Own.til" &&
					bytes_string(out) == files["Own.til"],
			"tiles: the terrain's polytrn_tileinfo when the mission has none");

	// A mission .til the load refuses sends it on: under 16 bytes, or no magic.
	files["short.til"] = files["op.til"].substr(0, 15);
	ok &= expect(ms::read_placed_tiles(src, "short.bms", mission, out) == "Own.til",
			"tiles: a short mission .til falls back");
	files["nomagic.til"] = std::string(16, '\0');
	ok &= expect(ms::read_placed_tiles(src, "nomagic.bms", mission, out) == "Own.til",
			"tiles: a bad-magic mission .til falls back");

	// The magic and 16 bytes are all the load checks: a header whose count runs
	// past the file is still the mission's.
	std::string truncated = files["op.til"].substr(0, 16);
	truncated[4] = 5;
	files["truncated.til"] = truncated;
	ok &= expect(ms::read_placed_tiles(src, "truncated.bms", mission, out) == "truncated.til" &&
					out.size() == 16,
			"tiles: a truncated body with the magic is taken");

	// The mission's .env reaches the terrain's parser after the .trn, so its
	// polytrn_tileinfo is the one the load forces.
	files["synthenv.env"] = "polytrn_tileinfo EnvOwn.dat\r\n";
	files["EnvOwn.til"] = til_of(4);
	ok &= expect(ms::read_placed_tiles(src, "other.bms", mission, out) == "EnvOwn.til" &&
					bytes_string(out) == files["EnvOwn.til"],
			"tiles: the .env's polytrn_tileinfo over the .trn's");
	files.erase("synthenv.env");

	// Neither loads: no tiles, no name.
	files.erase("Own.til");
	ok &= expect(ms::read_placed_tiles(src, "other.bms", mission, out).empty() && out.empty(),
			"tiles: neither file loads -> no tiles");
	files["Own.til"] = std::string(16, '\0');
	ok &= expect(ms::read_placed_tiles(src, "other.bms", mission, out).empty() && out.empty(),
			"tiles: a refused terrain tile info -> no tiles");

	// A terrain naming no tile info has no fallback.
	files["synthtrn.trn"] =
			"polytrn_colormap synth_c.tga\r\n"
			"polytrn_detailmap synth_d.tga\r\n"
			"polytrn_polydata synth.cpt\r\n";
	ok &= expect(ms::read_placed_tiles(src, "other.bms", mission, out).empty() && out.empty(),
			"tiles: no polytrn_tileinfo -> no fallback");
	ok &= expect(terrain::terrain_tileinfo_name("Own.v1.dat") == "Own.til" &&
					terrain::terrain_tileinfo_name("maps.v2/own.dat") == "maps.til" &&
					terrain::terrain_tileinfo_name("own") == "own.til" &&
					terrain::terrain_tileinfo_name("").empty(),
			"tiles: the extension forced from the value's first '.'");
	return ok;
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_text_fallback();
	ok &= run_aip_parse();
	ok &= run_aip_walk();
	ok &= run_aip_resolve();
	ok &= run_aip_fallback();
	ok &= run_placed_tiles();
	if (!ok) return 1;
	std::printf("runtime_boot_test: OK\n");
	return 0;
}
