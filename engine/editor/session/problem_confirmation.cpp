#include <editor/session/problem_confirmation.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <unordered_map>
#include <utility>

#include <base/io/strutil.h>
#include <editor/assets/asset_kind.h>
#include <editor/blank/blank_factory.h>
#include <editor/model/field_text.h>
#include <editor/model/finding_code_row.h>
#include <editor/project/project_files.h>
#include <editor/requirements/requirements.h>
#include <editor/session/finding_codes.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_core.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

// What a Fix all's confirmation ends with (every fix acts on the files).
constexpr const char *kNotUndoable = "What this does to the files cannot be undone with Undo.";

std::string joined(const std::vector<std::string> &names, const char *between) {
	std::string out;
	for (const std::string &name : names) out += (out.empty() ? "" : between) + name;
	return out;
}

// The file a CreateMissing role makes: its requirement row's name.
std::string role_file(const SessionView &view, const std::string &role) {
	if (view.project.requirements)
		for (const RequirementRow &row : view.project.requirements->rows)
			if (row.role == role) return row.name;
	return role;
}

// The placeholder textures a Fix all makes (each its own CreateFile), in one line.
std::string placeholder_textures(const std::vector<std::string> &files) {
	const char *what = "the checkerboard the game draws for a missing texture, to replace with your own art.";
	if (files.size() == 1) return "Create a placeholder " + files[0] + ": " + what;
	return "Create " + counted(files.size(), "placeholder texture") + ": " + joined(files, ", ") + ". Each is " + what;
}

} // namespace

ProblemQuery problem_query_of(const WorkspaceView::Problems &problems) {
	ProblemQuery query;
	query.errors = problems.errors;
	query.warnings = problems.warnings;
	query.infos = problems.infos;
	query.text = problems.text;
	query.scope = problems.scope;
	query.fixable = problems.fixable;
	query.grouping = problems.grouping;
	query.blocking = problems.blocking;
	return query;
}

std::string problem_finding_identity(const Diagnostic &d) {
	static const std::string none;
	const RequirementSubject *requirement = requirement_subject(d);
	std::string out = d.code();
	for (const std::string *part : { &d.asset, &d.record, &d.field, &subject_target(d), requirement ? &requirement->role : &none })
		out += '\x1f' + *part;
	for (const uint64_t number : { uint64_t(d.row_id), uint64_t(d.record_kind), uint64_t(d.child_id), uint64_t(d.line) })
		out += '\x1f' + std::to_string(number);
	return out;
}

std::string problem_finding_key(const SessionView &view, size_t index) {
	const std::vector<Diagnostic> &findings = view.findings.diagnostics;
	if (index >= findings.size()) return std::string();
	const std::string identity = problem_finding_identity(findings[index]);
	size_t alike = 0;
	for (size_t i = 0; i < index; ++i)
		if (problem_finding_identity(findings[i]) == identity) ++alike;
	return identity + '\x1e' + std::to_string(alike);
}

size_t problem_finding_at(const SessionView &view, const std::string &key) {
	if (key.empty()) return SIZE_MAX;
	std::unordered_map<std::string, size_t> alike;
	const std::vector<Diagnostic> &findings = view.findings.diagnostics;
	for (size_t i = 0; i < findings.size(); ++i) {
		const std::string identity = problem_finding_identity(findings[i]);
		if (identity + '\x1e' + std::to_string(alike[identity]++) == key) return i;
	}
	return SIZE_MAX;
}

bool fix_asks_first(const ProblemFix &fix) {
	return fix.request.kind == EditorRequestKind::AssignRequirement ||
	       (fix.request.kind == EditorRequestKind::Save && fix.request.force); // DI-01: Keep my edits
}

std::vector<ProblemFix> fixes_raised_at_once(std::vector<ProblemFix> fixes) {
	fixes.erase(std::remove_if(fixes.begin(), fixes.end(), [](const ProblemFix &fix) { return fix_asks_first(fix); }),
	            fixes.end());
	return fixes;
}

