#pragma once

// Internal to engine/runtime/mission — not part of the public interface. Split out of
// mission.cpp (quality campaign W3-1); the bodies are unchanged.
//
// The .mis text format, one TU per direction: mission_mis_writer.cpp emits it and
// mission_mis_parser.cpp reads it. Only these two entry points cross a TU boundary;
// every section writer and token parser stays private to its own file.

#include <formats/mission/mission.h>

#include <string>
#include <vector>

namespace opennova::mission::detail {

bool write_mis_text(const bms::File &file, std::string &out, std::string &error,
                    const std::vector<int32_t> *base_heights = nullptr);

// `resolve_item_type` classifies each `begin item` record into its BMS pool by
// items.def TYPE (mission.h MisItemTypeResolver); empty = all records land in
// the generic item pool.
bool parse_mis_text_to_bms(const std::string &text, const MisItemTypeResolver &resolve_item_type,
                           bms::File &out, std::string &error);

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
inline bool write_mis_text(const bms::File &file, std::string &out, std::string &error,
                           const std::vector<int32_t> *base_heights = nullptr) {
	return detail::write_mis_text(file, out, error, base_heights);
}
// `resolve_item_type` classifies each `begin item` record into its BMS pool by
// items.def TYPE (MisItemTypeResolver); pass {} when no items.def is loaded
// (all records land in the item pool).
inline bool parse_mis_text(const std::string &text, const MisItemTypeResolver &resolve_item_type,
                           bms::File &out, std::string &error) {
	return detail::parse_mis_text_to_bms(text, resolve_item_type, out, error);
}

} // namespace opennova::mission
