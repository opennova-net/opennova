#include <editor/ui/problems_list.h>

#include <algorithm>
#include <initializer_list>
#include <utility>

#include <base/io/json.h>
#include <editor/assets/asset_kind.h>
#include <editor/model/field_text.h>
#include <editor/project/project_files.h>
#include <editor/session/session_json.h>

namespace opennova::editor {

namespace {

// What a Fix all's confirmation ends with (every fix acts on the files).
constexpr const char *kNotUndoable = "What this does to the files cannot be undone with Undo.";

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
	for (const std::string *part : {&d.asset, &d.record, &d.field, &d.target, &d.role})
		out += '\x1f' + *part;
	for (const uint64_t number :
	     {uint64_t(d.row_id), uint64_t(d.record_kind), uint64_t(d.child_id), uint64_t(d.line)})
		out += '\x1f' + std::to_string(number);
	return out;
}

// A fix's request as a press saw it, for its release to be on the same: its wire form, every
// field its kind takes (S13 A4).
std::string signature(const EditorRequest &request) {
	return io::json_write(editor_request_to_json(request));
}

// The file a CreateMissing role makes: its requirement row's name.
std::string role_file(const SessionView &view, const std::string &role) {
	for (const RequirementRow &row : view.requirements.rows)
		if (row.role == role) return row.name;
	return role;
}

// The placeholder textures a Fix all makes (each its own CreateFile), in one line.
std::string placeholder_textures(const std::vector<std::string> &files) {
	const char *what =
	        "the checkerboard the game draws for a missing texture, to replace with your own art.";
	if (files.size() == 1) return "Create a placeholder " + files[0] + ": " + what;
	return "Create " + counted(files.size(), "placeholder texture") + ": " + joined(files, ", ") +
	       ". Each is " + what;
}

} // namespace

bool ProblemsList::PressLatch::released_on(const std::string &id, bool pressed, bool clicked,
                                           bool mouse_released) {
	if (pressed) pressed_ = id;
	return clicked && (pressed_ == id || !mouse_released);
}

ProblemsList::ProblemsList() { query_.grouping = ProblemGrouping::Kind; }

