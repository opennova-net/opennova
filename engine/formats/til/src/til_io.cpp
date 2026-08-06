#include <til/til_io.h>

// Engine: jodemo.exe Terrain_LoadTileInfoFile@0x5CA730,
// sub_6081D0@0x6081D0, sub_6080F0@0x6080F0
// [orig: Terrain_LoadTileInfoFile @ 0x5CA730 (jodemo); the retail overlay render is PolyTrn_RenderTile @ 0x60df0d, docs/tiles/til-re.md]
// docs/engine_spec_tiles.md 4.1, 5.1

#include <limits>

namespace opennova {

namespace {

constexpr size_t TIL_HEADER_SIZE = 16;
constexpr size_t TIL_ENTRY_SIZE = 12;

uint16_t read_u16_le(const uint8_t *data) {
	return static_cast<uint16_t>(data[0])
		| (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t read_u32_le(const uint8_t *data) {
	return static_cast<uint32_t>(data[0])
		| (static_cast<uint32_t>(data[1]) << 8)
		| (static_cast<uint32_t>(data[2]) << 16)
		| (static_cast<uint32_t>(data[3]) << 24);
}

int32_t read_i32_le(const uint8_t *data) {
	return static_cast<int32_t>(read_u32_le(data));
}

void write_u16_le(uint8_t *dst, uint16_t value) {
	dst[0] = static_cast<uint8_t>(value & 0xFFu);
	dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void write_u32_le(uint8_t *dst, uint32_t value) {
	dst[0] = static_cast<uint8_t>(value & 0xFFu);
	dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
	dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
	dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

} // namespace

bool load_til(const uint8_t *data, size_t size, TilFile &out, std::string &error) {
	out = TilFile{};
	error.clear();

	if (data == nullptr) {
		error = "til data pointer is null";
		return false;
	}
	if (size < TIL_HEADER_SIZE) {
		error = "til payload too small for header";
		return false;
	}

	const uint32_t magic = read_u32_le(data);
	if (magic != TIL_MAGIC) {
		error = "til magic mismatch";
		return false;
	}

	const uint32_t entry_count = read_u32_le(data + 4);
	const size_t payload_bytes = static_cast<size_t>(entry_count) * TIL_ENTRY_SIZE;
	if (payload_bytes / TIL_ENTRY_SIZE != static_cast<size_t>(entry_count)) {
		error = "til entry count overflow";
		return false;
	}
	if (size < TIL_HEADER_SIZE + payload_bytes) {
		error = "til payload truncated";
		return false;
	}

	out.entries.resize(static_cast<size_t>(entry_count));

	const uint8_t *cursor = data + TIL_HEADER_SIZE;
	for (uint32_t i = 0; i < entry_count; ++i) {
		TilOverlayEntry &entry = out.entries[static_cast<size_t>(i)];
		entry.x_fixed = read_i32_le(cursor + 0);
		entry.z_fixed = read_i32_le(cursor + 4);
		entry.tile_index = cursor[8];
		entry.flags = cursor[9];
		entry.reserved = read_u16_le(cursor + 10);
		entry = til_normalize_overlay_entry(entry);
		cursor += TIL_ENTRY_SIZE;
	}

	out = til_normalize_file(out);
	return true;
}

bool save_til(const TilFile &til, std::vector<uint8_t> &out, std::string &error) {
	error.clear();
	out.clear();

	const TilFile normalized = til_normalize_file(til);

	if (normalized.entries.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
		error = "til entry count exceeds 32-bit header field";
		return false;
	}

	const size_t total_size = TIL_HEADER_SIZE + normalized.entries.size() * TIL_ENTRY_SIZE;
	if (total_size < TIL_HEADER_SIZE) {
		error = "til size overflow";
		return false;
	}

	out.resize(total_size, 0);
	write_u32_le(out.data() + 0, TIL_MAGIC);
	write_u32_le(out.data() + 4, static_cast<uint32_t>(normalized.entries.size()));
	write_u32_le(out.data() + 8, normalized.reserved0);
	write_u32_le(out.data() + 12, normalized.reserved1);

	uint8_t *cursor = out.data() + TIL_HEADER_SIZE;
	for (const TilOverlayEntry &entry : normalized.entries) {
		write_u32_le(cursor + 0, static_cast<uint32_t>(entry.x_fixed));
		write_u32_le(cursor + 4, static_cast<uint32_t>(entry.z_fixed));
		cursor[8] = entry.tile_index;
		cursor[9] = entry.flags;
		write_u16_le(cursor + 10, entry.reserved);
		cursor += TIL_ENTRY_SIZE;
	}

	return true;
}

} // namespace opennova
