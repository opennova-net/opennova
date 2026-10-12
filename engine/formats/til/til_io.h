#pragma once

#include "til.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// What a .til's header says beside its records: read for a document's source notes, never written
// back (save_til computes the header). MED writes +8 = 12 * count + 16 and +12 = 0 [orig: JOTACmed
// MapTileStore_Serialize @ 0x433F44..0x433F50]. The game reads no byte past the count's records and
// draws, collides and places nothing by the two words [orig: Terrain_LoadTileInfoFile @
// 0x60A7E6..0x60A7EF]: a host copies them onto the S2C 0x45 wire as the file holds them [orig:
// Terrain_SerializeTiles @ 0x60815A..0x608166], and a joiner rebuilds the header as MED writes it,
// 12 * count + 16 and 0 [orig: PolyTrn_LoadTileData @ 0x608209..0x60823D].
struct TilHeaderFacts {
	uint32_t size_word = 0;
	uint32_t word12 = 0;
	size_t trailing = 0;
};

// The records the header's count names, every byte a field. Refuses what the game cannot place: under
// 16 bytes or no til0 magic (the game's -1, which sends the authority on to the terrain's own tile
// info), and a count past the file's bytes (the game trusts the count and reads past its buffer
// [orig: Terrain_LoadTileInfoFile @ 0x60A7EF; PolyTrn_RenderTile @ 0x60DDAD]).
bool load_til(const uint8_t *data, size_t size, TilFile &out, std::string &error,
              TilHeaderFacts *facts = nullptr);
// Writes MED's header (til0, count, 12 * count + 16, 0) and the records in the model's order, the
// game's draw order [orig: JOTACmed MapTileStore_Serialize @ 0x433EC0].
bool save_til(const TilFile &til, std::vector<uint8_t> &out, std::string &error);

// Whether the game's tile info load takes a file of these bytes: 16 bytes or more that open
// with the til0 magic, whatever the entry count says after them. The load refuses anything
// else as it refuses a missing file, which sends the authority on to the terrain's own tile
// info (runtime/mission read_placed_tiles).
// [orig: Terrain_LoadTileInfoFile @0x60a740: -1 under 16 bytes @0x60a78b, -1 without the
//  magic @0x60a7ba]
bool til_load_accepts(const uint8_t *data, size_t size);

} // namespace opennova
