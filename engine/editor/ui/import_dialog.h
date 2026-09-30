#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/ui/editor_host.h>

namespace opennova::editor {

// The import dialog (ADR 0046 S8, S11d, S11g), open while the view previews an import
// (SessionView::import_preview: the shell's file pick, the game data, an Import fix). A
// listing's files to choose from (a picked archive's members, the game install's files)
// with a filter, each choice planned again at once (PlanImport); "Include the files these
// need", the editor's setting (SetImportDependencies, the plan made again); the plan's rows,
// the chosen files first and then the files they need in the plan's order, each checked or
// not (the files a converter makes from one source together; a dependency the project cannot
// take unchecked, its problem said; a chosen file it cannot take checked, Import waiting,
// its problem said, until it is unchecked); the files not found and what needs them; the
// files found in more than one place; the kinds whose dependencies are not followed; a
// notice when the plan stopped at its cap; Replace existing files; Import N files or Cancel.
// Import raises ImportFiles with the checked rows' sources; the session asks to save a file
// with unsaved edits it would write over, and while that prompt is open the dialog gives way
// to it, coming back as it was (its checks, filter and Replace existing files) while the
// preview stays open. The workspace draws it every frame, whichever window asked for it.
class ImportDialog {
public:
	void draw(EditorHost &host);

private:
	void take(const SessionView::ImportPreview &preview);
	void draw_choices(EditorHost &host, const SessionView::ImportPreview &preview);
	void draw_plan(EditorHost &host, const SessionView::ImportPreview &preview);
	void draw_notes(const SessionView::ImportPreview &preview);
	void choose(EditorHost &host, const SessionView::ImportPreview &preview);

	uint64_t serial_ = 0;       // the plan the checks were taken from
	std::vector<bool> checked_; // per plan row: taken by the import
	// Per plan row: why the import cannot take it ("" when it can), found once per plan.
	std::vector<std::string> why_not_;
	std::vector<bool> chosen_;  // per choice: among the files chosen
	char filter_[128]{};
	bool replace_existing_ = false;
	bool previewing_ = false; // the view previewed an import last frame: its state is this one's
};

} // namespace opennova::editor
