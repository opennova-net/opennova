#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/import/import_plan_groups.h>
#include <editor/session/view/dialogs_view.h>
#include <editor/ui/view_event_mailbox.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The import dialog (ADR 0046 S8, S11d, S11g), open while the view previews an import
// (DialogsView::import_preview: the shell's file pick, the game data, an Import fix), sized to the
// editor's window as it opens. A listing's files to choose from (a picked archive's members, the
// game install's files), each with its kind and size (the UX round's project lane), with a filter
// over the names (a kind's name finds that kind: "texture"), a kind to show alone and how many
// show, each choice planned again at once (PlanImport); "Include the files these need", the
// editor's setting (SetImportDependencies, the plan made again); the plan in short (ADR 0046 S14:
// a mission's closure is thousands of rows): its files and bytes, then each kind with its count
// and size, a toggle that shows that kind's rows alone, a filter over the rows' names, and Check
// shown / Uncheck shown; the plan's rows by what they come for (import_plan_groups: each chosen
// file, under it the kinds of the files it brings, main.mnu > Sound bank > Wave), each kind a line
// with its count, its size and one check for all of it, opened to its rows (a filter or a kind
// shown lists the rows flat); each row with its size, what wanted it in words and where it comes
// from, checked or not (the files a converter makes from one source together; a dependency the
// project cannot take unchecked, its problem said; a chosen file it cannot take checked, Import
// waiting, its problem said, until it is unchecked; a chosen file the project has, whether the
// project's is the same); the files not found and what needs them (collapsed when they are many);
// the files found in more than one place; the kinds whose dependencies are not followed; a notice
// when the plan stopped at its cap; Replace existing files; Import N files or Cancel.
// Import raises ImportFiles with the checked rows' sources; the session asks to save a file
// with unsaved edits it would write over, and while that prompt is open the dialog gives way
// to it, coming back as it was (its checks, filter and Replace existing files) while the
// preview stays open. The workspace draws it every frame, whichever window asked for it, and
// each plan made (an ImportPlanned view event) has its checks taken again.
class ImportDialog {
public:
	// An ImportPlanned view event, held until the dialog draws (every frame).
	void receive(const ViewEvent &event) { events_.post(event); }
	void draw(Workspace &workspace);

private:
	// One line of the plan's table: a row of the plan, or a kind's group of rows.
	struct Line {
		bool group = false;
		size_t index = 0; // the row's, or the group's
		size_t depth = 0;
	};

	void take(const DialogsView::ImportPreview &preview);
	void draw_choices(Workspace &workspace, const DialogsView::ImportPreview &preview, float height);
	void draw_plan(Workspace &workspace, const DialogsView::ImportPreview &preview);
	void draw_notes(const DialogsView::ImportPreview &preview);
	void choose(Workspace &workspace, const DialogsView::ImportPreview &preview);
	// The plan's groups and the lines they show, made for a new plan: each open as it starts (a
	// chosen file while they are few, a kind while its files are few).
	void group(const DialogsView::ImportPreview &preview);
	void lay_out();
	void lay_out(size_t group);
	void draw_row(const ImportPlan &plan, size_t index, size_t depth, bool tree);
	void draw_group(const ImportPlan &plan, size_t group);

	ViewEventMailbox<> events_;
	bool retake_ = true;        // a plan made since the checks were taken: they are taken again
	std::vector<bool> checked_; // per plan row: taken by the import
	// Per plan row: why the import cannot take it ("" when it can), found once per plan.
	std::vector<std::string> why_not_;
	std::vector<bool> chosen_;  // per choice: among the files chosen
	char filter_[128]{};
	AssetKind choice_kind_ = AssetKind::kCount; // the choices of one kind alone (kCount: every kind)
	// The plan's rows shown: those of one kind (kCount: every kind) whose names hold the text.
	AssetKind kind_shown_ = AssetKind::kCount;
	char rows_filter_[128]{};
	bool replace_existing_ = false;
	bool previewing_ = false; // the view previewed an import last frame: its state is this one's
	// The plan's tree: the plan it was made of, its groups (import_plan_groups), which are open, each
	// group's rows and those of its groups in `order_` ([first, last)), and the lines it shows (made
	// again when a group opens or closes: `laid_out_` false).
	std::shared_ptr<const ImportPlan> grouped_;
	std::vector<ImportPlanGroup> groups_;
	std::vector<bool> open_;
	std::vector<size_t> order_;
	std::vector<std::pair<size_t, size_t>> spans_;
	std::vector<size_t> brings_; // per plan row: the group of a chosen file that brings others (kNone)
	std::vector<Line> lines_;
	bool laid_out_ = false;
};

} // namespace opennova::editor
