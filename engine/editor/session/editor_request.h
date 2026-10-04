#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <editor/assets/import_choice.h>
#include <editor/model/diagnostic.h>
#include <editor/model/edit.h>
#include <editor/session/view/viewport_kind.h>

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
	Export,
	StopPlay,
	CancelOperation,
	CreateFile,
	OpenDocument,
	ShowInFiles,
	SelectFile,
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
	PreviewRenameBack,
	RenameBack,
	Reimport,
	SetImportOptions,
	PreviewInstallImport,
	ClearOutput,
	SetViewport,
	EditInViewport,
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
	ImportFiles,
	BuildFolder // Build > Build to folder...: where a build for players lands
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
// project's (its name, its features and its expansion, written to project.opennova, which
// needs a project open) and the editor's (the game install, the runtime Play runs, Play in
// the game install, written to the editor's settings). Only what differs from the value in
// effect is written. `serial` names the application: the SettingsApplied view event carries it
// back (its tag), the view's settings_result what could not be written.
struct ProjectSettingsChange {
	uint64_t serial = 0;
	std::optional<std::string> title;
	std::optional<bool> mission;
	std::optional<bool> multiplayer;
	// The project's expansion (ADR 0046 S16): the name it builds as ("" a standalone project) and the
	// installed one it builds on ("" the base game; it needs a name).
	std::optional<std::string> expansion;
	std::optional<std::string> builds_on;
	std::optional<std::string> game_install;
	std::optional<std::string> runtime_executable; // "" = the runtime packaged beside the editor
	std::optional<bool> play_in_install;
	// The folder Build to folder builds into, kept with the project's local settings ("" for none): the
	// modder's pick (a build's out_dir keeps nothing).
	std::optional<std::string> build_folder;
};

