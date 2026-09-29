#include <editor/ui/problems_window.h>

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <utility>

#include <editor/assets/asset_kind.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// The modal a Use fix and a Fix all wait in for Apply.
constexpr const char *kConfirm = "Apply fixes";
// What a Fix all's confirmation ends with (every fix acts on the files).
constexpr const char *kNotUndoable = "What this does to the files cannot be undone with Undo.";

template <class T> struct Choice {
	const char *label;
	T value;
};
constexpr Choice<ProblemScope> kScopes[] = {
	{"Project", ProblemScope::Project}, {"Active file", ProblemScope::ActiveFile}, {"Open files", ProblemScope::OpenFiles}};
constexpr Choice<ProblemGrouping> kGroupings[] = {
	{"None", ProblemGrouping::None}, {"File", ProblemGrouping::File}, {"Kind", ProblemGrouping::Kind}};

std::string file_name(const std::string &path) { return std::filesystem::path(path).filename().generic_string(); }

std::string counted(size_t n, const char *noun) { return std::to_string(n) + " " + noun + (n == 1 ? "" : "s"); }

std::string joined(const std::vector<std::string> &names, const char *between) {
	std::string out;
	for (const std::string &name : names) out += (out.empty() ? "" : between) + name;
	return out;
}

// What a finding is about, whatever its place among the findings: its code, its file, the
// record (its address and name), the field, the line and what it names (the file or symbol,
// the role). Findings alike in all of it are told apart by their order (refresh()).
std::string identity(const Diagnostic &d) {
	std::string out = d.code;
	for (const std::string *part : {&d.asset, &d.record, &d.field, &d.target, &d.role}) out += '\x1f' + *part;
	for (const uint64_t number : {uint64_t(d.row_id), uint64_t(d.record_kind), uint64_t(d.child_id), uint64_t(d.line)})
		out += '\x1f' + std::to_string(number);
	return out;
}

// A fix's request as a press saw it, for its release to be on the same.
std::string signature(const EditorRequest &request) {
	std::string out = std::to_string(static_cast<int>(request.kind));
	for (const std::string *part : {&request.path, &request.text}) out += '\x1f' + *part;
	out += request.flag ? "\x1f+" : "\x1f-";
	for (const std::string &name : request.names) out += '\x1f' + name;
	return out;
}

// Where a finding is, on one line: its file (the whole path, or its name) and line, the
// record and the field ("items.def:12 - Marker - graphic"); for a finding no file of the
// project is at fault for (a required file it lacks), the file it is about.
std::string location_of(const Diagnostic &d, bool whole_path) {
	std::string where = d.asset.empty() ? d.target : whole_path ? d.asset : file_name(d.asset);
	if (!d.asset.empty() && d.line) where += ":" + std::to_string(d.line);
	for (const std::string *part : {&d.record, &d.field})
		if (!part->empty()) where += (where.empty() ? "" : " - ") + *part;
	return where;
}

std::string severity_counts(size_t errors, size_t warnings, size_t infos) {
	std::string out;
	const auto add = [&out](size_t n, const std::string &words) {
		if (n) out += (out.empty() ? "" : ", ") + words;
	};
	add(errors, counted(errors, "error"));
	add(warnings, counted(warnings, "warning"));
	add(infos, std::to_string(infos) + " info");
	return out;
}

// The summary: the game cannot start while a required file is missing or is another kind
// of file than the game reads.
std::string cannot_start(const RequirementReport &report) {
	const auto are = [](int n) { return n == 1 ? " is " : " are "; };
	std::string text = "The game cannot start: ";
	if (report.required_missing)
		text += counted(size_t(report.required_missing), "required file") + are(report.required_missing) + "missing";
	if (report.required_wrong_kind) {
		if (report.required_missing) text += ", and ";
		text += counted(size_t(report.required_wrong_kind), "required file") + are(report.required_wrong_kind) +
		        "not the kind of file the game reads";
	}
	return text + ".";
}

// The file a CreateMissing role makes: its requirement row's name.
std::string role_file(const SessionView &view, const std::string &role) {
	for (const RequirementRow &row : view.requirements.rows)
		if (row.role == role) return row.name;
	return role;
}

