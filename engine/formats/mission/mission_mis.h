#pragma once

// Internal to engine/formats/mission — not part of the public interface.
//
// The .mis text format, one TU per direction: mission_mis_writer.cpp emits it and
// mission_mis_parser.cpp reads it. Only these two entry points cross a TU boundary;
// every section writer and token parser stays private to its own file.

#include <formats/mission/mission.h>

#include <array>
#include <string>
#include <vector>

namespace opennova::mission {

// A line of a .mis the reader takes otherwise than it reads: its line (1-based) and why, in words.
struct MisIssue {
	size_t line = 0;
	std::string message;
};

// The room a name has, less its terminator. Both stock readers copy the value with no bound into a fixed
// slot of the record, an event's `name` 64 bytes (its `attrib` @+64), an area trigger's, a group's and a
// layer's `description` 256 and a path's 1280, so a longer value runs into the fields after it [orig:
// MisLdr_ParseMisLine @0x100017b0 (the strcpys @0x10003d58 event, @0x10004328 areatrig, @0x100040b8 group,
// @0x1000427a layer, @0x10003c1f waypoint), misldr.dll; MIS_ParseToken @0x450e50 (the copies @0x453962
// event, @0x4540da areatrig, @0x453d35 group, @0x453f12 layer, @0x453802 waypoint), JOTACmed.exe]. The
// original editor's dialogs take less: an event's name 59 characters, an area's, a path's and a group's
// description 249 [orig: JOTACmed.exe sub_4118D0 @0x411917 (60), sub_40F360 @0x40f37f, sub_40EA10
// @0x40ea30, sub_40FC60 @0x40fc7f (250)], so a longer name loses its tail at the dialog's next OK. A name is
// written to the narrower.
inline constexpr size_t kMisEventNameChars = 59;
inline constexpr size_t kMisDescriptionChars = 249;
// The original editor's area slots, an area's zone id naming its slot; it writes slots 1..99 [orig:
// JOTACmed.exe MIS_SaveDocument @0x454630, the loop @0x454daf; sub_44D240 @0x44d240, the 100 slots].
inline constexpr size_t kMisAreaSlots = 100;

// The names the .mis gives a mission's records where the .bms has no room for them (ADR 0046 S24
// "Names"): an event's `name` and an area trigger's and a waypoint path's `description`. An event
// by its number (`begin event N`, its place among the events), an area by its zone id (`begin
// areatrig N`, the original editor's slot, 1..99; slot 0 is never one), a path by its record index
// (`begin waypoint N`). An empty name is none; an index past a list is none. A group's description
// is its name in the .bms record (bms.h GroupRecord), so it is not here. A name is one
// mis_name_allowed takes; the writer writes at most kMisEventNameChars of an event's name and
// kMisDescriptionChars of a description, cut before a UTF-8 character the width would split. The
// variables a name may be given: V0..V255, the numbered words a script's V# reaches (the compiler
// caps a V# at 255 [orig: WacScript_ResolveParameter @0x4F2AA4..0x4F2B06, "V# too big" @0x4f2ac5])
// and the mission's variable trigger and action name by the same number. The bank's words past them
// hold the script's own VAR declarations, which the script names itself
// (runtime/world/var_store.h).
inline constexpr int32_t kVariableNameIndexMax = 255;
inline constexpr size_t kVariableNameCount = size_t(kVariableNameIndexMax) + 1;

// A mission's variable names are the editor's alone (ADR 0046 S24 "Names"): neither the .bms, the
// game nor the original editor holds one. They ride the .mis as lines both original readers skip,
// the line's first token starting with '/' [orig: JOTACmed.exe sub_463A90 @ 0x463b3a; MisLdr's
// tokenize_mis_text port, misldr.dll sub_10007E40 @ 0x10007e40], one a name: `//var <index>
// "<name>"` (kMisVariableLine). The table after the groups, where they rode before S26, is the
// original editor's wav list (bms.h WavListRecord, D-MIS-13).
inline constexpr const char *kMisVariableLine = "//var";
struct MisNames {
	std::vector<std::string> events;
	std::array<std::string, kMisAreaSlots> areas{};
	std::array<std::string, bms::kWaypointRecordCount> paths{};
	std::array<std::string, kVariableNameCount> variables{};
};

// Whether `name` is one the .mis carries as written in a slot of `width` bytes (kMisEventNameChars
// for an event's name, kMisDescriptionChars for an area's or a path's description): no longer than
// `width` and no character bms::mis_value_char refuses (a control character, a double quote, a
// ';'), the rule a group's and a variable's name keep (a variable's at most kMisDescriptionChars).
// A byte past 0x7F (UTF-8) is taken: both readers keep bytes as read. Refused: false, `error` says
// why. The editor's name setters refuse what this refuses; nothing rewrites a name.
inline bool mis_name_allowed(const std::string &name, size_t width, std::string &error) {
	if (name.size() > width) {
		error = "A name holds " + std::to_string(width) + " bytes at most (the original editor's dialog)";
		return false;
	}
	for (char ch : name) {
		if (!bms::mis_value_char(ch)) {
			error = "A name takes no control character, double quote or ';', which the .mis cannot carry";
			return false;
		}
	}
	return true;
}

} // namespace opennova::mission

