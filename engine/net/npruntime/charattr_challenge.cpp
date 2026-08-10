// The boot-loaded `g_CharAttr[16]` character-attribute table and the two things the
// anti-cheat challenge path does with it: hash one 124-byte CHARACTER row (S2C 0x39 ->
// C2S 0x1C) and clear one property across every row (S2C 0x41). The IDB's neighbouring
// AnimMap_* names are misnomers -- this is charattr.def data, not .adm animation data.
// [orig: CharAttr_LoadFromDef @0x412140 (the loader this file reproduces),
//  AnimMap_GetSlotChecksum @0x412aa0 (row select + hash),
//  CharAttr_SetSlotProperty @0x412890 (the property clear)]
#include "npruntime/charattr_challenge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <io/strutil.h>

namespace opennova::np {

namespace {

enum class ValueKind : uint8_t {
	Float,
	Integer,
	Attributes,
};

struct Field {
	std::string_view name;
	std::size_t offset;
	ValueKind kind;
	uint16_t seen_bit;
};

// The twelve scalar keys plus tokenized ATTRIBUTES, at their byte offsets inside the
// 124-byte row. Offsets 0 and 4 are the active dword and the embedded class id, written
// by the section header; +60..123 stay zero for the stock table.
// [orig: CharAttr_LoadFromDef @0x412140 -- the key list and its per-field stores]
constexpr std::array<Field, 13> kFields{{
		{"STEALTH", 8, ValueKind::Float, 1u << 0},
		{"HPBONUS", 12, ValueKind::Float, 1u << 1},
		{"MANABONUS", 16, ValueKind::Float, 1u << 2},
		{"RECOIL_MUTE", 20, ValueKind::Float, 1u << 3},
		{"RELOAD_MUTE", 24, ValueKind::Float, 1u << 4},
		{"XHAIR_MUTE", 28, ValueKind::Float, 1u << 5},
		{"XHAIRDX_MUTE", 32, ValueKind::Float, 1u << 6},
		{"SCOPE_MUTE", 36, ValueKind::Float, 1u << 7},
		{"ATTRIBUTES", 40, ValueKind::Attributes, 1u << 8},
		{"JUNGLE_CAMMO", 44, ValueKind::Integer, 1u << 9},
		{"DESERT_CAMMO", 48, ValueKind::Integer, 1u << 10},
		{"ARCTIC_CAMMO", 52, ValueKind::Integer, 1u << 11},
		{"RUN_MODIFIER", 56, ValueKind::Integer, 1u << 12},
}};

bool ascii_iequals(std::string_view a, std::string_view b) { return opennova::strutil::iequals(a, b); }

bool ascii_istarts_with(std::string_view value, std::string_view prefix) {
	return value.size() >= prefix.size() &&
			ascii_iequals(value.substr(0, prefix.size()), prefix);
}

std::string_view trim(std::string_view value) {
	while (!value.empty() &&
	       std::isspace(static_cast<unsigned char>(value.front())) != 0) {
		value.remove_prefix(1);
	}
	while (!value.empty() &&
	       std::isspace(static_cast<unsigned char>(value.back())) != 0) {
		value.remove_suffix(1);
	}
	return value;
}

void write_u32(CharAttrChallengeRow &row, std::size_t offset, uint32_t value) {
	row[offset + 0] = static_cast<uint8_t>(value);
	row[offset + 1] = static_cast<uint8_t>(value >> 8);
	row[offset + 2] = static_cast<uint8_t>(value >> 16);
	row[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint32_t read_u32(const CharAttrChallengeRow &row, std::size_t offset) {
	return static_cast<uint32_t>(row[offset + 0]) |
			(static_cast<uint32_t>(row[offset + 1]) << 8) |
			(static_cast<uint32_t>(row[offset + 2]) << 16) |
			(static_cast<uint32_t>(row[offset + 3]) << 24);
}

// Retail's config reader converts a type-2 value with atof and stores the float; every
// other numeric key goes through atol. strtod/strtol are the CRT equivalents.
// [orig: ConfigFile_ReadKeyValue @0x75fc90 (the atof/atol split),
//  ConfigFile_FindSectionAndReadValue @0x75ff00 (section reset + first matching key)]
void write_float(CharAttrChallengeRow &row, std::size_t offset, std::string_view value) {
	const std::string terminated(value);
	const double parsed = std::strtod(terminated.c_str(), nullptr);
	const float stored = static_cast<float>(parsed);
	static_assert(sizeof(stored) == sizeof(uint32_t), "retail charattr float is 32-bit");
	uint32_t bits = 0;
	std::memcpy(&bits, &stored, sizeof(bits));
	write_u32(row, offset, bits);
}

void write_integer(CharAttrChallengeRow &row, std::size_t offset, std::string_view value) {
	const std::string terminated(value);
	const long parsed = std::strtol(terminated.c_str(), nullptr, 10);
	write_u32(row, offset, static_cast<uint32_t>(parsed));
}

uint32_t attribute_flag(std::string_view token) {
	// Five named rows plus an empty-name sentinel; the flag is 1 << row index, so the
	// sentinel's slot leaves 0x10 unused.
	// [orig: g_CharAttrAttributeNames @0x813F18, tokenized by CharAttr_LoadFromDef @0x412140]
	if (ascii_iequals(token, "AutoScope")) return 0x01;
	if (ascii_iequals(token, "SpreadBonus")) return 0x02;
	if (ascii_iequals(token, "KnifeBonus")) return 0x04;
	if (ascii_iequals(token, "Medic")) return 0x08;
	if (ascii_iequals(token, "WaterGirl")) return 0x20;
	return 0;
}

void write_attributes(
		CharAttrChallengeRow &row, std::size_t offset, std::string_view value) {
	uint32_t flags = 0;
	std::size_t cursor = 0;
	while (cursor < value.size()) {
		while (cursor < value.size() &&
		       (value[cursor] == ',' ||
		        std::isspace(static_cast<unsigned char>(value[cursor])) != 0)) {
			++cursor;
		}
		const std::size_t begin = cursor;
		while (cursor < value.size() && value[cursor] != ',' &&
		       std::isspace(static_cast<unsigned char>(value[cursor])) == 0) {
			++cursor;
		}
		if (cursor > begin) flags |= attribute_flag(value.substr(begin, cursor - begin));
	}
	write_u32(row, offset, flags);
}

int character_section_index(std::string_view line) {
	line = trim(line);
	if (line.size() < 3 || line.front() != '[' || line.back() != ']') return -1;
	std::string_view name = trim(line.substr(1, line.size() - 2));
	constexpr std::string_view prefix = "CHARACTER";
	if (!ascii_istarts_with(name, prefix)) return -1;
	name.remove_prefix(prefix.size());
	if (name.empty() || name.size() > 2) return -1;
	unsigned value = 0;
	for (char c : name) {
		if (c < '0' || c > '9') return -1;
		value = value * 10 + static_cast<unsigned>(c - '0');
	}
	return value >= 1 && value <= kCharAttrChallengeRowCount
			? static_cast<int>(value - 1)
			: -1;
}

} // namespace

bool parse_charattr_challenge_table(
		const uint8_t *data, std::size_t size, CharAttrChallengeTable &out) {
	out = {};
	if (data == nullptr || size == 0) return false;

	// ConfigFile is C-string-backed in retail. Ignore bytes after the first NUL.
	const void *nul = std::memchr(data, 0, size);
	if (nul != nullptr) {
		size = static_cast<std::size_t>(
				static_cast<const uint8_t *>(nul) - data);
	}
	if (size == 0) return false;

	const std::string_view text(
			reinterpret_cast<const char *>(data), size);
	std::array<bool, kCharAttrChallengeRowCount> section_seen{};
	std::array<uint16_t, kCharAttrChallengeRowCount> field_seen{};
	int current_section = -1;

	for (std::size_t cursor = 0; cursor < text.size();) {
		const std::size_t line_end = text.find_first_of("\r\n", cursor);
		std::string_view line = text.substr(
				cursor, line_end == std::string_view::npos
						? text.size() - cursor
						: line_end - cursor);
		if (line_end == std::string_view::npos) {
			cursor = text.size();
		} else {
			cursor = line_end + 1;
			if (cursor < text.size() && text[line_end] == '\r' &&
			    text[cursor] == '\n') {
				++cursor;
			}
		}

		line = trim(line);
		if (line.empty() || ascii_istarts_with(line, "//")) continue;
		if (line.front() == '[') {
			const int index = character_section_index(line);
			if (index < 0 || section_seen[static_cast<std::size_t>(index)]) {
				current_section = -1;
				continue;
			}
			current_section = index;
			section_seen[static_cast<std::size_t>(index)] = true;
			CharAttrChallengeRow &row =
					out.rows[static_cast<std::size_t>(index)];
			write_u32(row, 0, 1);
			row[4] = static_cast<uint8_t>(index + 1);
			continue;
		}
		if (current_section < 0) continue;

		const std::size_t equals = line.find('=');
		if (equals == std::string_view::npos) continue;
		const std::string_view key = trim(line.substr(0, equals));
		std::string_view value = trim(line.substr(equals + 1));
		// ConfigFile_ReadKeyValue's sscanf value conversion ends at ';'.
		const std::size_t semicolon = value.find(';');
		if (semicolon != std::string_view::npos)
			value = trim(value.substr(0, semicolon));
		const std::size_t comment = value.find("//");
		if (comment != std::string_view::npos)
			value = trim(value.substr(0, comment));
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
			value = value.substr(1, value.size() - 2);

		const std::size_t row_index = static_cast<std::size_t>(current_section);
		for (const Field &field : kFields) {
			if (!ascii_iequals(key, field.name) ||
			    (field_seen[row_index] & field.seen_bit) != 0) {
				continue;
			}
			field_seen[row_index] |= field.seen_bit;
			CharAttrChallengeRow &row = out.rows[row_index];
			switch (field.kind) {
				case ValueKind::Float:
					write_float(row, field.offset, value);
					break;
				case ValueKind::Integer:
					write_integer(row, field.offset, value);
					break;
				case ValueKind::Attributes:
					write_attributes(row, field.offset, value);
					break;
			}
			break;
		}
	}

	// The loader asks for CHARACTER1, then 2, ... and breaks on the first missing
	// section. A later out-of-order section therefore never lives.
	// [orig: CharAttr_LoadFromDef @0x412140 -- the sequential section walk and its
	//  first-missing-section stop]
	bool enumeration_live = true;
	for (std::size_t i = 0; i < kCharAttrChallengeRowCount; ++i) {
		enumeration_live = enumeration_live && section_seen[i];
		if (!enumeration_live) out.rows[i].fill(0);
	}
	return true;
}

// Row select: wrap the class into the sixteen slots, then reject unless the row is active
// (+0) and its embedded class id (+4) matches the caller's. The caller XORs the challenge
// seed with crc32 over the whole 124-byte row; an absent/inactive/mismatched row makes the
// original return zero instead.
// [orig: AnimMap_GetSlotChecksum @0x412aa0 (IDB misname) -- slot `(class-1) & 0x0F`,
//  the two validity gates, and the `return 0` arm @0x412abf]
const CharAttrChallengeRow *find_charattr_challenge_row(
		const CharAttrChallengeTable &table, uint8_t class_id) {
	const uint8_t slot =
			static_cast<uint8_t>(static_cast<uint8_t>(class_id - 1u) & 0x0Fu);
	const CharAttrChallengeRow &row = table.rows[slot];
	if (read_u32(row, 0) == 0 || row[4] != class_id) return nullptr;
	return &row;
}

// One property id zeroes one dword across all sixteen rows. Nine ids map to a field; id 1
// and every id >= 10 are the only no-ops.
// Every original caller passes 0.0, so id 0's integer store and ids 2..9's float stores are
// the same four zero bytes here, and two riders of the original are unobservable through
// the C2S 0x1C checksum and deliberately omitted: the per-row ACTIVE gate (@0x4128b3) skips
// rows whose +0 is zero, and such a row is all-zero here and is rejected by the checksum's
// own row select anyway; the per-property disable latch (AnimMap_SetSlotDisabled @0x4125c0,
// called @0x42550d) sets dword_A79508[property] = 1, short-circuiting later sets of that
// property at @0x4128b3 -- which can only skip a write of zero over zero.
// [orig: CharAttr_SetSlotProperty @0x412890 -- the nine live cases @0x4128ef (0),
//  @0x41290b (2), @0x41291e (3), @0x412931 (4), @0x412944 (5), @0x412957 (6),
//  @0x41297d (7), @0x41296a (8), @0x412990 (9), plus the `default: return 0` arm that
//  ids 1 and >= 10 fall to; reached from NapiNPClientMsg_ClearAnimSlot @0x4254c0
//  (IDB misname) on S2C 0x41]
void clear_charattr_challenge_property(
		CharAttrChallengeTable &table, uint8_t property_id) {
	std::size_t offset = 0;
	switch (property_id) {
		case 0: offset = 40; break; // ATTRIBUTES
		case 2: offset = 8; break;  // STEALTH
		case 3: offset = 12; break; // HPBONUS
		case 4: offset = 16; break; // MANABONUS
		case 5: offset = 20; break; // RECOIL_MUTE
		case 6: offset = 28; break; // XHAIR_MUTE
		case 7: offset = 36; break; // SCOPE_MUTE
		case 8: offset = 32; break; // XHAIRDX_MUTE
		case 9: offset = 24; break; // RELOAD_MUTE
		default: return;
	}
	for (CharAttrChallengeRow &row : table.rows)
		write_u32(row, offset, 0);
}

} // namespace opennova::np
