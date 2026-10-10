#include "authoring/mission_placed_tiles.h"

#include <cstring>
#include <vector>

#include <base/io/strutil.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/mission/runtime_boot.h>

namespace godot {

Ref<TerrainTileInfo> read_mission_placed_tiles(const Ref<ResourceRoot> &root, const std::string &mission_file,
		const std::string &terrain, const std::string &environment_file, bool *terrain_own) {
	if (terrain_own) *terrain_own = false;
	if (root.is_null()) return Ref<TerrainTileInfo>();
	const opennova::mission::BootFileSource files = opennova::mission::boot_files_from_index(root->native_index());
	// The environment by its name, as the header names it (the read appends .env).
	std::string environment = environment_file;
	if (environment.size() >= 4 && opennova::strutil::iequals(environment.substr(environment.size() - 4), ".env"))
		environment.erase(environment.size() - 4);
	std::vector<uint8_t> bytes;
	const std::string took = opennova::mission::read_placed_tiles(files, mission_file, terrain, environment, bytes);
	if (took.empty()) return Ref<TerrainTileInfo>();
	// The tiles taken are the terrain's own unless they are the mission's (its tiles row's name).
	if (terrain_own)
		*terrain_own = mission_file.empty() ||
				took != opennova::mission::sidecar_name(mission_file, *opennova::mission::sidecar_for_role("tiles"));
	PackedByteArray til;
	til.resize(static_cast<int64_t>(bytes.size()));
	std::memcpy(til.ptrw(), bytes.data(), bytes.size());
	Ref<TerrainTileInfo> tiles;
	tiles.instantiate();
	return tiles->load_from_bytes(til) == OK ? tiles : Ref<TerrainTileInfo>();
}

} // namespace godot
