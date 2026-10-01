#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/file_dialog.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>
#include <vector>

#include <editor/preview/viewport_kinds.h>
#include <editor/session/editor_request.h>
#include <editor/session/project_session.h>

#include "authoring/child_process.h"
#include "devtools/imgui_pass_node.h"
#include "mnu/menu_frame.h"
#include "object/object_model.h"

#if OPENNOVA_EDITOR_UI
#include <editor/ui/editor_windows.h>
#endif

namespace opennova::editor {
class FilePreferencesStore;
class ViewportDeviceCache;
class ViewportModel;
struct ViewportContext;
} // namespace opennova::editor

namespace godot {

// The OpenNova Editor's shell (ADR 0046 d4/d10): the root node of
// res://editor/editor_root.tscn. It owns the portable project session and the
// ImGui workspace, attaches the workspace's pass through the shared ImGuiPassNode,
// pumps the session once per frame, and serves the requests only an OS can: the
// directory and file pickers, "show in folder", quitting. Nothing about the project
// lives here: GDScript, the editor MCP and the tests ask the session through the one wire seam
// (request_json, query_json), the windows through their typed requests.
//
// Headless (tests, the packaging smoke): no ImGui context attaches, the windows never
// draw, and the wire seam drives the session directly. `--editor-smoke` on the
// game command line names the loaded variant and quits, for the packaged boot check.
class EditorApp : public ImGuiPassNode {
	GDCLASS(EditorApp, ImGuiPassNode)

public:
	EditorApp();
	~EditorApp() override;

	void _ready() override;
	void _exit_tree() override;
	void _process(double p_delta) override;

	// Where the editor keeps its own settings (recent projects, the runtime path): the file its
	// preferences store keeps them in (opennova::editor::FilePreferencesStore), read when the
	// node enters the tree, so a test points it at a scratch file first.
	void set_settings_path(const String &p_path) { settings_path_ = p_path; }
	String get_settings_path() const { return settings_path_; }
	// Godot options handed to the game child before its game flags (a test runs the
	// child headless and self-quitting).
	void set_play_engine_args(const PackedStringArray &p_args) { play_engine_args_ = p_args; }
	PackedStringArray get_play_engine_args() const { return play_engine_args_; }

	// One session poll (what _process does each frame): a build or a Play raised through
	// request_json steps across the polls, never blocking the frame, and its operation shows it.
	void pump();
	// How much a poll steps the running operation: `p_ms` milliseconds of steps of `p_step_bytes`
	// bytes each (0 ms: one step per poll). A test slows a build to read it mid-way.
	void set_poll_budget(int p_ms, int64_t p_step_bytes);
	// How long each frame steps the viewports' builds (S13 V6, ViewportDeviceCache::step; the
	// `build_budget_ms` property): units while `p_ms` milliseconds have not passed since the frame's
	// first, one unit a frame in all at least, shared by every build in flight (0 ms: exactly one a
	// frame, a test's slow build). The editor's: kBuildBudgetMs.
	static constexpr int kBuildBudgetMs = 4;
	void set_build_budget_ms(int p_ms) { build_budget_ms_ = p_ms > 0 ? p_ms : 0; }
	int get_build_budget_ms() const { return build_budget_ms_; }

	// The wire seam (ADR 0046 d10, S13 A5): a request and a query as JSON text, read and answered
	// by the portable session (ProjectSession::handle_json and query), so the transport is a pump
	// and nothing about the project lives here. request_json answers {ok, served, error?, outcome
	// (with added, and made for an edit_record), status, revision}; the pickers are refused by
	// their kind, before their fields are read (they need a person), reveal_path is served here.
	// query_json answers the query `p_name` asks with `p_args` (an object of its params:
	// editor_queries.h), or {error} naming the query when it refuses them.
	String request_json(const String &p_json);
	String query_json(const String &p_name, const String &p_args = "{}");

