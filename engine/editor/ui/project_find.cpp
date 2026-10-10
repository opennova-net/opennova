#include "project_find.h"

#include <editor/graph/display_names.h>
#include <editor/graph/jump_queries.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <cstring>

#include <base/io/strutil.h>

#include <imgui.h>
#include <imgui_internal.h>

namespace opennova::editor {
namespace {

using Scope = WorkspaceView::FindScope;

// The modal's id, one whatever scope it shows (its title says which).
constexpr const char *kId = "###project_find";
constexpr size_t kShownMax = 200; // the results listed; more ask for a longer text

const char *title_of(Scope scope) {
	switch (scope) {
	case Scope::All: return "Find in project";
	case Scope::Files: return "Go to file";
	case Scope::Names: return "Go to name";
	case Scope::Usages: return "Find usages";
	}
	return "Find in project";
}

const char *hint_of(Scope scope) {
	switch (scope) {
	case Scope::All: return "A file, a name the files define, or an item's name";
	case Scope::Files: return "A file's name: Enter opens the first";
	case Scope::Names: return "A name the files define: an item, a weapon, a string, a screen...";
	case Scope::Usages: return "Only the uses holding this";
	}
	return "";
}

SearchScope search_of(Scope scope) {
	return scope == Scope::Files ? SearchScope::Files : scope == Scope::Names ? SearchScope::Names : SearchScope::All;
}

// What a result is: a file, or a symbol's kind.
std::string what_of(const GraphSearchHit &hit) {
	if (!hit.symbol) return "file";
	return std::string(reference_row(hit.symbol->kind).label) + (hit.symbol->inert ? ", unreachable" : "");
}

// What the hits and their uses read of the view: the graph (its files and symbols, their uses,
// where each leads: the graph moves with the files' paths and kinds too).
RevisionKey cache_key(const SessionView &view) {
	return revision_key(view.revisions, {ViewConcern::Graph});
}

// Where a hit leads: the record defining the symbol, or the file.
ReferenceTarget hit_target(const SessionView &view, const GraphSearchHit &hit) {
	return hit.symbol ? symbol_target(*view.project.scan, *hit.symbol) : file_target(*view.project.scan, hit.file);
}

// The marked line kept in what lists `count` lines, moved by the arrows.
size_t moved_cursor(size_t cursor, size_t count) {
	if (count == 0) return 0;
	if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) cursor = std::min(cursor + 1, count - 1);
	if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) cursor = cursor == 0 ? 0 : cursor - 1;
	return std::min(cursor, count - 1);
}

} // namespace

std::vector<ProjectFind::Usage> ProjectFind::usage_lines(const SessionView &view, const std::vector<const GraphEdge *> &edges) {
	std::vector<Usage> out;
	out.reserve(edges.size());
	for (const GraphEdge *edge : edges) {
		Usage use;
		// Its place in words, its file open or not (the plain-words lane: a record by its type's words).
		const AssetEntry *source = view.project.scan->at_path(edge->source);
		const std::string place = edge_place_words(*edge, source ? source->kind : AssetKind::Unknown);
		use.line = edge->source + (place.empty() ? std::string() : ": " + place);
		use.tip = use.line + "\n" + edge->field + " = " + edge->value;
		use.target = usage_target(*view.project.scan, *edge);
		out.push_back(std::move(use));
	}
	return out;
}

const std::vector<ProjectFind::Usage> &ProjectFind::usages(const SessionView &view, size_t hit) {
	auto found = usages_.find(hit);
	if (found != usages_.end()) return found->second;
	const GraphSearchHit &result = hits_[hit];
	return usages_
	        .emplace(hit, usage_lines(view, result.symbol ? view.findings.graph->users_of(*result.symbol)
	                                                      : view.findings.graph->usages_of(result.file)))
	        .first->second;
}

void ProjectFind::open(Workspace &workspace, Scope scope) {
	io::JsonValue members = io::JsonValue::make_object();
	members.set("open", io::JsonValue::make_bool(true));
	members.set("scope", io::JsonValue::make_string(find_scope_token(scope)));
	window_requests::set_workspace(workspace, "project_find", std::move(members));
}

void ProjectFind::open_usages(Workspace &workspace, const std::string &path, const std::string &locator) {
	io::JsonValue members = io::JsonValue::make_object();
	members.set("open", io::JsonValue::make_bool(true));
	members.set("scope", io::JsonValue::make_string(find_scope_token(Scope::Usages)));
	members.set("path", io::JsonValue::make_string(path));
	members.set("locator", io::JsonValue::make_string(locator));
	window_requests::set_workspace(workspace, "project_find", std::move(members));
}

