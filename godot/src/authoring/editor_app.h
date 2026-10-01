#pragma once

#include <godot_cpp/classes/file_dialog.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>
#include <vector>

#include <editor/session/editor_request.h>
#include <editor/session/project_session.h>

#include "authoring/child_process.h"
#include "devtools/imgui_pass_node.h"

#if OPENNOVA_EDITOR_UI
#include <editor/ui/editor_windows.h>
#endif

namespace opennova::editor {
class FilePreferencesStore;
class ViewportDeviceCache;
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
	// (with added, made for an edit_record, gesture for an edit_in_viewport's drag), status,
	// view_revision}; the pickers are refused by their kind, before their fields are read (they need
	// a person), reveal_path is served here. query_json answers the query `p_name` asks with `p_args`
	// (an object of its params: editor_queries.h), or {error} naming the query when it refuses them.
	// A viewport is read and changed through them alone (S13 V7: the viewport query, set_viewport and
	// edit_in_viewport), its device taking what changed at the next pump.
	String request_json(const String &p_json);
	String query_json(const String &p_name, const String &p_args = "{}");

	// The device drawing the viewport of `p_kind` ("menu", "model") over the document at `p_path`
	// (its project-relative path, as the viewport query's envelope names it): its offscreen
	// SubViewport, the kind's nodes under it (a menu's MenuFrame; a model's Camera3D and
	// ObjectModel), or null while the Shell holds none for it. For the device parity tests alone: a
	// viewport is read through query_json's `viewport` and changed through request_json's
	// set_viewport and edit_in_viewport (S13 V7).
	SubViewport *get_viewport_device(const String &p_path, const String &p_kind) const;
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
};

} // namespace godot
