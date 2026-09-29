#include <editor/ui/problems_window.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// The modal a Use fix and a Fix all wait in for Apply.
constexpr const char *kConfirm = "Apply fixes";

template <class T> struct Choice {
	const char *label;
	T value;
};
constexpr Choice<ProblemScope> kScopes[] = {{"Project", ProblemScope::Project},
	{"Active file", ProblemScope::ActiveFile}, {"Open files", ProblemScope::OpenFiles}};
constexpr Choice<ProblemGrouping> kGroupings[] = {
	{"None", ProblemGrouping::None}, {"File", ProblemGrouping::File}, {"Kind", ProblemGrouping::Kind}};

// A severity's toggle: its word and how many findings of it the project has; off, dimmed.
void severity_toggle(ui_kit::WrapRow &row, const char *word, const char *id, size_t count, const char *plural, bool &on) {
	const std::string label = std::string(word) + " " + std::to_string(count) + id;
	row.next(ui_kit::button_width(label.c_str()));
	const bool shown = on;
	if (!shown) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	}
	if (ImGui::Button(label.c_str())) on = !on;
	if (!shown) ImGui::PopStyleColor(2);
	ui_kit::tooltip(std::string(shown ? "Hide " : "Show ") + plural + ".");
}

// A choice among a few as a combo as wide as the longest, its label on its right.
template <class T, size_t N>
void choice_combo(ui_kit::WrapRow &row, const char *label, const Choice<T> (&choices)[N], T &value, const char *tip) {
	float widest = 0.0f;
	const char *current = choices[0].label;
	for (const Choice<T> &choice : choices) {
		widest = std::max(widest, ImGui::CalcTextSize(choice.label).x);
		if (choice.value == value) current = choice.label;
	}
	const float width = widest + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
	row.next(ui_kit::field_width(width, label));
	ImGui::SetNextItemWidth(width);
	const bool open = ImGui::BeginCombo(label, current);
	if (!open) return ui_kit::tooltip(tip);
	for (const Choice<T> &choice : choices)
		if (ImGui::Selectable(choice.label, choice.value == value)) value = choice.value;
	ImGui::EndCombo();
}

void disabled_wrapped(const std::string &text) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
}

float line_height() { return ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2.0f; }

} // namespace

void ProblemsWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &view = host_.view();
	if (view.project_open) list_.refresh(view);
	if (!view.project_open) {
		ui_kit::empty_state("No project open.");
	} else if (view.diagnostics.empty()) {
		ui_kit::empty_state("No problems.");
	} else {
		const ProblemAnswer &answer = draw_filters(view);
		draw_summary(view);
		draw_list(view, answer);
	}
	draw_more(view);
	draw_confirm(view);
}

// The filter bar, wrapping whole controls in a narrow dock; the answer to the query it sets.
const ProblemAnswer &ProblemsWindow::draw_filters(const SessionView &view) {
	const ProblemAnswer &counts = list_.answer(view); // every finding's, whatever it shows
	ProblemQuery &query = list_.query();
	ui_kit::WrapRow row;
	severity_toggle(row, "Errors", "###errors", counts.errors, "errors", query.errors);
	severity_toggle(row, "Warnings", "###warnings", counts.warnings, "warnings", query.warnings);
	severity_toggle(row, "Info", "###infos", counts.infos, "info", query.infos);
	const float filter = ImGui::GetFontSize() * 13.0f;
	row.next(filter);
	ui_kit::filter_box("##filter", text_, sizeof(text_), "Filter", filter,
	                   "Only the problems whose message, file, record, field or code has this text.");
	query.text = text_;
	choice_combo(row, "Scope", kScopes, query.scope,
	             "Which files' problems: the project's, the active file's, the open files'.");
	choice_combo(row, "Group", kGroupings, query.grouping,
	             "Group the problems by file or by kind, or list them as one.");
	row.next(ui_kit::checkbox_width("Only fixable"));
	ImGui::Checkbox("Only fixable", &query.fixable);
	ui_kit::tooltip("Only the problems the editor offers a fix for.");
	const ProblemAnswer &answer = list_.refresh(view);
	const std::string shown = std::to_string(answer.rows.size()) + " of " + std::to_string(answer.total());
	row.next(ui_kit::text_width(shown.c_str()));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", shown.c_str());
	return answer;
}