	// The Preview's viewports through the thirteen device methods (until S13 V7's viewport tool), each
	// read with the devices synced first (ViewportDeviceCache::sync: the Preview's viewport of the
	// kind given a device and followed), so it shows the last edit, and each answering from the one
	// envelope (editor/preview/viewport_json.h: kind, path, status, reason, message, ..., body, items,
	// notes, view_revision). The setters are each a SetViewport of the Preview's viewport of the kind:
	// `device` {width, height} and `clock` {playing, rate, time_ms, ticks} as the change's own members
	// (objects; the device's size refused while a canvas sizes the picture), every other key the
	// kind's options (or its camera); false for an unknown key or value, or with no viewport of the
	// kind shown, get_preview_error() saying why.
	// The menu's (S9j), headless included: its envelope (body: the screen, the files the project
	// lacks; items: every widget's rect and text in design units, `device_rect` where the device
	// placed it), the widget the game's hit test finds at a design-space point ({kind, index, id,
	// name, current}), and its options (show_hidden, force_id, force_state: "normal", "mouseover",
	// "selected" or "disabled", and checked, popup_open, focus: the force_id window checked, its list
	// open, focused).
	String get_menu_preview_json();
	String menu_preview_hit_json(double p_x, double p_y);
	bool set_menu_preview_options(const Dictionary &p_options);
	// A window of the shown screen dragged by one of its handles (S9k1): "move",
	// "left", "right", "top", "bottom", "top_left", "top_right", "bottom_left" or
	// "bottom_right", by (dx, dy) design units from where the viewport shows it, the moved
	// edges snapped to the grid of 8 when `p_snap`; written as the canvas's drag writes it
	// (MenuViewport::drag), one undo step. A move of a selected window takes every selected
	// window of the screen with it (S9k2). False when the viewport does not show the menu's
	// current revision, the id is not a window of its screen, the handle is unknown, the drag
	// leaves no area, or the session refused it.
	bool menu_preview_drag(int64_t p_id, const String &p_handle, int p_dx, int p_dy, bool p_snap);
	// Windows of the shown screen arranged (S9k2, editor/preview/menu_arrange.h): `p_op`
	// "align_left", "align_right", "align_top", "align_bottom", "align_horizontal_centers",
	// "align_vertical_centers" (to the first id's window), "distribute_horizontally",
	// "distribute_vertically" (three or more), "bring_to_front", "bring_forward",
	// "send_backward" or "send_to_back", one undo step (MenuViewport::command). False for an
	// unknown op, too few windows, an id that is not a window the viewport shows at the menu's
	// current revision.
	bool menu_preview_arrange(const PackedInt64Array &p_ids, const String &p_op);
	// The model's (S10p3), headless included: its envelope (body: the level, the sphere, the
	// registers, the animation; items: the markers, each on the device's pixels; camera; clock);
	// its options (lod: a level or "auto"; ctrl: a Dictionary of register -> value, the whole held
	// set; overlays {user_points, lights, pivots}; rig_model: the model an animation plays on, ""
	// the one an item pairs); its camera (yaw, pitch, distance, target [x, y, z], frame: true to
	// look at the whole model).
	String get_model_preview_json();
	// The marker the model viewport's device point picks: {kind, index, id, name, current}.
	String model_preview_hit_json(double p_x, double p_y);
	bool set_model_preview_options(const Dictionary &p_options);
	bool set_model_preview_camera(const Dictionary &p_camera);
	// Why the last of the setters answered false ("" after one that answered true).
	String get_preview_error() const { return preview_error_; }
	// A marker of the model viewport dragged (S10p5): the record `p_id` (a user point or a
	// light) moved ("place") or its axis turned ("axis") to the point under device pixel
	// (p_x, p_y) on the plane through the handle that faces the eye, the place snapped to
	// `p_snap` metres (0: free); one undo step (ModelViewport::drag). False when the viewport is
	// not showing the document's revision, the record is no marker that moves, the handle is
	// unknown, or the session refused it.
	bool model_preview_drag(int64_t p_id, const String &p_handle, double p_x, double p_y, double p_snap);
	// The model viewport's device's camera and model, and the menu viewport's device's frame (null
	// without one), for the parity tests.
	Camera3D *get_model_preview_camera() const;
	ObjectModel *get_model_preview_model() const;
	MenuFrame *get_menu_preview_frame() const;
	// The editor's own MCP endpoint (the transport under res://editor/mcp/), started
	// by `--mcp-port <n>` at boot or by a test; the bound port, or 0 when it failed.
	int start_mcp_endpoint(int p_port);
	int get_mcp_port() const { return mcp_port_; }