namespace opennova::mission::detail {

bool write_mis_text(const bms::File &file, std::string &out, std::string &error,
                    const std::vector<int32_t> *base_heights = nullptr, const MisNames *names = nullptr);
bool parse_mis_names(const std::string &text, MisNames &out, std::string &error, std::vector<MisIssue> *issues);

// `resolve_item_type` classifies each `begin item` record into its BMS pool by
// items.def TYPE (mission.h MisItemTypeResolver); empty = all records land in
// the generic item pool.
bool parse_mis_text_to_bms(const std::string &text, const MisItemTypeResolver &resolve_item_type,
                           bms::File &out, std::string &error, std::vector<MisIssue> *issues = nullptr);

} // namespace opennova::mission::detail

namespace opennova::mission {

// The public .mis entries (ADR 0043 slice E11: the document facade died; the
// embedder holds the bms::File and calls the two directions itself).
// The entities' heights go as the file's mis_frame holds them (the frames below). A file in
// the .mis's frame (parse_mis_text's) writes each z, base and lock as it stands, whatever
// `base_heights`. A file in the .bms's frame writes them as the original editor's save does:
// `base_heights` (optional) is the terrain's height under each entity, 16.16 fixed-point,
// FLAT in WRITE ORDER (items, buildings, markers, organics), as the original editor samples
// it (mis_terrain_height); an unlocked entity's z is written less its height there, its base
// as it stands, and no height_lock line; a locked entity's z as it stands (absolute), its
// base plus the terrain's height, and its height_lock as it stands [orig: JOTACmed.exe
// sub_44E5F0 @ 0x44e5f0, the .bms export's undo; sub_44E640 @ 0x44e640, from
// MIS_SaveDocument @ 0x4546b5; the height_lock line written for a lock alone,
// MIS_SaveDocument @ 0x455889..0x45589a]. An entity past `base_heights` (all of them with
// none) has no terrain height known and is written locked, its z as it stands, which the
// original reads exact over whatever terrain it loads, its base as it stands and
// `height_lock 1` (its own lock if it has one): a divergence, the original always has its
// terrain (D-MIS-17). See docs/mission/mis-format-re.md (D-MIS-4).
// `names` (optional): the names written into the events' `name` and the area triggers' and the
// waypoint paths' `description` lines (empty strings without it); a path is written for a stop or a
// description, whatever its flags, as the original editor writes it. Refused (false, `error` names the area)
// when an area's zone id is outside 1..99 or two areas share one: the original's .mis has an area slot per
// zone id 1..99 alone (D-MIS-12).
inline bool write_mis_text(const bms::File &file, std::string &out, std::string &error,
                           const std::vector<int32_t> *base_heights = nullptr, const MisNames *names = nullptr) {
	return detail::write_mis_text(file, out, error, base_heights, names);
}
// The names a .mis text holds (MisNames), read beside parse_mis_text's records by the same indexes. `issues`
// (optional) receives the lines read otherwise than written: an areatrig number outside 1..99 (its name
// skipped) and a second section of a zone id (its description replacing the first's, as the original's slot).
inline bool parse_mis_names(const std::string &text, MisNames &out, std::string &error,
                            std::vector<MisIssue> *issues = nullptr) {
	return detail::parse_mis_names(text, out, error, issues);
}
// `resolve_item_type` classifies each `begin item` record into its BMS pool by
// items.def TYPE (MisItemTypeResolver); pass {} when no items.def is loaded
// (all records land in the item pool). `issues` (optional) receives the lines
// read otherwise than written: a group's description that is no group name
// (bms::set_group_name's rules) is skipped; an areatrig number outside 1..99 makes
// no area and a second section of a zone id goes to the first's area, as the
// original's slot; a key the original editor's reader refuses in its section is
// skipped (the original fails the load); a teleport_target, wav_list or wind
// section, which the original reads and the .bms has no room for, is walked.
inline bool parse_mis_text(const std::string &text, const MisItemTypeResolver &resolve_item_type,
                           bms::File &out, std::string &error, std::vector<MisIssue> *issues = nullptr) {
	return detail::parse_mis_text_to_bms(text, resolve_item_type, out, error, issues);
}

// The terrain the original editor measures an item's base height over: its save adds the terrain's height under a
// height-locked item to the item's z and to its base (extra_bheight), its read takes it off again [orig: JOTACmed.exe
// sub_44E640 @ 0x44e640, from MIS_SaveDocument @ 0x4546b5; sub_44E6A0 @ 0x44e6a0, from MIS_LoadDocument @
// 0x45460e], and its .bms export adds it to every item's z [orig: sub_44E5A0 @ 0x44e5a0], so the .bms's z less the
// terrain's height there is the item's z in the editor's frame (write_mis_text's `base_heights`,
// mis_items_to_bms_frame's `terrain`). `heights`: the .cpt's height atlas, 1024 x 1024 depth words in the
// file's order, the grid the game loads (CptFile::depth_buffer, the .cpt the .trn's polytrn_polydata names), which
// the original editor holds as twice each word [orig: JOTACmed.exe sub_44C2B0 @ 0x44c2b0, its DPTH arm @ 0x44c64b
// and CDEP arm @ 0x44c5e7]. The rest is the .trn's own (the original editor reads the .trn alone): `sectors` its
// 16 x 16 sector grid, row by row, extended past the rows and the width read as trn_extend_sector_grid extends it
// [orig: sub_464BD0 @ 0x464bd0]; `origin_x`/`origin_y` polytrn_origin; the wraps polytrn_wrapx and polytrn_wrapy;
// `scale` polytrn_scale, the original editor's height factor (scale / 128, 128 where the .trn names none: no shipped
// one does; the game reads the key for its file check alone) [orig: sub_463C90 @ 0x463c90, its polytrn_scale arm @
// 0x464a38; the default, sub_464B60 @ 0x464bb6].
struct MisTerrain {
	const uint16_t *heights = nullptr;
	const int *sectors = nullptr;
	int32_t origin_x = 0;
	int32_t origin_y = 0;
	bool wrap_x = false;
	bool wrap_y = false;
	int32_t scale = 128;
};
// The terrain's height at mission (x, y), 16.16, as the original editor samples it for an item's base: the sector
// under the point (its grid cell, the cell past the grid's edge clamped to the edge's where the axis does not wrap,
// else wrapped) picks a 512-unit quadrant of the atlas, and the four texels about the point blend bilinearly in
// 16-bit fractions, the height times scale / 128 truncated. 0 where the cell names no sector, and for no terrain.
int32_t mis_terrain_height(const MisTerrain &terrain, int32_t x, int32_t y);

// The original editor's two frames of an item's height (its z) and its base (extra_bheight). Its
// document keeps both relative to the terrain under the item; its .mis save writes a height-locked
// item's both absolute, the terrain's height added [orig: JOTACmed.exe sub_44E640 @ 0x44e640, from
// MIS_SaveDocument @ 0x4546b5], and its read takes it off again [orig: sub_44E6A0 @ 0x44e6a0, from
// MIS_LoadDocument @ 0x45460e]; its .bms export adds it to every item's z and base [orig:
// sub_44E5A0 @ 0x44e5a0, from sub_44F920 @ 0x44fa4e]; and a change of the terrain takes a locked
// item's both to absolute over the old terrain and back to relative over the new [orig: sub_402770
// @ 0x402c9f..0x402ccd], so a locked item keeps its height and its base and an unlocked one follows
// the ground. A bms::File's mis_frame names the frame its entities are in (bms::MisFrame): the
// .bms's (a parsed .bms, a new file), each entity's z absolute (the game's), its mis_extra_bheight
// a locked entity's base absolute (carried as the original carries it through a terrain change: a
// sculpt under it moves it not) and an unlocked one's relative, its mis_height_lock 0 unless a .mis
// locked it; or the .mis's as written (parse_mis_text's), an unlocked item's z and base relative, a
// locked one's absolute. mis_items_to_bms_frame takes a file in the .mis's frame to the .bms's over
// `terrain`: the original's read and its .bms export, which leave a locked entity's z and base
// absolute and add the terrain's height under an unlocked one to its z (the export adds it to the
// base too, which reaches no .bms byte, so an unlocked base stays relative). A file already in the
// .bms's frame is left as it is, so the height is never added twice; so is one given no terrain
// (the heights or the sectors null): it stays in the .mis's frame, which the writer writes back as
// it stands.
void mis_items_to_bms_frame(bms::File &file, const MisTerrain &terrain);

// The original editor's areas (D-MIS-14): an area is no section of its .mis but the box its
// type-6013 items of one zone id span, the item's wpnumber of 1..99 its zone id, the least x, y and
// z and the greatest of every such item [orig: JOTACmed.exe sub_44D240 @ 0x44d240: the 6013 def @
// 0x44d296, the range @ 0x44d2a9, the bounds
// @ 0x44d2d8..0x44d444], the `areatrig` section of its id giving it its flags (`attrib`) and its
// description; its .bms export writes the area where the item of the least x (the first of a tie)
// holds an extra_valmode of 0 [orig: sub_44F920 @ 0x450a16..0x450ab3, the test @ 0x450a3a], in zone
// id order, and leaves the 6013 items out of the markers [orig: @ 0x450785]. rebuild_mis_areas
// makes `file`'s areas so from its mis_area_markers, each area's flags those an area of its id held
// before (an `areatrig` section's); an area of no marker goes. parse_mis_text reads a 6013 item so
// (none of the pools) and makes the areas in the file's frame; mis_items_to_bms_frame makes them
// again once the markers stand in the .bms's. The writer writes an area as two 6013 items at the
// box's least and greatest corners, locked (their z absolute), with its zone id.
inline constexpr int32_t kMisAreaMarkerType = 6013;
void rebuild_mis_areas(bms::File &file);

} // namespace opennova::mission
