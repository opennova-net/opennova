#pragma once

// Internal to libs/mission — not part of the public interface. Split out of
// mission.cpp (quality campaign W3-1); the bodies are unchanged.
//
// The small shared primitives: fixed-width string fields, header byte accessors,
// the .mis scalar formatters, entity-kind mapping, and the waypoint-record
// helpers. They stay header-inline because every one is a few lines and all four
// TUs below use some of them.

#include "mission/mission.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <io/strutil.h>

namespace opennova::mission::detail {

inline std::string fixed_string(const char *data, size_t max_len) {
	size_t len = 0;
	while (len < max_len && data[len] != '\0') {
		++len;
	}
	return std::string(data, len);
}

inline std::string mis_string(std::string value) {
	for (char &ch : value) {
		if (ch == '\r' || ch == '\n') {
			ch = '|';
		} else if (ch == '"') {
			ch = '\'';
		}
	}
	return value;
}

inline void append_line(std::string &out, const std::string &line = std::string()) {
	out += line;
	out += "\r\n";
}

template <typename... Args>
inline void append_kv(std::string &out, Args &&...args) {
	std::ostringstream stream;
	(stream << ... << args);
	append_line(out, stream.str());
}

inline const uint8_t *header_bytes(const bms::Header &header) {
	return reinterpret_cast<const uint8_t *>(&header);
}

inline uint32_t read_u32_at(const bms::Header &header, size_t offset) {
	const uint8_t *bytes = header_bytes(header);
	return static_cast<uint32_t>(bytes[offset]) |
	       (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
	       (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
	       (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

// The .mis writer emits gen_def_val1..4 from raw header offsets 264..276; the parser stores them
// back through the named fields. Pin the correspondence so a Header layout change cannot silently
// break the symmetry. (Header is #pragma pack(1), so offsetof is exact.)
static_assert(offsetof(bms::Header, health) == 264, "gen_def_val1 <-> Header.health @264");
static_assert(offsetof(bms::Header, mana) == 268, "gen_def_val2 <-> Header.mana @268");
static_assert(offsetof(bms::Header, music) == 272, "gen_def_val3 <-> Header.music @272");
static_assert(offsetof(bms::Header, reverb) == 276, "gen_def_val4 <-> Header.reverb @276");
// water_level / fog_level are RAW u32s at 152/156 straddling the named u16 fields; both sides use
// raw-offset access, anchored here.
static_assert(offsetof(bms::Header, water_override) == 152, "water_level u32 starts @152");
static_assert(offsetof(bms::Header, fog_override) == 158, "fog_level u32 (@156) ends in fog_override @158");

inline int32_t read_i32_at(const uint8_t *bytes, size_t offset) {
	return static_cast<int32_t>(static_cast<uint32_t>(bytes[offset]) |
	                            (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
	                            (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
	                            (static_cast<uint32_t>(bytes[offset + 3]) << 24));
}

inline void write_i32_at(uint8_t *bytes, size_t offset, int32_t value) {
	const uint32_t v = static_cast<uint32_t>(value);
	bytes[offset] = static_cast<uint8_t>(v & 0xFF);
	bytes[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
	bytes[offset + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
	bytes[offset + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline int header_time_to_hhmm(uint16_t encoded) {
	const int hours = (encoded >> 8) & 0xFF;
	const int frac = encoded & 0xFF;
	const int minutes = (frac * 60 + 128) / 256;
	return hours * 100 + minutes;
}

inline std::string four_digit(int value) {
	std::ostringstream stream;
	if (value < 0) {
		stream << value;
		return stream.str();
	}
	stream.width(4);
	stream.fill('0');
	stream << value;
	return stream.str();
}

inline uint32_t packed_rgb(const uint8_t rgb[3]) {
	return static_cast<uint32_t>(rgb[2]) |
	       (static_cast<uint32_t>(rgb[1]) << 8) |
	       (static_cast<uint32_t>(rgb[0]) << 16);
}

inline uint16_t combined_u16(uint8_t lo, uint8_t hi) {
	return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
}

inline int32_t combined_i32_from_i16(int16_t lo, int16_t hi) {
	return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(lo)) |
	                            (static_cast<uint32_t>(static_cast<uint16_t>(hi)) << 16));
}

inline int32_t byte_from_i16(int16_t value, int index) {
	return static_cast<int32_t>((static_cast<uint16_t>(value) >> (index * 8)) & 0xFF);
}

inline int32_t byte_from_i32(int32_t value, int index) {
	return static_cast<int32_t>((static_cast<uint32_t>(value) >> (index * 8)) & 0xFF);
}

inline int item_id_to_bms_type_id(int item_id) {
	return item_id >= kItemIdOffset ? item_id - kItemIdOffset : item_id;
}

inline int bms_type_id_to_item_id(int type_id) {
	return type_id + kItemIdOffset;
}

inline bms::ItemType to_bms_type(EntityKind kind) {
	switch (kind) {
		case EntityKind::Marker: return bms::ItemType::Marker;
		case EntityKind::Item: return bms::ItemType::Item;
		case EntityKind::Building: return bms::ItemType::Building;
		case EntityKind::Organic: return bms::ItemType::Organic;
	}
	return bms::ItemType::Item;
}

inline bool from_int_kind(int kind, EntityKind &out) {
	switch (kind) {
		case 0:
			out = EntityKind::Marker;
			return true;
		case 1:
			out = EntityKind::Item;
			return true;
		case 2:
			out = EntityKind::Building;
			return true;
		case 3:
			out = EntityKind::Organic;
			return true;
		default:
			return false;
	}
}

// preserve_over_count keeps a shipped on-disk marker_count that exceeds the 32-slot capacity verbatim
// (CP19.bms ships one == 39) so an UNTOUCHED path round-trips byte-exact. parse_waypoint_record records
// the over-count; sync_counts runs this on every load/save and passes true to preserve it. An AUTHORED
// edit (apply_waypoint_path_to_record / repair_waypoint_marker_references) rewrites the marker list, so
// the stored over-count no longer describes the data: those call sites pass false to resync marker_count
// to the real slot count. (Without that, a reorder or flag-only edit on a saturated 32-marker path would
// keep advertising 39 markers, and the engine would walk 7 phantom waypoints.)
inline void resize_waypoint_padding(bms::WaypointRecord &record, bool preserve_over_count) {
	const size_t slots = std::min<size_t>(record.waypoint_numbers.size(), kMaxWaypointPathMarkers);
	if (record.waypoint_numbers.size() != slots) {
		record.waypoint_numbers.resize(slots);
	}
	const bool keep_over_count = preserve_over_count
			&& record.marker_count > kMaxWaypointPathMarkers
			&& slots == kMaxWaypointPathMarkers;
	if (!keep_over_count) {
		record.marker_count = static_cast<uint32_t>(slots);
	}
	const size_t used = slots * sizeof(uint32_t);
	record.padding.assign(128 - used, 0);
}

inline WaypointPath to_path(const bms::WaypointRecord &record, size_t index) {
	WaypointPath out;
	out.index = index;
	out.flags = static_cast<int>(record.flags);
	out.marker_indices.reserve(record.waypoint_numbers.size());
	for (uint32_t marker_index : record.waypoint_numbers) {
		out.marker_indices.push_back(static_cast<int>(marker_index));
	}
	return out;
}

inline bool validate_waypoint_path(const bms::File &file,
                            size_t index,
                            const std::vector<int> &marker_indices,
                            std::string &error) {
	if (index >= file.waypoint_records.size()) {
		error = "Waypoint path index out of range";
		return false;
	}
	if (marker_indices.size() > kMaxWaypointPathMarkers) {
		error = "Waypoint path marker count exceeds 32";
		return false;
	}
	for (int marker_index : marker_indices) {
		if (marker_index < 0 || static_cast<size_t>(marker_index) >= file.markers.size()) {
			error = "Waypoint path marker index out of range";
			return false;
		}
	}
	return true;
}

inline void apply_waypoint_path_to_record(bms::WaypointRecord &record, const std::vector<int> &marker_indices, int flags) {
	record.flags = static_cast<bms::WaypointFlags>(static_cast<uint32_t>(flags));
	record.waypoint_numbers.clear();
	record.waypoint_numbers.reserve(marker_indices.size());
	for (int marker_index : marker_indices) {
		record.waypoint_numbers.push_back(static_cast<uint32_t>(marker_index));
	}
	resize_waypoint_padding(record, /*preserve_over_count=*/false); // authored edit: count tracks the new list
}

inline void repair_waypoint_marker_references(bms::File &file, size_t removed_index) {
	for (bms::WaypointRecord &record : file.waypoint_records) {
		bool changed = false;
		std::vector<uint32_t> repaired;
		repaired.reserve(record.waypoint_numbers.size());
		for (uint32_t marker_index : record.waypoint_numbers) {
			if (marker_index == removed_index) {
				changed = true;
				continue;
			}
			if (marker_index > removed_index) {
				repaired.push_back(marker_index - 1);
				changed = true;
			} else {
				repaired.push_back(marker_index);
			}
		}
		if (changed) {
			record.waypoint_numbers = repaired;
			resize_waypoint_padding(record, /*preserve_over_count=*/false); // marker delete rewrote the list
		}
	}
}

inline std::string unknown_label(const char *prefix, int value) {
	return std::string(prefix) + "(" + std::to_string(value) + ")";
}

inline std::string extension_lower(const std::string &path) {
	const size_t slash = path.find_last_of("/\\");
	const size_t dot = path.find_last_of('.');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
		return {};
	}
	std::string ext = path.substr(dot + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return ext;
}

// Copy into a fixed-width on-disk field that may use ALL dest_size bytes (no reserved NUL
// terminator). The format's name1/name2 are 8-byte slots a shipped mission can fill completely,
// so copy_cstr (which forces dest[dest_size-1] = '\0') would drop the 8th byte and silently
// truncate an 8-char name on every property round-trip. Values longer than the field are cut to
// dest_size; shorter values zero-pad the remainder. fixed_string reads it back symmetrically.
inline void copy_fixed_field(char *dest, size_t dest_size, const std::string &value) {
	const size_t copy_len = std::min(dest_size, value.size());
	if (copy_len > 0) {
		std::memcpy(dest, value.data(), copy_len);
	}
	if (copy_len < dest_size) {
		std::memset(dest + copy_len, 0, dest_size - copy_len);
	}
}

inline void copy_cstr(char *dest, size_t dest_size, const std::string &value) {
	if (dest_size == 0) {
		return;
	}
	const size_t copy_len = std::min(dest_size - 1, value.size());
	std::memcpy(dest, value.data(), copy_len);
	dest[copy_len] = '\0';
	if (copy_len + 1 < dest_size) {
		std::memset(dest + copy_len + 1, 0, dest_size - copy_len - 1);
	}
}

} // namespace opennova::mission::detail
