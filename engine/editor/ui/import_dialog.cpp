#include <editor/ui/import_dialog.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_kinds.h>
#include <editor/graph/asset_graph.h>
#include <editor/import/import_plan.h>
#include <editor/model/field_text.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>
#include <imgui_internal.h>

namespace opennova::editor {

namespace {

using State = ImportPlanRow::State;
using Group = ImportPlanGroup;

// "a", "a and b", "a, b and c".
std::string joined(const std::vector<std::string> &words) {
	std::string out;
	for (size_t i = 0; i < words.size(); ++i) {
		if (i > 0) out += i + 1 == words.size() ? " and " : ", ";
		out += words[i];
	}
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

// How many lines the notes under the plan take at most, for the table's height: the files not found
// (their header, and a few of them while it opens), the rivals, what is not followed, the cap and the
// findings.
size_t note_lines(const ImportPlan &plan) {
	size_t missing = 0, rivals = 0;
	for (const ImportPlanRow &row : plan.rows) {
		missing += row.state == State::NotFound ? 1 : 0;
		rivals += row.rivals.size();
	}
	size_t lines = (missing ? 1 + std::min<size_t>(missing <= 20 ? missing + 1 : 0, 6) : 0) + std::min<size_t>(rivals, 3) +
	               plan.diagnostics.size() + (plan.truncated ? 1 : 0);
	if (!plan.not_followed.empty() || !plan.undefined.empty() || !plan.shadowed.empty()) lines += 2;
	return std::min<size_t>(lines, 10);
}

// A tree line's lead in its cell: indented to its depth as far as the cell leaves room for the arrow and
// a few letters, then the arrow that opens or closes it (`open` null: a line with none, its name lined
// up with those after an arrow). True when the arrow was pressed.
bool tree_lead(size_t depth, const bool *open) {
	const float arrow = ImGui::GetFrameHeight();
	const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
	const float room = std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 2.0f);
	const float indent = std::min(static_cast<float>(depth) * ImGui::GetStyle().IndentSpacing, std::max(0.0f, room - arrow - spacing));
	if (!open) {
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::min(indent + arrow + spacing, room));
		return false;
	}
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	const bool pressed = ImGui::ArrowButton("##open", *open ? ImGuiDir_Down : ImGuiDir_Right);
	ImGui::PopStyleColor();
	ui_kit::tooltip(*open ? "Close it." : "Open it: the files in it.");
	ImGui::SameLine(0.0f, spacing);
	return pressed;
}

// What a chosen file the project has already says of the project's (the plan compared the two where
// it could), and its tooltip.
std::pair<std::string, std::string> held_words(const ImportPlanRow &row) {
	switch (row.held_as) {
	case ImportPlanRow::Held::Same:
		return {"chosen; the project has the same file",
		        "The project's " + row.destination + " holds the same bytes: replacing it changes nothing."};
	case ImportPlanRow::Held::Differs:
		return {"chosen; the project's differs",
		        "The project's " + row.destination + " is not this file (edited, or another version): checked, the import "
		        "writes over it and what it holds now is lost."};
	case ImportPlanRow::Held::Unknown: break;
	}
	return {"chosen, the project has it", "One of the files chosen, which the project has already."};
}

} // namespace

// The dialog's own as the workspace holds it (the MCP gaps lane): its filters, its kinds, Replace existing
// files, taken where the session's moved, and its checks, taken again whenever their serial moves (a plan
// made, a client's check). A new preview starts them afresh, the session's doing; one that stays open (an
// Import the session refused, or that waits on the unsaved prompt) keeps them.
void ImportDialog::follow(const SessionView &view) {
	const WorkspaceView::Import &held = view.workspace.import;
	// A member the session moved is the one sent last too (review X8): what the person sets after it, back to
	// what the window sent before, differs from it and goes to the session.
	if (held.filter != filter_.seen) sent_.filter = held.filter;
	if (held.rows_filter != rows_filter_.seen) sent_.rows_filter = held.rows_filter;
	filter_.follow(held.filter);
	rows_filter_.follow(held.rows_filter);
	if (choice_kind_held_.follow(held.choice_kind)) sent_.choice_kind = choice_kind_ = held.choice_kind;
	if (kind_shown_held_.follow(held.kind_shown)) sent_.kind_shown = kind_shown_ = held.kind_shown;
	if (replace_held_.follow(held.replace_existing)) sent_.replace_existing = replace_existing_ = held.replace_existing;
	if (checks_held_.follow(held.serial)) retake_ = true;
}