ConfirmationProposal propose_fix_all(const SessionView &view, ProblemFixCache &fixes, const std::vector<size_t> &findings,
                                     const EditorRequestKind *only) {
	ConfirmationProposal out;
	std::vector<ProblemFix> firsts;
	for (const size_t finding : findings) {
		if (finding >= view.findings.diagnostics.size()) continue;
		std::vector<ProblemFix> bulk = fixes.bulk(view, finding);
		if (bulk.empty()) continue;
		firsts.push_back(std::move(bulk.front()));
		++out.findings;
	}
	for (EditorRequest &request : merge_fixes(firsts))
		if (!only || request.kind == *only) out.requests.push_back(std::move(request));
	std::vector<std::string> placeholders;
	for (const EditorRequest &request : out.requests) {
		// The pointer's name makes the pointer (find_blank_factory), never the checkerboard.
		if (request.kind == EditorRequestKind::CreateFile && request.file_kind == asset_kind_token(AssetKind::Texture) &&
		    find_blank_factory("", request.path, AssetKind::Texture) == find_blank_factory_for_kind(AssetKind::Texture))
			placeholders.push_back(request.path);
		else
			out.lines.push_back(fix_request_words(view, request));
	}
	if (!placeholders.empty()) out.lines.push_back(placeholder_textures(placeholders));
	if (!out.requests.empty()) out.lines.push_back(kNotUndoable);
	return out;
}

ConfirmationProposal propose_fix(const SessionView &view, ProblemFixCache &fixes, size_t finding, const std::string &label) {
	ConfirmationProposal out;
	if (finding >= view.findings.diagnostics.size()) return out;
	for (const ProblemFix &fix : fixes.fixes(view, finding))
		if (fix.label == label) {
			out.lines = { fix.label, fix.detail };
			out.requests = { fix.request };
			out.findings = 1;
			break;
		}
	return out;
}

std::vector<size_t> required_findings(const SessionView &view) {
	std::vector<size_t> out;
	for (size_t i = 0; i < view.findings.diagnostics.size(); ++i)
		if (view.findings.diagnostics[i].row() == &finding_code(CoreFinding::RequirementMissing)) out.push_back(i);
	return out;
}

bool propose_confirmation(const SessionView &view, const WorkspaceView::Problems::Confirm &confirm,
                          const WorkspaceView::Problems &problems, ProblemQueryCache &answers, ProblemFixCache &fixes,
                          ConfirmationProposal &out, std::string &why) {
	out = ConfirmationProposal();
	if (!confirm.open()) {
		why = "No confirmation is open in Problems: a Fix all or a Use fix opens one (problems.confirm).";
		return false;
	}
	if (!view.project.open) {
		why = "No project is open.";
		return false;
	}
	if (!confirm.group.empty()) {
		// A group of the answer as the workspace's filters make it, by its key: its Fix all where Problems offers one.
		const ProblemAnswer &answer = answers.answer(problem_query_of(problems), view);
		for (const ProblemGroup &group : answer.groups) {
			if (group.key != confirm.group || !group.header) continue;
			if (group.original) {
				why = "The game's own data's problems have no Fix all: each is fixed alone.";
				return false;
			}
			out = propose_fix_all(view, fixes, group.rows);
			if (out.findings < 2 || out.requests.empty()) {
				why = "The group " + group.title + " offers no Fix all: fewer than two of its problems have a fix that runs with the others.";
				out = ConfirmationProposal();
				return false;
			}
			return true;
		}
		std::string keys;
		for (const ProblemGroup &group : answer.groups)
			if (group.header && !group.original) keys += (keys.empty() ? "" : ", ") + group.key;
		why = "Problems shows no group \"" + confirm.group + "\" with its filters and grouping (" +
		      (keys.empty() ? std::string("it shows none") : "it shows " + keys) + ").";
		return false;
	}
	if (!confirm.required.empty()) {
		EditorRequestKind kind = EditorRequestKind::CreateMissing;
		if (!editor_request_kind_from_token(confirm.required, kind)) {
			why = "problems.confirm.required names a request kind, not \"" + confirm.required + "\".";
			return false;
		}
		out = propose_fix_all(view, fixes, required_findings(view), &kind);
		if (out.requests.empty()) {
			why = "The required files' summary offers no Fix all of " + confirm.required + " now.";
			return false;
		}
		return true;
	}
	size_t finding = problem_finding_at(view, confirm.finding_key);
	if (confirm.finding_key.empty()) {
		const std::optional<unsigned long> index = strutil::parse_ulong(confirm.finding);
		finding = index && *index < view.findings.diagnostics.size() ? size_t(*index) : SIZE_MAX;
	}
	if (finding == SIZE_MAX) {
		why = "The problem the confirmation is for is gone (finding " + confirm.finding + ").";
		return false;
	}
	out = propose_fix(view, fixes, finding, confirm.label);
	if (out.requests.empty()) {
		std::string labels;
		for (const ProblemFix &fix : fixes.fixes(view, finding)) labels += (labels.empty() ? "\"" : ", \"") + fix.label + "\"";
		why = "The problem at " + std::to_string(finding) + " has no fix \"" + confirm.label + "\" (" +
		      (labels.empty() ? std::string("it has none") : "it has " + labels) + ").";
		return false;
	}
	return true;
}