void ProjectFind::refresh(const SessionView &view, const std::string &text) {
	const WorkspaceView::ProjectFind &find = view.workspace.project_find;
	const bool subject = find.scope != scope_ || find.path != path_ || find.locator != locator_;
	if (view_ == &view && key_ == cache_key(view) && searched_ == text && !subject) return;
	const bool moved = subject || view_ != &view || key_ != cache_key(view);
	view_ = &view;
	key_ = cache_key(view);
	if (searched_ != text || subject) cursor_ = 0;
	searched_ = text;
	scope_ = find.scope;
	path_ = find.path;
	locator_ = find.locator;
	usages_.clear();
	if (scope_ != Scope::Usages) {
		hits_ = search_project(*view.findings.graph, searched_, search_of(scope_));
		return;
	}
	hits_.clear();
	if (moved) {
		uses_ = usage_lines(view, usages_at(*view.findings.graph, path_, locator_));
		subject_ = usages_subject_words(*view.findings.graph, path_, locator_);
	}
	shown_uses_.clear();
	const std::string wanted = strutil::to_upper(searched_);
	for (size_t i = 0; i < uses_.size(); ++i)
		if (wanted.empty() || strutil::to_upper(uses_[i].line).find(wanted) != std::string::npos) shown_uses_.push_back(i);
}

void ProjectFind::draw(Workspace &workspace) {
	const SessionView &view = workspace.view();
	const WorkspaceView::ProjectFind &find = view.workspace.project_find;
	// A text the session put in its place (another scope or subject starts afresh; the wire's): the box, which
	// would keep what it holds while it has the keyboard, takes it.
	const bool replaced = find.text != text_.seen && find.text != text_.sent();
	text_.follow(find.text);
	const auto close = [&] {
		window_requests::set_workspace(workspace, "project_find", "open", io::JsonValue::make_bool(false));
	};
	const float em = ImGui::GetFontSize();
	ImGui::SetNextWindowSize(ImVec2(em * 40.0f, em * 30.0f), ImGuiCond_Appearing);
	// Held open, it shows when no dialog before it in the session's order is held (shown_modal).
	const bool held = find.open && view.project.open && view.findings.graph && modal_may_show(view, HeldModal::ProjectFind);
	const std::string title = std::string(title_of(find.scope)) + kId;
	if (!popup_.begin(title.c_str(), held, true, 0, true, view.workspace.opened)) {
		if (popup_.dismissed()) close();
		return;
	}
	if (replaced && ImGui::GetActiveID() == ImGui::GetID("##text"))
		if (ImGuiInputTextState *state = ImGui::GetInputTextState(ImGui::GetID("##text"))) state->ReloadUserBufAndSelectAll();
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	// Escape closes the modal, the text typed kept: the text box, which has the keyboard, would
	// put back the text it had when it took it.
	const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	const bool enter = !escape && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
	char typed[sizeof(text_.text)];
	std::memcpy(typed, text_.text, sizeof(typed));
	ImGui::SetNextItemWidth(-FLT_MIN);
	const bool edited = ImGui::InputTextWithHint("##text", hint_of(find.scope), text_.text, sizeof(text_.text),
	                                             ImGuiInputTextFlags_AutoSelectAll);
	ui_kit::tooltip("Up and Down move through the list; Enter goes to the one marked. Ctrl+P lists the files alone, "
	                "Ctrl+T the names alone, Ctrl+Shift+F both.");
	// Enter takes the text box's keyboard away as it goes: kept there while nothing is gone to.
	if (enter) ImGui::SetKeyboardFocusHere(-1);
	if (escape) {
		std::memcpy(text_.text, typed, sizeof(typed));
		close();
		popup_.close();
	} else if (edited) {
		window_requests::set_workspace(workspace, "project_find", "text", io::JsonValue::make_string(text_.sent()));
	}
	refresh(view, text_.sent());
	// Somewhere to go: the modal closes as it goes.
	bool gone = false;
	const auto go = [&](const ReferenceTarget &target) {
		if (gone) return;
		gone = true;
		window_requests::go_to(workspace, target);
		close();
		popup_.close();
	};
	const bool moved = !escape && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow));
	if (find.scope == Scope::Usages) draw_uses(go, enter, moved);
	else draw_hits(view, go, enter, moved);
	if (ImGui::Button("Close")) {
		close();
		popup_.close();
	}
	ImGui::EndPopup();
}