// What a request of a Fix all does, in its confirmation's words.
std::string describe(const SessionView &view, const EditorRequest &request) {
	switch (request.kind) {
	case EditorRequestKind::CreateMissing: {
		std::vector<std::string> files;
		for (const std::string &role : request.names) files.push_back(role_file(view, role));
		if (files.size() == 1) return "Create " + files[0] + ". It starts as placeholder content, to replace with your own.";
		return "Create " + counted(files.size(), "file") + ": " + joined(files, ", ") +
		       ". They start as placeholder content, to replace with your own.";
	}
	case EditorRequestKind::PreviewRetailImport: {
		const std::string needs = request.flag ? ", with the files they need" : "";
		if (request.names.size() == 1)
			return "Import " + request.names[0] + " from the game data: the import dialog opens on it" +
			       (request.flag ? ", with the files it needs." : ".");
		return "Import " + counted(request.names.size(), "file") + " from the game data: " + joined(request.names, ", ") +
		       ". The import dialog opens on them" + needs + ".";
	}
	case EditorRequestKind::Reimport: return "Import " + file_name(request.path) + " again.";
	case EditorRequestKind::Save: return "Rewrite " + request.path + ".";
	case EditorRequestKind::CreateFile: return "Create " + request.path + ".";
	default: return std::string();
	}
}

// The placeholder textures a Fix all makes (each its own CreateFile), in one line.
std::string placeholder_textures(const std::vector<std::string> &files) {
	const char *what = "the checkerboard the game draws for a missing texture, to replace with your own art.";
	if (files.size() == 1) return "Create a placeholder " + files[0] + ": " + what;
	return "Create " + counted(files.size(), "placeholder texture") + ": " + joined(files, ", ") + ". Each is " + what;
}

// A Fix all's button in the summary: what it does and to how many files.
std::string fix_all_label(const EditorRequest &request) {
	switch (request.kind) {
	case EditorRequestKind::CreateMissing: return "Create " + std::to_string(request.names.size());
	case EditorRequestKind::PreviewRetailImport:
		return "Import " + std::to_string(request.names.size()) + " from the game data...";
	case EditorRequestKind::Reimport: return "Import " + file_name(request.path) + " again";
	case EditorRequestKind::Save: return "Rewrite " + file_name(request.path);
	default: return "Apply";
	}
}

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
	if (view.project_open) refresh(view);
	if (!view.project_open) ui_kit::empty_state("No project open.");
	else if (view.diagnostics.empty()) ui_kit::empty_state("No problems.");
	else draw_problems(view);
	draw_more(view);
	draw_confirm(view);
}

void ProblemsWindow::draw_problems(const SessionView &view) {
	const ProblemAnswer &answer = draw_filters(view);
	draw_summary(view);
	draw_list(view, answer);
}

// The filter bar, wrapping whole controls in a narrow dock; the answer to the query it sets.
const ProblemAnswer &ProblemsWindow::draw_filters(const SessionView &view) {
	const ProblemAnswer &counts = answers_.answer(query_, view); // every finding's, whatever it shows
	ui_kit::WrapRow row;
	severity_toggle(row, "Errors", "###errors", counts.errors, "errors", query_.errors);
	severity_toggle(row, "Warnings", "###warnings", counts.warnings, "warnings", query_.warnings);
	severity_toggle(row, "Info", "###infos", counts.infos, "info", query_.infos);
	const float filter = ImGui::GetFontSize() * 13.0f;
	row.next(filter);
	ui_kit::filter_box("##filter", text_, sizeof(text_), "Filter", filter,
	                   "Only the problems whose message, file, record, field or code has this text.");
	query_.text = text_;
	choice_combo(row, "Scope", kScopes, query_.scope, "Which files' problems: the project's, the active file's, the open files'.");
	choice_combo(row, "Group", kGroupings, query_.grouping, "Group the problems by file or by kind, or list them as one.");
	row.next(ui_kit::checkbox_width("Only fixable"));
	ImGui::Checkbox("Only fixable", &query_.fixable);
	ui_kit::tooltip("Only the problems the editor offers a fix for.");
	const ProblemAnswer &answer = refresh(view);
	const std::string shown = std::to_string(answer.rows.size()) + " of " + std::to_string(answer.total());
	row.next(ui_kit::text_width(shown.c_str()));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", shown.c_str());
	return answer;
}

