#pragma once

#include <editor/assets/asset_import.h>
#include <editor/model/document.h>
#include <editor/session/editor_request.h>
#include <editor/session/output_log.h>
#include <editor/session/session_operation.h>
#include <editor/session/session_revisions.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <editor/project_build/build_run.h>
#include <editor/requirements/requirements.h>
#include <editor/run/play_session.h>

namespace opennova::editor {

class MenuRenderCheck;

// Everything the editor's windows draw (ADR 0046 d10): the records in. The session owns
// one and rewrites it as the project changes; the windows read it by const reference
// every frame and never reach into the session. `revisions` counts the changes of each
// concern (session_revisions.h), so a window keeps what it derives until a concern it reads
// moves.
struct SessionView {
	ViewRevisions revisions;

	std::vector<std::shared_ptr<const Document>> documents;
	std::string active_document;
	// The selection in the active document: the primary record (the inspector's, the one
	// a new record goes beside) and every selected record, the primary among them, all
	// inside one row. Repaired after every edit, undo and redo: a record that is gone
	// drops out, a removed primary gives way to its owner, a new record is selected.
	NodeAddress selection;
	std::vector<NodeAddress> selected;
	// The field of the selected record a request asked to be shown (OpenDocument's
	// edit.field: a Problems row's field), for the inspector; cleared when the selection
	// changes. Every such ask bumps reveal_serial, the same field asked again too (a second
	// click on a Problems row), so the inspector shows it again each time.
	std::string reveal_field;
	uint64_t reveal_serial = 0;
	// The project file a request asked Files to show (ShowInFiles: a Problems row about a file
	// the editor does not open, or about a file's name), project-relative, and whether Files
	// asks its new name (Rename...). Every ask bumps reveal_file_serial, the same file asked
	// again too, so Files shows it again each time.
	std::string reveal_file;
	bool reveal_file_rename = false;
	uint64_t reveal_file_serial = 0;
	// What Copy and Cut put on the clipboard: the payload of the document type that made
	// it (Document::copy), which Paste hands back to the same type.
	std::string clipboard;
	// The selection is `address` alone (none for an empty address).
	void select_only(const NodeAddress &address);
	// SelectRecord in `path` (which becomes the active document): see SelectMode.
	void select(const std::string &path, const NodeAddress &address, SelectMode mode);
	// After an edit that added records to `document`: they are the selection (the first
	// the primary; one another of them holds is left out) and the document is the active
	// one.
	void select_added(const Document &document);
	// After an edit, an undo or a redo of the active `document`: the selected records it no
	// longer has drop out; a primary that is gone gives way to `owner` (the owner the
	// primary had before the edit) when it is still there, else to the last record
	// still selected.
	void repair_selection(const Document &document, const NodeAddress &owner);
	// What the menu preview shows: the selected screen (row) of the last menu document a
	// selection landed in. It stays while another document is active (the stylesheet the
	// screen draws with), and clears when that menu closes or the screen is gone.
	struct MenuPreviewTarget {
		std::string path;
		NodeId screen = 0;
	};
	MenuPreviewTarget menu_preview;
	// What the model preview shows (S10p2, S10p6): the last model, clip or animation table
	// document made active (a clip or a table plays on its rig's model). It stays while
	// another document is active, and clears when that document closes.
	struct ModelPreviewTarget {
		std::string path;
	};
	ModelPreviewTarget model_preview;
	// Follow the active document and the selection (every view change calls it).
	void update_previews();
	// The project's asset graph (S7), rebuilt with every validation: the inspector's
	// badges, the pickers, "find references" and the Problems rows read the same edges.
	std::shared_ptr<const AssetGraph> graph;
	// The project's files by logical name, the open documents standing in for theirs: what
	// the menu preview reads the way the game reads its mounted files. Follows every
	// rescan and every change to the open documents (its generation moves).
	std::shared_ptr<const ProjectAssetSource> assets;
	// Every menu screen of the project compiled headless as the game draws it (S9j2), run
	// with every validation: its notes are Problems rows (never a build's gate), and a
	// screen's render answers the MCP's editor_menu_preview render.
	std::shared_ptr<const MenuRenderCheck> render_check;
	// The unsaved-changes prompt, open while a request waits on it: what waits (`action`,
	// and the path it names: the document a Close or a Reload closes, the directory of a
	// project to open, the file a rename renames), the files with unsaved edits it lists (what
	// its Save writes), and whether Discard is offered (not for Build and Play, which pack the
	// files on disk, nor for an import or a rename, which write over them).
	struct UnsavedPrompt {
		bool open = false;
		EditorRequestKind action = EditorRequestKind::Quit;
		std::string target;
		std::vector<std::string> files;
		bool can_discard = true;
	};
	UnsavedPrompt unsaved_prompt;
	bool quit_requested = false;
	bool project_open = false;
	std::string project_root;
	ProjectDocument document;
	AssetScan scan;
	RequirementReport requirements;
	// The project's current findings (scan + requirements) followed by the last action's.
	std::vector<Diagnostic> diagnostics;