// While required files are missing: the game cannot start, and the Fix alls that make them,
// each asking first.
void ProblemsWindow::draw_summary(const SessionView &view) {
	const std::string sentence = ProblemsList::summary(view.requirements);
	if (sentence.empty()) return;
	ui_kit::WrapRow row;
	row.next(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x + ui_kit::text_width(sentence.c_str()));
	ui_kit::severity_marker(DiagnosticSeverity::Error);
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextWrapped("%s", sentence.c_str());
	ImGui::PushID(view.project_root.c_str());
	ImGui::PushID("required");
	for (const EditorRequest &request : list_.required_fixes().requests) {
		const std::string label = ProblemsList::fix_all_label(request);
		row.next(ui_kit::button_width(label.c_str()));
		ImGui::PushID(static_cast<int>(request.kind));
		if (ui_kit::fitted_button(label, "fix", ImGui::GetContentRegionAvail().x))
			ask(view, list_.required_fix(view, request.kind));
		ui_kit::tooltip_lazy([&] { return ProblemsList::describe(view, request); });
		ImGui::PopID();
	}
	ImGui::PopID();
	ImGui::PopID();
}

void ProblemsWindow::draw_list(const SessionView &view, const ProblemAnswer &answer) {
	if (answer.rows.empty()) {
		ui_kit::empty_state("No problem matches the filters.", "Show a severity again, clear the text or widen the scope.");
		return;
	}
	const ImGuiTableFlags flags =
	        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("problems", 4, flags)) return;
	ImGui::TableSetupColumn("##severity", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
	ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch, 4.0f);
	ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableSetupColumn("Fix", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	// The project's own ids: a press in one project never lands in the next.
	ImGui::PushID(view.project_root.c_str());
	// The selected finding's line is drawn whole (its message wrapped, every fix); the lines
	// either side of it, all as high as a control, only where they show.
	const std::vector<Line> &lines = list_.lines();
	size_t split = lines.size();
	for (size_t i = 0; i < lines.size(); ++i)
		if (!lines[i].header && lines[i].finding == list_.selected()) split = i;
	draw_lines(view, answer, 0, split);
	if (split < lines.size()) {
		draw_finding(view, lines[split], true);
		draw_lines(view, answer, split + 1, lines.size());
	}
	ImGui::PopID();
	ImGui::EndTable();
}

void ProblemsWindow::draw_lines(const SessionView &view, const ProblemAnswer &answer, size_t first, size_t last) {
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(last - first), line_height());
	while (clipper.Step())
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			const Line &line = list_.lines()[first + size_t(i)];
			if (line.header) draw_header(view, answer, line);
			else if (line.finding < view.diagnostics.size()) draw_finding(view, line, false);
		}
}

// A group's header: folds it away, its title and counts, and its Fix all.
void ProblemsWindow::draw_header(const SessionView &view, const ProblemAnswer &answer, const Line &line) {
	const ProblemGroup &group = answer.groups[line.group];
	ImGui::PushID(("group:" + group.key).c_str());
	ImGui::TableNextRow(ImGuiTableRowFlags_None, line_height());
	ImGui::TableSetColumnIndex(1);
	const bool folded = list_.folded(group.key);
	const ImGuiTreeNodeFlags node = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_AllowOverlap |
	                                ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding |
	                                ImGuiTreeNodeFlags_NoAutoOpenOnLog;
	ImGui::SetNextItemOpen(!folded, ImGuiCond_Always);
	if (ImGui::TreeNodeEx("##group", node) == folded) list_.toggle_fold(group.key); // clicked
	ImGui::SameLine();
	const std::string counts =
	        ProblemsList::severity_counts(group.errors, group.warnings, group.infos);
	ui_kit::clipped_text(group.title + " (" + counts + ")");
	ImGui::TableSetColumnIndex(3);
	const ProblemsList::Proposal &all = list_.group_fixes(line.group);
	if (all.findings >= 2 && !all.requests.empty()) {
		if (ui_kit::fitted_button("Fix all", "fix_all", ImGui::GetContentRegionAvail().x))
			ask(view, list_.fix_all_of(view, group.rows));
		ui_kit::tooltip_lazy([&] {
			std::string tip;
			for (const std::string &text : all.lines) tip += (tip.empty() ? "" : "\n") + text;
			return tip;
		});
	}
	ImGui::PopID();
}

