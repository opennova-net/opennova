#include <editor/ui/problems_window.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/requirements/requirement_words.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/texture_preview.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/welcome_view.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// The modal a Use fix and a Fix all wait in for Apply.
constexpr const char *kConfirm = "Apply fixes";

// Whether the busy gate takes every request of `requests` now (SessionView::allows): a fix, a Fix
// all and the confirmation's Apply are enabled by it (a build packing the files refuses the fixes
// that write them).
bool allows_all(const SessionView &view, const std::vector<EditorRequest> &requests) {
	return std::all_of(requests.begin(), requests.end(),
	                   [&view](const EditorRequest &request) { return view.allows(request.kind); });
}

// Said on a fix the running operation holds back.
constexpr const char *kWaits = "It waits for the running operation.";

template <class T> struct Choice {
	const char *label;
	T value;
};
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

// What a fix of a finding about the game's own data says first (S15): the game as it ships has the
// problem too (its install, validated as a whole, makes the same finding in the file of that name), and a
// fix makes the file the modder's. "" for any other finding.
std::string shipped_note(size_t row, const SessionView &view) {
	return in_original_data(row, view) ? "Edits a file the game ships: the game as it ships has this problem too, and "
	                                     "the file becomes yours."
	                                   : std::string();
}
// A fix's tooltip: its label, that note, what it does, whether it waits.
std::string fix_tip(const ProblemFix &fix, const std::string &note, bool allowed) {
	return fix.label + "\n\n" + (note.empty() ? std::string() : note + "\n\n") + fix.detail +
	       (allowed ? "" : std::string("\n") + kWaits);
}

} // namespace

void ProblemsWindow::show_blocking() {
	set_blocking(true);
	request_focus();
}

void ProblemsWindow::set_blocking(bool on) {
	ProblemQuery &query = list_.query();
	if (on == query.blocking) return;
	if (on) {
		// Every refusal shown: a required file's finding names no file (no scope but the project's shows it),
		// and a hidden severity, a text or Only fixable could hide the rest.
		before_blocking_ = query;
		text_before_ = text_;
		query.scope = ProblemScope::Project;
		query.errors = query.warnings = query.infos = true;
		query.fixable = false;
		query.text.clear();
		text_[0] = '\0';
		query.blocking = true;
		return;
	}
	if (before_blocking_) {
		query = *before_blocking_;
		const size_t n = std::min(text_before_.size(), sizeof(text_) - 1);
		text_before_.copy(text_, n);
		text_[n] = '\0';
		before_blocking_.reset();
	}
	query.blocking = false;
}

bool ProblemsWindow::stands_aside() const { return aside_for_welcome(workspace_.view(), welcome_asked_); }

void ProblemsWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &view = workspace_.view();
	if (view.project.open) list_.refresh(view);
	if (!view.project.open) {
		ui_kit::empty_state("No project open.");
	} else if (view.findings.diagnostics.empty()) {
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
	// Your project's own counts (S15: the game's own data's are counted apart, under their group).
	severity_toggle(row, "Errors", "###errors", counts.errors, "errors", query.errors);
	severity_toggle(row, "Warnings", "###warnings", counts.warnings, "warnings", query.warnings);
	severity_toggle(row, "Info", "###infos", counts.infos, "info", query.infos);
	// What a build is refused for (the gate's rows), one click: shown while any is, or while it is on.
	if (counts.blocking || query.blocking) {
		const std::string label = "Blocks the build " + std::to_string(counts.blocking) + "###blocking";
		row.next(ui_kit::button_width(label.c_str()));
		const bool on = query.blocking;
		if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::severity_color(DiagnosticSeverity::Error));
		if (ImGui::Button(label.c_str())) set_blocking(!on);
		ImGui::PopStyleColor(on ? 2 : 1);
		ui_kit::tooltip(on ? std::string("Showing only what a build is refused for, every one: click for the problems as "
		                                 "they were filtered before.")
		                   : std::string("Only what a build is refused for: what would stop the game as it stops the "
		                                 "original, and what the editor cannot pack as it is."));
	}
	const float filter = ImGui::GetFontSize() * 13.0f;
	row.next(filter);
	ui_kit::filter_box("##filter", text_, sizeof(text_), "Filter", filter,
	                   "Only the problems whose message, file, record, field or code has this text.");
	query.text = text_;
	// Whose problems, one control (the UX round's problems lane: a Scope combo beside an "Only <file>"
	// button read like two states): the whole project's, the active file's (by its name, one click: S15),
	// the open files'; the lit one is the scope.
	struct Scope {
		std::string label;
		ProblemScope scope;
		std::string tip;
	};
	std::vector<Scope> scopes = {{"Whole project###scope_project", ProblemScope::Project, "Every problem of the project."}};
	if (!view.documents.active.empty())
		scopes.push_back({basename_of(view.documents.active) + "###scope_active", ProblemScope::ActiveFile,
		                  "Only the problems of " + view.documents.active + ", the active file."});
	scopes.push_back({"Open files###scope_open", ProblemScope::OpenFiles, "Only the problems of the files open in Document."});
	if (query.scope == ProblemScope::ActiveFile && view.documents.active.empty()) query.scope = ProblemScope::Project;
	float scopes_width = 0.0f;
	for (const Scope &scope : scopes) scopes_width += ui_kit::button_width(scope.label.c_str()) + 1.0f;
	row.next(scopes_width);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, ImGui::GetStyle().ItemSpacing.y));
	for (size_t i = 0; i < scopes.size(); ++i) {
		const bool lit = query.scope == scopes[i].scope;
		if (i) ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(lit ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
		if (ImGui::Button(scopes[i].label.c_str())) query.scope = scopes[i].scope;
		ImGui::PopStyleColor();
		ui_kit::tooltip(scopes[i].tip);
	}
	ImGui::PopStyleVar();
	choice_combo(row, "Group", kGroupings, query.grouping,
	             "Group the problems by file or by kind, or list them as one.");
	row.next(ui_kit::checkbox_width("Only fixable"));
	ImGui::Checkbox("Only fixable", &query.fixable);
	ui_kit::tooltip("Only the problems the editor offers a fix for.");
	const ProblemAnswer &answer = list_.refresh(view);
	// The severities count the modder's findings; the game's own data's are said after the shown count.
	std::string shown = std::to_string(answer.rows.size()) + " of " + std::to_string(answer.total());
	if (answer.original()) shown += " (" + std::to_string(answer.original()) + " in the game's own data)";
	row.next(ui_kit::text_width(shown.c_str()));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", shown.c_str());
	return answer;
}

