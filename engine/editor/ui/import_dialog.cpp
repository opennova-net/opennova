#include <editor/ui/import_dialog.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/graph/asset_graph.h>
#include <editor/import/import_plan.h>
#include <editor/model/field_text.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

using State = ImportPlanRow::State;

// "a", "a and b", "a, b and c".
std::string joined(const std::vector<std::string> &words) {
	std::string out;
	for (size_t i = 0; i < words.size(); ++i) {
		if (i > 0) out += i + 1 == words.size() ? " and " : ", ";
		out += words[i];
	}
	return out;
}

// What wanted a file: the file naming it, then the record and the field.
std::string need_words(const ImportNeed &need) {
	std::string out = need.file;
	if (!need.record.empty()) out += ": " + need.record;
	if (!need.field.empty()) out += (need.record.empty() ? ": " : " ") + need.field;
	return out;
}

// Where a row's file comes from; a converter's output, the source it is made from too. Short,
// for its cell: a folder or an archive by its own name (the whole path is the tooltip's).
std::string origin_words(const ImportPlanRow &row, bool short_place) {
	std::string place = row.found_in;
	const std::filesystem::path path = path_of(row.source.path);
	if (short_place && !row.source.install && row.source.entry.empty() && !path.parent_path().filename().empty())
		place = "the folder " + utf8_of(path.parent_path().filename());
	else if (short_place && !row.source.install && !row.source.entry.empty())
		place = "the archive " + basename_of(row.source.path);
	return row.made_from.empty() ? place : "made from " + row.made_from + ", " + place;
}

// Why the import cannot take a row: its file's problem, or that of another file its converter
// source makes (the files of one source come together or not at all); "" when it can.
std::string cannot_take(const ImportPlan &plan, size_t index) {
	const ImportPlanRow &row = plan.rows[index];
	if (!row.problem.empty()) return row.problem;
	if (row.made_from.empty()) return std::string();
	for (const ImportPlanRow &other : plan.rows)
		if (!other.made_from.empty() && other.source == row.source && !other.problem.empty())
			return other.name + ", made from " + row.made_from + " too: " + other.problem;
	return std::string();
}

// What the plan does not follow, in one line, and each kind with its count in its tooltip.
void not_followed_line(const ImportPlan &plan) {
	std::vector<std::string> files, references;
	std::string tip;
	for (const ImportNotFollowed &entry : plan.not_followed) {
		if (entry.reference == ReferenceKind::None) {
			files.push_back(asset_kind_label(entry.kind));
			tip += std::string(asset_kind_label(entry.kind)) + ": " + counted(entry.count, "file") + ", the first " +
			       entry.first + "\n";
		} else {
			const std::string label = reference_row(entry.reference).label;
			references.push_back(label);
			tip += label + ": " + counted(entry.count, "reference") + ", the first in " + entry.first + "\n";
		}
	}
	std::string line;
	if (!files.empty()) line = "The files these kinds name are not looked for yet: " + joined(files) + ".";
	if (!references.empty())
		line += std::string(line.empty() ? "" : " ") + "References that name no file are not followed: " + joined(references) +
		        ".";
	// The symbols followed to no file: what no place defines.
	std::vector<std::string> undefined;
	for (const ImportNotFollowed &entry : plan.undefined) {
		const std::string label = reference_row(entry.reference).label;
		undefined.push_back(counted(entry.count, label.c_str()));
		tip += label + ": " + counted(entry.count, "reference") + " no place defines, the first in " + entry.first + "\n";
	}
	if (!undefined.empty())
		line += std::string(line.empty() ? "" : " ") + "Named by the files but defined nowhere: " + joined(undefined) + ".";
	// The symbols only a place's copy of a file the project holds defines (review F6).
	std::vector<std::string> shadowed;
	for (const ImportNotFollowed &entry : plan.shadowed) {
		const std::string label = reference_row(entry.reference).label;
		shadowed.push_back(counted(entry.count, label.c_str()));
		tip += label + ": " + counted(entry.count, "reference") + " only the source's " + asset_kind_label(entry.kind) +
		       " defines, which the project's own does not, the first in " + entry.first + "\n";
	}
	if (!shadowed.empty())
		line += std::string(line.empty() ? "" : " ") + "Defined only in the source's copy of a file the project has (its own is kept, "
		        "so these stay undefined; import that file with Replace to take the source's): " + joined(shadowed) + ".";
	if (line.empty()) return;
	ImGui::TextWrapped("%s", line.c_str());
	if (!tip.empty()) tip.pop_back();
	ui_kit::tooltip(tip);
}

} // namespace

