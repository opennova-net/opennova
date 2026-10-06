#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_items.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/mission_poses.h>
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

// A mission's fog level as its start settles it, with no weather ticking it here: the engine's own settle
// (env::EnvScalarChannels::settled_fog_level, the start's ticks holding the current within 1000), as the
// game draws a mission whose level is past it (00TRe's 1500, TKH_C1B's 1024) from its first ticks. The
// mission device fogs by it.
float mission_settled_fog_level(float level);
// How far a framing sees through a mission's fog (world units, metres): the editor's framing choice, not
// the game's (as kMissionFrameDistance is), half the fog's end at the settled level as the game sets it
// [orig: Render_SetFogState @ 0x58a950 via env::compute_fog_params], the level the .env's under the
// header's overrides [orig: Game_StartMission @ 0x525371..0x525383] (no overcast at the start); 0 when the
// project lacks the .env the header names or its fog is off. A framing stands no farther (CP10's fog ends
// at 325 m: framed 400 m off, the picture was the fog's colour alone, as a game camera there would show it).
float mission_fog_reach(const FileSource &files, const MissionSceneHeader &header);
inline constexpr float kMissionFrameRadius = 10.0f;
inline constexpr float kMissionTurnSnap = 15.0f;
// A framing of everything on a mission whose marks spread past kMissionFrameSpread metres from their
// middle looks at its densest place instead: the marks within a cell of the kMissionFrameCell-metre
// cell that holds the most (S15).
inline constexpr float kMissionFrameSpread = 1500.0f;
inline constexpr float kMissionFrameCell = 400.0f;

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

	// The ground under the picture's point (x, y) in the game's words (DI-07, mission_ground_facts.h): what
	// the device's ray meets first (an entity's drawn surface: a body standing on it; the terrain: the class
	// the game reads there, its footstep slots and the round's effects row; nothing, or no device to say:
	// Nothing), the mission's terrain, tiles and water read through the game's own loads when first asked.
	MissionGroundFacts ground_under(const ViewportContext &context, float x, float y) const;
	// What that reads of the mission's files (its terrain, its char map, its tiles and its water plane), as
	// last asked.
	const MissionGround &ground_reader() const { return terrain_ground_; }

	// The marks on a picture `width` x `height` (mission_scene.h), an area's anchor on the ground of
	// `device` where it answers, each entity's with its item's bound (picked by it).
	std::vector<MissionMark> marks(int width, int height, const ViewportDevice *device) const;
	// What it reads of its entities' items (preview/mission_items: a drop's facts, the bound each item's
	// entity is picked by), as last followed.
	const MissionItemCache &items() const { return items_; }
	// Its people's spawn poses (DI-38, preview/mission_poses: what the game's organic init and its
	// warmup leave each placed person in), as last followed: the device poses each person's model by
	// its row's.
	const MissionPoses &poses() const { return poses_; }
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
	// a picture `width` x `height`, from its angles now, no farther than kMissionFrameDistance nor the
	// mission's fog reach (mission_fog_reach; kMissionFrameRadius at least).
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
	// A click (ViewportModel::click, its canvas driven): taken in every join while the picture is the
	// mission's as it is, at the picture's size.
	bool click_frame(const ViewportContext &context, SelectMode mode, int &width, int &height,
			std::string &error) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
			std::string &error) const override;
	// A drag of an entity's or an area's handle (mission_handle_edit.h): `move` on the ground (the
	// device's terrain, else the plane through the anchor), `height`, `yaw`, an area's edges; a
	// dragged record that is selected takes every selected entity with it (and every selected area,
	// for a move), one that is not moves alone. Its snap is metres, degrees for a turn.
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	// `frame` (the first named record, else the selection, else everything) and `top` (straight down
	// over the target, north up): each a SetViewport of the camera. `ground`: each named entity (else
	// each selected one) set down on the device's ground under it, its z the ground's less its model's
	// anchor height (preview/mission_items), one batch; refused with no ground under one.
	// `select_same` (S15): one SelectRecord of every entity whose item is a named (else a selected)
	// entity's, the primary kept where it is among them.
	bool command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
			CanvasRequests &out, std::string &error) const override;
	// The commands that take more than their records (S15): `duplicate {ids?, by?: [east, north]}`
	// (each named, else each selected, entity and area copied and moved by `by` metres, with stick its
	// height over the ground kept: one batch, preview/mission_place), `paste {at: [x, y]}` (the
	// clipboard's copied entities and areas pasted with their middle where the point meets the
	// ground: one batch of one Paste whose payload is moved there); the rest as command() plans them.
	bool command_of(const ViewportContext &context, const ViewportCommand &command, CanvasRequests &out,
			std::string &error) const override;
	// A drop (a model file, or an item by its id: the Place tool's): the item's entity added to the
	// pool its TYPE puts it in where the point meets the ground (the device's terrain, its model's
	// ground anchor baked in; else the plane through the camera's target), facing the way the camera
	// looks (S15: its yaw the camera's heading), one batch (an Add, then its x, y, z and yaw through
	// batch_made). Refused: a file that is no model, a model no item draws or several do (naming
	// them), an item no catalog of the project defines, a point over no ground. S15: a path's next
	// stop (`reference` "path", `name` its number: a marker of the item its stops use added at the
	// point and a stop naming it, preview/mission_place) and an area (`reference` "area", a box: an
	// area trigger over the ground the box's corners meet).
	bool drop(const ViewportContext &context, const ViewportDrop &drop, CanvasRequests &out,
			std::string &error) const override;
	// The Place tool's palette (preview/mission_palette) over the project's graph, the recently placed
	// (the preferences') first.
	io::JsonValue palette_json(
			const SessionView &view, const std::string &text, const JsonPage &page, std::string &error) const override;
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
	bool report_(const ViewportDeviceReport &report) override;

