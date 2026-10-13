#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <editor/documents/terrain_planes.h>
#include <editor/model/change_set.h>

namespace opennova {
class FileSource;
}

namespace opennova::editor {

class DocumentBase;
class MissionGround;
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
	// added to `out`. A file first followed adds none (the picture drew it as it is); one whose document opened since
	// adds its changes since its load (it loaded the file the picture read); one whose document closed or was read
	// again since is no patch (false).
	bool follow(const SessionView &view, const std::vector<std::pair<TerrainPlane, std::string>> &files, TerrainPatches &out);
	const std::vector<Followed> &followed() const { return followed_; }
	// The files the last follow found changed as no patch says (closed, read again, a history that no longer holds the
	// state followed).
	const std::vector<std::string> &unsaid() const { return unsaid_; }
	void clear() {
		followed_.clear();
		unsaid_.clear();
	}

private:
	std::vector<Followed> followed_;
	std::vector<std::string> unsaid_;
};

// The views' rule during a stroke (ADR 0046 S25, the shared seams): a view whose picture reads a terrain family's files
// (the mission's 3D view, its map, the terrain's own), while a gesture is open in one of them (ViewportModel::
// gesture_feeding_), patches its ground in place from the documents' changes (MissionGround::patch) and takes their new
// stamps, never reading them again whole per dab; the frame's patches kept for its device. Where a change cannot be
// patched (read again, a whole change, a plane the ground cannot patch) the caller reads the files again as before.
// Out of a stroke it follows the documents' states, patching the planes its device patches in place alone (a bake's
// landing, an undo or redo of a sculpt or a paint: the device patches its terrain, S25 A4 and the maps), so a stroke's
// first patch is against the state drawn.
class TerrainStrokeFollow {
public:
	// `in_place`: the planes (a bit each, terrain_plane_bit) the view's device patches in place, in a stroke and out of
	// one (an undo, a redo, a bake's landing): the heights (S25 A4), and for the mission's and the terrain's 3D views
	// the colour, blend and surface maps too (S25, the maps: TerrainMapsFollow). A patch of another plane makes the
	// picture again at the stroke's end (patched()), and out of a stroke is read again as before.
	explicit TerrainStrokeFollow(uint32_t in_place = terrain_plane_bit(TerrainPlane::Heights)) : in_place_(in_place) {}
	// The family of `document` (its view: a mission or a terrain) followed; `stroking`: a gesture feeds the picture now.
	// True when the ground holds every change since (each patched, or none), the patched files' names in `names` (for
	// the picture's own stamps: PreviewFollow::restamp). False: the caller reads again. An imported terrain's open
	// source closed or read again (its paint dropped) has the ground read whole at its next follow, in a stroke or out
	// of one: the import's output it stands for never moved, so nothing else would (MissionGround::read_again).
	bool follow(const SessionView &view, const DocumentBase &document, bool stroking, MissionGround &ground,
	            const FileSource &files, std::vector<std::string> &names);
	// The last follow's patches (each file once, its regions), and a serial that moves with each patch: the device takes
	// a frame's once.
	const TerrainPatches &patches() const { return patches_; }
	// A patch of a plane the device does not patch in place went in since the picture was last made: the stroke's end
	// makes it again.
	bool patched() const { return patched_; }
	void made() { patched_ = false; }
	void clear() {
		follow_.clear();
		patches_.clear();
		patched_ = false;
	}

private:
	uint32_t in_place_;
	TerrainFamilyFollow follow_;
	TerrainPatches patches_;
	bool patched_ = false;
};

// The open sources of an imported terrain's surface and foliage maps laid over `ground` (S25, the painting feedback):
// an import's output moves only as its set is saved and imported again, so a ground read from the project's files holds
// those maps as last imported while their source images, open, hold the paint since; each open source's texels written
// over the map its import makes, through the import's translation, as a view's stroke follow writes a stroke's
// (MissionGround::patch). The family is that of the open document at `view_path` (a mission or a terrain); true when
// one was laid. A view lays them over its ground each time it reads it whole (its first read among them, which may come
// after a stroke: its stroke follow then has no change to patch), so its ground holds them as the sources do.
bool lay_open_sources(const SessionView &view, const std::string &view_path, MissionGround &ground);

} // namespace opennova::editor