std::string fix_request_words(const SessionView &view, const EditorRequest &request) {
	switch (request.kind) {
	case EditorRequestKind::CreateMissing: {
		std::vector<std::string> files;
		for (const std::string &role : request.roles) files.push_back(role_file(view, role));
		if (files.size() == 1) return "Create " + files[0] + ". It starts as placeholder content, to replace with your own.";
		return "Create " + counted(files.size(), "file") + ": " + joined(files, ", ") +
		       ". They start as placeholder content, to replace with your own.";
	}
	case EditorRequestKind::PreviewInstallImport: {
		const std::string needs = request.with_dependencies ? ", with the files they need" : "";
		if (request.names.size() == 1)
			return "Import " + request.names[0] + " from the game data: the import dialog opens on it" +
			       (request.with_dependencies ? ", with the files it needs." : ".");
		return "Import " + counted(request.names.size(), "file") + " from the game data: " + joined(request.names, ", ") +
		       ". The import dialog opens on them" + needs + ".";
	}
	case EditorRequestKind::Reimport: return "Import " + basename_of(request.path) + " again.";
	case EditorRequestKind::Save: return "Rewrite " + request.path + ".";
	case EditorRequestKind::CreateFile: return "Create " + request.path + ".";
	default: return std::string();
	}
}

std::string fix_all_label(const EditorRequest &request) {
	switch (request.kind) {
	case EditorRequestKind::CreateMissing: return "Create " + counted(request.roles.size(), "placeholder");
	case EditorRequestKind::PreviewInstallImport: return "Import " + std::to_string(request.names.size()) + " from the game data...";
	case EditorRequestKind::Reimport: return "Import " + basename_of(request.path) + " again";
	case EditorRequestKind::Save: return "Rewrite " + basename_of(request.path);
	default: return "Apply";
	}
}

void apply_confirmation(SessionCore &core) {
	SessionView &view = core.view();
	WorkspaceView::Problems &problems = view.workspace.problems;
	ProblemQueryCache answers;
	ProblemFixCache fixes;
	ConfirmationProposal proposal;
	std::string why;
	if (!propose_confirmation(view, problems.confirm, problems, answers, fixes, proposal, why))
		return core.refuse_now(CoreFinding::WorkspaceRefused, why);
	// Closed as Apply closes it, then what it proposed raised, each as the window raises it.
	problems.confirm = WorkspaceView::Problems::Confirm();
	++problems.confirm_serial;
	core.touch(ViewConcern::Workspace);
	for (const EditorRequest &request : proposal.requests) serve_request(core, request);
}

} // namespace opennova::editor