// Each finding's key and the finding a key names, the lines of the list, each group's Fix
// all and the summary's, made again when the revision, the query or the folded groups move.
const ProblemAnswer &ProblemsWindow::refresh(const SessionView &view) {
	const ProblemAnswer &answer = answers_.answer(query_, view);
	if (!stale_ && view_ == &view && revision_ == view.revision && refreshed_ == query_) return answer;
	view_ = &view;
	revision_ = view.revision;
	refreshed_ = query_;
	stale_ = false;
	keys_.clear();
	index_.clear();
	std::unordered_map<std::string, size_t> alike;
	for (const Diagnostic &d : view.diagnostics) {
		const std::string id = identity(d);
		std::string key = id + '\x1e' + std::to_string(alike[id]++);
		std::replace(key.begin(), key.end(), '#', '\x1d'); // a key is an ImGui id: no "###" in it
		index_.emplace(key, keys_.size());
		keys_.push_back(std::move(key));
	}
	selected_index_ = resolve(view, selected_);
	lines_.clear();
	group_fixes_.clear();
	// A group of notes alone starts folded the first time the project shows it, so the list
	// opens on the errors and warnings; folded so, it opens again once it holds an error or a
	// warning, while one the user folded stays as the user left it. The groups folded and seen
	// are the project's.
	if (view.project_root != groups_root_) {
		groups_root_ = view.project_root;
		folded_.clear();
		auto_folded_.clear();
		seen_groups_.clear();
	}
	if (answer.grouped) {
		for (size_t g = 0; g < answer.groups.size(); ++g) {
			const ProblemGroup &group = answer.groups[g];
			const bool notes_alone = group.errors == 0 && group.warnings == 0;
			if (seen_groups_.insert(group.key).second && notes_alone) {
				folded_.insert(group.key);
				auto_folded_.insert(group.key);
			} else if (!notes_alone && auto_folded_.erase(group.key)) {
				folded_.erase(group.key);
			}
			lines_.push_back({true, g, 0});
			group_fixes_.push_back(propose(view, fix_all_of(view, group.rows)));
			if (folded_.count(group.key)) continue;
			for (const size_t finding : group.rows) lines_.push_back({false, g, finding});
		}
	} else {
		for (const size_t finding : answer.rows) lines_.push_back({false, 0, finding});
	}
	required_.clear();
	for (size_t i = 0; i < view.diagnostics.size(); ++i)
		if (view.diagnostics[i].code == "requirement.missing") required_.push_back(i);
	required_fixes_ = propose(view, fix_all_of(view, required_));
	return answer;
}

// While required files are missing: the game cannot start, and the Fix alls that make them
// (one Create naming every file a factory makes, one import list of the ones the game data
// has), each asking first.
void ProblemsWindow::draw_summary(const SessionView &view) {
	const RequirementReport &report = view.requirements;
	if (report.required_missing + report.required_wrong_kind == 0) return;
	const std::string sentence = cannot_start(report);
	ui_kit::WrapRow row;
	row.next(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x + ui_kit::text_width(sentence.c_str()));
	ui_kit::severity_marker(DiagnosticSeverity::Error);
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextWrapped("%s", sentence.c_str());
	ImGui::PushID(view.project_root.c_str());
	ImGui::PushID("required");
	for (const EditorRequest &request : required_fixes_.requests) {
		const std::string label = fix_all_label(request);
		row.next(ui_kit::button_width(label.c_str()));
		ImGui::PushID(static_cast<int>(request.kind));
		if (ui_kit::fitted_button(label, "fix", ImGui::GetContentRegionAvail().x)) {
			Confirmation merged = fix_all_of(view, required_);
			merged.of = Confirmation::Of::Kind;
			merged.kind = request.kind;
			ask(view, std::move(merged));
		}
		ui_kit::tooltip(describe(view, request));
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
	size_t split = lines_.size();
	for (size_t i = 0; i < lines_.size(); ++i)
		if (!lines_[i].header && lines_[i].finding == selected_index_) split = i;
	draw_lines(view, answer, 0, split);
	if (split < lines_.size()) {
		draw_finding(view, lines_[split], true);
		draw_lines(view, answer, split + 1, lines_.size());
	}
	ImGui::PopID();
	ImGui::EndTable();
}

void ProblemsWindow::draw_lines(const SessionView &view, const ProblemAnswer &answer, size_t first, size_t last) {
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(last - first), line_height());
	while (clipper.Step())
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			const Line &line = lines_[first + size_t(i)];
			if (line.header) draw_header(view, answer, line);
			else if (line.finding < view.diagnostics.size()) draw_finding(view, line, false);
		}
}

