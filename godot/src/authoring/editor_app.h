#pragma once

#include <godot_cpp/classes/file_dialog.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>

#include <editor/session/editor_request.h>
#include <editor/session/project_session.h>

#include "authoring/child_process.h"
#include "devtools/imgui_pass_node.h"

#if OPENNOVA_EDITOR_UI
#include <editor/ui/editor_windows.h>
#endif

namespace godot {

// The OpenNova Editor's shell (ADR 0046 d4/d10): the root node of
// res://editor/editor_root.tscn. It owns the portable project session and the
// ImGui workspace, attaches the workspace's pass through the shared ImGuiPassNode,
// pumps the session once per frame, and serves the requests only an OS can: the
// directory and file pickers, "show in folder", quitting. Nothing about the project
// lives here; the same typed methods GDScript calls are what the windows raise.
//
// Headless (tests, the packaging smoke): no ImGui context attaches, the windows never
// draw, and the typed methods drive the session directly. `--editor-smoke` on the
// game command line names the loaded variant and quits, for the packaged boot check.
class EditorApp : public ImGuiPassNode {
	GDCLASS(EditorApp, ImGuiPassNode)

public:
	EditorApp();
	~EditorApp() override;

	void _ready() override;
	void _exit_tree() override;
	void _process(double p_delta) override;

	// Where the editor keeps its own settings (recent projects, the runtime path);
	// read when the node enters the tree, so a test points it at a scratch file first.
	void set_settings_path(const String &p_path) { settings_path_ = p_path; }
	String get_settings_path() const { return settings_path_; }
	// Godot options handed to the game child before its game flags (a test runs the
	// child headless and self-quitting).
	void set_play_engine_args(const PackedStringArray &p_args) { play_engine_args_ = p_args; }
	PackedStringArray get_play_engine_args() const { return play_engine_args_; }

	// The typed seam: what the windows ask for, callable the same way from GDScript.
	bool new_project(const String &p_dir, const String &p_title);
	bool open_project(const String &p_dir);
	void close_project();
	// Creates every missing required file; returns how many required files are still
	// unmet afterwards.
	int create_missing_files();
	// Runs a build to its end; true when the build is good.
	bool build();
	// Builds, then starts the game on the build; true when the game started.
	bool play();
	void stop_play();
	// One session poll (what _process does each frame).
	void pump();


	bool create_catalog(const godot::String &p_path);
	bool open_catalog(const String &p_path);
	int get_catalog_row_count() const;
	int64_t get_catalog_row_id(int p_index) const;
	String get_catalog_row_name(int p_index) const;
	int64_t add_catalog_record(const String &p_kind, int64_t p_parent = 0);
	bool remove_catalog_record(int64_t p_id);
	bool set_catalog_text(int64_t p_id, const String &p_field, const String &p_value);
	bool set_catalog_integer(int64_t p_id, const String &p_field, int64_t p_value);
	bool set_catalog_real(int64_t p_id, const String &p_field, double p_value);
	String get_catalog_text(int64_t p_id, const String &p_field) const;
	int64_t get_catalog_integer(int64_t p_id, const String &p_field) const;
	bool save_catalogs();
	void catalog_undo();
	void catalog_redo();
	bool is_catalog_dirty() const;
	bool has_unsaved_prompt() const;
	void resolve_unsaved(int p_choice);
	int get_play_mcp_port() const;

	bool is_project_open() const;
	String get_project_title() const;
	String get_project_root() const;
	int get_required_missing() const;
	int get_required_total() const;
	String get_last_build_dir() const;
	bool is_last_build_ok() const;
	String get_play_state() const;
	bool did_game_exit_on_its_own() const;
	int get_problem_count() const;
	PackedStringArray get_output_lines() const;
	PackedStringArray get_recent_projects() const;
	// "editor": the variant this library is (the runtime variant has no EditorApp).
	String get_loaded_variant() const { return "editor"; }
	// True when Play drives the Godot binary at the source checkout instead of a
	// packaged runtime.
	bool is_source_run() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();
	opennova::devtools::ImGuiPass *engine_pass() override;
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
	int allocate_mcp_port();
	opennova::editor::PlayLauncher make_launcher(int p_mcp_port) const;

	std::unique_ptr<ChildProcessPlatform> platform_;
	std::unique_ptr<opennova::editor::ProjectSession> session_;
#if OPENNOVA_EDITOR_UI
	std::unique_ptr<opennova::editor::EditorWindows> windows_;
#endif
	String settings_path_ = "user://editor_settings.json";
	PackedStringArray play_engine_args_;
	FileDialog *picker_ = nullptr;
	opennova::editor::PickPurpose pending_pick_ = opennova::editor::PickPurpose::None;
};

} // namespace godot
