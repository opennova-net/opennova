#include <editor/session/terrain_queries.h>

#include <algorithm>

#include <editor/documents/mission_document.h>
#include <editor/documents/terrain_heights_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/session_core.h>
#include <editor/session/terrain_family.h>
#include <editor/session/terrain_follow.h>
#include <editor/session/terrain_sculpt.h>

namespace opennova::editor {

using io::JsonValue;

// bake: the view's heights file's bake (S25 A3): whether it is open, baked (a .cpt's tiles as read, an image's after
// its first bake), the regions whose bake is due and whether one runs, the pane's words for it, the bakes the session
// landed and started so far, and why the bake refuses the heights as they stand ("" none).
JsonValue answer_terrain_bake(const TerrainQueryView &query, std::string &error) {
	TerrainFamily family;
	if (!resolve_terrain_family(query.context.core.view(), query.view, family)) {
		error = "terrain: " + family.error;
		return JsonValue();
	}
	const TerrainFamilyFile &file = family.plane(TerrainPlane::Heights);
	if (file.path.empty()) {
		error = "terrain: " + (file.refusal.empty() ? std::string("the view's terrain names no heights") : file.refusal);
		return JsonValue();
	}
	TerrainSculpt &sculpt = query.context.core.sculpt();
	const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(query.context.core.documents().document_for(file.path));
	JsonValue out = JsonValue::make_object();
	out.set("path", io::json_string(file.path));
	out.set("open", JsonValue::make_bool(heights != nullptr));
	const std::string words = sculpt.bake_words(file.path);
	const bool running = words.rfind("Baking ", 0) == 0;
	JsonValue regions = JsonValue::make_array();
	if (heights) {
		for (const HeightsRect &rect : heights->heights().unbaked) {
			JsonValue region = JsonValue::make_array();
			region.push(io::json_number(rect.x));
			region.push(io::json_number(rect.y));
			region.push(io::json_number(rect.w));
			region.push(io::json_number(rect.h));
			regions.push(std::move(region));
		}
	}
	out.set("baked", JsonValue::make_bool(heights ? heights->heights().baked : true));
	out.set("due", JsonValue::make_bool(running || (heights && !heights->heights().unbaked.empty())));
	out.set("running", JsonValue::make_bool(running));
	out.set("regions", std::move(regions));
	out.set("words", io::json_string(words));
	out.set("landed", io::json_number(double(sculpt.bakes_landed())));
	out.set("started", io::json_number(double(sculpt.bakes_started())));
	out.set("refused", io::json_string(heights ? heights->bake_refusal() : std::string()));
	return out;
}

// lock: the entities under a disc at (x, y) (radius: the brush's where none is asked) that follow the ground when
// the heights are sculpted and those height-locked, in each open mission the stroke's bake moves: the view's own
// where it is a mission, else the open missions whose own strokes edit the same heights (the follow pass's routing,
// terrain_follow.h).
JsonValue answer_terrain_lock(const TerrainQueryView &query, std::string &error) {
	const SessionView &view = query.context.core.view();
	TerrainFamily family;
	if (!resolve_terrain_family(view, query.view, family)) {
		error = "terrain: " + family.error;
		return JsonValue();
	}
	if (!query.args.has("x") || !query.args.has("y")) {
		error = "terrain: lock needs x and y.";
		return JsonValue();
	}
	const double x = query.args.number("x"), y = query.args.number("y");
	const double radius = query.args.has("radius") ? query.args.number("radius") : view.workspace.terrain_brush.radius;
	const std::vector<const MissionDocument *> missions = missions_under_view(view, query.view, family);
	JsonValue out = JsonValue::make_object();
	out.set("path", io::json_string(query.view.path()));
	JsonValue at = JsonValue::make_array();
	at.push(io::json_number(x));
	at.push(io::json_number(y));
	out.set("at", std::move(at));
	out.set("radius", io::json_number(radius));
	JsonValue rows = JsonValue::make_array();
	size_t follow = 0, locked = 0;
	for (const MissionDocument *mission : missions) {
		const GroundFollowers under = ground_followers(*mission, x, y, radius);
		follow += under.follow;
		locked += under.locked;
		JsonValue row = JsonValue::make_object();
		row.set("mission", io::json_string(mission->path()));
		row.set("follow", io::json_number(double(under.follow)));
		row.set("locked", io::json_number(double(under.locked)));
		rows.push(std::move(row));
	}
	out.set("missions", std::move(rows));
	out.set("follow", io::json_number(double(follow)));
	out.set("locked", io::json_number(double(locked)));
	return out;
}

} // namespace opennova::editor