// While required files are missing: what stops the game (the gate's rows: a build is refused for them)
// and what it starts without, then the Fix alls that bring them, each asking first (the game's own copies
// where the install has them, placeholders for the rest).
void ProblemsWindow::draw_summary(const SessionView &view) {
	const ProblemsList::Summary summary = ProblemsList::summary(*view.project.requirements);
	if (summary.empty()) return;
	ui_kit::WrapRow row;
	for (const auto &[sentence, severity] : {std::make_pair(summary.stops, DiagnosticSeverity::Error),
	                                         std::make_pair(summary.more, DiagnosticSeverity::Warning)}) {
		if (sentence.empty()) continue;
		row.next(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x + ui_kit::text_width(sentence.c_str()));
		ui_kit::severity_marker(severity);
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextWrapped("%s", sentence.c_str());
	}
	ImGui::PushID(view.project.root.c_str());
	ImGui::PushID("required");
	// The game's own copies offered before the placeholders (each button raises its own request).
	std::vector<EditorRequest> offered = list_.required_fixes().requests;
	std::stable_partition(offered.begin(), offered.end(),
	                      [](const EditorRequest &request) { return request.kind == EditorRequestKind::PreviewInstallImport; });
	for (const EditorRequest &request : offered) {
		const std::string label = ProblemsList::fix_all_label(request);
		row.next(ui_kit::button_width(label.c_str()));
		ImGui::PushID(static_cast<int>(request.kind));
		const bool allowed = view.allows(request.kind);
		ImGui::BeginDisabled(!allowed);
		if (ui_kit::fitted_button(label, "fix", ImGui::GetContentRegionAvail().x) && allowed)
			ask(view, list_.required_fix(view, request.kind));
		ImGui::EndDisabled();
		ui_kit::tooltip_lazy([&] { return ProblemsList::describe(view, request) + (allowed ? "" : std::string("\n") + kWaits); });
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
	ImGui::PushID(view.project.root.c_str());
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
			else if (line.finding < view.findings.diagnostics.size())
				draw_finding(view, line, false);
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
	if (group.original)
		ui_kit::tooltip("Problems the game has as it ships: its install, checked as a whole, has each of them too, in "
		                "the file of the same name, so they are not yours to fix. A problem your edits bring, or your "
		                "project's other files bring (a texture the install has that your project lacks), is never in "
		                "it, nor one that would stop a build.");
	ImGui::TableSetColumnIndex(3);
	const ProblemsList::Proposal &all = list_.group_fixes(line.group);
	if (all.findings >= 2 && !all.requests.empty()) {
		const bool allowed = allows_all(view, all.requests);
		ImGui::BeginDisabled(!allowed);
		if (ui_kit::fitted_button("Fix all", "fix_all", ImGui::GetContentRegionAvail().x) && allowed)
			ask(view, list_.fix_all_of(view, group.rows));
		ImGui::EndDisabled();
		ui_kit::tooltip_lazy([&] {
			std::string tip;
			for (const std::string &text : all.lines) tip += (tip.empty() ? "" : "\n") + text;
			return allowed ? tip : tip + "\n" + kWaits;
		});
	}
	ImGui::PopID();
}

// A finding's line: its severity, its message, where it is and its fixes. Expanded (the
// selected one), the whole message wrapped, every fix with what it does, and its code.
void ProblemsWindow::draw_finding(const SessionView &view, const Line &line, bool expanded) {
	const Diagnostic &d = view.findings.diagnostics[line.finding];
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
		if (!location.empty()) workspace_.request(location.request());
	}
	ImGui::SameLine(0.0f, 0.0f);
	ui_kit::severity_marker(d.severity);
	const std::vector<ProblemFix> &fixes = list_.fixes(view, line.finding);
	ImGui::TableNextColumn();
	ImGui::AlignTextToFramePadding();
	const std::string note = fixes.empty() ? std::string() : shipped_note(line.finding, view);
	// A row a build is refused for says so first, why in its tooltip (the refusal it follows, cited).
	const bool blocks = blocks_the_build(line.finding, view);
	if (blocks) {
		ImGui::TextColored(ui_kit::severity_color(DiagnosticSeverity::Error), "Blocks the build");
		ui_kit::tooltip_lazy([&] { return blocker_reason(d); });
		ImGui::SameLine();
	}
	if (expanded) {
		ImGui::TextWrapped("%s", d.message.c_str());
		if (blocks) disabled_wrapped(blocker_reason(d));
		// A file the game reads by name: the manifest's own record of it, the plain words' cited detail.
		if (const RequirementSubject *requirement = requirement_subject(d))
			if (const std::string witness = requirement_witness(requirement->role); !witness.empty())
				disabled_wrapped("As the original was seen to do: " + witness);
		// A fix of the game's own data is marked as editing a file the game ships (S15).
		if (!note.empty()) disabled_wrapped(note);
		for (const ProblemFix &fix : fixes) {
			ImGui::PushID(fix.label.c_str());
			const float room = ImGui::GetContentRegionAvail().x;
			const bool allowed = view.allows(fix.request.kind);
			ImGui::BeginDisabled(!allowed);
			if (fix_pressed(view, line.finding, fix, ui_kit::fitted_button(fix.label, "fix", room)) && allowed)
				apply(view, line.finding, fix);
			ImGui::EndDisabled();
			ui_kit::tooltip_lazy([&] { return fix_tip(fix, note, allowed); });
			// What it does beside it when there is room for a few words, else under it.
			if (room - ImGui::GetItemRectSize().x - ImGui::GetStyle().ItemSpacing.x >= ImGui::GetFontSize() * 12.0f)
				ImGui::SameLine();
			disabled_wrapped(fix.detail);
			ImGui::PopID();
		}
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ui_kit::clipped_text(d.code());
		ImGui::PopStyleColor();
	} else {
		ui_kit::clipped_text(d.message, d.code().empty() ? std::string() : d.message + "\n\n" + d.code());
	}
	ImGui::TableNextColumn();
	ImGui::AlignTextToFramePadding();
	// The record in the words its document shows it by where it is open (a mission's trigger as what it
	// tests, an entity by its item's name, S15), and the field by what the record calls it; its path in
	// the tooltip.
	const std::string title = finding_record_title(d, view), field = finding_field_title(d, view);
	const std::string plain = ProblemsList::location_of(d, false);
	std::string where = plain;
	if (!title.empty() || !field.empty()) {
		where = basename_of(d.asset);
		for (const std::string *part : {title.empty() ? &d.record : &title, field.empty() ? &d.field : &field})
			if (!part->empty()) where += " - " + *part;
	}
	const std::string whole = ProblemsList::location_of(d, true) + (where == plain ? std::string() : "\n" + where);
	// A texture's finding shows the texture as its tooltip (S18).
	const AssetEntry *asset = view.project.scan && !d.asset.empty() ? view.project.scan->at_path(d.asset) : nullptr;
	const bool texture = asset && asset->kind == AssetKind::Texture;
	ui_kit::clipped_text(where, !texture && whole != where ? whole : std::string());
	if (texture) texture_preview::file_tooltip(workspace_, d.asset, whole);
	ImGui::TableNextColumn();
	if (!expanded && !fixes.empty()) draw_fixes(view, line.finding, fixes, note);
	ImGui::PopID();
}

