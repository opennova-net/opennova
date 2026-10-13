#pragma once

#include <cstddef>
#include <string>

#include <base/io/json.h>
#include <editor/session/editor_queries.h>

namespace opennova::editor {

class DocumentBase;

// The terrain query (ADR 0046 S25, the shared seams): one question a row of an op table, each lane its rows. brush:
// the workspace's brush, whether the view brushes and where its edits go; family: the files a view's brush edits,
// as the game resolves them for it; sample: the ground at (x, y) in each plane (the views'); bake and lock: a heights
// file's bake and the height locks under a disc (the heights'); legend, foliage, tiles and tiles_at: the surface
// classes, the foliage codes, a view's tiles and those in a box (the maps' and the tiles').

// The op param's index-th token, the op table's order (null past the last).
const char *terrain_query_op_choice(size_t index);

inline constexpr QueryChoices kTerrainChoices[] = {
	{ "op", terrain_query_op_choice },
};

inline constexpr QueryParam kTerrainParams[] = {
	{ "path", QueryJson::String, false, nullptr,
			"An open mission or terrain (the view) by its project-relative path or its logical name; left out, the active "
			"document." },
	{ "op", QueryJson::String, true, nullptr, "What is asked: brush, family, sample, bake, lock, legend, foliage, tiles, tiles_at." },
	{ "x", QueryJson::Number, false, nullptr, "sample's, lock's and tiles_at's point (tiles_at: its box's first corner), mission metres east." },
	{ "y", QueryJson::Number, false, nullptr, "The same, mission metres north." },
	{ "x2", QueryJson::Number, false, nullptr, "tiles_at's box's other corner, mission metres east." },
	{ "y2", QueryJson::Number, false, nullptr, "tiles_at's box's other corner, mission metres north." },
	{ "radius", QueryJson::Number, false, nullptr, "lock's disc, metres (left out, the brush's)." },
};

inline constexpr const char *kTerrainDoc =
		"The terrain tools' questions of a view (ADR 0046 S25), by op. brush: the workspace's terrain brush, whether "
		"the view brushes with it now (its Terrain pane shown and a tool picked), the plane its tool edits and where "
		"that plane's edits go. family: the files the view's brush edits as the game resolves them for it (view, "
		"terrain, planes {heights, surface, foliage, colour, blend: {path, name, import_output, refusal, words}}, tiles). "
		"sample: the ground at (x, y) in each plane's words. bake: a heights file's bake (its state, the regions it "
		"waits on, the bakes landed and started, why it refuses the heights). lock: the entities under a disc at (x, y) that follow the ground and those height-locked. legend: the "
		"surface classes as the game names them (classes [{class, name, words, colour}], default, past). foliage: the "
		"foliage map and its codes (map, refusal, texels, codes [{code, texels, share, grows [{slot, graphic}]}], "
		"definitions [{slot, graphic, codes, kept_off_by_tiles}]). tiles: the view's tiles "
		"(its .til's rows in draw order). tiles_at: the tiles whose square meets a box. An op not answered yet in this "
		"build is refused, saying so.";

io::JsonValue answer_terrain(const QueryContext &context, const QueryArgs &args, std::string &error);

// The ops' answers, by the lane whose files hold them: the views' (session/terrain_queries.cpp), the heights'
// (session/terrain_queries_heights.cpp) and the maps' and tiles' (session/terrain_queries_maps.cpp). Each reads the
// view document `view` (open, a mission or a terrain); null with `error` for a question it cannot answer.
struct TerrainQueryView {
	const QueryContext &context;
	const QueryArgs &args;
	const DocumentBase &view;
};
io::JsonValue answer_terrain_bake(const TerrainQueryView &query, std::string &error);
io::JsonValue answer_terrain_lock(const TerrainQueryView &query, std::string &error);
io::JsonValue answer_terrain_legend(const TerrainQueryView &query, std::string &error);
io::JsonValue answer_terrain_foliage(const TerrainQueryView &query, std::string &error);
io::JsonValue answer_terrain_tiles(const TerrainQueryView &query, std::string &error);
io::JsonValue answer_terrain_tiles_at(const TerrainQueryView &query, std::string &error);

} // namespace opennova::editor
