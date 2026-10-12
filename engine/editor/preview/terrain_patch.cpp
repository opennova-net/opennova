#include <editor/preview/terrain_patch.h>

#include <variant>

#include <editor/model/document_base.h>
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
		if (was && (was->open != state.open || was->identity != state.identity || was->load != state.load)) {
			told = false;
		} else if (was && document && was->revision != state.revision) {
			ChangeSet changes;
			const RasterChanges *raster = nullptr;
			if (document->changes_since(was->load, was->revision, changes)) raster = std::get_if<RasterChanges>(&changes);
			if (!raster) told = false;
			else
				for (const RasterRegion &region : raster->regions) out.add(plane, path, region);
		}
		now.push_back(std::move(state));
	}
	followed_ = std::move(now);
	return told;
}

} // namespace opennova::editor