	// The operation that runs (a build: its progress, what it works on, whether it can be
	// cancelled, what it holds; id 0 when none), and what the last one came to (id 0 before the
	// first ends).
	OperationStatus operation;
	OperationOutcome last_operation;
	bool has_build = false;
	BuildReport last_build;

	PlayState play_state = PlayState::Stopped;
	int64_t play_pid = -1;
	int play_mcp_port = 0; // the port the running game's MCP endpoint answers on (0: no game, or none)
	std::string play_command_line;
	bool play_exited_on_its_own = false;
	// The code the last game exited with on its own (PlaySession::exit_code; -1: none, or it
	// was stopped). Nonzero, it is a Problems row (play.crashed) until Play starts again or
	// the project closes.
	int64_t play_exit_code = -1;
	std::string runtime_executable; // what Play launches (resolved; "" = none found)
	std::string runtime_setting;    // the runtime the editor's settings name ("" = the one packaged beside the editor)
	bool source_run = false;        // OpenNova Play drives the Godot binary at the source project
	std::string retail_directory;
	bool play_retail = false;
	// What the last ApplyProjectSettings came to: its serial and the settings it could not
	// write (each also a finding). The project settings dialog waits for its serial.
	struct SettingsResult {
		uint64_t serial = 0;
		std::vector<Diagnostic> failures;
	};
	SettingsResult settings_result;
	// The files the running (or last) game of this project reported missing when it
	// booted, from its log: each is a Problems row (play.boot_missing) until Play starts
	// again or the project closes; a report from a game of another project is ignored.
	std::vector<std::string> boot_missing;
	// True when the last boot report named `name`, compared as the game compares names.
	bool missing_at_boot(const std::string &name) const {
		for (const std::string &reported : boot_missing)
			if (normalized_logical_name(reported) == normalized_logical_name(name)) return true;
		return false;
	}

	// The import dialog (ADR 0046 S8, S11g), open while an import is previewed: what a listing
	// offers to choose from (`choices`: a picked archive's members, or every file of the game
	// install when no file was named), the files chosen (`roots`: the loose files picked, the
	// files an Import fix names, those chosen from the list), and the plan of importing them
	// (editor/import/import_plan), with the files they need when `with_dependencies`: its rows
	// are what the dialog checks, then the files not found, the competing candidates and the
	// kinds not followed. `changed`: an Import found the files changed since the plan it was
	// shown, wrote nothing, and this is the plan made again. `serial` moves with every plan.
	struct ImportPreview {
		bool open = false;
		std::vector<ImportSource> choices;
		std::vector<ImportSource> roots;
		bool with_dependencies = false;
		ImportPlan plan;
		bool changed = false;
		uint64_t serial = 0;
	};
	ImportPreview import_preview;
	// What a rename would do (PreviewRename): a name renamed everywhere (`symbol`: its kind, the
	// file, record and field defining it) or a file renamed, the old and the new name, every site
	// it rewrites (file, record, field, the value before and after) and why it would be refused.
	// Every preview bumps `serial`; one that asks the new name bumps `ask_serial` too (the Rename
	// everywhere dialog opens).
	struct RenamePreview {
		uint64_t serial = 0;
		uint64_t ask_serial = 0;
		bool symbol = false;
		ReferenceKind kind = ReferenceKind::None;
		std::string path;
		std::string locator;
		std::string field;
		std::string old_name;
		std::string new_name;  // as the definition takes it (an item id "0100302" is 100302)
		std::string requested; // as asked: what a window compares the name it sent with
		std::vector<RenameSite> sites;
		std::vector<Diagnostic> refusals;
	};
	RenamePreview rename_preview;
	// The editor's setting (EditorSettings::import_dependencies): what a preview the windows
	// raise plans with.
	bool import_dependencies = true;
	// The project's importable sources (a PNG) with the outputs their importers made
	// (editor/import), refreshed with the scan.
	std::vector<ImportedSource> imports;
	// The logical names the game install under `retail_directory` resolves, sorted by
	// their normalized form; empty when no install is set or it mounts nothing.
	std::vector<std::string> retail_files;

	OutputLog output; // what the editor said and the running game's log, oldest first
	std::vector<std::string> recent_projects;
	std::string status; // the last thing that happened, one line
};

// Whether a selection's records (a view's `selected`) hold `address`: every list and tree of
// records marks a row selected by it. The primary is one of them whenever there is one
// (select_only, select, select_added and repair_selection keep it there), so a row is marked
// exactly when Copy, Cut, Duplicate and Remove take it.
bool holds(const std::vector<NodeAddress> &selected, const NodeAddress &address);

// The editor's OS window title, which the shell applies when it changes: "OpenNova Editor",
// the open project's name before it ("Armory - OpenNova Editor"), and a bullet (U+25CF)
// after the name while a file has unsaved changes.
std::string editor_window_title(const SessionView &view);

} // namespace opennova::editor