private:
	ViewportAction stop_(MissionViewStatus reason);
	// The mission's ground followed over the project's files (its terrain, tiles and water, DI-07).
	void follow_ground_(const SessionView &view) const;
	// The posed people stood on that ground (MissionPoses::stand) where a record, a pose (`posed`) or
	// the terrain moved; true when a person's lift moved.
	bool stand_people_(const SessionView &view, bool posed);
	// The scene's items' bounds asked of the project, where the scene, the graph or the asset source's
	// generation moved.
	void bound_items_(const SessionView &view);
	// The mission document a planner works over: the one at its path while the picture is current;
	// null, with why, otherwise.
	const Document *planned_(const ViewportContext &context, std::string &error) const;
	// The mark of the record whose row is `id` among `marks` (made from the scene now; -1: none).
	int mark_of_(const std::vector<MissionMark> &marks, NodeId id) const;
	// Where the picture point (x, y) meets the ground for a placing gesture (the device's terrain, else
	// the plane through the camera's target, that only as far as a pick reaches from the target: near
	// the horizon the plane is tens of kilometres out); false past it, or past what the file's
	// positions hold.
	bool ground_of_(const ViewportContext &context, float x, float y, double out[3], bool *on_terrain = nullptr) const;
	// The records a drag of `record` by `handle` takes with it, as pressed: the selected entities (and
	// areas, for a move) when it is selected, itself alone when not; `grabbed` its place among them.
	std::vector<MissionPressed> taken_(const ViewportContext &context, const Document &document,
			const NodeAddress &record, MissionHandle handle, size_t &grabbed) const;

	MissionViewportOptions options_;
	bool options_moved_ = false;
	OrbitCamera camera_;
	bool framed_ = false; // the camera framed a document's entities once
	float fog_reach_ = 0.0f; // mission_fog_reach of the scene's header over the files read, 0 for none
	MissionViewStatus reason_ = MissionViewStatus::NoProject;
	std::string detail_;
	PreviewFollow picture_;
	MissionScene scene_;
	// Mutable: a planner's facts (a drop's, the ground command's) read through it, each file parsed once
	// while it stands.
	mutable MissionItemCache items_;
	uint64_t bounds_serial_ = 0; // the scene's serial the bounds were last asked over
	uint64_t bounds_graph_ = 0; // the graph's generation then
	uint64_t bounds_files_ = 0; // and the asset source's
	MissionPoses poses_;
	uint64_t stood_serial_ = 0; // the scene's serial the people were last stood over
	int stood_reads_ = -1; // the ground's reads then
	std::vector<std::string> missing_;
	bool ground_ = false;
	// Mutable: the ground's facts read through it, its files read once while they stand (DI-07).
	mutable MissionGround terrain_ground_;
	bool gesture_open_ = false;
};

} // namespace opennova::editor