// What the person changed of the dialog's own this frame, sent to the workspace: the filters, the kinds,
// Replace existing files, and the checks that differ from the session's, of the plan they index (each
// change once: what the session takes it follows, and a value it moved since is sent again).
void ImportDialog::send(Workspace &workspace, const DialogsView::ImportPreview &preview) {
	const WorkspaceView::Import &held = workspace.view().workspace.import;
	io::JsonValue members = io::JsonValue::make_object();
	const auto kind_text = [](AssetKind kind) { return io::JsonValue::make_string(kind == AssetKind::kCount ? "" : asset_kind_token(kind)); };
	if (filter_.sent() != held.filter && filter_.sent() != sent_.filter) members.set("filter", io::JsonValue::make_string(filter_.sent()));
	if (rows_filter_.sent() != held.rows_filter && rows_filter_.sent() != sent_.rows_filter)
		members.set("rows_filter", io::JsonValue::make_string(rows_filter_.sent()));
	if (choice_kind_ != held.choice_kind && choice_kind_ != sent_.choice_kind) members.set("choice_kind", kind_text(choice_kind_));
	if (kind_shown_ != held.kind_shown && kind_shown_ != sent_.kind_shown) members.set("kind_shown", kind_text(kind_shown_));
	if (replace_existing_ != held.replace_existing && replace_existing_ != sent_.replace_existing)
		members.set("replace_existing", io::JsonValue::make_bool(replace_existing_));
	if (preview.plan && held.checked.size() == checked_.size() && checked_ != sent_.checked) {
		io::JsonValue check = io::JsonValue::make_array(), uncheck = io::JsonValue::make_array();
		for (size_t i = 0; i < checked_.size(); ++i)
			if (checked_[i] != held.checked[i]) (checked_[i] ? check : uncheck).push(io::JsonValue::make_number(double(i)));
		// The plan its rows' indices are of: one made since refuses them, and the dialog takes the new one's.
		if (!check.array.empty() || !uncheck.array.empty())
			members.set("plan", io::JsonValue::make_number(double(preview.plan_serial)));
		if (!check.array.empty()) members.set("check", std::move(check));
		if (!uncheck.array.empty()) members.set("uncheck", std::move(uncheck));
	}
	if (members.object.empty()) return;
	sent_.filter = filter_.sent();
	sent_.rows_filter = rows_filter_.sent();
	sent_.choice_kind = choice_kind_;
	sent_.kind_shown = kind_shown_;
	sent_.replace_existing = replace_existing_;
	sent_.checked = checked_;
	window_requests::set_workspace(workspace, "import", std::move(members));
}

void ImportDialog::draw(Workspace &workspace) {
	const SessionView &v = workspace.view();
	const DialogsView::ImportPreview &preview = v.dialogs.import_preview;
	follow(v);
	// The unsaved prompt an Import raised (it writes over a file with unsaved edits) takes the
	// dialog's place until it is answered: of the modals at the top level, where opening one
	// closes the other, the session's order says which shows (shown_modal), the others waiting.
	const bool shows = preview.open && modal_may_show(v, HeldModal::Import);
	if (shows && !ImGui::IsPopupOpen("Import files")) ImGui::OpenPopup("Import files");
	// As large as the editor's window allows (a plan of thousands of rows, a list of the game's nine
	// thousand files), centred, as it opens; the author's resize holds while it stays open.
	const float em = ImGui::GetFontSize();
	const ImGuiViewport *viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(std::min(viewport->WorkSize.x * 0.92f, em * 120.0f),
	                                std::max(viewport->WorkSize.y * 0.88f, std::min(viewport->WorkSize.y, em * 30.0f))),
	                         ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("Import files")) return;
	if (!shows) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	if (retake_ || checked_.size() != preview.plan->rows.size() ||
			chosen_.size() != preview.choices.size())
		take(v, preview);
	if (grouped_ != preview.plan) group(preview);
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
	// in a narrow dialog. A list to choose from takes some of the height, the plan the rest.
	ImGui::BeginChild("import_body", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3));
	if (!preview.choices.empty())
		draw_choices(workspace, preview,
		             std::max(ImGui::GetFrameHeightWithSpacing() * 12.0f, ImGui::GetContentRegionAvail().y * 0.42f));
	draw_plan(workspace, preview);
	draw_notes(preview);
	ImGui::EndChild();

	// What Import takes, by the one rule import_files planned takes it by (import_selection): a chosen file the
	// project cannot take stays checked, and the import waits until it is unchecked (or its source fixed and
	// planned again): the import would refuse it.
	ImportSelection selection = import_selection(*preview.plan, checked_, replace_existing_);
	const size_t count = selection.checked;
	const std::string blocked = selection.blocked;
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
	// While the plan is being made there is nothing to count yet: Import waits for it.
	const std::string label = (preview.planning ? std::string("Import") : "Import " + counted(count, "file")) + "###import";
	// An import writes the project's files: while an operation holds them (a build packing
	// them), the busy gate refuses it, and Import waits with it (SessionView::allows).
	const bool allowed = workspace.view().allows(EditorRequestKind::ImportFiles);
	const std::string why = preview.planning ? "The import is being planned: Import waits for its plan."
	                        : count == 0      ? "Check the files to import first."
	                        : !blocked.empty() ? blocked
	                        : !allowed         ? "An import writes the project's files: it waits for the running operation."
	                                           : "Copy the checked files into the project (Undo cannot take the copy back).";
	if (ui_kit::tool(actions, label.c_str(), !preview.planning && count > 0 && blocked.empty() && allowed, why)) {
		// Replace existing files, or a checked file the project holds, asks to replace (import_selection).
		EditorRequest request = request::import_files(std::move(selection.sources), selection.replace);
		send(workspace, preview);
		workspace.request(std::move(request));
		ImGui::CloseCurrentPopup();
	}
	if (ui_kit::tool(actions, "Cancel", workspace.view().allows(EditorRequestKind::CancelImport), "Import nothing.")) {
		workspace.request(request::cancel_import());
		ImGui::CloseCurrentPopup();
	}
	send(workspace, preview);
	ImGui::EndPopup();
}

