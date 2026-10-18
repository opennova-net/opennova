#include <editor/session/terrain_queries.h>

#include <cstdio>
#include <memory>
#include <vector>

#include <editor/assets/project_asset_source.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/til_document.h>
#include <editor/model/document_base.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/tile_tools.h>
#include <editor/project/project_files.h>
#include <formats/foliage/runtime.h>
#include <formats/pcx/pcx.h>
#include <formats/trn/charmap_legend.h>
#include <runtime/terrain/terrain_map_source.h>
#include <editor/session/session_core.h>
#include <editor/session/terrain_family.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms_edit.h>
#include <formats/til/til_io.h>
#include <runtime/terrain_query/terrain_field_build.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

// The tiles the game draws for a view: the open .til's rows (each its identity), else the file the game reads for it
// (the mission's .til; where the mission has none, the terrain's own [orig: PolyTrn_LoadTerrainConfig @
// 0x60E6D2..0x60E6E5]), its rows with no identity (0).
struct ViewTiles {
	TerrainFamily family;
	std::string from; // the file the tiles are read from ("" none)
	bool open = false;
	TilFile file;
	std::vector<NodeId> ids;
};

bool view_tiles(const TerrainQueryView &query, ViewTiles &out, std::string &error) {
	const SessionView &view = query.context.core.view();
	if (!resolve_terrain_family(view, query.view, out.family)) {
		error = "terrain: " + out.family.error;
		return false;
	}
	const TerrainFamilyFile &tiles = out.family.tiles;
	for (const auto &open : view.documents.open)
		if (open->path() == tiles.path)
			if (const auto *document = dynamic_cast<const TilDocument *>(open.get())) {
				out.from = tiles.path;
				out.open = true;
				out.file = document->file();
				for (const auto &row : document->rows()) out.ids.push_back(row->id);
				return true;
			}
	const std::shared_ptr<const ProjectAssetSource> files = view.findings.assets;
	if (!files) return true;
	const terrain::TerrainFileReader read = [&files](const std::string &name, std::vector<uint8_t> &bytes) {
		return files->read(name, bytes);
	};
	// Under the view's mission (a mission's own, or the one a terrain's viewport draws it under): its .til, else the
	// terrain's own as that mission's environment reads it.
	const std::string mission_til = tiles.missing || tiles.path.empty() ? std::string() : tiles.name;
	std::vector<uint8_t> bytes;
	out.from = terrain::read_placed_tile_bytes(read, read, mission_til, out.family.terrain_name, out.family.environment,
	                                           bytes);
	std::string why;
	if (!out.from.empty() && !load_til(bytes.data(), bytes.size(), out.file, why)) out.file = TilFile();
	out.ids.assign(out.file.entries.size(), 0);
	return true;
}

JsonValue tile_json(const ViewTiles &tiles, size_t i, const std::vector<NodeId> &selected) {
	const TilOverlayEntry &entry = tiles.file.entries[i];
	JsonValue out = JsonValue::make_object();
	out.set("id", io::json_number(double(tiles.ids[i])));
	out.set("index", io::json_number(double(i)));
	out.set("tile", io::json_number(entry.tile_index));
	out.set("x", io::json_number(double(entry.x_fixed) / 65536.0));
	out.set("y", io::json_number(double(entry.z_fixed) / 65536.0));
	out.set("flags", io::json_number(entry.flags));
	out.set("layer", io::json_number(entry.layer));
	bool picked = false;
	for (const NodeId id : selected) picked = picked || (id && id == tiles.ids[i]);
	out.set("selected", JsonValue::make_bool(picked));
	return out;
}

JsonValue tiles_head(const TerrainQueryView &query, const ViewTiles &tiles) {
	JsonValue out = JsonValue::make_object();
	out.set("path", io::json_string(tiles.family.tiles.path));
	out.set("missing", JsonValue::make_bool(tiles.family.tiles.missing));
	out.set("open", JsonValue::make_bool(tiles.open));
	out.set("from", io::json_string(tiles.from));
	out.set("tile_set", io::json_string(tiles.family.tile_set));
	out.set("count", io::json_number(double(tiles.file.entries.size())));
	JsonValue selected = JsonValue::make_array();
	for (const NodeId id : query.context.core.view().workspace.document(query.view.path()).tiles)
		selected.push(io::json_number(double(id)));
	out.set("selected", std::move(selected));
	return out;
}

} // namespace

// legend: the surface classes the char map holds, as the game names them (the twenty the tile set tables name, each
// its legend colour, its TSD name and its words), the default class (where a terrain names no char map) and what a
// class past them is.
io::JsonValue answer_terrain_legend(const TerrainQueryView &, std::string &) {
	JsonValue out = JsonValue::make_object();
	JsonValue classes = JsonValue::make_array();
	for (int surface = 0; surface < kCharmapLegendCount; ++surface) {
		const CharmapLegendColour &c = kCharmapLegend[surface];
		char colour[8];
		std::snprintf(colour, sizeof colour, "#%02X%02X%02X", c.r, c.g, c.b);
		JsonValue row = JsonValue::make_object();
		row.set("class", io::json_number(surface));
		const char *name = mission_surface_name(surface);
		row.set("name", io::json_string(name ? name : ""));
		row.set("words", io::json_string(mission_surface_words(surface)));
		row.set("colour", io::json_string(colour));
		classes.push(std::move(row));
	}
	out.set("classes", std::move(classes));
	// The game's class where a terrain names no char map [orig: Terrain_LoadCharmapPCX].
	out.set("default", io::json_number(1));
	out.set("past", io::json_string("A class 20 to 255 the game reads as it stands: footsteps and weather treat it as no "
	                                "class they name, and a round striking there takes an impact row no shipped table "
	                                "authors (20 to 23) or the ammo's obj row (24 and past)."));
	return out;
}