// A finding's line: its severity, its message, where it is and its fixes. Expanded (the
// selected one), the whole message wrapped, every fix with what it does, and its code.
void ProblemsWindow::draw_finding(const SessionView &view, const Line &line, bool expanded) {
	const Diagnostic &d = view.diagnostics[line.finding];
	ImGui::PushID(list_.key(line.finding).c_str());
	ImGui::TableNextRow(ImGuiTableRowFlags_None, line_height());
	if (expanded) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
	ImGui::TableNextColumn();
	// The line itself: a click selects the finding (the selected one folds back) and goes to
	// its place, when it has one.
	if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
	                      ImVec2(0.0f, ImGui::GetFrameHeight()))) {
		list_.toggle_selected(view, line.finding);
		const ProblemLocation location = problem_location(d, view);
		if (!location.empty()) host_.request(location.request());
	}
	ImGui::SameLine(0.0f, 0.0f);
	ui_kit::severity_marker(d.severity);
	const std::vector<ProblemFix> &fixes = list_.fixes(view, line.finding);
	ImGui::TableNextColumn();
	ImGui::AlignTextToFramePadding();
	if (expanded) {
		ImGui::TextWrapped("%s", d.message.c_str());
		for (const ProblemFix &fix : fixes) {
			ImGui::PushID(fix.label.c_str());
			const float room = ImGui::GetContentRegionAvail().x;
			if (fix_pressed(view, line.finding, fix, ui_kit::fitted_button(fix.label, "fix", room)))
				apply(view, line.finding, fix);
			ui_kit::tooltip_lazy([&] { return fix.label + "\n\n" + fix.detail; });
			// What it does beside it when there is room for a few words, else under it.
			if (room - ImGui::GetItemRectSize().x - ImGui::GetStyle().ItemSpacing.x >= ImGui::GetFontSize() * 12.0f)
				ImGui::SameLine();
			disabled_wrapped(fix.detail);
			ImGui::PopID();
		}
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ui_kit::clipped_text(d.code);
		ImGui::PopStyleColor();
	} else {
		ui_kit::clipped_text(d.message, d.code.empty() ? std::string() : d.message + "\n\n" + d.code);
	}
	ImGui::TableNextColumn();
	ImGui::AlignTextToFramePadding();
	const std::string where = ProblemsList::location_of(d, false);
	const std::string whole = ProblemsList::location_of(d, true);
	ui_kit::clipped_text(where, whole != where ? whole : std::string());
	ImGui::TableNextColumn();
	if (!expanded && !fixes.empty()) draw_fixes(view, line.finding, fixes);
	ImGui::PopID();
}

// The Fix column: the first fix and More (every fix) when both fit with the fix's words
// legible, else one Fix... that lists every fix; each no wider than the column.
void ProblemsWindow::draw_fixes(const SessionView &view, size_t finding, const std::vector<ProblemFix> &fixes) {
	const ImGuiStyle &style = ImGui::GetStyle();
	const float room = ImGui::GetContentRegionAvail().x;
	const float more = fixes.size() > 1 ? ui_kit::button_width("More") + style.ItemSpacing.x : 0.0f;
	bool open = false;
	if (room - more - style.FramePadding.x * 2.0f >= ImGui::GetFontSize() * 4.0f) {
		const ProblemFix &first = fixes.front();
		const bool clicked = ui_kit::fitted_button(first.label, "fix", room - more);
		if (fix_pressed(view, finding, first, clicked)) apply(view, finding, first);
		ui_kit::tooltip_lazy([&] { return first.label + "\n\n" + first.detail; });
		if (fixes.size() == 1) return;
		ImGui::SameLine();
		open = ImGui::Button("More");
	} else {
		open = ui_kit::fitted_button("Fix...", "fixes", room);
	}
	ui_kit::tooltip("Every fix for this problem");
	if (!open) return;
	more_ = list_.ref(view, finding);
	open_more_ = true;
}