// The checks of a plan, the workspace's (each plan made takes them anew: import_default_checks, a chosen
// file whatever its problem, a dependency only when the project can take it), and why each row cannot be
// imported; each choice among the files chosen.
void ImportDialog::take(const SessionView &view, const DialogsView::ImportPreview &preview) {
	retake_ = false;
	const ImportPlan &plan = *preview.plan;
	why_not_.assign(plan.rows.size(), std::string());
	for (size_t i = 0; i < plan.rows.size(); ++i) why_not_[i] = import_row_refusal(plan, i);
	const std::vector<bool> &held = view.workspace.import.checked;
	checked_ = held.size() == plan.rows.size() ? held : import_default_checks(plan, replace_existing_);
	sent_.checked = checked_;
	chosen_.assign(preview.choices.size(), false);
	const std::set<ImportChoice> roots(preview.roots.begin(), preview.roots.end());
	for (size_t i = 0; i < preview.choices.size(); ++i) chosen_[i] = roots.count(preview.choices[i]) > 0;
}

// The plan's groups, each open as it starts: the chosen files while they are few (so what each
// brings shows by kind), a kind while it holds few files.
void ImportDialog::group(const DialogsView::ImportPreview &preview) {
	grouped_ = preview.plan;
	groups_ = import_plan_groups(*preview.plan);
	size_t chosen = 0;
	for (const Group &group : groups_) chosen += group.chosen() ? 1 : 0;
	open_.assign(groups_.size(), false);
	for (size_t g = 0; g < groups_.size(); ++g)
		open_[g] = groups_[g].chosen() ? chosen <= 8 : groups_[g].files <= 12 || groups_.size() == 1;
	// Each group's rows (those it lists for the file above it too) and those of the groups in it, together,
	// depth first.
	order_.clear();
	spans_.assign(groups_.size(), {0, 0});
	const auto walk = [this](auto &&self, size_t g) -> void {
		spans_[g].first = order_.size();
		order_.insert(order_.end(), groups_[g].rows.begin(), groups_[g].rows.end());
		order_.insert(order_.end(), groups_[g].also.begin(), groups_[g].also.end());
		for (const size_t child : groups_[g].children) self(self, child);
		spans_[g].second = order_.size();
	};
	// Each planned file's row by its name: whom a row's other wanting files are.
	row_of_.clear();
	for (size_t i = 0; i < preview.plan->rows.size(); ++i)
		if (preview.plan->rows[i].state != State::NotFound)
			row_of_.emplace(normalized_logical_name(preview.plan->rows[i].name), i);
	for (size_t g = 0; g < groups_.size(); ++g)
		if (groups_[g].parent == Group::kNone) walk(walk, g);
	// The chosen files that bring others, by row: their arrows.
	brings_.assign(preview.plan->rows.size(), Group::kNone);
	for (size_t g = 0; g < groups_.size(); ++g)
		if (groups_[g].chosen() && !groups_[g].children.empty()) brings_[groups_[g].root] = g;
	laid_out_ = false;
}

