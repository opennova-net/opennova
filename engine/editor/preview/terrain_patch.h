#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <editor/documents/terrain_planes.h>
#include <editor/model/change_set.h>

namespace opennova::editor {

struct SessionView;

// What changed of one terrain file a view's picture reads, since the view last drew it (ADR 0046 S25, the shared
// seams): its plane, its project path and the regions of the plane's texels that changed (RasterRegion), or the whole
// plane. During a stroke a viewport's device patches these in place (an Update), never building its picture again;
// the device applies the union of a frame's regions once.
struct TerrainPatch {
	TerrainPlane plane = TerrainPlane::Heights;
	std::string path;
	std::vector<RasterRegion> regions;
	bool whole = false;
};

// A frame's patches (each file once, its regions gathered), and a serial that moves with each one added: what a
// device compares to take them once.
struct TerrainPatches {
	std::vector<TerrainPatch> patches;
	uint64_t serial = 0;
	bool empty() const { return patches.empty(); }
	void clear() { patches.clear(); }
	// `region` of `path` changed (`whole`: all of it).
	void add(TerrainPlane plane, const std::string &path, const RasterRegion &region, bool whole = false) {
		++serial;
		for (TerrainPatch &patch : patches)
			if (patch.path == path && patch.plane == plane) {
				patch.whole = patch.whole || whole;
				if (!whole) patch.regions.push_back(region);
				return;
			}
		TerrainPatch patch;
		patch.plane = plane;
		patch.path = path;
		patch.whole = whole;
		if (!whole) patch.regions.push_back(region);
		patches.push_back(std::move(patch));
	}
};

// What a viewport keeps of the open documents that hold the terrain files its picture reads (S25): each one's state
// (its identity, load and revision) as the picture last followed it. A follow gathers what changed since as patches,
// from the documents' changes (RasterChanges): false when one changed as no patch says (read again, or a history that
// no longer holds the state followed), and the picture is then made again.
class TerrainFamilyFollow {
public:
	struct Followed {
		TerrainPlane plane = TerrainPlane::Heights;
		std::string path;
		uint64_t identity = 0, load = 0, revision = 0;
		bool open = false;
	};
	// Follow the files (each its plane and project path) as the open documents of `view` hold them, the patches since
	// added to `out`. A file first followed adds none (the picture drew it as it is); one whose document closed or opened
	// since is no patch (false).
	bool follow(const SessionView &view, const std::vector<std::pair<TerrainPlane, std::string>> &files, TerrainPatches &out);
	const std::vector<Followed> &followed() const { return followed_; }
	void clear() { followed_.clear(); }

private:
	std::vector<Followed> followed_;
};

} // namespace opennova::editor
