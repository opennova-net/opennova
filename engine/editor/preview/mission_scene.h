#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/model/change_set.h>
#include <editor/model/node.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/preview/model_preview_camera.h>

namespace opennova::editor {

class ViewportDevice;
struct MissionViewportOptions;

// What a mission's picture and its overlays read of the document (ADR 0046 S14): the entities of its
// four pools (what the device places, what the canvas marks), its area triggers and its paths (what
// the overlays draw), and the header's fields the device reads (the terrain, the environment, the
// time). Read whole when a document is first shown or read again, patched by the change set an edit
// answers (the changed rows alone), each in the mission's own units: positions as the document reads
// them (metres, x east, y north, z up; z absolute), eulers whole degrees.

enum class MissionPool : uint8_t { Item, Building, Marker, Organic };
// "item", "building", "marker", "organic".
const char *mission_pool_token(MissionPool pool);

struct MissionEntityMark {
	NodeId row = 0;
	NodeKind kind = 0;
	MissionPool pool = MissionPool::Item;
	int index = 0; // its place in its pool, the file's order
	int64_t item = 0; // the `item` field
	double x = 0.0, y = 0.0, z = 0.0;
	int pitch = 0, yaw = 0, roll = 0;
	int team = 0;
	// What the placement reads of it beside its item: its group and its attributes (bmsi_attributes:
	// NoShadow, Reflective, ... [orig: Entity_SpawnFromBMSRecord @ 0x40e9f0]).
	int group = 0;
	uint32_t attributes = 0;
	PreviewVec3 at; // its position in the presentation frame (the camera's)
	uint32_t stamp = 0; // moves when its transform, its item, its group or its attributes do (the scene's serial then)
};
// Whether two reads of an entity place alike beside the transform: the same item, group and
// attributes (a difference is placed again, as another item is).
inline bool mission_entity_places_alike(const MissionEntityMark &a, const MissionEntityMark &b) {
	return a.item == b.item && a.group == b.group && a.attributes == b.attributes;
}
struct MissionAreaMark {
	NodeId row = 0;
	NodeKind kind = 0;
	int index = 0;
	int zone = 0;
	double min[3] = { 0.0, 0.0, 0.0 };
	double max[3] = { 0.0, 0.0, 0.0 };
	bool constrains_z = false; // its z bounds hold (the file's flag bit 1)
};
struct MissionPathMark {
	NodeId row = 0;
	NodeKind kind = 0;
	int index = 0; // its number, 0..127
	uint32_t flags = 0; // bms::WaypointFlags: bit 0 does not loop, bit 1 blue, bit 2 red
	std::vector<NodeId> stops; // the marker rows its stops name (0: a stop naming none)
};
// The header's fields the device reads (formats/mission/mission.h's MissionInfo): the terrain, the
// tile set, the environment, the clock (the start time as the file packs it), and the environment
// overrides the attrib flags gate (env::bms_env_overrides_from_header takes them as they are).
struct MissionSceneHeader {
	std::string terrain, tile_set, environment;
	int start_time = 0, minutes_per_day = 0;
	uint32_t attrib_flags = 0;
	int water_override = 0, fog_override = 0;
	int fog_color[3] = { 0, 0, 0 }, water_color[3] = { 0, 0, 0 };
	int water_murk = 0;
};
bool operator==(const MissionSceneHeader &a, const MissionSceneHeader &b);
inline bool operator!=(const MissionSceneHeader &a, const MissionSceneHeader &b) { return !(a == b); }

// What a patch found changed, in the picture's terms: a header field the device reads; an entity
// added, removed or with another item, group or attributes (the device places again what differs); an entity's
// transform (the device moves it in place); an area, a path, a team, a name (the overlays alone).
struct MissionSceneDelta {
	bool header = false;
	bool reshaped = false;
	bool moved = false;
	bool overlays = false;
	bool any() const { return header || reshaped || moved || overlays; }
};

// Where the scene reads from: the mission viewport's, over the document's typed reads
// (documents/mission_reads.h); a test's, over what it holds. A row is an entity, an area or a path by
// which read answers it; the header row is the one header_row names.
class MissionSceneSource {
public:
	virtual ~MissionSceneSource() = default;
	virtual bool header(MissionSceneHeader &out) const = 0;
	virtual bool header_row(NodeId row) const = 0;
	// Every row of each kind, in the file's order (the entities pool by pool; the paths with a stop).
	virtual void entities(std::vector<MissionEntityMark> &out) const = 0;
	virtual void areas(std::vector<MissionAreaMark> &out) const = 0;
	virtual void paths(std::vector<MissionPathMark> &out) const = 0;
	// One row as it stands now, when it is of the kind (a path, only with a stop).
	virtual bool entity(NodeId row, MissionEntityMark &out) const = 0;
	virtual bool area(NodeId row, MissionAreaMark &out) const = 0;
	virtual bool path(NodeId row, MissionPathMark &out) const = 0;
};

class MissionScene {
public:
	// Everything anew from `source`.
	void read(const MissionSceneSource &source);
	// The rows `changes` names read again (an added or removed row reshapes the scene: everything
	// read again, the delta from what differs); what the picture makes of it.
	MissionSceneDelta patch(const RowChanges &changes, const MissionSceneSource &source);
	void clear();

