#include <editor/session/terrain_queries.h>

#include <editor/documents/terrain_heights_document.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/session_core.h>
#include <editor/session/terrain_family.h>
#include <editor/session/terrain_sculpt.h>

namespace opennova::editor {

using io::JsonValue;

// bake: the view's heights file's bake (S25 A3): whether it is open, baked (a .cpt's tiles as read, an image's after
// its first bake), the regions whose bake is due and whether one runs, the pane's words for it, the bakes the session
// landed so far.
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
	return out;
}

io::JsonValue answer_terrain_lock(const TerrainQueryView &query, std::string &error) {
	(void)query;
	error = "terrain: lock is not answered yet in this build.";
	return io::JsonValue();
}

} // namespace opennova::editor