void ImportDialog::lay_out() {
	lines_.clear();
	for (size_t g = 0; g < groups_.size(); ++g)
		if (groups_[g].parent == Group::kNone) lay_out(g);
	laid_out_ = true;
}

// A chosen file is its row's line, what it brings under it; a kind's group is a line of its own, the
// kinds its files bring, then its rows.
void ImportDialog::lay_out(size_t g) {
	const Group &group = groups_[g];
	if (group.chosen()) lines_.push_back({false, group.root, 0});
	else lines_.push_back({true, g, group.depth});
	if (!open_[g]) return;
	for (const size_t child : group.children) lay_out(child);
	if (group.chosen()) return;
	for (const size_t row : group.rows) lines_.push_back({false, row, group.depth + 1});
	for (const size_t row : group.also) lines_.push_back({false, row, group.depth + 1, true});
}

// Whether another checked file wants the row's file, one neither of the branch (`excluded`: its rows) nor of
// the files above it (whose want of it is what leaving the branch out declines): it stays checked then.
bool ImportDialog::needed_outside(const ImportPlan &plan, size_t row, const std::set<size_t> &excluded) const {
	for (const std::string &file : plan.rows[row].wanted_by) {
		const auto other = row_of_.find(normalized_logical_name(file));
		if (other != row_of_.end() && !excluded.count(other->second) && other->second < checked_.size() && checked_[other->second])
			return true;
	}
	return false;
}

// Whether a check of many rows at once (a kind's line, Check shown) takes the row: one the project can take,
// and not a file the project has unless Replace existing files is on (each of those only by its own check).
bool ImportDialog::takes_together(const ImportPlan &plan, size_t row) const {
	return why_not_[row].empty() && (!plan.rows[row].held || replace_existing_);
}

