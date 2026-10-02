#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_model.h>

namespace opennova::editor {

class Document;
struct MissionCanvasFrame;

// What a mission viewport shows, and why not (ADR 0046 S14): the kind's reason.
enum class MissionViewStatus : uint8_t {
	NoProject, // no project is open
	NoMission, // no mission is open at its path
	Ready,
};
// "no_project", "no_mission", "ready": its token on the wire.
const char *mission_view_status_token(MissionViewStatus status);
// The line a mission viewport shows for a status ("" when ready).
std::string mission_view_status_message(MissionViewStatus status);

// How far the camera stands from what it frames at most (metres), how far down a first framing looks
// (radians: 35 degrees), the least sphere a framing looks at (metres), and the degrees a turn snaps
// to while the canvas snaps.
inline constexpr float kMissionFrameDistance = 400.0f;
inline constexpr float kMissionFramePitch = 0.610865f;
inline constexpr float kMissionFrameRadius = 10.0f;
inline constexpr float kMissionTurnSnap = 15.0f;

// The change a SetViewport makes to set a mission viewport's camera to `camera` (its target, heading,
// pitch and distance, in the mission's terms: mission_camera.h): what a look, a fly, a pan, an orbit,
// a dolly or a framing on its canvas sends.
std::string mission_camera_change(const OrbitCamera &camera);

// A mission's viewport (ADR 0046 S14; ViewportKind::Mission, the Document tab's main view): the
// mission document at its path as it stands (never its bytes: a mission that cannot be written still
// shows), its terrain, environment and entities drawn by the Shell's device as the game draws them,
// its marks, areas and paths drawn over the picture by its canvas. It keeps a scene of what the
// picture and the overlays read of the document (mission_scene.h), read whole when the document is
// first followed, read again or changed in a way it cannot say, and patched by the change set an edit
// answers: the changed rows alone, so a drag of ten entities reads ten rows. What follows for the
// device: a header field it reads, an entity added, removed or of another item, or a file its picture
// read moved, builds again (the device diffs the scene against what it holds and builds only what
// differs); an entity's transform is an Update (moved in place); a team, an area, a path, an event
// are the overlays' alone (Keep). Its row holds a Rebuild for a gesture, so nothing builds under a
// drag. Its camera is an OrbitCamera in the presentation frame, flown as well as orbited
// (mission_camera.h), framed on the entities when a document is first read; its options the layers
// the device draws, the marks the canvas draws, the mark range, stick and the time of day
// (mission_options.h).
class MissionViewport final : public ViewportModel {
public:
	explicit MissionViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	MissionViewStatus view_status() const { return reason_; }
	const MissionViewportOptions &options() const { return options_; }
	const OrbitCamera &camera() const { return camera_; }
	const MissionScene &scene() const { return scene_; }
	// The names its device asked the project's files for and did not find (its notes), and whether
	// its device holds a surface a ray lands on (its terrain, built).
	const std::vector<std::string> &missing() const { return missing_; }
	bool ground() const { return ground_; }
	// A gesture is open in its document, as it last followed (the Shell's pump each frame): what its
	// device defers to the gesture's end (a moved entity's terrain shadow).
	bool gesture_open() const { return gesture_open_; }

	// The marks on a picture `width` x `height` (mission_scene.h), an area's anchor on the ground of
	// `device` where it answers.
	std::vector<MissionMark> marks(int width, int height, const ViewportDevice *device) const;
	// How far from its anchor the primary's handles stand, metres: a share of the camera's distance,
	// so they keep their size on the picture.
	float handle_reach() const { return camera_.distance * 0.08f; }
	// A record of the scene as a press finds it (an entity's position and yaw, an area's bounds);
	// false for a record that is neither.
	bool pressed(const NodeAddress &record, MissionPressed &out) const;
	// Where a mark's handle stands, the presentation frame: Move its anchor, Height and Yaw an
	// entity's handles at the reach, an edge the middle of an area's side at its anchor's height.
	// False for a handle the mark has not (an area has no height or yaw, an entity no edge).
	bool handle_at(const MissionMark &mark, MissionHandle handle, PreviewVec3 &out) const;
	// The camera looking at the marks `of` (the indexes into `marks`; none: every entity and area) on
	// a picture `width` x `height`, from its angles now, no farther than kMissionFrameDistance.
	OrbitCamera framed(const std::vector<MissionMark> &marks, const std::vector<int> &of, int width, int height) const;

	// What a canvas maps of it in a frame (mission_canvas.h): its marks and, while the mission is the
	// active document, the selected records' marks.
	MissionCanvasFrame canvas_frame(const ViewportContext &context) const;

	ViewportStatus status() const override;
	const char *reason() const override { return mission_view_status_token(reason_); }
	std::string message() const override { return mission_view_status_message(reason_); }
	const std::string &detail() const override { return detail_; }
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	std::vector<ViewportHit> box(const ViewportContext &context, float x0, float y0, float x1,
			float y1) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
			std::string &error) const override;
	// A drag of an entity's or an area's handle (mission_handle_edit.h): `move` on the ground (the
	// device's terrain, else the plane through the anchor), `height`, `yaw`, an area's edges; a
	// dragged record that is selected takes every selected entity with it (and every selected area,
	// for a move), one that is not moves alone. Its snap is metres, degrees for a turn.
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	// `frame` (the first named record, else the selection, else everything) and `top` (straight down
	// over the target, north up): each a SetViewport of the camera.
	bool command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
			CanvasRequests &out, std::string &error) const override;
	io::JsonValue options_json() const override;
	io::JsonValue camera_json() const override;
	io::JsonValue body_json(const ViewportInput &input) const override;
	io::JsonValue items_json(const ViewportInput &input) const override;
	io::JsonValue notes_json(const ViewportInput &input) const override;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;
	void report_(const ViewportDeviceReport &report) override;

private:
	ViewportAction stop_(MissionViewStatus reason);
	// The mission document a planner works over: the one at its path while the picture is current;
	// null, with why, otherwise.
	const Document *planned_(const ViewportContext &context, std::string &error) const;
	// The mark of the record whose row is `id` (-1: none shown).
	static int mark_of_(const std::vector<MissionMark> &marks, NodeId id);
	// The records a drag of `record` by `handle` takes with it, as pressed: the selected entities (and
	// areas, for a move) when it is selected, itself alone when not; `grabbed` its place among them.
	std::vector<MissionPressed> taken_(const ViewportContext &context, const Document &document,
			const NodeAddress &record, MissionHandle handle, size_t &grabbed) const;

	MissionViewportOptions options_;
	bool options_moved_ = false;
	OrbitCamera camera_;
	bool framed_ = false; // the camera framed a document's entities once
	MissionViewStatus reason_ = MissionViewStatus::NoProject;
	std::string detail_;
	PreviewFollow picture_;
	MissionScene scene_;
	std::vector<std::string> missing_;
	bool ground_ = false;
	bool gesture_open_ = false;
};

} // namespace opennova::editor