// A group's header: folds it away, its title and counts, and its Fix all when several of its
// findings have a fix that runs with the others.
void ProblemsWindow::draw_header(const SessionView &view, const ProblemAnswer &answer, const Line &line) {
	const ProblemGroup &group = answer.groups[line.group];
	ImGui::PushID(("group:" + group.key).c_str());
	ImGui::TableNextRow(ImGuiTableRowFlags_None, line_height());
	ImGui::TableSetColumnIndex(1);
	const bool folded = folded_.count(group.key) != 0;
	const ImGuiTreeNodeFlags node = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_AllowOverlap |
	                                ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding |
	                                ImGuiTreeNodeFlags_NoAutoOpenOnLog;
	ImGui::SetNextItemOpen(!folded, ImGuiCond_Always);
	if (ImGui::TreeNodeEx("##group", node) == folded) { // clicked: fold it away, or open it again
		if (folded) folded_.erase(group.key);
		else folded_.insert(group.key);
		auto_folded_.erase(group.key); // the user's now: a new error leaves it as it is
		stale_ = true;
	}
	ImGui::SameLine();
	ui_kit::clipped_text(group.title + " (" + severity_counts(group.errors, group.warnings, group.infos) + ")");
	ImGui::TableSetColumnIndex(3);
	const Proposal &all = group_fixes_[line.group];
	if (all.findings >= 2 && !all.requests.empty()) {
		if (ui_kit::fitted_button("Fix all", "fix_all", ImGui::GetContentRegionAvail().x)) ask(view, fix_all_of(view, group.rows));
		ui_kit::tooltip(joined(all.lines, "\n"));
	}
	ImGui::PopID();
}

