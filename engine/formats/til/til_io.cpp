#include <formats/til/til_io.h>

// [orig: jodemo Terrain_LoadTileInfoFile @0x5CA730]
// [orig: Terrain_LoadTileInfoFile @0x60a740 ('til0' magic, entries at +16, count at +4; ex kong
//  Terrain_LoadFoliageFile); PolyTrn_LoadTileData @0x6081d0 / Terrain_SerializeTiles @0x6080f0 are
//  the network form; the overlay render is PolyTrn_RenderTile @0x60df0d, docs/tiles/til-re.md]

#include <base/io/le.h>

#include <limits>

namespace opennova {

namespace {

constexpr size_t TIL_HEADER_SIZE = 16;
constexpr size_t TIL_ENTRY_SIZE = 12;

// The byte primitives are the shared opennova::io ones; the local names stay so
// the walk below reads as the witnessed layout.
using io::read_u16_le;
using io::read_u32_le;
using io::write_u16_le;
using io::write_u32_le;

int32_t read_i32_le(const uint8_t *data) {
	return static_cast<int32_t>(read_u32_le(data));
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

bool til_load_accepts(const uint8_t *data, size_t size) {
	return data != nullptr && size >= TIL_HEADER_SIZE && read_u32_le(data) == TIL_MAGIC;
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