	// "editor": the variant this library is (the runtime variant has no EditorApp).
	String get_loaded_variant() const { return "editor"; }
	// True when Play drives the Godot binary at the source checkout instead of a
	// packaged runtime.
	bool is_source_run() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();
	opennova::devtools::ImGuiPass *engine_pass() override;
	// The workspace's frame bracket around the layout pass (EditorWindows::begin_frame,
	// end_frame): the requests that act on the files as saved go after the frame's edits.
	void before_layout(double p_delta) override;
	void after_layout(uint64_t p_frame_index, bool p_drew, int64_t p_layout_us) override;

private:
	void ensure_session();
	void drain_requests();
	void serve(const opennova::editor::EditorRequest &p_request);
	void show_picker(opennova::editor::PickPurpose p_purpose, bool p_directory);
	void _on_dir_selected(const String &p_dir);
	void _on_file_selected(const String &p_file);
	void _on_files_selected(const PackedStringArray &p_files);
	void _on_picker_canceled();
	// A free loopback port for the game's MCP endpoint, allocated when the session spawns the game
	// (the launcher source the session asks, once the build lands).
	int allocate_mcp_port();
	opennova::editor::PlayLauncher make_launcher(int p_mcp_port) const;
	// The OS window's title follows the project and its unsaved files
	// (editor_window_title), set when it changes.
	void apply_window_title();
	// The devices synced (the Preview's targets given one and followed), then the viewport of
	// `kind` the Preview shows (null: none).
	const opennova::editor::ViewportModel *preview_(opennova::editor::ViewportKind p_kind);
	String preview_json_(opennova::editor::ViewportKind p_kind);
	// A SetViewport of the Preview's viewport of `kind` (its path left out): true when done.
	bool set_viewport_(opennova::editor::ViewportKind p_kind, const opennova::io::JsonValue &p_change);
	// What the planners read of the Preview's viewport of `kind`: the view, the clock, its document,
	// the size its device draws at (ViewportModel::size: a canvas's where one draws it), its device.
	opennova::editor::ViewportContext context_(const opennova::editor::ViewportModel &p_model, float p_snap) const;
	// The requests a planner made, handled in order: true when each was done.
	bool serve_(const std::vector<opennova::editor::EditorRequest> &p_requests);
	// The SubViewports of devices given up before this frame, freed (queued: they go at the frame's
	// end, after the ImGui pass of this frame drew without them).
	void free_retired_();

	std::unique_ptr<ChildProcessPlatform> platform_;
	// The preferences' store, owned here and outliving the session that reads and writes it.
	std::unique_ptr<opennova::editor::FilePreferencesStore> preferences_;
	std::unique_ptr<opennova::editor::ProjectSession> session_;
#if OPENNOVA_EDITOR_UI
	std::unique_ptr<opennova::editor::EditorWindows> windows_;
#endif
	// The SubViewports of the devices given up (their instance ids), kept in the tree until the next
	// frame: the ImGui pass may have drawn one's texture this frame. Before devices_, which retires
	// into it as it goes.
	std::vector<uint64_t> retired_;
	// The viewports' devices (S13 V5): at most four, by document and kind, made under this node.
	std::unique_ptr<opennova::editor::ViewportDeviceCache> devices_;
	// The milliseconds each frame gives the devices' builds (S13 V6).
	int build_budget_ms_ = kBuildBudgetMs;
	String settings_path_ = "user://editor_settings.json";
	PackedStringArray play_engine_args_;
	FileDialog *picker_ = nullptr;
	opennova::editor::PickPurpose pending_pick_ = opennova::editor::PickPurpose::None;
	Node *mcp_service_ = nullptr;
	int mcp_port_ = 0;
	String window_title_; // the title last set on the OS window
	String preview_error_; // why the last preview setter refused ("" none)
};

} // namespace godot
