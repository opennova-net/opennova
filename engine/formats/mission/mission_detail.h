#pragma once

// Internal to engine/formats/mission — not part of the public interface.
//
// The small shared primitives: fixed-width string fields, header byte accessors,
// the .mis scalar formatters, entity-kind mapping, and the waypoint-record
// helpers. They stay header-inline because every one is a few lines and all four
// TUs below use some of them.

#include <formats/def/reserved_items.h>
#include <formats/mission/mission.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <base/io/le.h>
#include <base/io/strutil.h>

namespace opennova::mission::detail {

using strutil::fixed_string;

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
	return io::read_u32_le(header_bytes(header) + offset);
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
	return static_cast<int32_t>(io::read_u32_le(bytes + offset));
}

inline void write_i32_at(uint8_t *bytes, size_t offset, int32_t value) {
	const uint32_t v = static_cast<uint32_t>(value);
	bytes[offset] = static_cast<uint8_t>(v & 0xFF);
	bytes[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
	bytes[offset + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
	bytes[offset + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
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

// preserve_over_count keeps a shipped on-disk marker_count that exceeds the 32-slot capacity verbatim
// (CP19.bms ships one == 39) so an UNTOUCHED path round-trips byte-exact. parse_waypoint_record records
// the over-count; sync_counts runs this on every load/save and passes true to preserve it. A .mis read
// passes false (its path lists are the slots). An edit of a path's stops lays the path out from its
// markers instead (bms_edit's lay_out_waypoint_path: the count the markers that carry it, D-MIS-6).
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

inline bool validate_waypoint_path(const bms::File &file,
                            size_t index,
                            const std::vector<int> &marker_indices,
                            std::string &error) {
	if (index >= file.waypoint_records.size()) {
		error = "Waypoint path index out of range";
		return false;
	}
	// A path holds as many stops as waypoint markers carry it (the original's count has no bound [orig:
	// JOTACmed.exe sub_44CFD0 @ 0x44cfd0, the 6005 item's markers, a number of 0 none]); a marker carries
	// one place on one path.
	if (index == 0 && !marker_indices.empty()) {
		error = "Path 0 holds no stops: a marker carrying 0 is on no path.";
		return false;
	}
	for (size_t i = 0; i < marker_indices.size(); ++i) {
		const int marker_index = marker_indices[i];
		if (marker_index < 0 || static_cast<size_t>(marker_index) >= file.markers.size()) {
			error = "Waypoint path marker index out of range";
			return false;
		}
		if (file.markers[size_t(marker_index)].type_id != def::DEF_TYPE_WAYPOINT) {
			error = "Marker " + std::to_string(marker_index) + " is no waypoint marker (6005): a path's stops are those alone.";
			return false;
		}
		if (std::find(marker_indices.begin(), marker_indices.begin() + static_cast<std::ptrdiff_t>(i), marker_index) !=
		    marker_indices.begin() + static_cast<std::ptrdiff_t>(i)) {
			error = "A marker is named twice: a marker carries one place on a path.";
			return false;
		}
	}
	return true;
}

inline std::string unknown_label(const char *prefix, int value) {
	return std::string(prefix) + "(" + std::to_string(value) + ")";
}

// Copy into a fixed-width on-disk field that may use ALL dest_size bytes (no reserved NUL
// terminator). The format's name1/name2 are 8-byte slots a shipped mission can fill completely,
// so a strncpy-style copy that forces dest[dest_size-1] = '\0' would drop the 8th byte and
// silently truncate an 8-char name on every property round-trip. Values longer than the field
// are cut to dest_size; shorter values zero-pad the remainder. fixed_string reads it back
// symmetrically.
inline void copy_fixed_field(char *dest, size_t dest_size, const std::string &value) {
	const size_t copy_len = std::min(dest_size, value.size());
	if (copy_len > 0) {
		std::memcpy(dest, value.data(), copy_len);
	}
	if (copy_len < dest_size) {
		std::memset(dest + copy_len, 0, dest_size - copy_len);
	}
}

} // namespace opennova::mission::detail
