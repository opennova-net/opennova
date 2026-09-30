#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/file_dialog.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>

#include <editor/session/editor_request.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/project_session.h>

#include "authoring/child_process.h"
#include "devtools/imgui_pass_node.h"
#include "object/object_model.h"

#if OPENNOVA_EDITOR_UI
#include <editor/ui/editor_windows.h>
#include "authoring/menu_preview.h"
#include "authoring/model_preview.h"
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
	// Creates every missing required file (the roles of the unmet rows); returns how many
	// required files are still unmet afterwards.
	int create_missing_files();
	void stop_play();
	// One session poll (what _process does each frame): a build or a Play raised through
	// request_json steps across the polls, never blocking the frame, and its operation shows it.
	void pump();
	// How much a poll steps the running operation: `p_ms` milliseconds of steps of `p_step_bytes`
	// bytes each (0 ms: one step per poll). A test slows a build to read it mid-way.
	void set_poll_budget(int p_ms, int64_t p_step_bytes);


	// The document seam (ADR 0046 d9), one family for every document type: the active
	// document's rows and records by identity, fields as Variants, the undo journal.
	// create_file is true when the file was made (or was there) and, for a kind the editor
	// edits, opened; a kind it makes but does not edit (a font) is made, not opened.
	bool create_file(const String &p_path);
	bool open_document(const String &p_path);
	int get_row_count() const;
	int64_t get_row_id(int p_index) const;
	String get_row_name(int p_index) const;
	// A new record of the document type's kind name ("weapon", "action", "window", ...): a
	// row when `p_parent` is 0, else inside the record (or row) `p_parent`, at `p_position`
	// among that owner's records of the kind (-1 = the end). The new identity, or 0.
	int64_t add_record(const String &p_kind, int64_t p_parent = 0, int64_t p_position = -1);
	bool remove_record(int64_t p_id);
	bool set_field(int64_t p_id, const String &p_field, const Variant &p_value);
	// An optional field left out of the file (its value kept for when it is written again).
	bool clear_field(int64_t p_id, const String &p_field);
	// An optional field the file leaves out, written again with the value it reads (a
	// set_field of that same value leaves it out).
	bool write_field(int64_t p_id, const String &p_field);
	// A field's value; nil for an optional field the file leaves out.
	Variant get_field(int64_t p_id, const String &p_field) const;
	bool save_documents();
	void undo();
	void redo();
	bool is_document_dirty() const;
	// The records a row or a record holds (every collection, or the one of kind name
	// `p_kind`), the record that holds a record (0 for a row), a record's name by
	// identity, and a record by the symbol another document names it with, in a scope ("" any;
	// 0 = none).
	PackedInt64Array get_child_records(int64_t p_id, const String &p_kind = String()) const;
	int64_t get_record_owner(int64_t p_id) const;
	String get_record_name(int64_t p_id) const;
	int64_t find_record(const String &p_symbol, const String &p_scope = String()) const;
	// The unsaved-changes prompt: open while a request waits on it; answered with 0 (Save:
	// the files it lists, then what waited runs), 1 (Discard) or 2 (Cancel).
	bool has_unsaved_prompt() const;
	void resolve_unsaved(int p_choice);
	int get_play_mcp_port() const;
	// The addressed record copied after itself, or moved to `p_position` among its
	// siblings or, with `p_parent`, among the records of another owner in its row.
	bool duplicate_record(int64_t p_id);
	bool move_record(int64_t p_id, int p_position, int64_t p_parent = 0);
	// The selection: a record selected ("replace", "add" to it, "toggle" in or out of it;
	// the selected records stay inside one row), the selected identities (the primary
	// last selected among them).
	bool select_record(int64_t p_id, const String &p_mode = "replace");
	PackedInt64Array get_selected_records() const;
	// The clipboard: the selected records copied, or cut (copied, then removed as one
	// step), and pasted into `p_parent` at `p_position` (0 / -1: after the selection).
	bool copy_records();
	bool cut_records();
	bool paste_records(int64_t p_parent = 0, int p_position = -1);
	// The coalesced edit group (typing) or the gesture (a drag) ends.
	void end_edit();

	// The wire seam (ADR 0046 d10, the editor MCP): the same requests and view as JSON
	// text, marshalled by the portable session_json so the transport stays a pump.
	// request_json answers {ok, served, error, outcome, status, revision (the view's any:
	// session_revisions.h)}; the pickers are
	// refused (they need a person), reveal_path and quit are served here. get_outcome_json
	// is what the last request came to (a typed seam call's included): {done,
	// unsaved_prompt, operation, findings}.
	String request_json(const String &p_json);
	String get_outcome_json() const;
	// The running operation and what the last one came to, as the view JSON has them
	// ({operation, last_operation}): what editor_build and editor_play wait on, frame by frame.
	String get_operation_json() const;
	// The view (session_view_to_json): a page of the output lines and of the import dialog's lists.
	String get_view_json(int p_output_cursor = 0, int p_output_limit = 200, int p_import_offset = 0,
			int p_import_limit = 200) const;
	// "" = the active document; with rows, every row and its nested records.
	String get_document_json(const String &p_path, bool p_with_rows) const;
	String get_record_json(int64_t p_id) const;
	// A reference field of a record of the active document: the names its picker offers
	// (session_json's reference_choices_to_json), and the places its Go to leads with the
	// value it holds (reference_targets_to_json); null for no such record or field.
	String get_reference_choices_json(int64_t p_id, const String &p_field) const;
	String get_reference_targets_json(int64_t p_id, const String &p_field) const;
	// Find in a document ("" = the active one): every field whose value as the Inspector shows it
	// holds the text (session_json's document_hits_to_json), or null when it is not open.
	String search_document_json(const String &p_path, const String &p_text, bool p_match_case) const;
	// The Problems query (session_json's problem_query_from_json: severities, text, scope,
	// fixable, group, offset, limit) answered as the window answers it: {total, shown,
	// counts, groups when grouped, problems with their fixes}, or {error} for a query that
	// does not parse.
	String get_problems_json(const String &p_query = "{}");
	PackedStringArray get_request_kinds() const;
	// The asset graph (S7): what a file references, who names a file or a symbol (kind token +
	// name, in the scope the symbol is defined in: "" any), who uses a file (who names it or
	// what it defines), every missing reference, the symbols of a kind.
	String get_references_json(const String &p_path) const;
	String get_referrers_json(const String &p_path) const;
	String get_symbol_referrers_json(const String &p_kind, const String &p_name, const String &p_scope = String()) const;
	String get_usages_json(const String &p_path) const;
	String get_missing_references_json() const;
	String get_symbols_json(const String &p_kind) const;
	// Find in the project: the files and symbols whose names hold the text, each with its usage
	// count (session_json's graph_search_to_json).
	String search_project_json(const String &p_text) const;
	// The menu preview (S9j), headless included: what it shows as JSON (status, message,
	// the screen, the missing files, every widget's rect and text in design units:
	// editor/preview/menu_preview_json.h), the widget the game's hit test finds at a
	// design-space point, and its options (width, height, show_hidden, force_id,
	// force_state: "normal", "mouseover", "selected" or "disabled", and checked,
	// popup_open, focus: the force_id window checked, its list open, focused); false for an
	// unknown option or value. Each read follows the session first, so it shows the last
	// edit.
	String get_menu_preview_json();
	String menu_preview_hit_json(double p_x, double p_y);
	bool set_menu_preview_options(const Dictionary &p_options);
	// A window of the previewed screen dragged by one of its handles (S9k1): "move",
	// "left", "right", "top", "bottom", "top_left", "top_right", "bottom_left" or
	// "bottom_right", by (dx, dy) design units from where the preview shows it, the moved
	// edges snapped to the grid of 8 when `p_snap`; written as the preview window's drag
	// writes it (editor/preview/menu_layout_edit.h), one undo step. A move of a selected
	// window takes every selected window of the screen with it (S9k2). False when the
	// preview is not showing the menu's current revision, the id is not a window of its
	// screen, the handle is unknown, or the drag leaves no area.
	bool menu_preview_drag(int64_t p_id, const String &p_handle, int p_dx, int p_dy, bool p_snap);
	// Windows of the previewed screen arranged (S9k2, editor/preview/menu_arrange.h): `p_op`
	// "align_left", "align_right", "align_top", "align_bottom", "align_horizontal_centers",
	// "align_vertical_centers" (to the first id's window), "distribute_horizontally",
	// "distribute_vertically" (three or more), "bring_to_front", "bring_forward",
	// "send_backward" or "send_to_back", one undo step. False for an unknown op, too few
	// windows, an id that is not a window the preview shows at the menu's current revision.
	bool menu_preview_arrange(const PackedInt64Array &p_ids, const String &p_op);
	// A screen (its row id) of a menu (project-relative path) as the render check compiled
	// it headless with every validation (S9j2), in the preview's schema: the parity read of
	// the preview (the same widgets, the same notes). status no_screen when the check has
	// no such screen.
	String get_menu_render_json(const String &p_path, int64_t p_screen) const;
	// The editor MCP's menu tools (S9m, editor/preview/menu_report.h,
	// editor/session/record_batch.h), `p_path` a project-relative path or a logical name,
	// "" the previewed menu (else the active document when it is a menu), the same menu for
	// all three. A menu's tree: every screen with its windows (identity, name, type, owner,
	// depth, text, the lists each holds, the rect the render check placed it at while that
	// render is current); "null" for no menu. Its Problems rows by source (graph, render,
	// menu, ...) and each screen's note count.
	String get_menu_tree_json(const String &p_path) const;
	String get_menu_findings_json(const String &p_path) const;
	// A batch by record identity and label (`p_json` {edits: [...]}) or a list replaced
	// ({id, list, records}) on the menu at `p_path` (opened first when it is not; refused
	// when it names no menu), as one EditRecord request, one undo step: {ok, error?,
	// outcome, made {label: id}, added, revision}.
	String edit_menu_json(const String &p_path, const String &p_json);
	// The model preview (S10p3), headless included: what it shows as JSON (the status, the
	// level, the camera, the registers, the user points on the device:
	// editor/preview/model_preview_json.h); its options (lod: a level or "auto"; ctrl: a
	// Dictionary of register -> value, the whole held set); its camera (yaw, pitch,
	// distance, target [x, y, z], frame: true to look at the whole model, width and height:
	// the device size in pixels). The options also take playing (the clock runs), overlays
	// {user_points, lights, pivots} (what it marks), time_ms (a seek of its clock), rig_model
	// (the model an animation plays on, "" the one an item pairs) and clip_ticks (a seek of
	// the clip clock). False
	// for an unknown key or value. Each read follows the session first, so it shows the
	// last edit.
	String get_model_preview_json();
	// The marker the model preview's device point picks: {kind, index, id, name, current}.
	String model_preview_hit_json(double p_x, double p_y);
	bool set_model_preview_options(const Dictionary &p_options);
	bool set_model_preview_camera(const Dictionary &p_camera);
	// A marker of the model preview dragged (S10p5): the record `p_id` (a user point or a
	// light) moved ("place") or its axis turned ("axis") to the point under device pixel
	// (p_x, p_y) on the plane through the handle that faces the eye, the place snapped to
	// `p_snap` metres (0: free); one undo step. False when the preview is not showing the
	// document's revision, the record is no marker that moves, or the handle is unknown.
	bool model_preview_drag(int64_t p_id, const String &p_handle, double p_x, double p_y, double p_snap);
	// The device's camera and model (null without one), for the parity tests.
	Camera3D *get_model_preview_camera() const;
	ObjectModel *get_model_preview_model() const;
	// The editor's own MCP endpoint (the transport under res://editor/mcp/), started
	// by `--mcp-port <n>` at boot or by a test; the bound port, or 0 when it failed.
	int start_mcp_endpoint(int p_port);
	int get_mcp_port() const { return mcp_port_; }

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
	int allocate_mcp_port();
	opennova::editor::PlayLauncher make_launcher(int p_mcp_port) const;
	// The OS window's title follows the project and its unsaved files
	// (editor_window_title), set when it changes.
	void apply_window_title();

	std::unique_ptr<ChildProcessPlatform> platform_;
	std::unique_ptr<opennova::editor::ProjectSession> session_;
	opennova::editor::ProblemQueryCache problems_; // get_problems_json's answer while the view and query stand
	opennova::editor::ProblemFixCache fixes_;      // and its problems' fixes while the view stands
#if OPENNOVA_EDITOR_UI
	std::unique_ptr<opennova::editor::EditorWindows> windows_;
	std::unique_ptr<MenuPreview> menu_preview_;
	std::unique_ptr<ModelPreview> model_preview_;
#endif
	String settings_path_ = "user://editor_settings.json";
	PackedStringArray play_engine_args_;
	FileDialog *picker_ = nullptr;
	opennova::editor::PickPurpose pending_pick_ = opennova::editor::PickPurpose::None;
	Node *mcp_service_ = nullptr;
	int mcp_port_ = 0;
	String window_title_; // the title last set on the OS window
};

} // namespace godot
