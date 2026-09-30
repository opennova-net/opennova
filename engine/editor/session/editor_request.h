#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <editor/assets/import_source.h>
#include <editor/model/diagnostic.h>
#include <editor/model/edit.h>

namespace opennova::editor {

// What the editor's windows and menus ask for (ADR 0046 d10): typed requests out of the
// UI, drained by the project session (the portable kinds) or by the shell (the kinds
// only an OS can serve: a native dialog, a file manager). A request carries plain
// fields, never a callback, so a test can enqueue and inspect it. Each kind is one row of
// the request table (request_kinds.cpp, S13 A4): its token, who serves it, the fields it
// takes, what it reads and writes, what the unsaved-changes prompt guards of it and what
// it does; request_factories.h makes a request of each kind.
enum class EditorRequestKind {
	NewProject,
	OpenProject,
	CloseProject,
	ForgetRecent,
	Rescan,
	ApplyProjectSettings,
	PreviewImport,
	PlanImport,
	SetImportDependencies,
	ImportFiles,
	CancelImport,
	CreateMissing,
	Build,
	Play,
	StopPlay,
	CancelOperation,
	CreateFile,
	OpenDocument,
	ShowInFiles,
	ReloadDocument,
	CloseDocument,
	SelectRecord,
	EditRecord,
	RevertToSaved,
	EndEdit,
	Copy,
	Cut,
	Paste,
	Duplicate,
	Save,
	SaveAll,
	Undo,
	Redo,
	ResolveUnsaved,
	RenameAsset,
	AssignRequirement,
	PreviewRename,
	RenameSymbol,
	Reimport,
	PreviewInstallImport,
	ClearOutput,
	Quit,
	// The shell's: the portable session cannot serve these.
	PickDirectory,
	PickFile,
	RevealPath,
	kCount,
};

inline constexpr size_t kEditorRequestKindCount = static_cast<size_t>(EditorRequestKind::kCount);

// What a picked directory or file is for (PickDirectory, PickFile).
enum class PickPurpose {
	None,
	NewProjectLocation,
	OpenProject,
	RuntimeExecutable,
	GameInstall,
	ImportFiles
};

// The unsaved-changes prompt's answer: Save writes the files it lists, then what waited
// runs; Discard drops their unsaved edits, then it runs (never offered for Build and Play,
// which pack the files on disk); Cancel runs nothing.
enum class UnsavedChoice { Save, Discard, Cancel };

// How SelectRecord changes the selection (Selection::select): Replace makes the records named the
// selection; Add joins them (the primary becoming the one named); Toggle joins each that is not
// selected and leaves each that is. Records of another document than the selection's replace it;
// any rows of one document may be selected together (S13 D7).
enum class SelectMode { Replace, Add, Toggle };

// The settings ApplyProjectSettings sets, each one left out staying as it is: the
// project's (its name and features, written to project.opennova, which needs a project
// open) and the editor's (the game install, the runtime Play runs, Play in the game
// install, written to the editor's settings). Only what differs from the value in effect
// is written. `serial` names the application: the SettingsApplied view event carries it back
// (its tag), the view's settings_result what could not be written.
struct ProjectSettingsChange {
	uint64_t serial = 0;
	std::optional<std::string> title;
	std::optional<bool> mission;
	std::optional<bool> multiplayer;
	std::optional<std::string> game_install;
	std::optional<std::string> runtime_executable; // "" = the runtime packaged beside the editor
	std::optional<bool> play_in_install;
};

inline bool operator==(const ProjectSettingsChange &a, const ProjectSettingsChange &b) {
	return a.serial == b.serial && a.title == b.title && a.mission == b.mission &&
			a.multiplayer == b.multiplayer && a.game_install == b.game_install &&
			a.runtime_executable == b.runtime_executable && a.play_in_install == b.play_in_install;
}

// Where Paste puts the clipboard: into the owner `parent` (0 = the row `row` itself) at
// `position` in its collection (SIZE_MAX = the end). None named (row and parent 0): after
// the selection, the session's own rule (DocumentSet::position_after).
struct PasteAt {
	NodeId row = 0;
	NodeId parent = 0;
	size_t position = SIZE_MAX;
	bool named() const { return row != 0 || parent != 0; }
};

inline bool operator==(const PasteAt &a, const PasteAt &b) {
	return a.row == b.row && a.parent == b.parent && a.position == b.position;
}

// One request: its kind and the fields that kind takes, each field meaning one thing
// whatever the kind (request_fields.cpp has a row per field: its token on the wire, its
// JSON type and what it means; the kind's row lists the fields it takes and those it must
// carry). A field a kind does not take stays as it was made.
struct EditorRequest {
	EditorRequestKind kind = EditorRequestKind::Rescan;
	// A project's directory; a new project's title.
	std::string dir;
	std::string title;
	// A file: a project file or open document ("" the active one where the kind names it), a
	// source to import again, a path to reveal.
	std::string path;
	// A record by its locator (Document::locator), and a field of it.
	std::string locator;
	std::string field;
	// The name a rename gives; a requirement's role; an asset kind's token, where a file's name
	// cannot say its kind.
	std::string new_name;
	std::string role;
	std::string file_kind;
	// Requirements' roles; the game install's files by logical name; files on disk to import.
	std::vector<std::string> roles;
	std::vector<std::string> names;
	std::vector<std::string> paths;
	// Import sources, as the view's import rows carry them.
	std::vector<ImportSource> imports;
	// A batch over any rows of one document, one undo step.
	std::vector<Edit> edits;
	// A record by its address; the records a selection takes with it (SelectRecord); where a Paste
	// goes.
	NodeAddress address;
	std::vector<NodeAddress> records;
	PasteAt paste_at;
	SelectMode mode = SelectMode::Replace;
	UnsavedChoice choice = UnsavedChoice::Cancel;
	ProjectSettingsChange settings;
	PickPurpose purpose = PickPurpose::None;
	// An import brings the files the chosen ones need; it replaces the project's files of the
	// names; a source imports again even when unchanged; and asks the new name (Files'
	// Rename..., Rename everywhere); the document opens first when it is not (a fix's edit).
	bool with_dependencies = false;
	bool replace = false;
	bool force = false;
	bool ask_name = false;
	bool open_first = false;
};

inline bool operator==(const EditorRequest &a, const EditorRequest &b) {
	return a.kind == b.kind && a.dir == b.dir && a.title == b.title && a.path == b.path &&
			a.locator == b.locator && a.field == b.field && a.new_name == b.new_name &&
			a.role == b.role && a.file_kind == b.file_kind && a.roles == b.roles &&
			a.names == b.names && a.paths == b.paths && a.imports == b.imports &&
			a.edits == b.edits && a.address == b.address && a.records == b.records &&
			a.paste_at == b.paste_at &&
			a.mode == b.mode && a.choice == b.choice && a.settings == b.settings &&
			a.purpose == b.purpose && a.with_dependencies == b.with_dependencies &&
			a.replace == b.replace && a.force == b.force && a.ask_name == b.ask_name &&
			a.open_first == b.open_first;
}
inline bool operator!=(const EditorRequest &a, const EditorRequest &b) {
	return !(a == b);
}

// What one request came to, for whoever raised it and must know whether it happened
// (the editor MCP): the findings it reported, whether it was refused or failed (an
// error among them), and whether it now waits on the unsaved-changes prompt. The
// session resets it when a request arrives from outside; the requests it raises while
// serving that one (a rename's close and reload) add to the same outcome.
struct ActionOutcome {
	bool refused = false;        // refused or did not finish: an error was reported, or the
	                             // request could not run now (an operation holds what it
	                             // needs, the document is not open; a warning says which)
	bool unsaved_prompt = false; // waits on resolve_unsaved (save, discard or cancel): the
	                             // view's unsaved_prompt says what waits and on which files
	uint64_t operation = 0;      // the operation it started or joined (the view's operation
	                             // while it runs, its last_operation once it ends); 0 for none
	std::vector<Diagnostic> findings;
	// The records its edits made and kept, in order (S13 A5): an EditRecord's adds and
	// duplicates, a Paste's records, a Duplicate's copies (Document::last_added_records());
	// none for a request that made none. And an EditRecord's by its edits' indexes
	// (Document::last_made(): 0 for an edit that made nothing or whose record a later edit
	// removed), which its labels name.
	std::vector<NodeId> added, made;
	bool done() const { return !refused && !unsaved_prompt; }
};

} // namespace opennova::editor
