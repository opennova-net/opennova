#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_model.h>

namespace opennova::editor {

class Document;

// A mission's 2D map (ADR 0046 S23 C; ViewportKind::Map, token `map`, the Preview window's beside the mission's 3D
// view): the mission seen from straight above, north up, its terrain's colour map as the game's commander map (CMAP)
// draws it, its entities, areas, paths and the player's route over it as pins, picked, boxed and dragged as the 3D
// view's marks are (the same SelectRecord, the same move batches, one undo step a drag).
//
// The projection is the CMAP's own [orig: CMapWindow_HandleEvent @0x5497f0 -> HUD_BuildMapOverlayView @0x5a7e10, mode
// 4; hud::CommandMapView::render]: the map centred on a point of the mission (the player's place plus the pan in the
// game, the camera's centre here), north up, its scale the CMAP's law, Z x 65536 over the picture's width x 200, in
// metres a pixel (HUD_DrawMapOverlay @0x5a64eb..0x5a650b), a mission point at (cx + (x - X) / scale, cy - (y - Y) /
// scale) about the picture's middle (Terrain_FixedPointToWorldFloat @0x607060, its +Y negated). The device draws the
// terrain through the game's own compile of that pass (hud::HudFrameCompiler::compile_command_map over the project's
// hudpos.def and the mission's terrain); the canvas draws the editor's pins over it.
//
// The editor's choices beside the CMAP's: the zoom reaches past the CMAP's 10 to kMissionMapZoomMax so a whole map
// fits a pane (the CMAP clamps to [0.1, 10]); the wheel zooms by the CMAP's buttons' step about the pointer rather
// than the centre; the left button selects, boxes and drags records, so the map pans by the right or middle button
// (the CMAP pans by its left).

// The CMAP's zoom law, and the editor's reach past its ceiling.
inline constexpr float kMissionMapZoomMin = 0.1f;
inline constexpr float kMissionMapZoomMax = 40.0f;

// The map's camera: the mission point at the picture's middle (metres, x east, y north) and the CMAP's zoom.
struct MissionMapCamera {
	double center[2] = { 0.0, 0.0 };
	float zoom = 4.0f; // the CMAP's opening zoom [orig: sub_54B280 @0x54b280]
	bool operator==(const MissionMapCamera &o) const {
		return center[0] == o.center[0] && center[1] == o.center[1] && zoom == o.zoom;
	}
	bool operator!=(const MissionMapCamera &o) const { return !(*this == o); }
};
// On the wire: {center: [x, y], zoom, scale (read only: metres a pixel at the device's size)}.
io::JsonValue mission_map_camera_to_json(const MissionMapCamera &camera, int width);
// A SetViewport's camera member, {center?, zoom?}, over `camera`: false, nothing changed, with why.
bool mission_map_camera_from_json(const io::JsonValue &json, MissionMapCamera &camera, std::string &error);

// The map's view of a picture `width` x `height`: the CMAP's rect over the whole picture, its scale and its middle.
struct MissionMapView {
	int width = 0, height = 0;
	double center[2] = { 0.0, 0.0 };
	float zoom = 4.0f;
	float scale = 1.0f; // metres a pixel
	float middle_x = 0.0f, middle_y = 0.0f; // the pixel the centre lands on
	// A mission point's pixel, and a pixel's mission point.
	void project(double x, double y, float &px, float &py) const;
	void unproject(float px, float py, double &x, double &y) const;
};
MissionMapView mission_map_view(const MissionMapCamera &camera, int width, int height);
// The zoom that shows `metres` across a picture `width` pixels wide (the CMAP's law inverted), in the map's range.
float mission_map_zoom_for(double metres, int width);

// The map's options: the marks by kind and the labels (the 3D view's), the CMAP's grid and text toggles (the
// commander map's GRID and TEXT, which the device's compile draws), stick (a moved entity keeps its height over the
// ground) and the grid a drag snaps to (metres; 0 free).
struct MissionMapOptions {
	bool items = true, buildings = true, markers = true, organics = true;
	bool areas = true, paths = true, labels = false;
	bool grid = true, text = true;
	bool stick = true;
	float snap = 1.0f;
	bool operator==(const MissionMapOptions &o) const;
	bool operator!=(const MissionMapOptions &o) const { return !(*this == o); }
};
// {marks: {items, buildings, markers, organics, areas, paths, labels}, grid, text, stick, snap}.
io::JsonValue mission_map_options_to_json(const MissionMapOptions &options);
bool mission_map_options_from_json(const io::JsonValue &json, MissionMapOptions &held, std::string &error);

// A record as the map shows it: an entity at its position, an area at its middle (its box beside), a pixel each.
struct MissionMapMark {
	NodeAddress record;
	const char *kind = ""; // "item", "building", "marker", "organic", "area"
	double x = 0.0, y = 0.0; // mission metres
	float px = 0.0f, py = 0.0f;
	bool shown = false; // on the picture, its kind's marks on
	int entity = -1; // into scene.entities()
	int area = -1; // into scene.areas()
	int team = 0;
	MissionPool pool = MissionPool::Item;
};
// Every entity (the scene's order) then every area, shown or not, so an index is stable within a frame.
std::vector<MissionMapMark> mission_map_marks(const MissionScene &scene, const MissionMapOptions &options,
		const MissionMapView &view);
// The shown mark nearest (x, y) within kMissionPickSlop pixels, the later drawn first among equals (-1 none).
int pick_mission_map_mark(const std::vector<MissionMapMark> &marks, float x, float y);
// The records of the shown marks inside the box from `a` to `b`.
std::vector<NodeAddress> mission_map_box_records(const std::vector<MissionMapMark> &marks, CanvasPoint a, CanvasPoint b);

// The player's route (the first path whose flags carry PlayerRoute, the route the game's single player follows:
// documents' mission_reads), -1 for none.
int mission_map_route(const MissionScene &scene);

// What the device draws the map from beside the camera and the options: the mission's terrain and water plane (as the
// game resolves it, DI-07's MissionGround), and the commander map grid's origin, the first marker whose record's type
// is 2043 (Map Centerpoint) [orig: HUD_InitOverlaySystem @0x5a4999, the scan of entity+80 == 2043].
struct MissionMapGround {
	std::string terrain;
	bool water = false;
	double water_height = 0.0;
	bool grid_origin = false;
	double grid_x = 0.0, grid_y = 0.0;
};

class MissionMapViewport final : public ViewportModel {
public:
	explicit MissionMapViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	const MissionMapOptions &options() const { return options_; }
	const MissionMapCamera &camera() const { return camera_; }
	const MissionScene &scene() const { return scene_; }
	const MissionMapGround &ground() const { return ground_; }
	MissionMapView view(int width, int height) const { return mission_map_view(camera_, width, height); }
	std::vector<MissionMapMark> marks(int width, int height) const;
	// A record as a press finds it (MissionViewport::pressed's).
	bool pressed(const NodeAddress &record, MissionPressed &out) const;
	// The records a drag of `record` takes: the selected entities and areas when it is selected, else itself;
	// `grabbed` its place among them.
	std::vector<MissionPressed> taken(const ViewportContext &context, const Document &document, const NodeAddress &record,
			size_t &grabbed) const;
	// The mission document a planner works over while the picture is current; null, with why, otherwise.
	const Document *planned(const ViewportContext &context, std::string &error) const;
	// A record by the project's display names.
	std::string title(const ViewportContext &context, const NodeAddress &record) const;
	// The camera that shows the marks `of` (rows; none: every entity and area) on a picture `width` x `height`.
	MissionMapCamera framed(const std::vector<NodeId> &of, int width, int height) const;
	// The SetViewport of a camera.
	std::string camera_change(const MissionMapCamera &camera) const;