// Every fix of the finding More was pressed on, found again by its key: gone (or in another
// project), the list closes.
void ProblemsWindow::draw_more(const SessionView &view) {
	if (open_more_) {
		ImGui::OpenPopup("more");
		open_more_ = false;
	}
	if (!ImGui::BeginPopup("more")) return;
	const size_t finding = list_.resolve(view, more_);
	if (finding == SIZE_MAX) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
	for (const ProblemFix &fix : list_.fixes(view, finding)) {
		if (fix_pressed(view, finding, fix, ImGui::Selectable(fix.label.c_str())))
			apply(view, finding, fix);
		ImGui::Indent();
		ImGui::TextDisabled("%s", fix.detail.c_str());
		ImGui::Unindent();
	}
	ImGui::PopTextWrapPos();
	ImGui::EndPopup();
}

// What a Use fix or a Fix all will do (the list's confirmation, which follows the project it
// was asked in and the findings it is for), raised on Apply; Cancel raises nothing.
void ProblemsWindow::draw_confirm(const SessionView &view) {
	if (open_confirm_) {
		ImGui::OpenPopup(kConfirm);
		open_confirm_ = false;
	}
	if (!ImGui::BeginPopupModal(kConfirm, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	if (!list_.follow(view)) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	const ProblemsList::Proposal &shown = list_.shown();
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
	if (shown.requests.empty())
		ImGui::TextUnformatted("Nothing is left to do: the problems it was for are gone.");
	for (const std::string &line : shown.lines) ImGui::TextUnformatted(line.c_str());
	ImGui::PopTextWrapPos();
	ImGui::BeginDisabled(shown.requests.empty());
	const bool apply = ImGui::Button("Apply");
	// A mouse click counts only when pressed on what shows now; a key's, in one frame, does.
	const bool applies = apply_press_.released_on(std::to_string(list_.version()),
	                                              ImGui::IsItemActivated(), apply,
	                                              ImGui::IsMouseReleased(ImGuiMouseButton_Left));
	ImGui::EndDisabled();
	if (applies) {
		for (const EditorRequest &request : shown.requests) host_.request(request);
		list_.close();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		list_.close();
		ImGui::CloseCurrentPopup();
	}
	// Said beside the buttons, which then stay where they were pressed.
	if (list_.changed()) {
		ImGui::SameLine();
		ui_kit::severity_marker(DiagnosticSeverity::Warning);
		ImGui::SameLine();
		ImGui::TextUnformatted("Changed while open: this is what Apply does now.");
	}
	ImGui::EndPopup();
}

bool ProblemsWindow::fix_pressed(const SessionView &view, size_t finding, const ProblemFix &fix,
                                 bool clicked) {
	const bool pressed = ImGui::IsItemActivated();
	if (!pressed && !clicked) return false;
	return fix_press_.released_on(list_.fix_id(view, finding, fix), pressed, clicked,
	                              ImGui::IsMouseReleased(ImGuiMouseButton_Left));
}

void ProblemsWindow::apply(const SessionView &view, size_t finding, const ProblemFix &fix) {
	if (!ProblemsList::asks_first(fix)) return host_.request(fix.request);
	ask(view, list_.use_fix(view, finding, fix));
}

void ProblemsWindow::ask(const SessionView &view, ProblemsList::Confirmation confirmation) {
	list_.ask(view, std::move(confirmation));
	open_confirm_ = true;
}

} // namespace opennova::editor