// foliage: the foliage map the view's terrain reads (or its import's source in its place) and the codes it holds, each
// with how many texels hold it, its share and the definitions it grows (their slots and models); code 0 grows nothing.
// Beside them the terrain's definitions, each its slot, model, codes and whether placed tiles keep it off.
io::JsonValue answer_terrain_foliage(const TerrainQueryView &query, std::string &error) {
	const SessionView &view = query.context.core.view();
	TerrainFamily family;
	if (!resolve_terrain_family(view, query.view, family)) {
		error = "terrain: " + family.error;
		return JsonValue();
	}
	JsonValue out = JsonValue::make_object();
	const TerrainFamilyFile &file = family.plane(TerrainPlane::Foliage);
	out.set("map", io::json_string(file.path.empty() ? file.name : file.path));
	out.set("refusal", io::json_string(file.refusal));
	std::vector<FoliageDef> defs;
	if (family.trn) defs = family.trn->foliage_defs;
	JsonValue definitions = JsonValue::make_array();
	for (size_t slot = 0; slot < defs.size(); ++slot) {
		JsonValue row = JsonValue::make_object();
		row.set("slot", io::json_number(double(slot)));
		row.set("graphic", io::json_string(defs[slot].graphic));
		JsonValue codes = JsonValue::make_array();
		for (const int code : defs[slot].match)
			if (code != FOLIAGE_MATCH_UNSET) codes.push(io::json_number(code));
		row.set("codes", std::move(codes));
		row.set("kept_off_by_tiles", JsonValue::make_bool(foliage::placed_tiles_keep_off(defs[slot].attrib_flags)));
		definitions.push(std::move(row));
	}
	out.set("definitions", std::move(definitions));
	// The census: the map as it stands (an open document's texels through the project's files).
	size_t counts[256] = {};
	size_t texels = 0;
	if (!file.path.empty() && view.findings.assets) {
		std::vector<uint8_t> bytes;
		IndexedImage8 map;
		std::string why;
		if (view.findings.assets->read(basename_of(file.path), bytes) &&
		    terrain::decode_foliage_map_source(basename_of(file.path), bytes, map, why))
			for (const uint8_t code : map.indices) {
				++counts[code];
				++texels;
			}
	}
	out.set("texels", io::json_number(double(texels)));
	JsonValue codes = JsonValue::make_array();
	for (int code = 0; code < 256; ++code) {
		if (!counts[code]) continue;
		JsonValue row = JsonValue::make_object();
		row.set("code", io::json_number(code));
		row.set("texels", io::json_number(double(counts[code])));
		row.set("share", io::json_number(double(counts[code]) / double(texels)));
		JsonValue grows = JsonValue::make_array();
		for (size_t slot = 0; code != 0 && slot < defs.size(); ++slot)
			for (const int match : defs[slot].match)
				if (match == code) {
					JsonValue def = JsonValue::make_object();
					def.set("slot", io::json_number(double(slot)));
					def.set("graphic", io::json_string(defs[slot].graphic));
					grows.push(std::move(def));
					break;
				}
		row.set("grows", std::move(grows));
		codes.push(std::move(row));
	}
	out.set("codes", std::move(codes));
	return out;
}

// tiles: the view's tiles in draw order (the last over the others), each {id, index, tile, x, y (its corner, mission
// metres), flags, layer, selected}, beside where they come from.
io::JsonValue answer_terrain_tiles(const TerrainQueryView &query, std::string &error) {
	ViewTiles tiles;
	if (!view_tiles(query, tiles, error)) return JsonValue();
	JsonValue out = tiles_head(query, tiles);
	const std::vector<NodeId> &selected = query.context.core.view().workspace.document(query.view.path()).tiles;
	JsonValue rows = JsonValue::make_array();
	for (size_t i = 0; i < tiles.file.entries.size(); ++i) rows.push(tile_json(tiles, i, selected));
	out.set("tiles", std::move(rows));
	return out;
}

// tiles_at: the tile a click at (x, y) picks (the one drawn on top), or with (x2, y2) every tile whose square the box
// crosses (Nile's click and drag box, preview/tile_tools).
io::JsonValue answer_terrain_tiles_at(const TerrainQueryView &query, std::string &error) {
	if (!query.args.has("x") || !query.args.has("y")) {
		error = "terrain: tiles_at takes a point, x and y (mission metres), and for a box its other corner, x2 and y2.";
		return JsonValue();
	}
	ViewTiles tiles;
	if (!view_tiles(query, tiles, error)) return JsonValue();
	const double x = query.args.number("x"), y = query.args.number("y");
	std::vector<size_t> hits;
	if (query.args.has("x2") || query.args.has("y2")) {
		hits = tiles_in_box(tiles.file, x, y, query.args.has("x2") ? query.args.number("x2") : x,
		                    query.args.has("y2") ? query.args.number("y2") : y);
	} else if (const size_t hit = tile_at(tiles.file, x, y); hit < tiles.file.entries.size()) {
		hits.push_back(hit);
	}
	JsonValue out = tiles_head(query, tiles);
	const std::vector<NodeId> &selected = query.context.core.view().workspace.document(query.view.path()).tiles;
	JsonValue rows = JsonValue::make_array();
	for (const size_t i : hits) rows.push(tile_json(tiles, i, selected));
	out.set("tiles", std::move(rows));
	return out;
}

} // namespace opennova::editor
