#include <editor/session/terrain_queries.h>

namespace opennova::editor {

// The maps' and the tiles' ops of the terrain query (S25 lane B): not answered yet in this build.
io::JsonValue answer_terrain_legend(const TerrainQueryView &query, std::string &error) {
	(void)query;
	error = "terrain: legend is not answered yet in this build.";
	return io::JsonValue();
}

io::JsonValue answer_terrain_foliage(const TerrainQueryView &query, std::string &error) {
	(void)query;
	error = "terrain: foliage is not answered yet in this build.";
	return io::JsonValue();
}

io::JsonValue answer_terrain_tiles(const TerrainQueryView &query, std::string &error) {
	(void)query;
	error = "terrain: tiles is not answered yet in this build.";
	return io::JsonValue();
}

io::JsonValue answer_terrain_tiles_at(const TerrainQueryView &query, std::string &error) {
	(void)query;
	error = "terrain: tiles_at is not answered yet in this build.";
	return io::JsonValue();
}

} // namespace opennova::editor
