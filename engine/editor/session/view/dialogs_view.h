#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/import_choice.h>
#include <editor/model/diagnostic.h>
#include <editor/model/value.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

struct ImportPlan;
struct RenameSite;

// What waits on the author, as the view shows it (ADR 0046 S13 V4; the Dialogs concern and
// Project's quit_requested): the unsaved-changes prompt, the import dialog's preview, the last
// rename's plan, and whether the editor asked to quit. The import's plan and the rename's sites
// are shared and never null (a DialogsView made empty holds empty ones), so a header naming the
// view pulls none of the import plan's or the rename transaction's headers.
struct DialogsView {
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
	// The import dialog (ADR 0046 S8, S11g), open while an import is previewed: what a listing
	// offers to choose from (`choices`: a picked archive's members, or every file of the game
	// install when no file was named), the files chosen (`roots`: the loose files picked, the
	// files an Import fix names, those chosen from the list), and the plan of importing them
	// (editor/import/import_plan), with the files they need when `with_dependencies`: its rows
	// are what the dialog checks, then the files not found, the competing candidates and the
	// kinds not followed. `changed`: an Import found the files changed since the plan it was
	// shown, wrote nothing, and this is the plan made again. `all`: every file of the game
	// install chosen at once (ADR 0046 S14), nothing to choose from and no walk (the closure of
	// everything is everything; the setting changes nothing of it). Every plan made posts an
	// ImportPlanned event (view_events.h).
	struct ImportPreview {
		ImportPreview(); // the plan made, empty
		bool open = false;
		std::vector<ImportChoice> choices;
		std::vector<ImportChoice> roots;
		bool with_dependencies = false;
		bool all = false;
		std::shared_ptr<const ImportPlan> plan;
		bool changed = false;
	};
	ImportPreview import_preview;
	// What a rename would do (PreviewRename): a name renamed everywhere (`symbol`: its kind, the
	// file, record and field defining it) or a file renamed, the old and the new name, every site
	// it rewrites (file, record, field, the value before and after) and why it would be refused.
	// Every preview bumps `serial`; one that asks the new name posts an AskRename event naming it
	// (the Rename everywhere dialog opens).
	struct RenamePreview {
		RenamePreview(); // the sites made, none
		uint64_t serial = 0;
		bool symbol = false;
		ReferenceKind kind = ReferenceKind::None;
		std::string path;
		std::string locator;
		std::string field;
		std::string old_name;
		std::string new_name;  // as the definition takes it (an item id "0100302" is 100302)
		std::string requested; // as asked: what a window compares the name it sent with
		std::shared_ptr<const std::vector<RenameSite>> sites;
		std::vector<Diagnostic> refusals;
	};
	RenamePreview rename_preview;
	bool quit_requested = false;
};

} // namespace opennova::editor
