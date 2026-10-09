#pragma once

#include "til.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

bool load_til(const uint8_t *data, size_t size, TilFile &out, std::string &error);
bool save_til(const TilFile &til, std::vector<uint8_t> &out, std::string &error);

// Whether the game's tile info load takes a file of these bytes: 16 bytes or more that open
// with the til0 magic, whatever the entry count says after them. The load refuses anything
// else as it refuses a missing file, which sends the authority on to the terrain's own tile
// info (runtime/mission read_placed_tiles).
// [orig: Terrain_LoadTileInfoFile @0x60a740: -1 under 16 bytes @0x60a78b, -1 without the
//  magic @0x60a7ba]
bool til_load_accepts(const uint8_t *data, size_t size);

} // namespace opennova
