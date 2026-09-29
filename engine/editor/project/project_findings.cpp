#include <editor/project/project_findings.h>

#include <base/gameprofile/required_resources.h>
#include <editor/documents/document_types.h>
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
	out.documents = validate_open_documents(input.paths, input.project, input.scan, input.open, &graph, &cache);
	out.documents.insert(out.documents.end(), input.open_findings.begin(), input.open_findings.end());
	const ValidationInput validation{input.paths, input.project, input.scan, input.open, cache};
	render_check.update(validation, files);
	out.rows = input.scan.diagnostics;
	out.rows.insert(out.rows.end(), input.requirements.diagnostics.begin(), input.requirements.diagnostics.end());
	for (const std::string &name : input.boot_missing) out.rows.push_back(boot_finding(name));
	out.rows.insert(out.rows.end(), input.play.begin(), input.play.end());
	out.rows.insert(out.rows.end(), out.documents.begin(), out.documents.end());
	out.rows.insert(out.rows.end(), render_check.diagnostics().begin(), render_check.diagnostics().end());
	out.rows.insert(out.rows.end(), input.build.begin(), input.build.end());
	return out;
}

} // namespace opennova::editor
