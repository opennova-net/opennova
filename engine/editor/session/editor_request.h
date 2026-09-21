#pragma once

#include <string>
#include <editor/documents/editable_document.h>

namespace opennova::editor {

// What the editor's windows and menus ask for (ADR 0046 d10): typed requests out of the
// UI, drained by the project session (the portable kinds) or by the shell (the kinds
// only an OS can serve: a native dialog, a file manager, quitting). A request carries
// plain fields, never a callback, so a test can enqueue and inspect it.
enum class EditorRequestKind {
	NewProject,           // path = the project directory, text = the title
	OpenProject,          // path
	CloseProject,
	ForgetRecent,         // path
	Rescan,               // re-read the project's files
	SetTitle,             // text
	SetFeature,           // text = "mission" | "multiplayer", flag = on/off
	SetRuntimeExecutable, // path ("" = the runtime packaged beside the editor)
	CreateMissing,        // text = one role, or "" for every missing Required file
	Build,
	Play,                 // build, then run the game on the build
	StopPlay,
	CreateCatalog, OpenDocument, ReloadDocument, CloseDocument,
	SelectRecord, EditRecord, EndEdit,
	Save, SaveAll, Undo, Redo, ResolveUnsaved,
	// Shell-only: the portable session cannot serve these.
	PickDirectory,        // purpose says what the picked directory is for
	PickFile,             // purpose = RuntimeExecutable
	RevealPath,           // path: show it in the OS file manager
	Quit,
};

enum class PickPurpose { None, NewProjectLocation, OpenProject, RuntimeExecutable };

enum class UnsavedChoice { SaveAll, Discard, Cancel };

struct EditorRequest {
	EditorRequestKind kind = EditorRequestKind::Rescan;
	std::string path;
	std::string text;
	bool flag = false;
	PickPurpose purpose = PickPurpose::None;
	CatalogEdit catalog_edit;
	UnsavedChoice unsaved_choice = UnsavedChoice::Cancel;
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
