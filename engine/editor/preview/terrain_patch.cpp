#include <editor/preview/terrain_patch.h>

#include <variant>

#include <base/io/strutil.h>
#include <editor/documents/terrain_heights_document.h>
#include <editor/model/document_base.h>
#include <editor/model/raster_document.h>
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
	const bool resolved = resolve_terrain_family(view, document, family);
	if (resolved)
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
		// An imported terrain's source stands for the map its import makes (the map the ground read).
		std::string stands_for;
		if (resolved && patch.plane != TerrainPlane::Heights) {
			const TerrainFamilyFile &file = family.plane(patch.plane);
			if (!file.import_output.empty() && file.path == patch.path) stands_for = basename_of(file.import_output);
		}
		if (!open || !ground.patch(patch, *open, stands_for)) {
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

bool lay_open_sources(const SessionView &view, const std::string &view_path, MissionGround &ground) {
	const DocumentBase *document = nullptr;
	for (const auto &each : view.documents.open)
		if (each && each->path() == view_path) document = each.get();
	TerrainFamily family;
	if (!document || !ground.terrain() || !resolve_terrain_family(view, *document, family)) return false;
	bool laid = false;
	for (const TerrainPlane plane : {TerrainPlane::Surface, TerrainPlane::Foliage}) {
		const TerrainFamilyFile &file = family.plane(plane);
		if (file.import_output.empty() || file.path.empty()) continue;
		const DocumentBase *open = nullptr;
		for (const auto &each : view.documents.open)
			if (each && each->path() == file.path) open = each.get();
		const RasterDocument *raster = open ? open->as_raster() : nullptr;
		if (!raster || raster->plane_count() != 1) continue;
		// The whole source, as one region of its texels (MissionGround::patch takes no whole map).
		TerrainPatch patch;
		patch.plane = plane;
		patch.path = file.path;
		patch.regions.push_back(RasterRegion{0, 0, raster->plane(0).width(), raster->plane(0).height(), 0});
		laid = ground.patch(patch, *open, basename_of(file.import_output)) || laid;
	}
	return laid;
}

} // namespace opennova::editor
