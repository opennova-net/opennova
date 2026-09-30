#include <editor/project/project_findings.h>

#include <iterator>

#include <base/gameprofile/required_resources.h>
#include <editor/graph/project_validation.h>
#include <editor/preview/menu_render_check.h>

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
                                         MenuRenderCheck &render_check, const FileSource &files) {
	ProjectFindings out;
	std::vector<Diagnostic> &rows = out.rows;
	const ValidationInput validation{ input.paths, input.project, input.scan, input.open };
	std::vector<Diagnostic> documents = validate_project(validation, graph, cache);
	render_check.update(validation, graph, files);
	rows.reserve(input.scan.diagnostics.size() + input.requirements.diagnostics.size() +
			input.boot_missing.size() + input.play.size() + documents.size() +
			input.open_findings.size() + render_check.diagnostics().size() + input.build.size());
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
	rows.insert(rows.end(), render_check.diagnostics().begin(), render_check.diagnostics().end());
	rows.insert(rows.end(), input.build.begin(), input.build.end());
	return out;
}

} // namespace opennova::editor
