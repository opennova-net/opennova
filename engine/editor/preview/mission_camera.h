#pragma once

#include <string>

#include <base/io/json.h>
#include <editor/preview/model_preview_camera.h>

namespace opennova::editor {

// The mission viewport's camera (ADR 0046 S14): the model preview's OrbitCamera in the presentation
// frame the scene renders in (world/presentation_frame.h: a mission's (x, y, z), x east, y north, z
// up, is the frame's (x, z, -y)), so the overlays project through OrbitCamera::project onto the pixel
// the device drew, and the device's camera takes the same eye and axes. Orbited, panned and dollied
// as the model's is, and flown as well: looked about from its eye and moved with it. On the wire it
// is in the mission's own terms: its target a mission point, its yaw the compass heading it looks
// along in degrees as an entity's yaw (0 north, 90 east), its pitch the degrees it looks down, its
// distance in metres.

// A fly's speed, metres a second, and what Shift multiplies it by; what a wheel notch dollies; what a
// pixel of a look's travel turns, radians.
inline constexpr float kMissionFlySpeed = 20.0f;
inline constexpr float kMissionFlyFast = 4.0f;
inline constexpr float kMissionWheelDolly = 0.85f;
inline constexpr float kMissionLookRadiansPerPixel = 0.005f;

// A mission point in the presentation frame, and a presentation point in the mission's.
PreviewVec3 mission_to_preview(const double mission[3]);
void preview_to_mission(const PreviewVec3 &point, double out[3]);

// A look: the camera turned about its eye by a pointer's travel in pixels (right turns it right,
// down looks down, its pitch kept short of the poles); the target goes, the eye stays.
void mission_camera_look(OrbitCamera &camera, float dx, float dy);
// The eye and the target moved together, metres along the camera's right, the world's up and the
// camera's forward.
void mission_camera_fly(OrbitCamera &camera, float right, float up, float forward);
// Where the ray through picture pixel (x, y) meets the horizontal plane at mission height `z`, a
// mission point; false when the ray runs level with it or away from it.
bool mission_camera_on_height(const OrbitCamera &camera, float x, float y, int width, int height, double z,
		double out[3]);
// The compass heading the camera looks along, degrees in 0..360 (the wire's yaw), and the degrees it
// looks down (the wire's pitch).
double mission_camera_heading(const OrbitCamera &camera);
double mission_camera_pitch(const OrbitCamera &camera);
// The camera straight down over its target, north up (the `top` command's): as far down as its pitch
// goes, its heading north.
void mission_camera_top(OrbitCamera &camera);

// The camera on the wire: {target: [x, y, z], yaw, pitch, distance, eye: [x, y, z], fov}, the points
// mission points, the angles degrees; `eye` and `fov` (the horizontal field of view) are read only.
io::JsonValue mission_camera_to_json(const OrbitCamera &camera);
// A SetViewport's camera member, {target?, yaw?, pitch?, distance?}, set over `camera`: every member
// checked before any applies; false, the camera as it was, with `error` naming the member and what it
// takes (a member it does not take, `eye` and `fov` among them, refused).
bool mission_camera_from_json(const io::JsonValue &json, OrbitCamera &camera, std::string &error);

} // namespace opennova::editor