void ProjectFind::draw_hits(const SessionView &view, const std::function<void(const ReferenceTarget &)> &go, bool enter,
                            bool moved) {
	const std::string &text = searched_;
	const size_t shown = std::min(hits_.size(), kShownMax);
	cursor_ = moved_cursor(cursor_, shown);
	if (text.empty())
		ui_kit::empty_state(scope_ == Scope::Files   ? "Type part of a file's name."
		                    : scope_ == Scope::Names ? "Type part of a name a file defines (an item's, a weapon's, a string's)."
		                                             : "Type part of a file's name, of a name a file defines, or of the record "
		                                               "that names a file (an item's name finds its model).");
	else if (hits_.empty())
		ui_kit::empty_state(scope_ == Scope::Files ? "No file's name holds it." : scope_ == Scope::Names ? "No name holds it."
		                                                                                                : "No file or name holds it.");
	else if (hits_.size() > kShownMax)
		ImGui::TextDisabled("%zu results; the first %zu listed. Type more to narrow them.", hits_.size(), kShownMax);
	ImGui::BeginChild("results", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
	const bool tree = scope_ == Scope::All;
	for (size_t i = 0; i < shown; ++i) {
		const GraphSearchHit &hit = hits_[i];
		ImGui::PushID(static_cast<int>(i));
		const bool marked = i == cursor_;
		// The result: its name, what it is and where, its uses; opened (Find in project's), the uses.
		const std::string uses = std::to_string(hit.usages) + (hit.usages == 1 ? " use" : " uses");
		// An item by its catalog's name first, its id after it; a file found by what names it says so:
		// "Dblkhwk1.3di, used by Flyable Blackhawk" (S17).
		const std::string named = hit.words.empty() ? hit.name : hit.words + " " + hit.name;
		const std::string by = hit.via.empty() ? std::string() : ", used by " + hit.via;
		// Where it is: Find in project's what and file; Go to name's kind and file; Go to file's folder (its path where
		// its name is not it).
		const std::string where = tree || hit.symbol ? what_of(hit) + ", " + hit.file : hit.file == hit.name ? std::string() : hit.file;
		const std::string title = named + by + (where.empty() ? std::string() : "  (" + where + ")") + "  " + uses;
		std::string tip = named + "\n" + what_of(hit) + " in " + hit.file + "\n" + uses;
		if (!hit.via.empty()) tip += "\nFound by " + hit.via + " (" + hit.via_file + "), which names it.";
		if (hit.symbol && hit.symbol->inert) tip += "\nNo lookup of the game finds it: " + hit.symbol->inert_reason + ".";
		if (marked && moved) ImGui::SetScrollHereY(0.5f);
		if (marked && enter) go(hit_target(view, hit));
		if (!tree) {
			// Go to file and Go to name: a line a click goes to.
			if (ImGui::Selectable((ui_kit::fit(title, ImGui::GetContentRegionAvail().x) + "###hit").c_str(), marked))
				go(hit_target(view, hit));
			ui_kit::tooltip(tip + "\nA click, or Enter while it is marked, goes there.");
			ImGui::PopID();
			continue;
		}
		const float go_width = ui_kit::button_width("Go to") + ImGui::GetStyle().ItemSpacing.x;
		const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		ImGuiTreeNodeFlags flags = hit.usages ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_Leaf;
		if (marked) flags |= ImGuiTreeNodeFlags_Selected;
		const bool expanded = ImGui::TreeNodeEx("result", flags, "%s",
		                                        ui_kit::fit(title, ImGui::GetContentRegionAvail().x - go_width -
		                                                                   ImGui::GetTreeNodeToLabelSpacing())
		                                                .c_str());
		ui_kit::tooltip(tip);
		ImGui::SameLine(right - ui_kit::button_width("Go to"));
		if (ImGui::SmallButton("Go to")) go(hit_target(view, hit));
		ui_kit::tooltip(hit.symbol ? "Open the record that defines it (Enter while it is marked)."
		                           : "Open the file, or its page (Enter while it is marked).");
		if (expanded) {
			// Its uses, made once while the hits stand, the rows out of sight not drawn.
			const std::vector<Usage> &uses_of = usages(view, i);
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(uses_of.size()));
			while (clipper.Step())
				for (int u = clipper.DisplayStart; u < clipper.DisplayEnd; ++u) {
					const Usage &use = uses_of[size_t(u)];
					ImGui::PushID(u);
					if (ImGui::Selectable((ui_kit::fit(use.line, ImGui::GetContentRegionAvail().x) + "###use").c_str()))
						go(use.target);
					ui_kit::tooltip(use.tip);
					ImGui::PopID();
				}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
}

void ProjectFind::draw_uses(const std::function<void(const ReferenceTarget &)> &go, bool enter, bool moved) {
	cursor_ = moved_cursor(cursor_, shown_uses_.size());
	const std::string count = shown_uses_.size() == uses_.size()
	                                  ? std::to_string(uses_.size()) + (uses_.size() == 1 ? " use" : " uses")
	                                  : std::to_string(shown_uses_.size()) + " of " + std::to_string(uses_.size()) + " uses";
	ImGui::TextWrapped("%s: %s", subject_.c_str(), count.c_str());
	ui_kit::tooltip("Who names it in the project's files (what a record defines: each name a lookup of the game finds), "
	                "each a Go to.");
	if (uses_.empty()) ui_kit::empty_state("No file of the project names it.");
	else if (shown_uses_.empty()) ui_kit::empty_state("No use holds the text.");
	ImGui::BeginChild("uses", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(shown_uses_.size()));
	if (moved) clipper.IncludeItemByIndex(static_cast<int>(cursor_));
	while (clipper.Step())
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			const Usage &use = uses_[shown_uses_[size_t(i)]];
			const bool marked = size_t(i) == cursor_;
			ImGui::PushID(i);
			if (marked && moved) ImGui::SetScrollHereY(0.5f);
			if (ImGui::Selectable((ui_kit::fit(use.line, ImGui::GetContentRegionAvail().x) + "###use").c_str(), marked))
				go(use.target);
			ui_kit::tooltip(use.tip + "\n" + window_requests::go_to_words(use.target));
			ImGui::PopID();
		}
	ImGui::EndChild();
	if (enter && cursor_ < shown_uses_.size()) go(uses_[shown_uses_[cursor_]].target);
}

} // namespace opennova::editor