	const MissionSceneHeader &header() const { return header_; }
	const std::vector<MissionEntityMark> &entities() const { return entities_; }
	const std::vector<MissionAreaMark> &areas() const { return areas_; }
	const std::vector<MissionPathMark> &paths() const { return paths_; }
	// A row's entity, area or path (null: none).
	const MissionEntityMark *entity(NodeId row) const;
	const MissionAreaMark *area(NodeId row) const;
	const MissionPathMark *path(NodeId row) const;
	// A row's mark among mission_marks' (every entity in the scene's order, then every area): an
	// entity's its index, an area's the entities' count plus its own; -1 for neither.
	int mark_index(NodeId row) const;
	// How many entities of each pool.
	size_t count(MissionPool pool) const;
	// Moves with every change the scene took.
	uint64_t serial() const { return serial_; }
	// How many rows the scene read from its source in all (a test pins what a patch reads).
	size_t rows_read() const { return rows_read_; }

private:
	void index_();
	MissionSceneHeader header_;
	std::vector<MissionEntityMark> entities_;
	std::vector<MissionAreaMark> areas_;
	std::vector<MissionPathMark> paths_;
	std::unordered_map<NodeId, size_t> entity_rows_, area_rows_, path_rows_;
	uint64_t serial_ = 0;
	size_t rows_read_ = 0;
};

// A mission point in the presentation frame (the camera's; world/presentation_frame.h).
PreviewVec3 mission_scene_point(double x, double y, double z);

// --- marks ---------------------------------------------------------------------------------------

// How far from a mark's pixel a press still takes it, and how many marks the overlays draw at most
// (the nearest).
inline constexpr float kMissionPickSlop = 8.0f;
inline constexpr size_t kMissionMarksDrawn = 2000;

// A mark: an entity or an area as the picture shows it, its anchor (an entity's position; an
// area's centre at its ground where a device answers, else at its z_min) projected by the camera.
// The marks are made for every entity (in the scene's order, then every area), shown or not, so a
// mark's index is stable within a frame.
struct MissionMark {
	NodeAddress record; // {row, kind, 0}
	const char *kind = ""; // "item", "building", "marker", "organic", "area"
	PreviewVec3 at;
	float x = 0.0f, y = 0.0f, depth = 0.0f; // the picture's pixels; depth in front of the eye
	bool shown = false; // on the picture, inside mark_range, its kind's marks on
	int entity = -1; // index into scene.entities(), -1 an area
	int area = -1; // index into scene.areas(), -1 an entity
};
std::vector<MissionMark> mission_marks(const MissionScene &scene, const MissionViewportOptions &options,
		const OrbitCamera &camera, int width, int height, const ViewportDevice *device);
// The front-most shown mark within `slop` of (x, y); -1 none.
int pick_mission_mark(const std::vector<MissionMark> &marks, float x, float y, float slop = kMissionPickSlop);
// The records of the shown marks whose anchors lie in the box from `a` to `b`, nearest first.
std::vector<NodeAddress> mission_box_records(const std::vector<MissionMark> &marks, CanvasPoint a, CanvasPoint b);

} // namespace opennova::editor