void ImportDialog::draw(Workspace &workspace) {
	const SessionView &v = workspace.view();
	const DialogsView::ImportPreview &preview = v.dialogs.import_preview;
	// A plan made since the dialog last drew (an ImportPlanned event): its checks are taken again.
	for (const ViewEvent &event : events_.take())
		retake_ = retake_ || event.kind == ViewEventKind::ImportPlanned;
	// A new preview starts clean; one an Import found changed keeps what was asked of it. A
	// preview that stays open (an Import the session refused, or that waits on the unsaved
	// prompt) keeps its checks, its filter and Replace existing files.
	if (preview.open && !previewing_) {
		if (!preview.changed) {
			filter_[0] = '\0';
			rows_filter_[0] = '\0';
			kind_shown_ = AssetKind::kCount;
			replace_existing_ = false;
		}
		retake_ = true;
	}
	previewing_ = preview.open;
	// The unsaved prompt an Import raised (it writes over a file with unsaved edits) takes the
	// dialog's place until it is answered: both are modals at the top level, where opening one
	// closes the other.
	if (v.dialogs.unsaved_prompt.open) return;
	if (preview.open && !ImGui::IsPopupOpen("Import files")) ImGui::OpenPopup("Import files");
	const float em = ImGui::GetFontSize();
	ImGui::SetNextWindowSize(ImVec2(em * 64.0f, em * 41.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::BeginPopupModal("Import files")) return;
	if (!preview.open) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	if (retake_ || checked_.size() != preview.plan->rows.size() ||
			chosen_.size() != preview.choices.size())
		take(preview);
	if (preview.changed) {
		ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::severity_color(DiagnosticSeverity::Warning));
		ImGui::TextWrapped("The files changed since the preview, so nothing was imported: this is the import as they "
		                   "are now.");
		ImGui::PopStyleColor();
	}
	const bool from_game = (!preview.roots.empty() && preview.roots.front().install) ||
	                       (!preview.choices.empty() && preview.choices.front().install);
	ImGui::TextWrapped("%s", preview.all ? "Copy every file of the game data into the project."
	                         : from_game  ? "Copy files from the game data into the project."
	                                      : "Copy files into the project.");
	// The lists scroll; Replace existing files, Import and Cancel stay under them, on two lines
	// in a narrow dialog.
	ImGui::BeginChild("import_body", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3));
	if (!preview.choices.empty()) draw_choices(workspace, preview);
	draw_plan(workspace, preview);
	draw_notes(preview);
	ImGui::EndChild();

	// A chosen file the project cannot take stays checked, and the import waits until it is
	// unchecked (or its source fixed and planned again): the import would refuse it.
	size_t count = 0;
	std::string blocked;
	for (size_t i = 0; i < checked_.size(); ++i) {
		if (!checked_[i]) continue;
		++count;
		if (blocked.empty() && i < why_not_.size() && !why_not_[i].empty())
			blocked = preview.plan->rows[i].name + " cannot be imported: " + why_not_[i] +
			          " Uncheck it to import the rest.";
	}
	// Unsaved edits hold nothing here: an import that would write over an edited file asks to
	// save it first (the session's unsaved prompt), one that writes over none goes ahead.
	ui_kit::WrapRow actions;
	actions.next(ui_kit::checkbox_width("Replace existing files"));
	// The files the project holds already are unchecked by default and kept as they are; Replace
	// existing files checks them all (each can still be checked or unchecked alone).
	if (ImGui::Checkbox("Replace existing files", &replace_existing_))
		for (size_t i = 0; i < checked_.size(); ++i)
			if (preview.plan->rows[i].held && why_not_[i].empty()) checked_[i] = replace_existing_;
	ui_kit::tooltip("Check every file the project has already, to write it over; left unchecked, the project's file "
	                "stays as it is.");
	const std::string label = "Import " + counted(count, "file") + "###import";
	// An import writes the project's files: while an operation holds them (a build packing
	// them), the busy gate refuses it, and Import waits with it (SessionView::allows).
	const bool allowed = workspace.view().allows(EditorRequestKind::ImportFiles);
	const std::string why = count == 0        ? "Check the files to import first."
	                        : !blocked.empty() ? blocked
	                        : !allowed         ? "An import writes the project's files: it waits for the running operation."
	                                           : "Copy the checked files into the project (Undo cannot take the copy back).";
	if (ui_kit::tool(actions, label.c_str(), count > 0 && blocked.empty() && allowed, why)) {
		std::vector<ImportChoice> imports;
		std::set<ImportChoice> sent; // each source once, looked up in log time (a whole install's rows)
		bool replaces = replace_existing_; // a checked file the project holds is one asked to be replaced
		for (size_t i = 0; i < checked_.size(); ++i) {
			const ImportChoice &source = preview.plan->rows[i].source;
			replaces = replaces || (checked_[i] && preview.plan->rows[i].held);
			if (checked_[i] && sent.insert(source).second) imports.push_back(source);
		}
		EditorRequest request = request::import_files(std::move(imports), replaces);
		workspace.request(std::move(request));
		ImGui::CloseCurrentPopup();
	}
	if (ui_kit::tool(actions, "Cancel", workspace.view().allows(EditorRequestKind::CancelImport), "Import nothing.")) {
		workspace.request(request::cancel_import());
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

// The checks of a new plan: each row the plan takes (a chosen file whatever its problem, a
// dependency only when the project can take it); each choice among the files chosen.
void ImportDialog::take(const DialogsView::ImportPreview &preview) {
	retake_ = false;
	const ImportPlan &plan = *preview.plan;
	why_not_.assign(plan.rows.size(), std::string());
	for (size_t i = 0; i < plan.rows.size(); ++i) why_not_[i] = cannot_take(plan, i);
	checked_.assign(plan.rows.size(), false);
	for (size_t i = 0; i < plan.rows.size(); ++i)
		checked_[i] = (plan.rows[i].selected && (plan.rows[i].state == State::Selected || why_not_[i].empty())) ||
		              (plan.rows[i].held && replace_existing_ && why_not_[i].empty());
	chosen_.assign(preview.choices.size(), false);
	const std::set<ImportChoice> roots(preview.roots.begin(), preview.roots.end());
	for (size_t i = 0; i < preview.choices.size(); ++i) chosen_[i] = roots.count(preview.choices[i]) > 0;
}

// The files chosen, planned again: the chosen ones that are not in the list, then those
// checked in it.
void ImportDialog::choose(Workspace &workspace, const DialogsView::ImportPreview &preview) {
	EditorRequest request = request::plan_import({}, preview.with_dependencies);
	const std::set<ImportChoice> listed(preview.choices.begin(), preview.choices.end());
	for (const ImportChoice &root : preview.roots)
		if (!listed.count(root)) request.imports.push_back(root);
	for (size_t i = 0; i < preview.choices.size(); ++i)
		if (chosen_[i]) request.imports.push_back(preview.choices[i]);
	workspace.request(std::move(request));
}

// A listing's files to choose from, with a filter: each change plans the import again.
void ImportDialog::draw_choices(Workspace &workspace, const DialogsView::ImportPreview &preview) {
	const ImportChoice &first = preview.choices.front();
	const std::string from =
	        first.install ? std::string("the game data") : "the archive " + basename_of(first.path);
	ImGui::TextWrapped("Choose the files to import from %s:", from.c_str());
	const float em = ImGui::GetFontSize();
	ui_kit::WrapRow controls;
	const float filter_width = em * 18.0f;
	controls.next(filter_width);
	ui_kit::filter_box("##filter", filter_, sizeof(filter_), "Filter files", filter_width);
	std::vector<size_t> visible;
	const std::string filter = normalized_logical_name(filter_);
	for (size_t i = 0; i < preview.choices.size(); ++i)
		if (normalized_logical_name(preview.choices[i].name()).find(filter) != std::string::npos) visible.push_back(i);
	if (ui_kit::tool(controls, "Select shown", !visible.empty(), "Choose every file the list shows.")) {
		for (size_t i : visible) chosen_[i] = true;
		choose(workspace, preview);
	}
	if (ui_kit::tool(controls, "Clear selection", true, "Choose none of the listed files.")) {
		std::fill(chosen_.begin(), chosen_.end(), false);
		choose(workspace, preview);
	}
	if (visible.empty()) {
		ui_kit::empty_state("No file matches the filter.");
		return;
	}
	const float rows = static_cast<float>(std::min<size_t>(visible.size(), 8)) + 1.5f;
	if (!ImGui::BeginTable("import_choices", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable,
	                       ImVec2(0, ImGui::GetFrameHeightWithSpacing() * rows)))
		return;
	ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableHeadersRow();
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(visible.size()));
	while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
		const size_t index = visible[static_cast<size_t>(row)];
		const ImportChoice &source = preview.choices[index];
		ImGui::PushID(static_cast<int>(index));
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		// A check box with the file's name, cut to its cell (whole in its tooltip).
		const std::string name = source.name();
		const float room = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x;
		const std::string shown = ui_kit::fit(name, room);
		bool chosen = chosen_[index];
		if (ImGui::Checkbox((shown + "###pick").c_str(), &chosen)) {
			chosen_[index] = chosen;
			choose(workspace, preview);
		}
		ui_kit::tooltip(shown != name ? name : std::string());
		ImGui::TableNextColumn();
		ui_kit::clipped_text(source.install ? "game data" : basename_of(source.path), source.path);
		ImGui::PopID();
	}
	ImGui::EndTable();
}

