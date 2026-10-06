#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/value.h>

namespace opennova::editor {

// Which findings Problems shows, by where they sit (the whole project, the active document, the open
// documents) and how it groups them (session/problem_query.h's ProblemQuery reads them; the workspace's
// Problems part holds the window's).
enum class ProblemScope { Project, ActiveFile, OpenFiles };
enum class ProblemGrouping { None, File, Kind };

// The text a window's field holds, its buffer's size (its terminator in it): a name, a filter or a find's
// text; a folder or an executable; a file's name typed and a New file value; an expansion's name. The
// workspace's table (workspace_parts.h) refuses a set_workspace text past its field's (the buffer less the
// terminator), so no window shows, or sends back, a cut copy of what the session holds.
inline constexpr size_t kWorkspaceText = 128;
inline constexpr size_t kWorkspacePath = 512;
inline constexpr size_t kWorkspaceFileName = 64;
inline constexpr size_t kWorkspaceExpansion = 32;

// What the workspace's windows show of their own, held by the session (ADR 0046, the MCP gaps lane; the
// Workspace concern, CONTEXT.md "Workspace"): each card, panel and dialog open and what its fields hold,
// and the sound the editor plays. The windows draw it and change it only by the set_workspace request
// (session/workspace_parts.h: one row per part, its members on the wire), so the editor MCP sets and reads
// it as a person's controls do; the state section `workspace` writes it. What only shows a window's own
// way (its scroll, its columns' widths, the docking, a tree's folds) stays the window's.
struct WorkspaceView {
	// Files' card of a file (the UX round's project lane: session/file_card.h): its project-relative path,
	// "" while none shows. An about_file opens it; a set_workspace closes it (its path ""), as the card's
	// close does, and so does its project closing, its file going (the session closes it as the files it reads
	// lose it: workspace_tidies) and another project opening; a rename of its file moves it to the new path.
	struct Card {
		std::string path;
	};
	Card card;

	// The sound the editor plays (play_sound, a card's Play): the project file it plays ("" none), the play's
	// serial (each play_sound moves it: the Shell starts the sound of a serial once), and how it stands as the
	// Shell reports it (ProjectSession::report_sound): Starting until the Shell has decoded it, Playing,
	// Ended once it played through, Stopped (stop_sound, the card closing, the project closing), or Failed
	// with why (`error`). A headless editor has no Shell to play it: its sound stays Starting.
	enum class SoundState : uint8_t { Idle, Starting, Playing, Ended, Stopped, Failed };
	struct Sound {
		std::string path;
		uint64_t serial = 0;
		SoundState state = SoundState::Idle;
		std::string error;
	};
	Sound sound;

	// The build's result (the UX round's problems lane), its panel open from a build's end (one a Play does
	// not wait on) or the menu bar's build state until closed.
	struct BuildResult {
		bool open = false;
	};
	BuildResult build_result;

	// The new-project form (the welcome page's, File > New project...'s modal): whether the modal is open
	// (the welcome page shows the form whenever no project is open), its name, its folder, the game install
	// it imports from (the editor's own until one is named: `install_named`), what it builds on (an
	// installed expansion by its folder's name, "" the base game) and whether it builds as an expansion of
	// that name (ADR 0046 S16; building on an expansion builds as one). New project raises new_project
	// with them; nothing here makes a project.
	struct NewProject {
		bool open = false;
		std::string title = "My Game";
		std::string dir;
		std::string game_install;
		bool install_named = false;
		std::string builds_on;
		bool as_expansion = false;
		std::string expansion;
	};
	NewProject new_project;

	// File > Project settings... (ADR 0046 S11d): whether it is open, and what its fields hold until Apply
	// (one apply_project_settings naming every one): the project's name and features, its expansion (S16), and
	// this computer's game install, the runtime Play runs ("" the one packaged beside the editor) and Play in
	// the game install, strictly or not. Opening it fills the fields with the settings in effect; its project
	// closing closes it.
	struct Settings {
		bool open = false;
		std::string title;
		bool mission = false;
		bool multiplayer = false;
		std::string builds_on;
		bool as_expansion = false;
		std::string expansion;
		std::string game_install;
		std::string runtime;
		bool play_in_install = false;
		bool play_in_install_strict = false;
	};
	Settings settings;

	// Files' New file prompt (New > a kind...): the kind it makes (kCount: closed), the name typed, and the
	// values its blank takes (ADR 0046 S14), by their params' tokens. Create raises create_file with them.
	struct NewFile {
		AssetKind kind = AssetKind::kCount;
		std::string name;
		std::vector<std::pair<std::string, std::string>> values;
	};
	NewFile new_file;

	// Files' Rename... of a file (its menu, F2, a card's Rename..., a show_in_files that asks the name): the
	// file ("" closed) and the name typed; Rename raises rename_asset.
	struct FileRename {
		std::string path;
		std::string name;
	};
	FileRename file_rename;

