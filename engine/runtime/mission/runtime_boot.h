#pragma once

#include "mission/bms.h"
#include "mission/promote.h" // PromoteOptions::AiProfileRow (the install row)

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// S9 (ADR 0028): the mission boot policy — the ordered sim-feed sequence a
// live mission runs between role bring-up and presentation composition, plus
// the file-resolution rules the shell used to carry. The retail boot is
// Game_StartMission's fixed sequence [orig: @0x5254b3 weapon.def ->
// Mission_LoadBMSFile -> the table/spawn chain]; our documented divergence is
// that the mission loads FIRST and the loadout-chunk promotion waits inside
// load_weapon_table (world/player_loadout.h, S7b) — every other dependency
// (spawn needs the promoted AI system, ai-weapon seeding needs the ammo
// table, collision needs the item db + models) is the same partial order.
// run_mission_boot owns that order and its gates; each step is an embedder
// functor (the Godot binding's existing feeds), so the sequence itself is
// portable and ctest-locked while asset/socket plumbing stays shell-side.
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
inline constexpr char kDefaultInfantryAdm[] = "E_STAND.adm";

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

// The mission's distinct ai_textfile profile set, lowercase, in entity order
// (first occurrence wins), and each profile's parsed .aip row. Files that
// parse no witnessed field contribute no row, exactly like the speeds-only
// resolver this extends.
std::vector<PromoteOptions::AiProfileRow> resolve_ai_profiles(
		const BootFileSource &files, const bms::File &mission);

// What the embedder supplied, gating which steps run. The flags mirror the
// nullable inputs of the shell's setup(): a missing resource root skips every
// file-fed step, a missing item db skips trait/collision resolution, and a
// joiner never spawns its own player here (L spawns on the name-match inside
// the joiner pump).
struct BootParams {
	bool is_joiner = false;
	bool playable = false; // spawn the host's own player after load
	bool has_resource_root = false;
	bool has_item_db = false;
	bool has_terrain = false;
	bool has_terrain_til = false;
	bool has_wac = false; // a wac basename was authored
};

// The boot steps, in the exact order run_mission_boot invokes them. Every
// functor is required when its gate can pass; bodies own their own warnings.
struct BootSteps {
	// Seat/mount specs install before promotion (they persist across resets).
	std::function<void()> install_seat_specs; // root && item_db
	// The .aip profiles per mission ai_textfile — without them, unscripted AI
	// vehicles crawl at the promote stand-in speed and SM weapons stay unarmed.
	std::function<void()> install_ai_profiles; // root
	// The mission's raw .til bytes BEFORE load so the host bring-up streams
	// the S2C 0x45 terrain-tile load to joiners (net-re §5.37).
	std::function<void()> install_terrain_til; // has_terrain_til
	// The per-mission RTXT table retail's NetPacket_WriteBriefingText reads
	// for world-stream phase 6 (S2C 0x7E). Runs unconditionally: empty bytes
	// clear the retained table.
	std::function<void()> install_mission_text;
	// Load + promote the mission; false aborts the boot (nothing later runs).
	// (The shell re-stamps its presentation/PANM clock right after the boot —
	// the load reset cleared it; an order-free scalar, not a boot step.)
	std::function<bool()> load_mission;
	// Ground the AI on the supplied terrain.
	std::function<void()> install_terrain_field; // has_terrain
	// SndProf.def -> the footstep/foley/landing/scream slot table.
	// [orig: SoundProfile_LoadAll @ 0x527490 from Game_InitSubsystems]
	std::function<void()> install_sound_profiles; // root
	// The infantry clip set (.adm -> .bad root-motion tracks).
	std::function<void()> install_infantry_anim; // root
	// Mission WAC scripts [orig: WacScript_InitAndLoad]; absent files skip.
	std::function<void()> install_wac; // root && has_wac
	// The host's own player as an authoritative pool-0 entity (ADR 0012 /
	// net-re §5.2b) — after load (the spawn needs the AI system wired). A
	// joiner's L spawns on the name-match instead.
	std::function<void()> spawn_local_player; // playable && !is_joiner
	// Per-entity grounding: each soldier's OWN model .adm (D-INF-6). After
	// the NPC promote AND the player spawn so both are covered.
	std::function<void()> resolve_infantry_adm; // root && item_db
	// items.def wire traits onto every entity + the replica-pipeline class
	// table (D-NET-97; §5.10b).
	std::function<void()> resolve_item_traits; // item_db
	// The sim's own .3di source (ADR 0028) for the collision resolve below.
	std::function<void()> install_asset_root; // item_db && root
	// World-object collision instances (BVOL/BPLN) [orig: the movement
	// collision resolver @0x4b2bd0 + the query set; §15].
	std::function<void()> resolve_collision; // item_db
	// Mission-start portal init over the occlusion models just attached.
	// [orig: Terrain_InitBuildingPortals @ 0x5c7480 from Game_StartMission
	//  @ 0x525e11]
	std::function<void()> occlusion_init; // item_db
	// Armory table (weapon.def) — the 0x5A ammo resolve + 0x2F filter source;
	// the stashed mission loadout/availability chunks promote INSIDE it
	// through the witnessed SP-vs-net gate (S7b) [orig: Mission_LoadBMSFile
	// @0x40F4E0 — gate @0x40f694].
	std::function<void()> load_weapon_table; // root
	// Ballistics table (ammo.def) + round_type resolve; false = not loaded.
	std::function<bool()> load_ammo_table; // root
	// Seed each NPC's anim-fire weapon (the D-AI-5 host seed) — only against
	// a loaded ammo table.
	std::function<void()> resolve_ai_weapons; // root && ammo ok && item_db
};

enum class BootAbort {
	kNone = 0,
	kLoadFailed, // load_mission returned false; no later step ran
};

// Run the boot sequence over the gates. The caller composes presentation
// (index/present passes/transform capture) around this — those are shell
// node concerns, not sim boot.
BootAbort run_mission_boot(const BootParams &params, const BootSteps &steps);

} // namespace opennova::mission
