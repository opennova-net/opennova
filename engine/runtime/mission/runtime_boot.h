#pragma once

#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <runtime/mission/promote.h> // PromoteOptions::AiProfileRow (the install row)
#include <runtime/world/ammo_table.h>

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
namespace opennova { class ResourceIndex; }

namespace opennova::mission {

// The embedder's mounted-resource reader (the Godot binding backs this with
// ResourceRoot's native ResourceIndex; tests use in-memory maps).
struct BootFileSource {
	std::function<bool(const std::string &name)> has_file;
	std::function<bool(const std::string &name, std::vector<uint8_t> &out)>
			read_file;
	// Optional mounted enumeration, in mount precedence order. Used by the
	// particle catalog; ordinary in-memory BMS/AI boots need only the readers.
	std::function<std::vector<std::string>(const std::string &extension)> list_files;
	// Optional loose-first reader: the game directory's own file ahead of the
	// archives whatever the session's lookup policy, for the reads retail
	// makes off the disk (score.ini, the mission .til). Unset (an in-memory
	// source, which has no archive layer) reads through read_file.
	std::function<bool(const std::string &name, std::vector<uint8_t> &out)> read_loose_first;
	bool read_loose(const std::string &name, std::vector<uint8_t> &out) const {
		if (read_loose_first) return read_loose_first(name, out);
		return read_file != nullptr && read_file(name, out);
	}
    std::string expansion_name; // mounted bank-chain metadata, empty for the base game
	bool valid() const { return has_file != nullptr && read_file != nullptr; }
};

// The index must outlive the returned readers.
BootFileSource boot_files_from_index(const ResourceIndex &index);

// How a def table's file read: no file of the name, a file the game's parser stops in, or read.
enum class DefTableRead { Missing, Unreadable, Read };

// weapon.def (`name`) through the game's parser as the mission load reads it: a SIGHTS row whose
// texture the mount lacks is no row [orig: the sights arm's FileSystem_FileExists @0x544AE2], and a
// zero-length file is an empty table (def_parse_weapons_memory). On Read `out` holds the parsed file
// (the caller builds its table, world::build_weapon_table, and frees it, def_free_weapons); the
// parser has zeroed it otherwise.
DefTableRead read_weapon_defs(const BootFileSource &files, const std::string &name,
		def::DefWeaponsFile &out);

// ammo.def (`name`) as the mission load reads it, into the dense file-order table
// (world::build_ammo_table) [orig: AmmoDef_LoadAll @ 0x40B0B0]; `out` is untouched unless Read.
// What the load binds to it afterwards is the caller's: the weapons' round types
// (world::resolve_weapon_round_types) and the whiz radii over the loaded sound sets
// (world::resolve_ammo_whiz_radii).
DefTableRead read_ammo_table(const BootFileSource &files, const std::string &name,
		world::AmmoTable &out);

// The infantry clip-set default when the embedder authors none.
inline constexpr char kDefaultInfantryAdm[] = "E_STAND.adm";

// Resolve the per-mission MissionText RTXT table bytes: <mission>.bin when it
// exists, else medmssn.bin. Preserves the original fallback — medmssn.bin is
// used only when <mission>.bin DOES NOT EXIST; a present but malformed table
// is passed through and rejected downstream without fallback. Returns which
// source produced the bytes.
enum class MissionTextSource { kNone, kMission, kFallback };
MissionTextSource resolve_mission_text(const BootFileSource &files,
		const std::string &mission_file_basename, std::vector<uint8_t> &out);

// The placed tiles the authority loads at mission start, the one .til the S2C 0x45
// stream, the render overlay and the surface walk share: <mission>.til (mission_sidecars'
// tiles row, the name cut at its FIRST '.'), else the terrain's own, the polytrn_tileinfo the
// mission's terrain configuration holds (its .trn, overcast.def and its .env through the
// terrain's parser) with its extension forced from its first '.'. Both read loose first, and
// each is taken only where the game's load takes it: a missing, short or bad-magic file sends
// the load on to the next (the reads and the checks are terrain_query's
// read_placed_tile_bytes, behind the ADR 0020 seam). A joiner calls none of this: its tiles
// are the host's stream. Returns the name it took, "" (with `out` empty) when neither loaded.
// [orig: PolyTrn_LoadTerrainConfig @ 0x60e3d0, the authority test @ 0x60e6c9:
//  Terrain_LoadTileInfoFile(<map>.TIL) @ 0x60e6d2, and on -1 @ 0x60e6dc
//  Terrain_LoadTileInfoFile(polytrn_tileinfo) @ 0x60e6e5; the names Terrain_Init
//  @ 0x60fcfd / @ 0x60fd0c; the loose-first force @ 0x60a74e]
std::string read_placed_tiles(const BootFileSource &files, const std::string &mission_file,
		const bms::File &mission, std::vector<uint8_t> &out);

// The .aip PARSE lives in engine/formats/aip (aip::parse_profile: every
// GROUND, HELO and ORGANIC key AIProfile_ParseProperty stores). This resolver
// keeps the profile walk and the install row. An unauthored key is the zeroed
// record's 0, and the class init copies whatever words sit at the offsets it
// hard-codes (aip::class_speed_words) [orig: Entity_InitVehicleAIFromDef
// stores brain[50] @0x4688D3 and brain[49] @0x4688C7].

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