inline bool operator==(const ProjectSettingsChange &a, const ProjectSettingsChange &b) {
	return a.serial == b.serial && a.title == b.title && a.mission == b.mission &&
			a.multiplayer == b.multiplayer && a.expansion == b.expansion && a.builds_on == b.builds_on &&
			a.game_install == b.game_install && a.runtime_executable == b.runtime_executable &&
			a.play_in_install == b.play_in_install && a.build_folder == b.build_folder;
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

// A drag in a viewport (EditInViewport, ADR 0046 S13 V7; S9k1, S10p5), as the viewport's canvas
// drags: the record `id`'s handle by its token (a menu window's "move", "left", ... "bottom_right";
// a model marker's "place" or "axis"), by (x, y) from where the picture shows the handle now (`by`),
// else to the point (x, y) of the picture, in the viewport's units (a menu's design units, a model's
// picture pixels), snapped by `snap` (a menu's grid of 8 when it is not 0; a model's grid in metres,
// 0 free), in the viewport of `kind` (kCount: the one the document shows in). A gesture's samples
// are consecutive drags of one handle: `gesture` 0 begins one (the session's token, which the answer
// names), and a sample that names the gesture open on the document goes on with it, its `by` from
// where the gesture's samples took the handle (as a canvas drags from its press), so its batches fold
// into one undo step; `end` ends the gesture with the sample.
struct ViewportDrag {
	NodeId id = 0;
	std::string handle;
	bool by = true;
	float x = 0.0f;
	float y = 0.0f;
	float snap = 0.0f;
	uint64_t gesture = 0;
	bool end = true;
	ViewportKind kind = ViewportKind::kCount;
};

inline bool operator==(const ViewportDrag &a, const ViewportDrag &b) {
	return a.id == b.id && a.handle == b.handle && a.by == b.by && a.x == b.x && a.y == b.y &&
			a.snap == b.snap && a.gesture == b.gesture && a.end == b.end && a.kind == b.kind;
}
inline bool operator!=(const ViewportDrag &a, const ViewportDrag &b) {
	return !(a == b);
}

// A command in a viewport (EditInViewport, S13 V7): its name (a menu's arrange of windows,
// "align_left" ... "send_to_back"; a model's "frame"; a mission's "frame", "top", "ground",
// "duplicate", "select_same", "paste") over the records `ids` (the windows arranged, the first the one
// the others follow; the marker a frame looks at, none the whole model; none: the selection), in the
// viewport of `kind` (kCount: the one the document shows in). What a command takes beside them (ADR
// 0046 S15): `by`, a way in the kind's units (a mission's duplicate: metres east and north the copies
// go; empty none), and `at`, a point of the picture in the viewport's units (a mission's paste: where
// the copied records' middle lands; `has_at` false none).
struct ViewportCommand {
	std::string name;
	std::vector<NodeId> ids;
	ViewportKind kind = ViewportKind::kCount;
	std::vector<double> by;
	bool has_at = false;
	float at_x = 0.0f;
	float at_y = 0.0f;
};

inline bool operator==(const ViewportCommand &a, const ViewportCommand &b) {
	return a.name == b.name && a.ids == b.ids && a.kind == b.kind && a.by == b.by && a.has_at == b.has_at &&
			a.at_x == b.at_x && a.at_y == b.at_y;
}
inline bool operator!=(const ViewportCommand &a, const ViewportCommand &b) {
	return !(a == b);
}

// A drop on a viewport's picture (EditInViewport, ADR 0046 S14): what is dropped, a project file by
// its logical name (a Files row let go on the canvas: a model, which the viewport finds the item of)
// or a name of a reference kind (`reference` its token, `name` the name as a field of that kind holds
// it: an item's id, picked for a mission's Place tool; a path's number, its Path tool's), at the
// picture's point (x, y) in the viewport's units, in the viewport of `kind` (kCount: the one the
// document shows in). A box drop (`box`, ADR 0046 S15) goes from (x, y) to (x2, y2) and names no name
// (a mission's "area": an area trigger over the box). The viewport plans what the drop makes (a
// mission: an entity of the item added where the point meets the ground, facing the way the camera
// looks; a path's next stop; an area, one batch), snapped by `snap` (a mission's grid in metres, 0
// free); a kind that takes no drop refuses it.
struct ViewportDrop {
	std::string file;
	std::string reference;
	std::string name;
	float x = 0.0f;
	float y = 0.0f;
	ViewportKind kind = ViewportKind::kCount;
	bool box = false;
	float x2 = 0.0f;
	float y2 = 0.0f;
	float snap = 0.0f;
};

inline bool operator==(const ViewportDrop &a, const ViewportDrop &b) {
	return a.file == b.file && a.reference == b.reference && a.name == b.name && a.x == b.x && a.y == b.y &&
			a.kind == b.kind && a.box == b.box && a.x2 == b.x2 && a.y2 == b.y2 && a.snap == b.snap;
}
inline bool operator!=(const ViewportDrop &a, const ViewportDrop &b) {
	return !(a == b);
}

// One request: its kind and the fields that kind takes, each field meaning one thing
// whatever the kind (request_fields.cpp has a row per field: its token on the wire, its
// JSON type and what it means; the kind's row lists the fields it takes and those it must
// carry). A field a kind does not take stays as it was made.
struct EditorRequest {
	EditorRequestKind kind = EditorRequestKind::Rescan;
	// A project's directory; a new project's title and game (a gameprofile code, "" the default),
	// the expansion it builds as and the installed one it builds on ("" each: standalone, on the base
	// game; ADR 0046 S16); a game install a project opens with for the session alone ("" its own).
	std::string dir;
	std::string title;
	std::string game;
	std::string expansion;
	std::string builds_on;
	std::string game_install;
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
	// Where a build lands ("" the project's own place under its cache); where an export lands ("" the
	// project's export folder, ADR 0046 S16).
	std::string out_dir;
	std::string export_dir;
	// The mission Play starts the game in, by its logical name ("" the game's menu; S14).
	std::string mission;
	// Named values, in their names' order: a new file's starting values, by its blank's parameter tokens
	// (blank_factory.h: a mission's title, terrain and environment); an import's options, by their keys
	// (SetImportOptions, S18).
	std::vector<std::pair<std::string, std::string>> values;
	// Requirements' roles; the game install's files by logical name; files on disk to import.
	std::vector<std::string> roles;
	std::vector<std::string> names;
	std::vector<std::string> paths;
	// The files chosen to import (ImportChoice), as the view's import rows carry them.
	std::vector<ImportChoice> imports;
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
	// A viewport's change (SetViewport, S13 V5): the JSON text of an object {kind?, device?, clock?,
	// options?, camera?}, as preview/viewports.h's set takes it (text: this header pulls no JSON
	// reader).
	std::string viewport;
	// A drag, a command or a drop in a viewport, one of them (EditInViewport, S13 V7, S14).
	ViewportDrag drag;
	ViewportCommand command;
	ViewportDrop drop;
	PickPurpose purpose = PickPurpose::None;
	// An import brings the files the chosen ones need; it replaces the project's files of the
	// names; a source imports again even when unchanged; and asks the new name (Files'
	// Rename..., Rename everywhere); the document opens first when it is not (a fix's edit); a
	// project opens with its import pass (false: on its files as they are, scanned and checked, no
	// source imported).
	bool with_dependencies = false;
	bool replace = false;
	bool force = false;
	bool ask_name = false;
	bool open_first = false;
	bool import_pass = true;
	// A build reads every file again, the build cache set aside (S13 A8).
	bool rehash = false;
	// Every file of the game install chosen, with no walk (S14: "Import the whole game install");
	// an import takes the open preview's rows as its plan has them, in place of `imports`.
	bool all = false;
	bool planned = false;
};

inline bool operator==(const EditorRequest &a, const EditorRequest &b) {
	return a.kind == b.kind && a.dir == b.dir && a.title == b.title && a.game == b.game &&
			a.expansion == b.expansion && a.builds_on == b.builds_on &&
			a.game_install == b.game_install && a.path == b.path && a.locator == b.locator &&
			a.field == b.field &&
			a.new_name == b.new_name && a.role == b.role && a.file_kind == b.file_kind &&
			a.out_dir == b.out_dir && a.export_dir == b.export_dir && a.mission == b.mission && a.values == b.values &&
			a.roles == b.roles &&
			a.names == b.names && a.paths == b.paths && a.imports == b.imports &&
			a.edits == b.edits && a.address == b.address && a.records == b.records &&
			a.paste_at == b.paste_at &&
			a.mode == b.mode && a.choice == b.choice && a.settings == b.settings &&
			a.viewport == b.viewport && a.drag == b.drag && a.command == b.command && a.drop == b.drop &&
			a.purpose == b.purpose &&
			a.with_dependencies == b.with_dependencies &&
			a.replace == b.replace && a.force == b.force && a.ask_name == b.ask_name &&
			a.open_first == b.open_first && a.import_pass == b.import_pass && a.rehash == b.rehash &&
			a.all == b.all && a.planned == b.planned;
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
	// The gesture an EditInViewport's drag carried (S13 V7: the one it named, refused or not, or the one
	// the session handed it), which the gesture's next sample names to fold into the same undo step; 0
	// for none (a first sample that planned nothing and ended its gesture with it).
	uint64_t gesture = 0;
	bool done() const { return !refused && !unsaved_prompt; }
};

} // namespace opennova::editor