	// Rename everywhere (the Inspector's Rename... on a field defining a name: a preview_rename that asks the
	// name opens it): the name it renames (the field of the record at the locator in the file defining it,
	// what it was called and the kind of name, as the plan it opened over had them) and the name typed,
	// each change of which plans the rename again (a plan of that name for the same field is the name typed:
	// a preview_rename moves it too); Rename back's, open over the plan preview_rename_back made. Rename
	// (rename_symbol) closes the one, Rename back (rename_back) the other.
	struct Rename {
		bool open = false;
		std::string path, locator, field, old_name;
		ReferenceKind kind = ReferenceKind::None;
		std::string name;
	};
	Rename rename;
	struct RenameBack {
		bool open = false;
	};
	RenameBack rename_back;

	// The Document window's find bar over the active document (Ctrl+F, Edit > Find...): open, its text and
	// whether case counts. Its hits are the document_search query's; one is shown by an open_document at it.
	struct Find {
		bool open = false;
		std::string text;
		bool match_case = false;
	};
	Find find;

	// Edit > Find in project... (Ctrl+Shift+F): open, its text. Its results are the project_search query's.
	struct ProjectFind {
		bool open = false;
		std::string text;
	};
	ProjectFind project_find;

	// Files' filter: its text and the kind it lists alone (kCount: every kind), as the files query lists them;
	// and whether the files it lists flat go by what the game's textures of each cost, the costliest first
	// (S18, the texture budget: session/texture_budget_list).
	struct Files {
		std::string filter;
		AssetKind kind = AssetKind::kCount;
		bool by_cost = false;
	};
	Files files;

	// The import dialog's own, over the preview the dialogs view holds (DialogsView::import_preview): the
	// filter of the files to choose from and the kind it lists alone, the filter of the plan's rows and the
	// kind they show alone (kCount: every kind), Replace existing files, and which of the plan's rows are
	// checked (each plan made takes them anew, import_default_checks; Import takes the checked ones), with a
	// serial that moves with each change of the checks. A new preview starts it afresh; its close ends it.
	struct Import {
		std::string filter;
		AssetKind choice_kind = AssetKind::kCount;
		std::string rows_filter;
		AssetKind kind_shown = AssetKind::kCount;
		bool replace_existing = false;
		std::vector<bool> checked;
		uint64_t serial = 0;
	};
	Import import;

	// Problems' filters, as the problems query takes them: the severities shown, a text, the scope, Only
	// fixable, how it groups (by kind until one is picked), and Blocks the build (only what a build is refused
	// for, every one: turned on, the filters before it are kept and the others shown whole; turned off, they
	// come back). And the confirmation a Fix all or a Use fix waits in for Apply: the group whose Fix all
	// (its key, as the problems query's groups name it), the request kind whose summary Fix all (`required`),
	// or the finding (its index among the findings, the problems query's `index`) whose fix of `label`; all
	// "" closed.
	struct Problems {
		bool errors = true, warnings = true, infos = true;
		std::string text;
		ProblemScope scope = ProblemScope::Project;
		bool fixable = false;
		ProblemGrouping grouping = ProblemGrouping::Kind;
		bool blocking = false;
		struct Filters {
			bool errors = true, warnings = true, infos = true;
			std::string text;
			ProblemScope scope = ProblemScope::Project;
			bool fixable = false;
		};
		std::optional<Filters> before_blocking;
		// `finding_key`: the finding's identity as it was asked (problem_finding_key), what an Apply finds it by
		// again once a validation has moved the findings (its index then names another, or none).
		struct Confirm {
			std::string group, required, finding, label;
			std::string finding_key;
			bool open() const { return !group.empty() || !required.empty() || !finding.empty(); }
		};
		Confirm confirm;
		uint64_t confirm_serial = 0; // moves with each confirmation asked: the window takes each once
	};
	Problems problems;

	// What a document's views show of it, by its path: its outline's filter, the kinds of row it lists (bits
	// over the document's kinds, Document::kinds), whether it lists every row (the empty paths), sorts by
	// name and filters every row's detail records (master and detail); the Inspector's filter over its
	// fields; a menu's type for a new window and the screen whose Remove waits on its confirmation (0: none);
	// a texture's palette remap, from and to. Closed with the document.
	struct DocumentView {
		std::string filter;
		uint64_t kinds = ~uint64_t(0);
		bool all_rows = false;
		bool sort = false;
		bool every = false;
		std::string inspector_filter;
		std::string new_window_type = "static";
		uint64_t remove_screen = 0;
		int remap_from = 0, remap_to = 0;
	};
	std::map<std::string, DocumentView> documents;

	// Moves with each dialog the workspace opens (a part opened, or opened on another target: a file, a kind, a
	// screen, a confirmation): a window that closed its own dialog and asked the session to close it tells a
	// dialog opened again since from its own close not served yet (ui_kit::HeldPopup).
	uint64_t opened = 0;

	// The document view's state at `path`, its defaults for one the session holds none of.
	const DocumentView &document(const std::string &path) const {
		static const DocumentView kDefaults;
		const auto found = documents.find(path);
		return found == documents.end() ? kDefaults : found->second;
	}
};

// A sound state's token: idle, starting, playing, ended, stopped, failed.
const char *sound_state_token(WorkspaceView::SoundState state);

} // namespace opennova::editor
