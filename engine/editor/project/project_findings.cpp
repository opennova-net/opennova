#include <editor/project/project_findings.h>

#include <iterator>

#include <base/gameprofile/required_resources.h>
#include <editor/documents/project_checks.h>
#include <editor/graph/project_validation.h>

namespace opennova::editor {

namespace {

// The Problems row of a file the game reported missing when it booted: the project's (no
// file of it is at fault), naming the manifest row's role and required name when the name
// is one, and what the game does without it.
Diagnostic boot_finding(const std::string &name) {
	const gameprofile::RequiredResource *row = gameprofile::gameprofile_required_resource_find(name.c_str());
	std::string message = "The game could not find " + name + " when it started";
	message += row != nullptr && row->failure != nullptr ? std::string(". Without it: ") + row->failure + "." : ".";
	Diagnostic d = make_diagnostic(DiagnosticSeverity::Error, "play.boot_missing", message);
	d.role = row != nullptr ? row->role : "";
	d.target = row != nullptr ? row->name : name;
	return d;
}

} // namespace

ProjectFindings compose_project_findings(const ProjectFindingsInput &input, AssetGraph &graph, ValidationCache &cache,
                                         ProjectChecks &checks, const FileSource &files) {
	refresh_project_findings(input, graph, cache, checks, files);
	return collect_project_findings(input, graph, cache, checks);
}

bool refresh_project_findings(const ProjectFindingsInput &input, AssetGraph &graph, ValidationCache &cache,
                              ProjectChecks &checks, const FileSource &files) {
	const ValidationInput validation{ input.paths, input.project, input.scan, input.open };
	const bool files_moved = refresh_project(validation, graph, cache);
	// After the files' own findings: a check reads which files' records their own checks read.
	const bool checks_moved = checks.update({ validation, cache, files });
	return files_moved || checks_moved;
}

ProjectFindings collect_project_findings(const ProjectFindingsInput &input, const AssetGraph &graph,
                                         const ValidationCache &cache, const ProjectChecks &checks) {
	ProjectFindings out;
	std::vector<Diagnostic> &rows = out.rows;
	const ValidationInput validation{ input.paths, input.project, input.scan, input.open };
	std::vector<Diagnostic> documents = project_rows(validation, graph, cache);
	rows.reserve(input.scan.diagnostics.size() + input.requirements.diagnostics.size() +
			input.boot_missing.size() + input.play.size() + documents.size() +
			input.open_findings.size() + checks.findings_size() + input.build.size());
	rows.insert(rows.end(), input.scan.diagnostics.begin(), input.scan.diagnostics.end());
	rows.insert(rows.end(), input.requirements.diagnostics.begin(),
			input.requirements.diagnostics.end());
	for (const std::string &name : input.boot_missing)
		rows.push_back(boot_finding(name));
	rows.insert(rows.end(), input.play.begin(), input.play.end());
	out.gate_begin = rows.size();
	rows.insert(rows.end(), std::make_move_iterator(documents.begin()),
			std::make_move_iterator(documents.end()));
	rows.insert(rows.end(), input.open_findings.begin(), input.open_findings.end());
	out.gate_end = rows.size();
	checks.append_findings(rows);
	rows.insert(rows.end(), input.build.begin(), input.build.end());
	return out;
}

} // namespace opennova::editor
