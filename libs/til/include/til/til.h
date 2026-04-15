#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace opennova {

constexpr uint32_t TIL_MAGIC = 0x74696C30u;

constexpr uint8_t TIL_FLAG_FLIP_X = 0x01u;
constexpr uint8_t TIL_FLAG_FLIP_Y = 0x02u;
constexpr uint8_t TIL_FLAG_ROTATE_90 = 0x04u;
// Bit 0x08 — the original game renderer emits a secondary LINELIST pass when this
// is set (4 edges / 8 vertices, interpreted as a perimeter outline). Named here for
// that observable effect. See memory/reference_ida_tiles.md Audit (2026-04-20).
constexpr uint8_t TIL_FLAG_OUTLINE = 0x08u;
constexpr uint8_t TIL_FLAG_AUTHORED_MASK = TIL_FLAG_FLIP_X | TIL_FLAG_FLIP_Y | TIL_FLAG_ROTATE_90 | TIL_FLAG_OUTLINE;

constexpr int TIL_ATLAS_TILE_PIXELS = 64;
constexpr int TIL_CELL_WORLD_UNITS = 16;
constexpr int TIL_FIXED_SHIFT = 16;
constexpr int32_t TIL_FIXED_UNITS = 1 << TIL_FIXED_SHIFT;
constexpr int32_t TIL_FIXED_CELL_UNITS = TIL_CELL_WORLD_UNITS << TIL_FIXED_SHIFT;

struct TilCellCoord {
	int x = 0;
	int z = 0;
};

struct TilOverlayEntry {
	int32_t x_fixed = 0;
	int32_t z_fixed = 0;
	uint8_t tile_index = 0;
	uint8_t flags = 0;
	uint16_t reserved = 0;
};

struct TilFile {
	uint32_t reserved0 = 0;
	uint32_t reserved1 = 0;
	std::vector<TilOverlayEntry> entries;

	bool empty() const { return entries.empty(); }
};

inline uint8_t til_normalize_authored_flags(uint8_t flags) {
	return static_cast<uint8_t>(flags & TIL_FLAG_AUTHORED_MASK);
}

inline TilOverlayEntry til_normalize_overlay_entry(TilOverlayEntry entry) {
	entry.flags = til_normalize_authored_flags(entry.flags);
	entry.reserved = 0;
	return entry;
}

inline TilFile til_normalize_file(TilFile file) {
	file.reserved0 = 0;
	file.reserved1 = 0;
	for (TilOverlayEntry &entry : file.entries) {
		entry = til_normalize_overlay_entry(entry);
	}
	return file;
}

inline int til_cell_from_world(double world_units) {
	return static_cast<int>(std::floor(world_units / static_cast<double>(TIL_CELL_WORLD_UNITS)));
}

inline int til_cell_x_from_world(double world_x) {
	return til_cell_from_world(world_x);
}

inline int til_cell_z_from_world(double world_z) {
	return til_cell_from_world(world_z);
}

inline float til_world_from_cell(int cell) {
	return static_cast<float>(cell * TIL_CELL_WORLD_UNITS);
}

inline float til_world_x_from_cell(int cell_x) {
	return til_world_from_cell(cell_x);
}

inline float til_world_z_from_cell(int cell_z) {
	return til_world_from_cell(cell_z);
}

inline float til_world_center_from_cell(int cell) {
	return til_world_from_cell(cell) + static_cast<float>(TIL_CELL_WORLD_UNITS) * 0.5f;
}

inline float til_world_x_from_fixed(int32_t x_fixed) {
	return static_cast<float>(x_fixed) / static_cast<float>(TIL_FIXED_UNITS);
}

inline float til_world_z_from_fixed(int32_t z_fixed) {
	return -static_cast<float>(z_fixed) / static_cast<float>(TIL_FIXED_UNITS);
}

inline int32_t til_x_fixed_from_world(double world_x) {
	return static_cast<int32_t>(std::llround(world_x * static_cast<double>(TIL_FIXED_UNITS)));
}

inline int32_t til_z_fixed_from_world(double world_z) {
	return -static_cast<int32_t>(std::llround(world_z * static_cast<double>(TIL_FIXED_UNITS)));
}

inline int til_floor_div_fixed(int32_t value, int32_t divisor) {
	if (divisor <= 0) {
		return 0;
	}

	int32_t quotient = value / divisor;
	const int32_t remainder = value % divisor;
	if (remainder != 0 && value < 0) {
		--quotient;
	}
	return static_cast<int>(quotient);
}

inline int til_cell_x_from_fixed(int32_t x_fixed) {
	return til_floor_div_fixed(x_fixed, TIL_FIXED_CELL_UNITS);
}

inline int til_cell_z_from_fixed(int32_t z_fixed) {
	return til_floor_div_fixed(-z_fixed, TIL_FIXED_CELL_UNITS);
}

inline int32_t til_x_fixed_from_cell(int cell_x) {
	return static_cast<int32_t>(static_cast<int64_t>(cell_x) * static_cast<int64_t>(TIL_FIXED_CELL_UNITS));
}

inline int32_t til_z_fixed_from_cell(int cell_z) {
	return -static_cast<int32_t>(static_cast<int64_t>(cell_z) * static_cast<int64_t>(TIL_FIXED_CELL_UNITS));
}

inline TilCellCoord til_cell_from_fixed(int32_t x_fixed, int32_t z_fixed) {
	return {til_cell_x_from_fixed(x_fixed), til_cell_z_from_fixed(z_fixed)};
}

inline TilCellCoord til_entry_cell(const TilOverlayEntry &entry) {
	return til_cell_from_fixed(entry.x_fixed, entry.z_fixed);
}

inline bool til_entry_matches_cell(const TilOverlayEntry &entry, int cell_x, int cell_z) {
	const TilCellCoord cell = til_entry_cell(entry);
	return cell.x == cell_x && cell.z == cell_z;
}

inline TilOverlayEntry make_til_overlay_entry(int cell_x,
	                                          int cell_z,
	                                          uint8_t tile_index,
	                                          uint8_t flags) {
	TilOverlayEntry entry;
	entry.x_fixed = til_x_fixed_from_cell(cell_x);
	entry.z_fixed = til_z_fixed_from_cell(cell_z);
	entry.tile_index = tile_index;
	entry.flags = flags;
	return til_normalize_overlay_entry(entry);
}

} // namespace opennova