const ProblemAnswer &ProblemsList::refresh(const SessionView &view) {
	const ProblemAnswer &answer = answers_.answer(query_, view);
	const uint64_t answered = answers_.generation();
	const uint64_t fixed = fixes_.generation(view);
	const RevisionKey key = cache_key(view, query_);
	if (!stale_ && view_ == &view && key_ == key && refreshed_ == query_ &&
	    answered_ == answered && fixed_ == fixed)
		return answer;
	view_ = &view;
	key_ = key;
	refreshed_ = query_;
	answered_ = answered;
	fixed_ = fixed;
	stale_ = false;
	++made_;
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
	// The groups folded and seen are the project's.
	if (view.project_root != groups_root_) {
		groups_root_ = view.project_root;
		folded_.clear();
		auto_folded_.clear();
		seen_groups_.clear();
	}
	if (answer.grouped) {
		for (size_t g = 0; g < answer.groups.size(); ++g) {
			const ProblemGroup &group = answer.groups[g];
			// A group of notes alone folds the first time it shows; folded so, it opens again
			// once it holds an error or a warning, while one the user folded stays folded.
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

size_t ProblemsList::resolve(const SessionView &view, const FindingRef &ref) const {
	if (ref.key.empty() || !view.project_open || ref.root != view.project_root) return SIZE_MAX;
	const auto found = index_.find(ref.key);
	if (found == index_.end() || found->second >= view.diagnostics.size()) return SIZE_MAX;
	return found->second;
}

void ProblemsList::toggle_fold(const std::string &group) {
	if (!folded_.erase(group)) folded_.insert(group);
	auto_folded_.erase(group); // the user's now: a new error leaves it as it is
	stale_ = true;
}

void ProblemsList::toggle_selected(const SessionView &view, size_t finding) {
	const bool selected = finding == selected_index_;
	selected_ = selected ? FindingRef() : ref(view, finding);
	selected_index_ = selected ? SIZE_MAX : finding;
}

std::string ProblemsList::fix_id(const SessionView &view, size_t finding,
                                 const ProblemFix &fix) const {
	return view.project_root + '\x1e' + keys_[finding] + '\x1e' + signature(fix.request);
}

ProblemsList::Confirmation ProblemsList::fix_all_of(const SessionView &view,
                                                    const std::vector<size_t> &findings) const {
	Confirmation all;
	all.root = view.project_root;
	for (const size_t finding : findings) all.keys.push_back(keys_[finding]);
	return all;
}

ProblemsList::Confirmation ProblemsList::required_fix(const SessionView &view,
                                                      EditorRequestKind kind) const {
	Confirmation merged = fix_all_of(view, required_);
	merged.of = Confirmation::Of::Kind;
	merged.kind = kind;
	return merged;
}

ProblemsList::Confirmation ProblemsList::use_fix(const SessionView &view, size_t finding,
                                                 const ProblemFix &fix) const {
	Confirmation use;
	use.of = Confirmation::Of::Fix;
	use.root = view.project_root;
	use.keys = {keys_[finding]};
	use.label = fix.label;
	return use;
}

ProblemsList::Proposal ProblemsList::propose(const SessionView &view,
                                             const Confirmation &confirmation) {
	Proposal out;
	if (confirmation.of == Confirmation::Of::Fix) {
		const std::string key = confirmation.keys.empty() ? std::string() : confirmation.keys[0];
		const size_t finding = resolve(view, {confirmation.root, key});
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
		if (request.kind == EditorRequestKind::CreateFile &&
		    request.file_kind == asset_kind_token(AssetKind::Texture))
			placeholders.push_back(request.path);
		else
			out.lines.push_back(describe(view, request));
	}
	if (!placeholders.empty()) out.lines.push_back(placeholder_textures(placeholders));
	if (!out.requests.empty()) out.lines.push_back(kNotUndoable);
	return out;
}

void ProblemsList::ask(const SessionView &view, Confirmation confirmation) {
	confirm_ = std::move(confirmation);
	shown_ = propose(view, confirm_);
	shown_made_ = made_;
	++shown_version_;
	shown_changed_ = false;
}

bool ProblemsList::follow(const SessionView &view) {
	if (!view.project_open || view.project_root != confirm_.root) return false;
	refresh(view);
	if (shown_made_ == made_) return true;
	Proposal now = propose(view, confirm_);
	shown_made_ = made_;
	if (now.lines != shown_.lines || now.requests != shown_.requests) {
		shown_ = std::move(now);
		++shown_version_;
		shown_changed_ = true;
	}
	return true;
}

std::string ProblemsList::location_of(const Diagnostic &d, bool whole_path) {
	std::string where = d.asset.empty() ? d.target : whole_path ? d.asset : basename_of(d.asset);
	if (!d.asset.empty() && d.line) where += ":" + std::to_string(d.line);
	for (const std::string *part : {&d.record, &d.field})
		if (!part->empty()) where += (where.empty() ? "" : " - ") + *part;
	return where;
}

std::string ProblemsList::severity_counts(size_t errors, size_t warnings, size_t infos) {
	std::string out;
	const auto add = [&out](size_t n, const std::string &words) {
		if (n) out += (out.empty() ? "" : ", ") + words;
	};
	add(errors, counted(errors, "error"));
	add(warnings, counted(warnings, "warning"));
	add(infos, std::to_string(infos) + " info");
	return out;
}

std::string ProblemsList::summary(const RequirementReport &report) {
	if (report.required_missing + report.required_wrong_kind == 0) return std::string();
	const auto are = [](int n) { return n == 1 ? " is " : " are "; };
	std::string text = "The game cannot start: ";
	if (report.required_missing)
		text += counted(size_t(report.required_missing), "required file") +
		        are(report.required_missing) + "missing";
	if (report.required_wrong_kind) {
		if (report.required_missing) text += ", and ";
		text += counted(size_t(report.required_wrong_kind), "required file") +
		        are(report.required_wrong_kind) + "not the kind of file the game reads";
	}
	return text + ".";
}

std::string ProblemsList::describe(const SessionView &view, const EditorRequest &request) {
	switch (request.kind) {
	case EditorRequestKind::CreateMissing: {
		std::vector<std::string> files;
		for (const std::string &role : request.roles) files.push_back(role_file(view, role));
		if (files.size() == 1)
			return "Create " + files[0] +
			       ". It starts as placeholder content, to replace with your own.";
		return "Create " + counted(files.size(), "file") + ": " + joined(files, ", ") +
		       ". They start as placeholder content, to replace with your own.";
	}
	case EditorRequestKind::PreviewInstallImport: {
		const std::string needs = request.with_dependencies ? ", with the files they need" : "";
		if (request.names.size() == 1)
			return "Import " + request.names[0] +
			       " from the game data: the import dialog opens on it" +
			       (request.with_dependencies ? ", with the files it needs." : ".");
		return "Import " + counted(request.names.size(), "file") + " from the game data: " +
		       joined(request.names, ", ") + ". The import dialog opens on them" + needs + ".";
	}
	case EditorRequestKind::Reimport: return "Import " + basename_of(request.path) + " again.";
	case EditorRequestKind::Save: return "Rewrite " + request.path + ".";
	case EditorRequestKind::CreateFile: return "Create " + request.path + ".";
	default: return std::string();
	}
}

std::string ProblemsList::fix_all_label(const EditorRequest &request) {
	switch (request.kind) {
	case EditorRequestKind::CreateMissing: return "Create " + std::to_string(request.roles.size());
	case EditorRequestKind::PreviewInstallImport:
		return "Import " + std::to_string(request.names.size()) + " from the game data...";
	case EditorRequestKind::Reimport: return "Import " + basename_of(request.path) + " again";
	case EditorRequestKind::Save: return "Rewrite " + basename_of(request.path);
	default: return "Apply";
	}
}

} // namespace opennova::editor