// A listing's files to choose from, each with its kind and size, with a filter over the names (a
// kind's name finds that kind) and a kind to show alone: each change plans the import again.
void ImportDialog::draw_choices(Workspace &workspace, const DialogsView::ImportPreview &preview, float height) {
	const float top = ImGui::GetCursorPosY();
	const ImportChoice &first = preview.choices.front();
	const std::string from =
	        first.install ? std::string("the game data") : "the archive " + basename_of(first.path);
	const auto fact = [&preview](size_t i) {
		return i < preview.facts.size() ? preview.facts[i] : ImportChoiceFacts{};
	};
	ImGui::TextWrapped("Choose the files to import from %s (%s):", from.c_str(),
	                   counted(preview.choices.size(), "file").c_str());
	const float em = ImGui::GetFontSize();
	ui_kit::WrapRow controls;
	const float filter_width = em * 18.0f;
	controls.next(filter_width);
	ui_kit::filter_box("##filter", filter_.text, sizeof(filter_.text), "Filter files", filter_width,
	                   "A part of the name, or a kind (\"texture\", \"waves\"; \"kind:texture\" for that kind alone).");
	// The kinds the list has, each with how many.
	std::vector<size_t> counts(kAssetKindCount, 0);
	for (size_t i = 0; i < preview.choices.size(); ++i)
		if (static_cast<size_t>(fact(i).kind) < kAssetKindCount) ++counts[static_cast<size_t>(fact(i).kind)];
	const float kind_width = em * 12.0f;
	controls.next(kind_width);
	const std::string kind_label = choice_kind_ == AssetKind::kCount ? std::string("Every kind") : std::string(asset_kind_label(choice_kind_));
	ImGui::SetNextItemWidth(kind_width);
	if (ImGui::BeginCombo("##choice_kind", kind_label.c_str(), ImGuiComboFlags_HeightLarge)) {
		if (ImGui::Selectable("Every kind", choice_kind_ == AssetKind::kCount)) choice_kind_ = AssetKind::kCount;
		for (size_t k = 0; k < kAssetKindCount; ++k) {
			if (!counts[k]) continue;
			const AssetKind kind = static_cast<AssetKind>(k);
			const std::string label = std::string(asset_kind_label(kind)) + " (" + strutil::grouped(counts[k]) + ")###" + asset_kind_token(kind);
			if (ImGui::Selectable(label.c_str(), choice_kind_ == kind)) choice_kind_ = kind;
		}
		ImGui::EndCombo();
	}
	ui_kit::tooltip(choice_kind_ == AssetKind::kCount ? "Only the files of one kind." : "Only the " + kind_label + " files: Every kind lists them all.");
	// The files the filter and the kind show: a name holding the text, or a file of the kind it names.
	std::vector<size_t> visible;
	const std::string filter = normalized_logical_name(filter_.text);
	bool kind_only = false;
	const AssetKind named = filter.empty() ? AssetKind::kCount : asset_kind_named_by(filter_.text, &kind_only);
	for (size_t i = 0; i < preview.choices.size(); ++i) {
		if (choice_kind_ != AssetKind::kCount && fact(i).kind != choice_kind_) continue;
		const bool by_name = !kind_only && normalized_logical_name(preview.choices[i].name()).find(filter) != std::string::npos;
		if (filter.empty() || by_name || (named != AssetKind::kCount && fact(i).kind == named)) visible.push_back(i);
	}
	if (ui_kit::tool(controls, "Select shown", !visible.empty(), "Choose every file the list shows.")) {
		for (size_t i : visible) chosen_[i] = true;
		choose(workspace, preview);
	}
	if (ui_kit::tool(controls, "Clear selection", true, "Choose none of the listed files.")) {
		std::fill(chosen_.begin(), chosen_.end(), false);
		choose(workspace, preview);
	}
	size_t picked = 0;
	for (const bool chosen : chosen_) picked += chosen ? 1 : 0;
	const std::string shown = (visible.size() == preview.choices.size() ? counted(visible.size(), "file")
	                                                                    : strutil::grouped(visible.size()) + " of " + counted(preview.choices.size(), "file")) +
	                          " shown, " + strutil::grouped(picked) + " chosen";
	controls.next(ui_kit::text_width(shown.c_str()));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", shown.c_str());
	if (visible.empty()) {
		ui_kit::empty_state("No file matches the filter.", "Clear the filter, or show every kind.");
		return;
	}
	// The place each file comes from, only where they are not all from one.
	bool mixed = false;
	for (const ImportChoice &choice : preview.choices)
		mixed = mixed || choice.install != first.install || choice.path != first.path;
	const float table_height = std::max(ImGui::GetFrameHeightWithSpacing() * 6.0f, height - (ImGui::GetCursorPosY() - top));
	if (!ImGui::BeginTable("import_choices", mixed ? 4 : 3,
	                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
	                       ImVec2(0, table_height)))
		return;
	// The kind as wide as the longest the list has.
	float kind_column = ui_kit::text_width("Kind");
	for (size_t k = 0; k < kAssetKindCount; ++k)
		if (counts[k]) kind_column = std::max(kind_column, ui_kit::text_width(asset_kind_label(static_cast<AssetKind>(k))));
	ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
	ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, std::min(kind_column, em * 12.0f));
	ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ui_kit::text_width("999.9 KB"));
	if (mixed) ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.0f);
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
		const std::string fitted = ui_kit::fit(name, room);
		bool chosen = chosen_[index];
		if (ImGui::Checkbox((fitted + "###pick").c_str(), &chosen)) {
			chosen_[index] = chosen;
			choose(workspace, preview);
		}
		ui_kit::tooltip(fitted != name ? name : std::string());
		ImGui::TableNextColumn();
		ui_kit::clipped_text(asset_kind_label(fact(index).kind));
		ImGui::TableNextColumn();
		ui_kit::clipped_text(strutil::byte_size_text(fact(index).size), strutil::grouped(size_t(fact(index).size)) + " bytes as stored");
		if (mixed) {
			ImGui::TableNextColumn();
			ui_kit::clipped_text(source.install ? "game data" : basename_of(source.path), source.path);
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
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

// One row of the plan: its check (the files one converter source makes together), its name (indented
// to its depth in the tree; a chosen file that brings others with the arrow that opens them), kind,
// size, what wanted it and where it comes from. `also`: the row listed under another file that names it
// too (its line there an id of its own, the same check).
void ImportDialog::draw_row(const ImportPlan &plan, size_t index, size_t depth, bool tree, bool also) {
	const ImportPlanRow &row = plan.rows[index];
	// A dependency the project cannot take stays unchecked; a chosen one can only be
	// unchecked (Import waits while it is checked).
	const std::string &why_not = why_not_[index];
	const bool can = why_not.empty() || (row.state == State::Selected && checked_[index]);
	if (also) ImGui::PushID("also");
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
	// A chosen file's group: the arrow that opens what it brings.
	const size_t brings = tree && index < brings_.size() ? brings_[index] : Group::kNone;
	if (tree) {
		const bool open = brings != Group::kNone && open_[brings];
		if (tree_lead(depth, brings == Group::kNone ? nullptr : &open)) {
			open_[brings] = !open;
			laid_out_ = false;
		}
	}
	if (!row.problem.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::severity_color(DiagnosticSeverity::Error));
	ui_kit::clipped_text(row.name, row.name + "\n" + where);
	if (!row.problem.empty()) ImGui::PopStyleColor();
	ImGui::TableNextColumn();
	ui_kit::clipped_text(asset_kind_label(row.kind));
	ImGui::TableNextColumn();
	ui_kit::clipped_text(strutil::byte_size_text(row.size), strutil::grouped(size_t(row.size)) + " bytes as stored");
	ImGui::TableNextColumn();
	if (also) {
		const std::string words = "named here too; it comes with " + row.needed_by.file;
		ui_kit::clipped_text(words, "Listed under " + row.needed_by.file + ", which names it first: " +
		                                    import_need_text(row.needed_by) + ". Every file that names it: " +
		                                    joined(row.wanted_by) + ".");
	} else if (row.state == State::Found) {
		const std::string need = import_need_text(row.needed_by);
		const std::string others = row.wanted_by.size() > 1 ? "\nNamed by " + joined(row.wanted_by) + " too." : std::string();
		ui_kit::clipped_text(need, need + " names " + row.needed_by.name + others);
	} else if (row.held) {
		const auto [words, tip] = held_words(row);
		ui_kit::clipped_text(words, tip);
	} else if (brings != Group::kNone) {
		const std::string words = "chosen; it brings " + counted(groups_[brings].files - 1, "file");
		ui_kit::clipped_text(words, "One of the files chosen, with the files it needs under it.");
	} else {
		ui_kit::clipped_text("chosen", "One of the files chosen.");
	}
	ImGui::TableNextColumn();
	ui_kit::clipped_text(origin_words(row, true), origin_words(row, false));
	ImGui::PopID();
	if (also) ImGui::PopID();
}

// A kind's group: one check for every file in it and under it, the arrow that opens it, its kind and count,
// its size, and what names its files. Its check takes the files the project can take, never one the
// project has unless Replace existing files is on (each of those by its own check: the project's copy may
// hold the modder's edits), and leaves out every file of it, a blocked chosen one too, but one another
// checked file outside it names, which stays checked.
void ImportDialog::draw_group(const ImportPlan &plan, size_t g) {
	const Group &group = groups_[g];
	const std::set<size_t> span(order_.begin() + std::ptrdiff_t(spans_[g].first), order_.begin() + std::ptrdiff_t(spans_[g].second));
	// The branch's rows and those of the lines above it: a want of theirs keeps nothing of it.
	std::set<size_t> excluded = span;
	for (size_t up = group.parent; up != Group::kNone; up = groups_[up].parent)
		excluded.insert(groups_[up].rows.begin(), groups_[up].rows.end());
	size_t on = 0, can = 0, held = 0, kept = 0;
	for (const size_t i : span) {
		if (plan.rows[i].held && !replace_existing_) ++held;
		if (checked_[i] && needed_outside(plan, i, excluded)) ++kept;
		if (!takes_together(plan, i)) continue;
		++can;
		on += checked_[i] ? 1 : 0;
	}
	ImGui::PushID(static_cast<int>(plan.rows.size() + g));
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	const bool mixed = on > 0 && on < can;
	bool all = can > 0 && on == can;
	ImGui::BeginDisabled(can == 0 && on == 0);
	if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
	if (ImGui::Checkbox("##group", &all)) {
		if (all) {
			for (const size_t i : span)
				if (takes_together(plan, i)) checked_[i] = true;
		} else {
			// Decided over the checks as they stand, then applied: a file this uncheck leaves out never keeps
			// another of the same branch.
			std::vector<size_t> out;
			for (const size_t i : span)
				if (!needed_outside(plan, i, excluded)) out.push_back(i);
			for (const size_t i : out) checked_[i] = false;
		}
	}
	if (mixed) ImGui::PopItemFlag();
	ImGui::EndDisabled();
	std::string tip = can == 0 ? std::string("The project can take none of these files at once.")
	                           : "Check or uncheck every file of this group at once (" + counted(can, "file") + ").";
	if (held)
		tip += "\n" + counted(held, "file") + " the project has already " + (held == 1 ? "is" : "are") +
		       " kept: check each to write over it, or Replace existing files.";
	if (kept)
		tip += "\n" + counted(kept, "file") + (kept == 1 ? " stays" : " stay") + " checked on an uncheck: other checked "
		       "files name " + (kept == 1 ? "it." : "them.");
	ui_kit::tooltip(tip);
	ImGui::TableNextColumn();
	const bool open = open_[g];
	if (tree_lead(group.depth, &open)) {
		open_[g] = !open;
		laid_out_ = false;
	}
	// Its own files, and with those under it where they bring more ("Sound bank (1 file, 50 in all)"); those
	// another file brings first, listed here too.
	std::string label = std::string(asset_kind_label(group.kind)) + " (" + counted(group.rows.size(), "file");
	if (group.files > group.rows.size()) label += ", " + strutil::grouped(group.files) + " in all";
	if (!group.also.empty()) label += ", " + strutil::grouped(group.also.size()) + " more another file brings";
	ui_kit::clipped_text(label + ")");
	ImGui::TableNextColumn();
	ImGui::TableNextColumn();
	ui_kit::clipped_text(strutil::byte_size_text(group.bytes), strutil::grouped(size_t(group.bytes)) + " bytes as stored");
	ImGui::TableNextColumn();
	// What names its files: the chosen file, or the files of the group above.
	if (group.parent != Group::kNone) {
		const Group &above = groups_[group.parent];
		const std::string words = above.chosen() ? "what " + plan.rows[above.root].name + " names"
		                                         : "what the " + std::string(asset_kind_label(above.kind)) + " files above name";
		ui_kit::clipped_text(words);
	}
	ImGui::TableNextColumn();
	ImGui::PopID();
}

// "Include the files these need", the plan in short (its files and bytes, each kind a toggle that
// shows its rows alone, a filter over the names, Check shown and Uncheck shown), then the rows: by what
// they come for, or flat while a filter or a kind narrows them.
void ImportDialog::draw_plan(Workspace &workspace, const DialogsView::ImportPreview &preview) {
	const ImportPlan &plan = *preview.plan;
	size_t found = 0;
	std::vector<size_t> rows;
	const std::string wanted = normalized_logical_name(rows_filter_.text);
	const bool narrowed = !wanted.empty() || kind_shown_ != AssetKind::kCount;
	for (size_t i = 0; i < plan.rows.size(); ++i) {
		if (plan.rows[i].state == State::NotFound) continue;
		if (plan.rows[i].state == State::Found) ++found;
		if (kind_shown_ != AssetKind::kCount && plan.rows[i].kind != kind_shown_) continue;
		if (!wanted.empty() && normalized_logical_name(plan.rows[i].name).find(wanted) == std::string::npos) continue;
		rows.push_back(i);
	}
	// The check box's label cut to the dialog's width (whole in its tooltip).
	bool with = preview.with_dependencies;
	const std::string include = "Include the files these need" + (with ? " (" + strutil::grouped(found) + " found)" : std::string());
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
	// While its plan is being made, the dialog says so and how far it is (the operation's progress), never
	// "Nothing to import" over the empty plan it holds meanwhile.
	if (preview.planning) {
		const OperationStatus &operation = workspace.view().activity.operation;
		const std::string far = operation.running() && operation.total > 0
		                                ? strutil::grouped(size_t(operation.done)) + " of " + counted(size_t(operation.total), "file") + " looked at."
		                                : std::string();
		ui_kit::empty_state("Planning the import...", far.empty() ? nullptr : far.c_str());
		return;
	}
	if (plan.file_count() == 0) {
		if (preview.roots.empty()) ui_kit::empty_state("No file chosen.", "Choose the files to import above.");
		else ui_kit::empty_state("Nothing to import.", plan.diagnostics.empty() ? nullptr : "See why below.");
		return;
	}
	// The plan in short: its files and bytes, then each kind with its count and size, a toggle that
	// shows that kind's rows alone (the videos of a mission's closure unchecked in two clicks).
	ImGui::TextWrapped("%s", (counted(plan.file_count(), "file") + ", " + strutil::byte_size_text(plan.total_bytes()) + ":").c_str());
	ui_kit::WrapRow kinds;
	float kind_width = ui_kit::text_width("Kind"); // the Kind column as wide as the longest the plan has
	for (const ImportPlanKind &entry : plan.by_kind()) {
		kind_width = std::max(kind_width, ui_kit::text_width(asset_kind_label(entry.kind)));
		const std::string label = std::string(asset_kind_label(entry.kind)) + " " + strutil::grouped(entry.files) + " (" +
		                          strutil::byte_size_text(entry.bytes) + ")";
		const std::string id = label + "###kind_" + asset_kind_token(entry.kind);
		const float width = ui_kit::text_width(label.c_str()) + ImGui::GetStyle().FramePadding.x * 2.0f;
		kinds.next(width);
		const bool on = kind_shown_ == entry.kind;
		if (ImGui::Selectable(id.c_str(), on, ImGuiSelectableFlags_None, ImVec2(width, 0.0f)))
			kind_shown_ = on ? AssetKind::kCount : entry.kind;
		ui_kit::tooltip(on ? std::string("Every kind's rows again.")
		                   : "Only the rows of this kind, " + counted(entry.files, "file") + ".");
	}
	// A filter over the rows' names (Ctrl+F is the listing's filter's where one is drawn), and the
	// shown rows checked or unchecked together.
	ui_kit::WrapRow controls;
	const float filter_width = ImGui::GetFontSize() * 18.0f;
	controls.next(filter_width);
	ui_kit::filter_box("##rows_filter", rows_filter_.text, sizeof(rows_filter_.text), "Filter the rows", filter_width, nullptr,
	                   preview.choices.empty());
	// The rows the table shows: those the filter or the kind lists, or in the tree those of its open lines
	// (a closed group's rows are not shown, so these buttons leave them as they are).
	if (!narrowed && !laid_out_) lay_out();
	std::vector<size_t> shown_rows;
	if (narrowed) {
		shown_rows = rows;
	} else {
		for (const Line &line : lines_)
			if (!line.group) shown_rows.push_back(line.index);
	}
	if (ui_kit::tool(controls, "Check shown", !shown_rows.empty(),
	                 "Take every row the table shows that the project can take (a file the project has only with "
	                 "Replace existing files).")) {
		for (const size_t i : shown_rows)
			if (takes_together(plan, i)) checked_[i] = true;
	}
	if (ui_kit::tool(controls, "Uncheck shown", !shown_rows.empty(), "Leave every row the table shows out.")) {
		for (const size_t i : shown_rows) checked_[i] = false;
	}
	if (rows.empty()) {
		ui_kit::empty_state("No row matches.", "Clear the filter, or show every kind.");
		return;
	}
	// The table takes the height the notes under it leave.
	const float line = ImGui::GetFrameHeightWithSpacing();
	const float notes = static_cast<float>(note_lines(plan)) * ImGui::GetTextLineHeightWithSpacing();
	const float height = std::max(line * 8.0f, ImGui::GetContentRegionAvail().y - notes);
	if (!ImGui::BeginTable("import_plan", 6,
	                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
	                               ImGuiTableFlags_BordersInnerV,
	                       ImVec2(0, height)))
		return;
	ImGui::TableSetupColumn("##take", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
	                        ImGui::GetFrameHeight());
	ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, std::min(kind_width, ImGui::GetFontSize() * 12.0f));
	ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ui_kit::text_width("999.9 KB"));
	ImGui::TableSetupColumn("Needed by", ImGuiTableColumnFlags_WidthStretch, 3.0f);
	ImGui::TableSetupColumn("Found in", ImGuiTableColumnFlags_WidthStretch, 1.5f);
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableHeadersRow();
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(narrowed ? rows.size() : lines_.size()));
	while (clipper.Step()) for (int at = clipper.DisplayStart; at < clipper.DisplayEnd; ++at) {
		if (narrowed) {
			draw_row(plan, rows[static_cast<size_t>(at)], 0, false, false);
			continue;
		}
		const Line &shown_line = lines_[static_cast<size_t>(at)];
		if (shown_line.group) draw_group(plan, shown_line.index);
		else draw_row(plan, shown_line.index, shown_line.depth, true, shown_line.also);
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
	const std::string header = "Not found (" + strutil::grouped(missing.size()) + ")###not_found";
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
			const std::string need = import_need_text(row.needed_by);
			ui_kit::clipped_text(need, need + " names " + row.needed_by.name);
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
		ImGui::TextWrapped("The plan stopped at %s files: the files past them are not listed and not imported.",
		                   strutil::grouped(kImportPlanFileCap).c_str());
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