	ViewportStatus status() const override;
	const char *reason() const override;
	std::string message() const override;
	const std::string &detail() const override { return detail_; }
	const FileStamps *picture_reads() const override { return &picture_.files(); }
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	std::vector<ViewportHit> box(const ViewportContext &context, float x0, float y0, float x1, float y1) const override;
	bool click_frame(const ViewportContext &context, SelectMode mode, int &width, int &height,
			std::string &error) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
			std::string &error) const override;
	// A drag of a record's `move` handle: the 3D view's move (mission_move_edits) to the point under the pointer,
	// the selected records with it, with stick over the device's ground; one batch a sample under one gesture.
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	// `frame` (the named records, else the selection, else everything), `zoom_in` and `zoom_out` (the CMAP's
	// ZOOMIN and ZOOMOUT steps): each a SetViewport of the camera.
	bool command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
			CanvasRequests &out, std::string &error) const override;
	io::JsonValue options_json() const override;
	io::JsonValue camera_json() const override;
	io::JsonValue body_json(const ViewportInput &input) const override;
	io::JsonValue items_json(const ViewportInput &input) const override;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;
	bool report_(const ViewportDeviceReport &report) override;

private:
	enum class Reason : uint8_t { NoProject, NoMission, Ready };
	ViewportAction stop_(Reason reason);
	void follow_ground_(const SessionView &view);

	Reason reason_ = Reason::NoProject;
	std::string detail_;
	PreviewFollow picture_;
	MissionScene scene_;
	MissionMapOptions options_;
	MissionMapCamera camera_;
	bool framed_ = false;
	bool moved_ = false; // what the device draws moved since the last follow (an Update)
	MissionGround reader_; // the terrain and water as the game reads them (DI-07)
	MissionMapGround ground_;
	bool surface_ = false; // the device read the terrain
	io::JsonValue drawn_; // what the device's last pass drew (its report)
};

} // namespace opennova::editor