// "Include the files these need", the plan in short (its files and bytes, each kind a toggle that
// shows its rows alone, a filter over the names, Check shown and Uncheck shown), then the rows:
// the chosen files first, then what they need.
void ImportDialog::draw_plan(Workspace &workspace, const DialogsView::ImportPreview &preview) {
	const ImportPlan &plan = *preview.plan;
	size_t found = 0;
	std::vector<size_t> rows;
	const std::string wanted = normalized_logical_name(rows_filter_);
	for (size_t i = 0; i < plan.rows.size(); ++i) {
		if (plan.rows[i].state == State::NotFound) continue;
		if (plan.rows[i].state == State::Found) ++found;
		if (kind_shown_ != AssetKind::kCount && plan.rows[i].kind != kind_shown_) continue;
		if (!wanted.empty() && normalized_logical_name(plan.rows[i].name).find(wanted) == std::string::npos) continue;
		rows.push_back(i);
	}
	// The check box's label cut to the dialog's width (whole in its tooltip).
	bool with = preview.with_dependencies;
	const std::string include = "Include the files these need" + (with ? " (" + std::to_string(found) + " found)" : std::string());
	const float room = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x;
	const std::string shown = ui_kit::fit(include, room);
	// The setting plans the open dialog again (a plan_import through the gate): held back while
	// that would be refused, and with everything chosen (a walk of every file finds nothing more).
	const bool plans = !preview.all && workspace.view().allows(EditorRequestKind::SetImportDependencies) &&
	                   workspace.view().allows(EditorRequestKind::PlanImport);
	ImGui::BeginDisabled(!plans);
	if (ImGui::Checkbox((shown + "###needs").c_str(), &with) && plans)
		workspace.request(request::set_import_dependencies(with));
	ImGui::EndDisabled();
	ui_kit::tooltip((shown != include ? include + ".\n" : std::string()) +
	                (preview.all ? std::string("Every file of the game install is chosen: there is nothing more to look for.")
	                             : std::string("Look for the files the chosen ones name (fonts, textures, models...) beside them "
	                                           "and in the game install, and import those found too. The editor remembers it.")));
	if (plan.file_count() == 0) {
		if (preview.roots.empty()) ui_kit::empty_state("No file chosen.", "Choose the files to import above.");
		else ui_kit::empty_state("Nothing to import.", plan.diagnostics.empty() ? nullptr : "See why below.");
		return;
	}
	// The plan in short: its files and bytes, then each kind with its count and size, a toggle that
	// shows that kind's rows alone (the videos of a mission's closure unchecked in two clicks).
	ImGui::TextWrapped("%s", (counted(plan.file_count(), "file") + ", " + ui_kit::size_text(plan.total_bytes()) + ":").c_str());
	ui_kit::WrapRow kinds;
	for (const ImportPlanKind &entry : plan.by_kind()) {
		const std::string label = std::string(asset_kind_label(entry.kind)) + " " + std::to_string(entry.files) + " (" +
		                          ui_kit::size_text(entry.bytes) + ")";
		const std::string id = label + "###kind_" + asset_kind_token(entry.kind);
		const float width = ui_kit::text_width(label.c_str()) + ImGui::GetStyle().FramePadding.x * 2.0f;
		kinds.next(width);
		const bool shown = kind_shown_ == entry.kind;
		if (ImGui::Selectable(id.c_str(), shown, ImGuiSelectableFlags_None, ImVec2(width, 0.0f)))
			kind_shown_ = shown ? AssetKind::kCount : entry.kind;
		ui_kit::tooltip(shown ? std::string("Every kind's rows again.")
		                      : "Only the rows of this kind, " + counted(entry.files, "file") + ".");
	}
	// A filter over the rows' names (Ctrl+F is the listing's filter's where one is drawn), and the
	// shown rows checked or unchecked together.
	ui_kit::WrapRow controls;
	const float filter_width = ImGui::GetFontSize() * 18.0f;
	controls.next(filter_width);
	ui_kit::filter_box("##rows_filter", rows_filter_, sizeof(rows_filter_), "Filter the rows", filter_width, nullptr,
	                   preview.choices.empty());
	if (ui_kit::tool(controls, "Check shown", !rows.empty(), "Take every row the table shows that the project can take.")) {
		for (const size_t i : rows)
			if (why_not_[i].empty()) checked_[i] = true;
	}
	if (ui_kit::tool(controls, "Uncheck shown", !rows.empty(), "Leave every row the table shows out.")) {
		for (const size_t i : rows) checked_[i] = false;
	}
	if (rows.empty()) {
		ui_kit::empty_state("No row matches.", "Clear the filter, or show every kind.");
		return;
	}
	const float lines = static_cast<float>(std::min<size_t>(rows.size(), 10)) + 1.5f;
	if (!ImGui::BeginTable("import_plan", 6,
	                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
	                               ImGuiTableFlags_BordersInnerV,
	                       ImVec2(0, ImGui::GetFrameHeightWithSpacing() * lines)))
		return;
	ImGui::TableSetupColumn("##take", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
	                        ImGui::GetFrameHeight());
	ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ui_kit::text_width("999.9 KB"));
	ImGui::TableSetupColumn("Needed by", ImGuiTableColumnFlags_WidthStretch, 3.0f);
	ImGui::TableSetupColumn("Found in", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableHeadersRow();
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(rows.size()));
	while (clipper.Step()) for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
		const size_t index = rows[static_cast<size_t>(line)];
		const ImportPlanRow &row = plan.rows[index];
		// A dependency the project cannot take stays unchecked; a chosen one can only be
		// unchecked (Import waits while it is checked).
		const std::string &why_not = why_not_[index];
		const bool can = why_not.empty() || (row.state == State::Selected && checked_[index]);
		ImGui::PushID(static_cast<int>(index));
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::BeginDisabled(!can);
		bool take = checked_[index];
		if (ImGui::Checkbox("##take", &take) && can) {
			// The files one converter source makes come together.
			for (size_t i = 0; i < plan.rows.size(); ++i)
				if (i == index || (!row.made_from.empty() && plan.rows[i].made_from == row.made_from &&
				                   plan.rows[i].source == row.source))
					checked_[i] = take;
		}
		ImGui::EndDisabled();
		ui_kit::tooltip(!why_not.empty() ? "It cannot be imported: " + why_not
		                : row.held       ? "The project has " + row.destination +
		                                     " already: checked, it is written over; left unchecked, it stays as it is."
		                : row.made_from.empty() ? std::string()
		                                        : "The files made from " + row.made_from + " come together.");
		ImGui::TableNextColumn();
		const std::string where = "Goes to " + row.destination + (row.problem.empty() ? "" : "\n" + row.problem);
		if (!row.problem.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::severity_color(DiagnosticSeverity::Error));
		ui_kit::clipped_text(row.name, row.name + "\n" + where);
		if (!row.problem.empty()) ImGui::PopStyleColor();
		ImGui::TableNextColumn();
		ui_kit::clipped_text(asset_kind_label(row.kind));
		ImGui::TableNextColumn();
		ui_kit::clipped_text(ui_kit::size_text(row.size), std::to_string(row.size) + " bytes as stored");
		ImGui::TableNextColumn();
		if (row.state == State::Found)
			ui_kit::clipped_text(need_words(row.needed_by), need_words(row.needed_by) + " names " + row.needed_by.name);
		else if (row.held)
			ui_kit::clipped_text("chosen, the project has it", "One of the files chosen, which the project has already.");
		else
			ui_kit::clipped_text("chosen", "One of the files chosen.");
		ImGui::TableNextColumn();
		ui_kit::clipped_text(origin_words(row, true), origin_words(row, false));
		ImGui::PopID();
	}
	ImGui::EndTable();
}

