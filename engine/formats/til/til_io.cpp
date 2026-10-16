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

} // namespace

bool load_til(const uint8_t *data, size_t size, TilFile &out, std::string &error, TilHeaderFacts *facts) {
	out = TilFile{};
	error.clear();
	if (facts != nullptr) *facts = TilHeaderFacts{};

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
	if (size - TIL_HEADER_SIZE < payload_bytes) {
		error = "til payload truncated";
		return false;
	}

	out.entries.resize(static_cast<size_t>(entry_count));

	const uint8_t *cursor = data + TIL_HEADER_SIZE;
	for (uint32_t i = 0; i < entry_count; ++i) {
		TilOverlayEntry &entry = out.entries[static_cast<size_t>(i)];
		entry.x_fixed = io::read_s32_le(cursor + 0);
		entry.z_fixed = io::read_s32_le(cursor + 4);
		entry.tile_index = cursor[8];
		entry.flags = cursor[9];
		entry.layer = read_u16_le(cursor + 10);
		cursor += TIL_ENTRY_SIZE;
	}

	if (facts != nullptr) {
		facts->size_word = read_u32_le(data + 8);
		facts->word12 = read_u32_le(data + 12);
		facts->trailing = size - TIL_HEADER_SIZE - payload_bytes;
	}
	return true;
}

bool til_load_accepts(const uint8_t *data, size_t size) {
	return data != nullptr && size >= TIL_HEADER_SIZE && read_u32_le(data) == TIL_MAGIC;
}

bool save_til(const TilFile &til, std::vector<uint8_t> &out, std::string &error) {
	error.clear();
	out.clear();

	// The header's size word is 32 bits: refuse a count whose 12 * count + 16 does not fit it.
	const size_t count = til.entries.size();
	if (count > (static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - TIL_HEADER_SIZE) / TIL_ENTRY_SIZE) {
		error = "til entry count exceeds the header's 32-bit size word";
		return false;
	}
	const size_t total_size = TIL_HEADER_SIZE + count * TIL_ENTRY_SIZE;

	// [orig: JOTACmed MapTileStore_Serialize @ 0x433F39..0x433F50: til0, the count, 12 * count + 16, 0]
	out.resize(total_size, 0);
	write_u32_le(out.data() + 0, TIL_MAGIC);
	write_u32_le(out.data() + 4, static_cast<uint32_t>(count));
	write_u32_le(out.data() + 8, static_cast<uint32_t>(total_size));
	write_u32_le(out.data() + 12, 0);

	uint8_t *cursor = out.data() + TIL_HEADER_SIZE;
	for (const TilOverlayEntry &entry : til.entries) {
		write_u32_le(cursor + 0, static_cast<uint32_t>(entry.x_fixed));
		write_u32_le(cursor + 4, static_cast<uint32_t>(entry.z_fixed));
		cursor[8] = entry.tile_index;
		cursor[9] = entry.flags;
		write_u16_le(cursor + 10, entry.layer);
		cursor += TIL_ENTRY_SIZE;
	}

	return true;
}

} // namespace opennova
