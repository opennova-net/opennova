#include <editor/session/terrain_family.h>

#include <editor/assets/asset_registry.h>
#include <editor/model/document_base.h>
#include <editor/project/project_files.h>
#include <editor/session/view/session_view.h>
#include <runtime/mission/mission_sidecars.h>

namespace opennova::editor {

namespace {

// The name without its extension.
std::string stem(const std::string &name) {
	const size_t dot = name.rfind('.');
	return dot == std::string::npos ? name : name.substr(0, dot);
}

// A file of the family by the name the game reads it by: the project's file of that name, where it has one.
TerrainFamilyFile named_file(const SessionView &view, const std::string &name) {
	TerrainFamilyFile file;
	file.name = name;
	if (view.project.scan)
		if (const AssetEntry *entry = view.project.scan->find(name)) file.path = entry->relative_path;
	return file;
}

} // namespace

bool resolve_terrain_family(const SessionView &view, const DocumentBase &document, TerrainFamily &out) {
	out = TerrainFamily();
	out.view = document.path();
	if (document.kind() == AssetKind::Mission) {
		// The mission's own tiles, which the game reads in place of the terrain's [orig: Terrain_Init @ 0x60FCFD;
		// runtime/mission/mission_sidecars.h's tiles row].
		if (const mission::Sidecar *tiles = mission::sidecar_for_role("tiles")) {
			out.tiles = named_file(view, mission::sidecar_name(basename_of(document.path()), *tiles));
			out.tiles.words = out.tiles.name + (out.tiles.path.empty() ? ", made with its first tile" : "");
		}
	} else if (document.kind() == AssetKind::Terrain) {
		out.terrain = document.path();
		out.terrain_name = stem(basename_of(document.path()));
	} else {
		out.error = basename_of(document.path()) + " is neither a mission nor a terrain: it has no terrain to brush.";
		return false;
	}
	// The planes' routes are the round's next step (S25 C1: the game's resolution per mission, D-TERRAIN-17/18); until
	// then no plane resolves, and a stroke says so.
	for (size_t i = 0; i < kTerrainPlaneCount; ++i) {
		TerrainFamilyFile &file = out.planes[i];
		file.refusal = std::string("Brushing ") + terrain_plane_words(static_cast<TerrainPlane>(i)) +
		               " is not ready yet in this build.";
	}
	return true;
}

} // namespace opennova::editor
