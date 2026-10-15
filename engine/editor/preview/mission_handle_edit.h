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
// degrees in 0..359, an area's `x_min`, `x_max`, `y_min`, `y_max` (S24: its corners, two of them at
// once, and where its flags bound z, its `z_min` and `z_max`). One batch a sample, every edit carrying
// the gesture, so a drag is one undo step.
//
// Every sample is planned from the records as the gesture's press found them (MissionPressed), so a
// sample is absolute and the gesture's fold keeps the first value before and the last after per row.
// A field gets a Set only where the sample changes what the row holds now: a position's 16.16 word,
// a yaw's degrees. A sample that changes nothing plans nothing.

enum class MissionHandle : uint8_t {
	Move, Height, Yaw, XMin, XMax, YMin, YMax,
	// S24: an area's corners (the south-west, south-east, north-east and north-west), its bottom and its top.
	XMinYMin, XMaxYMin, XMaxYMax, XMinYMax, ZMin, ZMax,
};
// "move", "height", "yaw" (an entity's; an area's move), "x_min", "x_max", "y_min", "y_max" (an
// area's edges), "x_min_y_min", "x_max_y_min", "x_max_y_max", "x_min_y_max" (its corners), "z_min",
// "z_max" (its bottom and its top).
bool mission_handle_from_token(const char *token, MissionHandle &out);
// The handle is an area's edge, its corner, its bottom or top, any of them.
inline bool mission_handle_is_edge(MissionHandle handle) {
	return handle >= MissionHandle::XMin && handle <= MissionHandle::YMax;
}
inline bool mission_handle_is_corner(MissionHandle handle) {
	return handle >= MissionHandle::XMinYMin && handle <= MissionHandle::XMinYMax;
}
inline bool mission_handle_is_level(MissionHandle handle) {
	return handle == MissionHandle::ZMin || handle == MissionHandle::ZMax;
}
inline bool mission_handle_is_area(MissionHandle handle) { return handle >= MissionHandle::XMin; }

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
// context's device draws (its terrain: ViewportDevice::surface_between along the camera's ray, the
// height there ViewportDevice::ground_at's), else the horizontal plane at mission height `plane_z`.
// `on_terrain` says which. False when neither is met (a ray level with the plane or away from it, over
// no terrain).
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
// whole degrees), every other pressed entity as far, each kept in 0..359 as the file stores it. One
// entity turns about its own origin; several records turn together about their group's centre
// (ADR 0046 S15, mission_turn_centre), each entity's position carried round it clockwise as a compass
// heading turns (its height over the ground kept with `stick` over a device that answers it) and each
// area's middle likewise, its box on the file's axes (its extents swapped by an odd number of quarter
// turns). False as a lift.
bool mission_yaw_edits(const Document &document, const std::vector<MissionPressed> &pressed, size_t grabbed,
		double delta, float snap, uint64_t gesture, std::vector<Edit> &out, bool stick = false,
		const ViewportDevice *device = nullptr);
// The centre a group turns about (two or more pressed records): the middle of the box of the entities'
// positions and the areas' middles. False for fewer than two, which turn about their own origin.
bool mission_turn_centre(const std::vector<MissionPressed> &pressed, double out[2]);
// Each pressed record put back where the press found it (place, height, heading; an area's bounds),
// the fields that moved alone: what a drag cancelled (Esc) writes under its gesture.
void mission_restore_edits(const Document &document, const std::vector<MissionPressed> &pressed, uint64_t gesture,
		std::vector<Edit> &out);
// An area's handle to the mission point `to` (x, y, z; each coordinate the handle moves snapped to `snap`
// metres), never past the opposite bound: an edge its one bound (x for the west and east edges, y for the
// south and north), a corner its two, the bottom and the top z_min and z_max. False for a record that is no
// area, or a handle that is none of an area's.
bool mission_area_handle_edits(const Document &document, const MissionPressed &area, MissionHandle handle,
		const double to[3], float snap, uint64_t gesture, std::vector<Edit> &out);

} // namespace opennova::editor
