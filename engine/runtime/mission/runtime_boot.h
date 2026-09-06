#pragma once

#include <formats/mission/bms.h>
#include <runtime/mission/promote.h> // PromoteOptions::AiProfileRow (the install row)

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// S9 (ADR 0028): the mission boot's file-resolution rules the shell used to
// carry (the mission-text fallback, the .aip profile walk) and the embedder's
// file source. The boot ORDER itself is MissionKernel::boot (ADR 0043 slice
// E9: one straight-line sequence with a recorded trace; the former functor
// table is gone). The retail boot is Game_StartMission's fixed sequence
// [orig: @0x5254b3 weapon.def -> Mission_LoadBMSFile -> the table/spawn
// chain]; our documented divergence is that the mission loads FIRST and the
// loadout-chunk promotion waits inside load_weapon_table
// (world/player_loadout.h, S7b) — every other dependency (spawn needs the
// promoted AI system, ai-weapon seeding needs the ammo table, collision needs
// the item db + models) is the same partial order.
namespace opennova::mission {

// The embedder's mounted-resource reader (the Godot binding backs this with
// ResourceRoot's native ResourceIndex; tests use in-memory maps).
struct BootFileSource {
	std::function<bool(const std::string &name)> has_file;
	std::function<bool(const std::string &name, std::vector<uint8_t> &out)>
			read_file;
	bool valid() const { return has_file != nullptr && read_file != nullptr; }
};

// The infantry clip-set default when the embedder authors none.
inline constexpr char kDefaultInfantryAdm[] = "person.adm";

// Resolve the per-mission MissionText RTXT table bytes: <mission>.bin when it
// exists, else medmssn.bin. Preserves the original fallback — medmssn.bin is
// used only when <mission>.bin DOES NOT EXIST; a present but malformed table
// is passed through and rejected downstream without fallback. Returns which
// source produced the bytes.
enum class MissionTextSource { kNone, kMission, kFallback };
MissionTextSource resolve_mission_text(const BootFileSource &files,
		const std::string &mission_file_basename, std::vector<uint8_t> &out);

// The .aip PARSE lives in engine/formats/aip (aip::parse_profile — the
// witnessed GROUND-type set; the HELO set is the remaining tracked gap).
// This resolver keeps the profile walk and the install row: absent keys keep
// their sentinels (the promote-time brain seed then keeps its stand-ins)
// [orig: Entity_InitVehicleAIFromDef seeds brain[50]/brain[49]
//  @0x4688D3/@0x4688C7].

// The mission's distinct profile-name set, lowercase, in entity order (first
// occurrence wins), and each profile's parsed .aip row. Each entity's name is
// resolved the way retail's AI init does (ai_profile_name_for: the ai_textfile,
// else — for a placed item whose class the embedder knows — the def's
// default_aip or "helo1"), so a nameless vehicle's fallback profile is loaded
// too. Files that parse no witnessed field contribute no row, exactly like
// the speeds-only resolver this extends.
std::vector<PromoteOptions::AiProfileRow> resolve_ai_profiles(
		const BootFileSource &files, const bms::File &mission,
		const std::function<PromoteOptions::AiProfileDefaults(int32_t)> &
				ai_profile_defaults = {});

} // namespace opennova::mission
