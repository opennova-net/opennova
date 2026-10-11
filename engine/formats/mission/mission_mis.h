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

// The room the stock reader gives a name, less its terminator: it copies the value with no bound into a
// fixed slot of the record, an event's `name` 64 bytes (its `attrib` @+64 of the 732-byte record), an
// area trigger's `description` 256 (its `attrib` @+256 of 312), a group's 256 (of 272), a layer's 256
// (of 260) and a path's 1280 (its `attrib` @+1280 of 1548), so a longer value runs into the fields after
// it [orig: MisLdr_ParseMisLine @0x100017b0 (the strcpys @0x10003d58 event, @0x10004328 areatrig,
// @0x100040b8 group, @0x1000427a layer, @0x10003c1f waypoint), misldr.dll]. A description takes the
// narrowest.
inline constexpr size_t kMisEventNameChars = 63;
inline constexpr size_t kMisDescriptionChars = 255;

// The names the .mis gives a mission's records where the .bms has no room for them (ADR 0046 S24
// "Names"): an event's `name` and an area trigger's and a waypoint path's `description`, by the index
// the parse gives the record (an event by its place in the file's events, an area trigger by its place
// in the file's areatrig sections, a path by its record index). An empty name is none; an index past
// a list is none. A group's description is its name in the .bms record (bms.h GroupRecord), so it is
// not here. The writer writes at most kMisEventNameChars of an event's name and kMisDescriptionChars of
// a description, and a value as mis_string carries it (a ';' as ',', a double quote as ').
struct MisNames {
	std::vector<std::string> events;
	std::vector<std::string> areas;
	std::array<std::string, bms::kWaypointRecordCount> paths{};
};

} // namespace opennova::mission

namespace opennova::mission::detail {

bool write_mis_text(const bms::File &file, std::string &out, std::string &error,
                    const std::vector<int32_t> *base_heights = nullptr, const MisNames *names = nullptr);
bool parse_mis_names(const std::string &text, MisNames &out, std::string &error);

// `resolve_item_type` classifies each `begin item` record into its BMS pool by
// items.def TYPE (mission.h MisItemTypeResolver); empty = all records land in
// the generic item pool.
bool parse_mis_text_to_bms(const std::string &text, const MisItemTypeResolver &resolve_item_type,
                           bms::File &out, std::string &error, std::vector<MisIssue> *issues = nullptr);

} // namespace opennova::mission::detail

namespace opennova::mission {

// The public .mis entries (ADR 0043 slice E11: the document facade died; the
// embedder holds the bms::File and calls the two directions itself).
// `base_heights` (optional): editor-sampled terrain heights under each entity,
// 16.16 fixed-point, FLAT in WRITE ORDER (items, buildings, markers, organics).
// When provided, each in-range entry is emitted as that entity's
// `extra_bheight` (the baked base height the original editor subtracts from
// the height-locked absolute z); out-of-range / absent entries fall back to
// the entity's own mis_extra_bheight. See docs/mission/mis-format-re.md
// (D-MIS-4).
// `names` (optional): the names written into the events' `name` and the area triggers' and the
// waypoint paths' `description` lines (empty strings without it); a path with a description is written
// even with no flags.
inline bool write_mis_text(const bms::File &file, std::string &out, std::string &error,
                           const std::vector<int32_t> *base_heights = nullptr, const MisNames *names = nullptr) {
	return detail::write_mis_text(file, out, error, base_heights, names);
}
// The names a .mis text holds (MisNames), read beside parse_mis_text's records by the same indexes.
inline bool parse_mis_names(const std::string &text, MisNames &out, std::string &error) {
	return detail::parse_mis_names(text, out, error);
}
// `resolve_item_type` classifies each `begin item` record into its BMS pool by
// items.def TYPE (MisItemTypeResolver); pass {} when no items.def is loaded
// (all records land in the item pool). `issues` (optional) receives the lines
// read otherwise than written: a group's description that is no group name
// (bms::set_group_name's rules) is skipped.
inline bool parse_mis_text(const std::string &text, const MisItemTypeResolver &resolve_item_type,
                           bms::File &out, std::string &error, std::vector<MisIssue> *issues = nullptr) {
	return detail::parse_mis_text_to_bms(text, resolve_item_type, out, error, issues);
}

} // namespace opennova::mission
