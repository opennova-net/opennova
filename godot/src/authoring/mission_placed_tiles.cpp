#include "authoring/mission_placed_tiles.h"

#include <cstring>
#include <vector>

#include <base/io/strutil.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/mission/runtime_boot.h>
#include <runtime/terrain_query/terrain_field_build.h>

namespace godot {

Ref<TerrainTileInfo> read_mission_placed_tiles(const Ref<ResourceRoot> &root, const std::string &mission_file,
		const std::string &terrain, const std::string &environment_file, bool *terrain_own) {
	if (terrain_own) *terrain_own = false;
	if (root.is_null()) return Ref<TerrainTileInfo>();
	const opennova::mission::BootFileSource files = opennova::mission::boot_files_from_index(root->native_index());
	// mission::read_placed_tiles's names: the tiles row's of the mission's file, and the environment by its name
	// (the read appends .env).
	const std::string own = mission_file.empty()
			? std::string()
			: opennova::mission::sidecar_name(mission_file, *opennova::mission::sidecar_for_role("tiles"));
	std::string environment = environment_file;
	if (environment.size() >= 4 && opennova::strutil::iequals(environment.substr(environment.size() - 4), ".env"))
		environment.erase(environment.size() - 4);
	const opennova::terrain::TerrainFileReader read_loose = [&files](const std::string &name,
			std::vector<uint8_t> &bytes) { return files.read_loose(name, bytes); };
	std::vector<uint8_t> bytes;
	const std::string took =
			opennova::terrain::read_placed_tile_bytes(read_loose, files.read_file, own, terrain, environment, bytes);
	if (took.empty()) return Ref<TerrainTileInfo>();
	if (terrain_own) *terrain_own = took != own;
	PackedByteArray til;
	til.resize(static_cast<int64_t>(bytes.size()));
	std::memcpy(til.ptrw(), bytes.data(), bytes.size());
	Ref<TerrainTileInfo> tiles;
	tiles.instantiate();
	return tiles->load_from_bytes(til) == OK ? tiles : Ref<TerrainTileInfo>();
}

} // namespace godot