// A finding's line: its severity, its message, where it is and its fixes. Expanded (the
// selected one), the whole message wrapped, every fix with what it does, and its code.
void ProblemsWindow::draw_finding(const SessionView &view, const Line &line, bool expanded) {
	const Diagnostic &d = view.diagnostics[line.finding];
	ImGui::PushID(keys_[line.finding].c_str());
	ImGui::TableNextRow(ImGuiTableRowFlags_None, line_height());
	if (expanded) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
	ImGui::TableNextColumn();
	// The line itself: a click selects the finding (the selected one folds back) and goes to
	// its place, when it has one.
	if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
	                      ImVec2(0.0f, ImGui::GetFrameHeight()))) {
		selected_ = expanded ? FindingRef() : FindingRef{view.project_root, keys_[line.finding]};
		selected_index_ = expanded ? SIZE_MAX : line.finding;
		const ProblemLocation location = problem_location(d, view);
		if (!location.empty()) host_.request(location.request());
	}
	ImGui::SameLine(0.0f, 0.0f);
	ui_kit::severity_marker(d.severity);
	const std::vector<ProblemFix> &fixes = fixes_.fixes(view, line.finding);
	ImGui::TableNextColumn();
	ImGui::AlignTextToFramePadding();
	if (expanded) {
		ImGui::TextWrapped("%s", d.message.c_str());
		for (const ProblemFix &fix : fixes) {
			ImGui::PushID(fix.label.c_str());
			const float room = ImGui::GetContentRegionAvail().x;
			if (released_on(ui_kit::fitted_button(fix.label, "fix", room), fix_id(view, line.finding, fix)))
				apply(view, line.finding, fix);
			ui_kit::tooltip(fix.label + "\n\n" + fix.detail);
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
	const std::string where = location_of(d, false), whole = location_of(d, true);
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
	if (room - more - style.FramePadding.x * 2.0f >= ImGui::GetFontSize() * 4.0f) {
		const ProblemFix &first = fixes.front();
		if (released_on(ui_kit::fitted_button(first.label, "fix", room - more), fix_id(view, finding, first)))
			apply(view, finding, first);
		ui_kit::tooltip(first.label + "\n\n" + first.detail);
		if (fixes.size() == 1) return;
		ImGui::SameLine();
		if (ImGui::Button("More")) open_more(view, finding);
	} else if (ui_kit::fitted_button("Fix...", "fixes", room)) {
		open_more(view, finding);
	}
	ui_kit::tooltip("Every fix for this problem");
}

// Every fix of the finding More was pressed on, found again by its key: gone (or in another
// project), the list closes.
void ProblemsWindow::draw_more(const SessionView &view) {
	if (open_more_) {
		ImGui::OpenPopup("more");
		open_more_ = false;
	}
	if (!ImGui::BeginPopup("more")) return;
	const size_t finding = resolve(view, more_);
	if (finding == SIZE_MAX) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
	for (const ProblemFix &fix : fixes_.fixes(view, finding)) {
		if (released_on(ImGui::Selectable(fix.label.c_str()), fix_id(view, finding, fix))) apply(view, finding, fix);
		ImGui::Indent();
		ImGui::TextDisabled("%s", fix.detail.c_str());
		ImGui::Unindent();
	}
	ImGui::PopTextWrapPos();
	ImGui::EndPopup();
}

// What a Use fix or a Fix all will do, raised on Apply; Cancel raises nothing. It belongs to
// the project it was asked in (another, or none, closes it) and is made again from its
// findings whenever the view moves: what changed is said, and Apply counts only when it was
// pressed on what the confirmation shows now.
void ProblemsWindow::draw_confirm(const SessionView &view) {
	if (open_confirm_) {
		ImGui::OpenPopup(kConfirm);
		open_confirm_ = false;
	}
	if (!ImGui::BeginPopupModal(kConfirm, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	if (!view.project_open || view.project_root != confirm_.root) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	if (shown_view_ != &view || shown_revision_ != view.revision) {
		Proposal now = propose(view, confirm_);
		shown_view_ = &view;
		shown_revision_ = view.revision;
		const auto signatures = [](const Proposal &proposal) {
			std::vector<std::string> out;
			for (const EditorRequest &request : proposal.requests) out.push_back(signature(request));
			return out;
		};
		if (now.lines != shown_.lines || signatures(now) != signatures(shown_)) {
			shown_ = std::move(now);
			++shown_version_;
			shown_changed_ = true;
		}
	}
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
	if (shown_.requests.empty()) ImGui::TextUnformatted("Nothing is left to do: the problems it was for are gone.");
	for (const std::string &line : shown_.lines) ImGui::TextUnformatted(line.c_str());
	ImGui::PopTextWrapPos();
	ImGui::BeginDisabled(shown_.requests.empty());
	const bool apply = ImGui::Button("Apply");
	if (ImGui::IsItemActivated()) apply_pressed_ = shown_version_;
	ImGui::EndDisabled();
	// A mouse click counts only when pressed on what shows now; a key's, in one frame, does.
	if (apply && (apply_pressed_ == shown_version_ || !ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
		for (const EditorRequest &request : shown_.requests) host_.request(request);
		shown_ = Proposal();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		shown_ = Proposal();
		ImGui::CloseCurrentPopup();
	}
	// Said beside the buttons, which then stay where they were pressed.
	if (shown_changed_) {
		ImGui::SameLine();
		ui_kit::severity_marker(DiagnosticSeverity::Warning);
		ImGui::SameLine();
		ImGui::TextUnformatted("Changed while open: this is what Apply does now.");
	}
	ImGui::EndPopup();
}

size_t ProblemsWindow::resolve(const SessionView &view, const FindingRef &ref) const {
	if (ref.key.empty() || !view.project_open || ref.root != view.project_root) return SIZE_MAX;
	const auto found = index_.find(ref.key);
	return found != index_.end() && found->second < view.diagnostics.size() ? found->second : SIZE_MAX;
}

std::string ProblemsWindow::fix_id(const SessionView &view, size_t finding, const ProblemFix &fix) const {
	return view.project_root + '\x1e' + keys_[finding] + '\x1e' + signature(fix.request);
}

// A fix's control counts a mouse click only when the press was on the same fix of the same
// finding: a view that moves between the press and the release can put another under it. An
// activation pressed and released in one frame (a key) is on what the frame shows.
bool ProblemsWindow::released_on(bool clicked, const std::string &id) {
	if (ImGui::IsItemActivated()) pressed_ = id;
	return clicked && (pressed_ == id || !ImGui::IsMouseReleased(ImGuiMouseButton_Left));
}

// What a confirmation shows and raises now: its findings found again by their keys (the gone
// ones left out), each one's first bulk fix merged (the fix cache's bulk: no rename planned)
// or, for a Use fix, the fix its label names with what the rename rewrites today.
ProblemsWindow::Proposal ProblemsWindow::propose(const SessionView &view, const Confirmation &confirmation) {
	Proposal out;
	if (confirmation.of == Confirmation::Of::Fix) {
		const size_t finding = resolve(view, {confirmation.root, confirmation.keys.empty() ? std::string() : confirmation.keys[0]});
		if (finding == SIZE_MAX) return out;
		for (const ProblemFix &fix : fixes_.fixes(view, finding))
			if (fix.label == confirmation.label) {
				out.lines = {fix.label, fix.detail};
				out.requests = {fix.request};
				out.findings = 1;
				break;
			}
		return out;
	}
	std::vector<ProblemFix> firsts;
	for (const std::string &key : confirmation.keys) {
		const size_t finding = resolve(view, {confirmation.root, key});
		if (finding == SIZE_MAX) continue;
		std::vector<ProblemFix> bulk = fixes_.bulk(view, finding);
		if (bulk.empty()) continue;
		firsts.push_back(std::move(bulk.front()));
		++out.findings;
	}
	for (EditorRequest &request : merge_fixes(firsts))
		if (confirmation.of == Confirmation::Of::FixAll || request.kind == confirmation.kind)
			out.requests.push_back(std::move(request));
	std::vector<std::string> placeholders;
	for (const EditorRequest &request : out.requests) {
		if (request.kind == EditorRequestKind::CreateFile && request.text == asset_kind_token(AssetKind::Texture))
			placeholders.push_back(request.path);
		else
			out.lines.push_back(describe(view, request));
	}
	if (!placeholders.empty()) out.lines.push_back(placeholder_textures(placeholders));
	if (!out.requests.empty()) out.lines.push_back(kNotUndoable);
	return out;
}

ProblemsWindow::Confirmation ProblemsWindow::fix_all_of(const SessionView &view, const std::vector<size_t> &findings) const {
	Confirmation all;
	all.root = view.project_root;
	for (const size_t finding : findings) all.keys.push_back(keys_[finding]);
	return all;
}

// A Use fix renames a file and rewrites what names it: it says so and waits for Apply. Any
// other fix is raised as it is (its detail, in its tooltip, says what it does).
void ProblemsWindow::apply(const SessionView &view, size_t finding, const ProblemFix &fix) {
	if (fix.request.kind != EditorRequestKind::AssignRequirement) {
		host_.request(fix.request);
		return;
	}
	Confirmation use;
	use.of = Confirmation::Of::Fix;
	use.root = view.project_root;
	use.keys = {keys_[finding]};
	use.label = fix.label;
	ask(view, std::move(use));
}

void ProblemsWindow::ask(const SessionView &view, Confirmation confirmation) {
	confirm_ = std::move(confirmation);
	shown_ = propose(view, confirm_);
	shown_view_ = &view;
	shown_revision_ = view.revision;
	++shown_version_;
	shown_changed_ = false;
	open_confirm_ = true;
}

void ProblemsWindow::open_more(const SessionView &view, size_t finding) {
	more_ = {view.project_root, keys_[finding]};
	open_more_ = true;
}

} // namespace opennova::editor