// Under the rows: the files not found, the ones found in more than one place, what is not
// followed, the cap, and what could not be read.
void ImportDialog::draw_notes(const DialogsView::ImportPreview &preview) {
	const ImportPlan &plan = *preview.plan;
	std::vector<const ImportPlanRow *> missing;
	for (const ImportPlanRow &row : plan.rows)
		if (row.state == State::NotFound) missing.push_back(&row);
	// Open by default while they are few; a mission's closure names dozens the install itself lacks.
	const std::string header = "Not found (" + std::to_string(missing.size()) + ")###not_found";
	const ImGuiTreeNodeFlags open_by_default = missing.size() <= 20 ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
	if (!missing.empty() && ImGui::CollapsingHeader(header.c_str(), open_by_default) &&
	    ImGui::BeginTable("import_missing", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Needed by", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableHeadersRow();
		for (size_t i = 0; i < missing.size(); ++i) {
			const ImportPlanRow &row = *missing[i];
			ImGui::PushID(static_cast<int>(i));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ui_kit::clipped_text(row.name);
			ImGui::TableNextColumn();
			ui_kit::clipped_text(asset_kind_label(row.kind));
			ImGui::TableNextColumn();
			ui_kit::clipped_text(need_words(row.needed_by), need_words(row.needed_by) + " names " + row.needed_by.name);
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	for (const ImportPlanRow &row : plan.rows)
		for (const ImportRival &rival : row.rivals) {
			const std::string line = row.name + ": found in both " + row.found_in + " and " + rival.found_in + "; using " +
			                         row.found_in + (rival.differs ? " (the two files differ)." : " (the same file).");
			ImGui::TextWrapped("%s", line.c_str());
		}
	not_followed_line(plan);
	if (plan.truncated) {
		ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::severity_color(DiagnosticSeverity::Warning));
		ImGui::TextWrapped("The plan stopped at %zu files: the files past them are not listed and not imported.",
		                   kImportPlanFileCap);
		ImGui::PopStyleColor();
	}
	for (const Diagnostic &d : plan.diagnostics) {
		ui_kit::severity_marker(d.severity);
		ImGui::SameLine();
		const std::string text = d.asset.empty() ? d.message : d.asset + ": " + d.message;
		ImGui::TextWrapped("%s", text.c_str());
	}
}

} // namespace opennova::editor
