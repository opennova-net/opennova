#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <editor/model/edit.h>
#include <editor/model/node.h>

namespace opennova::editor {

class Document;
class ViewportDevice;
struct OrbitCamera;
struct ViewportContext;

// What a drag in a mission's viewport writes (ADR 0046 S14): the handles of an entity (its place on
// the ground, its height, the heading it faces) and of an area trigger (its place, its four edges),
// each planned into Sets of the fields the file writes, in the file's own units: `x`, `y`, `z` in
// mission units (x east, y north, z up, absolute: a mission has no ground-snap flag), `yaw` whole
// degrees in 0..359, an area's `x_min`, `x_max`, `y_min`, `y_max`. One batch a sample, every edit
// carrying the gesture, so a drag is one undo step.
//
// Every sample is planned from the records as the gesture's press found them (MissionPressed), so a
// sample is absolute and the gesture's fold keeps the first value before and the last after per row.
// A field gets a Set only where the sample changes what the row holds now: a position's 16.16 word,
// a yaw's degrees. A sample that changes nothing plans nothing.

enum class MissionHandle : uint8_t { Move, Height, Yaw, XMin, XMax, YMin, YMax };
// "move", "height", "yaw" (an entity's; an area's move), "x_min", "x_max", "y_min", "y_max" (an
// area's edges).
bool mission_handle_from_token(const char *token, MissionHandle &out);
const char *mission_handle_token(MissionHandle handle);
// The handle is an area's edge.
inline bool mission_handle_is_edge(MissionHandle handle) { return handle >= MissionHandle::XMin; }

// A record as a gesture's press found it: an entity (its position and its yaw), or an area (its
// bounds: min and max on x, y and z; its anchor, x and y, the middle of its footprint).
struct MissionPressed {
	NodeAddress record;
	bool area = false;
	double x = 0.0, y = 0.0, z = 0.0;
	int yaw = 0;
	double min[3] = { 0.0, 0.0, 0.0 };
	double max[3] = { 0.0, 0.0, 0.0 };
};

// How far a ground pick reaches along the pointer's ray, metres (twice the widest world: sixteen
// 512 m sectors a side).
inline constexpr double kMissionPickReach = 16384.0;

// Where the ray through picture pixel (x, y) meets the ground, a mission point: the surface the
// context's device draws (its terrain: ViewportDevice::surface_between along the camera's ray), else
// the horizontal plane at mission height `plane_z`. `on_terrain` says which. False when neither is
// met (a ray level with the plane or away from it, over no terrain).
bool mission_ground_point(const ViewportContext &context, const OrbitCamera &camera, float x, float y,
		double plane_z, double out[3], bool *on_terrain = nullptr);

// A move: the grabbed record's anchor to the mission point `to` (x and y, each snapped to `snap`
// metres on the file's axes; 0 free), every other pressed record as far. With `stick` and a device
// that answers its ground, each entity keeps its height over the ground (its z the ground under
// where it goes plus what it stood above the ground where it was); else its z stands. An area moves
// its four x and y bounds; its z bounds stand. False for nothing pressed, or `grabbed` past them.
bool mission_move_edits(const Document &document, const std::vector<MissionPressed> &pressed, size_t grabbed,
		const double to[2], float snap, bool stick, const ViewportDevice *device, uint64_t gesture,
		std::vector<Edit> &out);
// A lift: the grabbed entity's z by `dz` (its z then snapped to `snap` metres), every other pressed
// entity as far; an area keeps its bounds. False as a move, or for an area grabbed.
bool mission_height_edits(const Document &document, const std::vector<MissionPressed> &pressed, size_t grabbed,
		double dz, float snap, uint64_t gesture, std::vector<Edit> &out);
// A turn: the grabbed entity's yaw by `delta` degrees (its yaw then snapped to `snap` degrees; 0:
// whole degrees), every other pressed entity as far, each about its own origin and kept in 0..359 as
// the file stores it. False as a lift.
bool mission_yaw_edits(const Document &document, const std::vector<MissionPressed> &pressed, size_t grabbed,
		double delta, float snap, uint64_t gesture, std::vector<Edit> &out);
// An area's edge to the mission coordinate `to` (snapped to `snap` metres), never past its opposite
// edge. False for a record that is no area, or a handle that is no edge.
bool mission_area_edge_edits(const Document &document, const MissionPressed &area, MissionHandle edge, double to,
		float snap, uint64_t gesture, std::vector<Edit> &out);

} // namespace opennova::editor
