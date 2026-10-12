#include <editor/session/terrain_queries.h>

#include <iterator>

#include <editor/assets/project_asset_source.h>
#include <editor/model/document.h>
#include <editor/model/document_base.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_source.h>
#include <editor/preview/terrain_brush_spaces.h>
#include <runtime/mission/mission_sidecars.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/session_core.h>
#include <editor/session/terrain_family.h>
#include <editor/session/terrain_tools_parts.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

JsonValue family_file_json(const TerrainFamilyFile &file) {
	JsonValue out = JsonValue::make_object();
	out.set("path", io::json_string(file.path));
	out.set("name", io::json_string(file.name));
	out.set("import_output", io::json_string(file.import_output));
	out.set("refusal", io::json_string(file.refusal));
	out.set("words", io::json_string(file.words));
	return out;
}

// brush: the workspace's brush, whether the view brushes with it and where its edits go.
JsonValue answer_brush(const TerrainQueryView &query, std::string &error) {
	(void)error;
	const SessionView &view = query.context.core.view();
	const TerrainBrush &brush = view.workspace.terrain_brush;
	JsonValue out = JsonValue::make_object();
	out.set("path", io::json_string(query.view.path()));
	out.set("brush", terrain_brush_to_json(brush));
	out.set("pane", JsonValue::make_bool(view.workspace.document(query.view.path()).terrain));
	out.set("live", JsonValue::make_bool(terrain_brush_live(view, query.view)));
	out.set("family", io::json_string(terrain_tool_family_label(terrain_tool_family(brush.tool))));
	TerrainPlane plane = TerrainPlane::Heights;
	if (terrain_tool_plane(brush.tool, plane)) {
		out.set("plane", io::json_string(terrain_plane_token(plane)));
		TerrainFamily family;
		if (resolve_terrain_family(view, query.view, family)) out.set("edits", family_file_json(family.plane(plane)));
	} else {
		out.set("plane", JsonValue::make_null());
	}
	return out;
}

// family: the files the view's brush edits, as the game resolves them for it.
JsonValue answer_family(const TerrainQueryView &query, std::string &error) {
	TerrainFamily family;
	if (!resolve_terrain_family(query.context.core.view(), query.view, family)) {
		error = "terrain: " + family.error;
		return JsonValue();
	}
	JsonValue out = JsonValue::make_object();
	out.set("view", io::json_string(family.view));
	out.set("terrain", io::json_string(family.terrain));
	JsonValue planes = JsonValue::make_object();
	for (size_t i = 0; i < kTerrainPlaneCount; ++i)
		planes.set(terrain_plane_token(static_cast<TerrainPlane>(i)), family_file_json(family.planes[i]));
	out.set("planes", std::move(planes));
	out.set("tiles", family_file_json(family.tiles));
	return out;
}

// sample: the ground at (x, y) as the game reads it for the view (DI-07's facts: its height, its surface class, its
// foliage code, a tile there), over the files the view's mission reads (a terrain under no mission: its own).
JsonValue answer_sample(const TerrainQueryView &query, std::string &error) {
	if (!query.args.has("x") || !query.args.has("y")) {
		error = "terrain: sample takes the point, x and y (mission metres).";
		return JsonValue();
	}
	const SessionView &view = query.context.core.view();
	MissionSceneHeader header;
	std::string mission;
	if (const Document *records = records_of(query.view); records && query.view.kind() == AssetKind::Mission) {
		const std::unique_ptr<MissionSceneSource> source = mission_scene_source(*records);
		if (!source || !source->header(header)) {
			error = "terrain: " + basename_of(query.view.path()) + " has no header to read its terrain from.";
			return JsonValue();
		}
		mission = mission::mission_base_name(query.view.path());
	} else {
		const std::string name = basename_of(query.view.path());
		header.terrain = name.substr(0, name.rfind('.'));
	}
	MissionGround ground;
	ground.follow(view.findings.assets, view.findings.assets ? view.findings.assets->generation() : 0, header, mission);
	if (!ground.terrain()) {
		error = "terrain: no terrain reads for " + basename_of(query.view.path()) + (ground.error().empty() ? "" : ": " + ground.error()) + ".";
		return JsonValue();
	}
	const TerrainPoint at{query.args.number("x"), query.args.number("y")};
	double height = 0.0;
	const bool on = mission_ground_height(ground, at, height);
	JsonValue out = JsonValue::make_object();
	out.set("path", io::json_string(query.view.path()));
	out.set("at", [&] {
		JsonValue point = JsonValue::make_array();
		point.push(io::json_number(at.x));
		point.push(io::json_number(at.y));
		return point;
	}());
	out.set("height", on ? io::json_number(height) : JsonValue::make_null());
	out.set("ground", mission_ground_to_json(ground.terrain_at(at.x, at.y, height)));
	return out;
}

struct OpRow {
	const char *token;
	JsonValue (*answer)(const TerrainQueryView &query, std::string &error);
};
constexpr OpRow kOps[] = {
	// the views' (lane C)
	{"brush", answer_brush},
	{"family", answer_family},
	{"sample", answer_sample},
	// the heights' (lane A: terrain_queries_heights.cpp)
	{"bake", answer_terrain_bake},
	{"lock", answer_terrain_lock},
	// the maps' and the tiles' (lane B: terrain_queries_maps.cpp)
	{"legend", answer_terrain_legend},
	{"foliage", answer_terrain_foliage},
	{"tiles", answer_terrain_tiles},
	{"tiles_at", answer_terrain_tiles_at},
};

} // namespace

const char *terrain_query_op_choice(size_t index) { return index < std::size(kOps) ? kOps[index].token : nullptr; }

JsonValue answer_terrain(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const std::string path = args.text("path");
	const DocumentBase *view = context.core.documents().document_for(path);
	if (!view) {
		error = path.empty() ? std::string("terrain: no document is active.") : "terrain: " + path + " is not open.";
		return JsonValue();
	}
	if (!terrain_pane_kind(view->kind())) {
		error = "terrain: " + basename_of(view->path()) + " is neither a mission nor a terrain.";
		return JsonValue();
	}
	const std::string op = args.text("op");
	for (const OpRow &row : kOps)
		if (op == row.token) return row.answer(TerrainQueryView{context, args, *view}, error);
	error = "terrain: no op \"" + op + "\".";
	return JsonValue();
}

} // namespace opennova::editor