// The Fix column: the first fix and More (every fix) when both fit with the fix's words
// legible, else one Fix... that lists every fix; each no wider than the column.
void ProblemsWindow::draw_fixes(const SessionView &view, size_t finding, const std::vector<ProblemFix> &fixes,
                                const std::string &note) {
	const ImGuiStyle &style = ImGui::GetStyle();
	const float room = ImGui::GetContentRegionAvail().x;
	const float more = fixes.size() > 1 ? ui_kit::button_width("More") + style.ItemSpacing.x : 0.0f;
	bool open = false;
	if (room - more - style.FramePadding.x * 2.0f >= ImGui::GetFontSize() * 4.0f) {
		const ProblemFix &first = fixes.front();
		const bool allowed = view.allows(first.request.kind);
		ImGui::BeginDisabled(!allowed);
		const bool clicked = ui_kit::fitted_button(first.label, "fix", room - more);
		if (fix_pressed(view, finding, first, clicked) && allowed) apply(view, finding, first);
		ImGui::EndDisabled();
		ui_kit::tooltip_lazy([&] { return fix_tip(first, note, allowed); });
		if (fixes.size() == 1) return;
		ImGui::SameLine();
		open = ImGui::Button("More");
	} else {
		open = ui_kit::fitted_button("Fix...", "fixes", room);
	}
	ui_kit::tooltip(note.empty() ? std::string("Every fix for this problem") : "Every fix for this problem\n\n" + note);
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
	// The game's own data's: said first (S15).
	const std::string note = shipped_note(finding, view);
	if (!note.empty()) ImGui::TextDisabled("%s", note.c_str());
	for (const ProblemFix &fix : list_.fixes(view, finding)) {
		const bool allowed = view.allows(fix.request.kind);
		const bool clicked = ImGui::Selectable(fix.label.c_str(), false, allowed ? 0 : ImGuiSelectableFlags_Disabled);
		if (fix_pressed(view, finding, fix, clicked) && allowed) apply(view, finding, fix);
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
	const bool allowed = allows_all(view, shown.requests);
	ImGui::BeginDisabled(shown.requests.empty() || !allowed);
	const bool apply = ImGui::Button("Apply");
	// A mouse click counts only when pressed on what shows now; a key's, in one frame, does.
	const bool applies = apply_press_.released_on(std::to_string(list_.version()),
	                                              ImGui::IsItemActivated(), apply,
	                                              ImGui::IsMouseReleased(ImGuiMouseButton_Left));
	ImGui::EndDisabled();
	if (!allowed) ui_kit::tooltip(kWaits);
	if (applies && allowed) {
		for (const EditorRequest &request : shown.requests) workspace_.request(request);
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
	if (!ProblemsList::asks_first(fix)) return workspace_.request(fix.request);
	ask(view, list_.use_fix(view, finding, fix));
}

void ProblemsWindow::ask(const SessionView &view, ProblemsList::Confirmation confirmation) {
	list_.ask(view, std::move(confirmation));
	open_confirm_ = true;
}

} // namespace opennova::editor
