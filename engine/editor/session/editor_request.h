#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/model/diagnostic.h>
#include <editor/model/edit.h>

namespace opennova::editor {

// What the editor's windows and menus ask for (ADR 0046 d10): typed requests out of the
// UI, drained by the project session (the portable kinds) or by the shell (the kinds
// only an OS can serve: a native dialog, a file manager). A request carries plain
// fields, never a callback, so a test can enqueue and inspect it.
enum class EditorRequestKind {
	NewProject,           // path = the project directory, text = the title
	OpenProject,          // path
	CloseProject,
	ForgetRecent,         // path
	Rescan,               // re-read the project's files
	ApplyProjectSettings, // settings = the settings to set, each left out as it is; the view's
	                      // settings_result says what came of it
	// The import dialog (the view's import_preview): a preview plans importing the files chosen
	// (editor/import/import_plan), with the files they need when its flag says so.
	PreviewImport,        // paths = the files picked: the loose ones are chosen, an archive's members are
	                      // listed to choose from; flag = with the files they need
	PlanImport,           // imports = the files chosen (the loose ones and those chosen from the list),
	                      // flag = with the files they need: planned again (a preview opens when none is)
	SetImportDependencies, // flag = whether an import brings the files the chosen ones need: the editor's
	                      // setting, remembered; an open preview is planned again with it
	ImportFiles,          // imports = the rows kept (their sources), flag = replace existing files; with a
	                      // preview open, planned again first, and nothing written when the plan changed;
	                      // replacing a file with unsaved edits asks to save them first
	CancelImport,
	CreateMissing,        // names = the roles of the required files to create from scratch (none: nothing);
	                      // a file there since the last refresh is refused, never overwritten
	Build,                // packs the files on disk: asks to save unsaved edits first
	Play,                 // build, then run the game on the build
	StopPlay,
	CreateFile,           // path = the file to create blank, from the name's requirement factory, else its
	                      // kind's free-form one; opened when the editor edits its kind (a font, a .coo,
	                      // character attributes are made and listed, not opened); text = its kind token
	                      // when the name alone cannot say
	OpenDocument,         // path; edit.address = the record to select once open, or text = its locator
	                      // (Document::locator: a record the graph read, found again once open; Go to),
	                      // edit.field = its field to show (the view's reveal_field)
	ShowInFiles,          // path = a project file (project-relative or logical): Files selects it and scrolls
	                      // to it (the view's reveal_file); flag = and asks its new name (Files' Rename...)
	ReloadDocument, CloseDocument,
	SelectRecord,         // path = the document, edit.address = the record, select_mode = how it
	                      // joins the selection (the selected records stay inside one row)
	EditRecord,           // path = the document, edit = the change, or edits = a batch on one row; flag =
	                      // open the document first when it is not (a fix's edit)
	RevertToSaved,        // path = the document, edit = a record's field (address, field), or edits = a
	                      // row's fields (a group's) of several records of one row: each given back the value and
	                      // presence the saved file holds (Document::revert_edits), one batch, one undo
	                      // step; refused when none has anything to go back to (the Inspector's Revert to
	                      // saved)
	EndEdit,              // path = the document: the coalesced edit group (or the gesture) ends
	Copy,                 // path = the document: its selected records onto the session's clipboard
	Cut,                  // Copy, then the selected records removed in one step
	Paste,                // path = the document: the clipboard into edit.parent (0 with edit.address.row
	                      // = the row) at edit.position; with no target, after the selection
	Duplicate,            // path = the document: each selected record (those no other selected one holds)
	                      // copied right after itself, one step; the copies become the selection
	Save,                 // path = the document ("" = the active one): written, with no unsaved edits too
	                      // when its file holds other bytes than it would write (a canonical rewrite); a
	                      // file that is not open is read, rewritten that way when it must be, and left closed
	SaveAll,              // every document with unsaved edits
	Undo, Redo,
	ResolveUnsaved,       // unsaved_choice answers the unsaved-changes prompt
	RenameAsset,          // path = the file (project-relative or logical), text = the new logical name; a
	                      // file it rewrites, or the file itself, with unsaved edits: asks to save them first
	AssignRequirement,    // text = a requirement's role, path = the project file to rename to its name (as
	                      // RenameAsset asks)
	PreviewRename,        // what a rename would do, planned into the view's rename_preview, nothing written:
	                      // path = the file defining a name, text = its record's locator, edit.field = the
	                      // field defining it, edit.value = the new name (Rename everywhere); edit.field "" =
	                      // the file at path renamed to edit.value (Files' Rename...); flag = and ask the new
	                      // name (the Rename everywhere dialog opens)
	RenameSymbol,         // path, text, edit.field and edit.value as PreviewRename's: the name and every use
	                      // that reaches it rewritten on disk (graph/rename_transaction), not undoable, or
	                      // refused with the reasons; a file it rewrites with unsaved edits: asks to save
	                      // them first
	Reimport,             // a refresh, so every stale source imports as on any; path = the source `flag` imports
	                      // again even when unchanged and whose findings are the outcome ("" = every source)
	PreviewRetailImport,  // the game install's files: names = those alone, chosen (an Import fix); none = every
	                      // file listed to choose from; flag = with the files they need
	ClearOutput,          // empty the view's output lines (Output's Clear)
	Quit,                 // asks about unsaved edits first, then sets the view's quit_requested,
	                      // which the shell acts on
	// Shell-only: the portable session cannot serve these.
	PickDirectory,        // purpose says what the picked directory is for
	PickFile,             // purpose = RuntimeExecutable; ImportFiles picks multiple files
	RevealPath,           // path: show it in the OS file manager
};

enum class PickPurpose { None, NewProjectLocation, OpenProject, RuntimeExecutable, RetailDirectory, ImportFiles };

// The unsaved-changes prompt's answer: Save writes the files it lists, then what waited
// runs; Discard drops their unsaved edits, then it runs (never offered for Build and Play,
// which pack the files on disk); Cancel runs nothing.
enum class UnsavedChoice { Save, Discard, Cancel };

// How SelectRecord changes the selection: Replace makes the record the only one; Add
// joins it (the primary becomes it); Toggle joins it, or leaves it when it is selected.
// A record in another row or document than the selection replaces it.
enum class SelectMode { Replace, Add, Toggle };

// The settings ApplyProjectSettings sets, each one left out staying as it is: the
// project's (its name and features, written to project.opennova, which needs a project
// open) and the editor's (the game install, the runtime Play runs, Play in the game
// install, written to the editor's settings). Only what differs from the value in effect
// is written. `serial` names the application: the view's settings_result carries it back
// with what could not be written.
struct ProjectSettingsChange {
	uint64_t serial = 0;
	std::optional<std::string> title;
	std::optional<bool> mission;
	std::optional<bool> multiplayer;
	std::optional<std::string> retail_directory;
	std::optional<std::string> runtime_executable; // "" = the runtime packaged beside the editor
	std::optional<bool> play_retail;
};

struct EditorRequest {
	EditorRequestKind kind = EditorRequestKind::Rescan;
	std::string path;
	std::string text;
	bool flag = false;
	PickPurpose purpose = PickPurpose::None;
	std::vector<std::string> paths; // files chosen for PreviewImport
	std::vector<std::string> names; // CreateMissing's roles; PreviewRetailImport's files
	std::vector<ImportSource> imports; // PlanImport's files chosen; ImportFiles' rows kept (flag = replace)
	Edit edit;
	std::vector<Edit> edits; // EditRecord: a batch on one row, one undo step (edit is then unused)
	SelectMode select_mode = SelectMode::Replace;
	UnsavedChoice unsaved_choice = UnsavedChoice::Cancel;
	ProjectSettingsChange settings; // ApplyProjectSettings
};

// What one request came to, for whoever raised it and must know whether it happened
// (the editor MCP): the findings it reported, whether it was refused or failed (an
// error among them), and whether it now waits on the unsaved-changes prompt. The
// session resets it when a request arrives from outside; the requests it raises while
// serving that one (a rename's close and reload) add to the same outcome.
struct ActionOutcome {
	bool refused = false;        // refused or did not finish: an error was reported, or the
	                             // request could not run now (a build is packing, the
	                             // document is not open; a warning says which)
	bool unsaved_prompt = false; // waits on resolve_unsaved (save, discard or cancel): the
	                             // view's unsaved_prompt says what waits and on which files
	std::vector<Diagnostic> findings;
	bool done() const { return !refused && !unsaved_prompt; }
};

inline EditorRequest make_request(EditorRequestKind kind, std::string path = std::string(),
                                  std::string text = std::string()) {
	EditorRequest request;
	request.kind = kind;
	request.path = std::move(path);
	request.text = std::move(text);
	return request;
}

} // namespace opennova::editor
