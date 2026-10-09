#pragma once

#include <string>

#include <godot_cpp/classes/ref.hpp>

#include "resource_index/resource_root.h"
#include "terrain/terrain_tile_info.h"

namespace godot {

// The tiles a mission places, read as the game's load reads them (the rule of mission::read_placed_tiles, which
// GameWorld::load_mission_tile_info reads the game's through): <mission>.til, the mission's file name cut at its
// first dot (mission_sidecars' tiles row), else the terrain's own polytrn_tileinfo (its .trn and the mission's .env
// through the terrain's parser), each read loose first and taken only where the game's loader takes it, a missing,
// short or bad-magic file sending the read on (terrain::read_placed_tile_bytes, behind the ADR 0020 seam). The
// editor's previews have no mission record, so the terrain and the environment come from the mission header the
// preview reads (`terrain`, its .trn's name, and `environment_file`, the .env the terrain's load reads after it, ""
// none), as preview/mission_ground_facts reads them. Null where neither loads or the bytes taken do not parse; the
// device sets it as the terrain's tile override, as the game sets its mission tiles. `terrain_own` (optional) says
// whether the tiles taken are the terrain's own.
Ref<TerrainTileInfo> read_mission_placed_tiles(const Ref<ResourceRoot> &root, const std::string &mission_file,
		const std::string &terrain, const std::string &environment_file, bool *terrain_own = nullptr);

} // namespace godot
