#include <editor/preview/terrain_patch.h>

#include <variant>

#include <base/io/strutil.h>
#include <editor/documents/terrain_heights_document.h>
#include <editor/model/document_base.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/project/project_files.h>
#include <editor/session/terrain_family.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

bool TerrainFamilyFollow::follow(const SessionView &view, const std::vector<std::pair<TerrainPlane, std::string>> &files,
                                 TerrainPatches &out) {
	bool told = true;
	std::vector<Followed> now;
	for (const auto &[plane, path] : files) {
		Followed state;
		state.plane = plane;
		state.path = path;
		const DocumentBase *document = nullptr;
		for (const auto &open : view.documents.open)
			if (open->path() == path) document = open.get();
		if (document) {
			state.open = true;
			state.identity = document->identity();
			state.load = document->load_generation();
			state.revision = document->revision();
		}
		const Followed *was = nullptr;
		for (const Followed &each : followed_)
			if (each.path == path && each.plane == plane) was = &each;
		// A document opened since (a stroke's file opened quietly) holds the file the picture read as it loaded: its
		// changes since its load (revision 0 of it) are what the picture lacks.
		const bool opened = was && !was->open && state.open;
		if (was && !opened && (was->open != state.open || was->identity != state.identity || was->load != state.load)) {
			told = false;
		} else if (was && document && (opened || was->revision != state.revision)) {
			ChangeSet changes;
			const RasterChanges *raster = nullptr;
			if (document->changes_since(opened ? state.load : was->load, opened ? 0 : was->revision, changes))
				raster = std::get_if<RasterChanges>(&changes);
			if (!raster) told = false;
			else
				for (const RasterRegion &region : raster->regions) out.add(plane, path, region);
		}
		now.push_back(std::move(state));
	}
	followed_ = std::move(now);
	return told;
}

bool TerrainStrokeFollow::follow(const SessionView &view, const DocumentBase &document, bool stroking,
                                 MissionGround &ground, const FileSource &files, std::vector<std::string> &names) {
	names.clear();
	patches_.clear();
	TerrainFamily family;
	std::vector<std::pair<TerrainPlane, std::string>> planes;
	if (resolve_terrain_family(view, document, family))
		for (size_t i = 0; i < kTerrainPlaneCount; ++i)
			if (!family.planes[i].path.empty() && !family.planes[i].missing)
				planes.emplace_back(static_cast<TerrainPlane>(i), family.planes[i].path);
	TerrainPatches now;
	const bool told = follow_.follow(view, planes, now);
	if (!told) return !stroking;
	bool maps = false;
	for (const TerrainPatch &patch : now.patches) {
		// Out of a stroke only the planes the device patches in place are (a bake's landing, an undo or a redo of a
		// sculpt or a paint: the device patches its terrain, S25 A4 and the maps); another's change there is read again
		// as before.
		const bool in_place = (in_place_ & terrain_plane_bit(patch.plane)) != 0;
		if (!stroking && !in_place) continue;
		const DocumentBase *open = nullptr;
		for (const auto &each : view.documents.open)
			if (each && each->path() == patch.path) open = each.get();
		if (!open || !ground.patch(patch, *open)) {
			if (stroking) return false;
			continue;
		}
		names.push_back(basename_of(patch.path));
		// A heightmap image's stroke moves the .cpt it stands in for (S25 A2b): that file's stamp is the picture's.
		if (patch.plane == TerrainPlane::Heights)
			if (const auto *document = dynamic_cast<const TerrainHeightsDocument *>(open); document && document->image())
				names.push_back(basename_of(document->cpt_path()));
		maps = maps || !in_place;
		if (patch.whole) patches_.add(patch.plane, patch.path, RasterRegion(), true);
		for (const RasterRegion &region : patch.regions) patches_.add(patch.plane, patch.path, region);
	}
	if (!names.empty()) ground.restamp(files, names);
	// The in-place planes' patches are the device's: only another's makes the picture again at the end.
	if (maps) patched_ = true;
	return true;
}

} // namespace opennova::editor
