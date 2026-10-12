#include <editor/session/terrain_queries.h>

namespace opennova::editor {

// The heights' ops of the terrain query (S25 lane A): not answered yet in this build.
io::JsonValue answer_terrain_bake(const TerrainQueryView &query, std::string &error) {
	(void)query;
	error = "terrain: bake is not answered yet in this build.";
	return io::JsonValue();
}

io::JsonValue answer_terrain_lock(const TerrainQueryView &query, std::string &error) {
	(void)query;
	error = "terrain: lock is not answered yet in this build.";
	return io::JsonValue();
}

} // namespace opennova::editor
