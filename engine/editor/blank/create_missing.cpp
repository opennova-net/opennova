#include <editor/blank/create_missing.h>

#include <filesystem>

#include <editor/blank/blank_factory.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

CreateMissingResult create_missing_requirements(const ProjectPaths &paths, const ProjectDocument &doc,
                                                const RequirementReport &report,
                                                std::string_view only_role) {
	CreateMissingResult result;
	for (const RequirementRow &row : report.rows) {
		const bool named = !only_role.empty() && row.role == only_role;
		if (!only_role.empty() && !named) continue;
		if (!row.required && !named) continue;
		if (row.state == RequirementState::Present) continue;
		if (row.state == RequirementState::WrongKind) {
			result.diagnostics.push_back(make_diagnostic(
			        DiagnosticSeverity::Error, "create_missing.wrong_kind",
			        row.name + " exists but is not a " + asset_kind_label(row.expected_kind) +
			                "; fix or remove that file first.",
			        row.asset_path));
			continue;
		}
		// A requirement's file has one meaning, so only the factory made for that
		// role fills it; the kind fallback (any menu, any stylesheet) is for free-form
		// "new file of this kind" creation, never for a row it could mislabel.
		const BlankFactory *factory = find_blank_factory_for_role(row.role);
		if (factory == nullptr) {
			result.unavailable.push_back(row.name);
			continue;
		}
		BlankRequest request;
		request.logical_name = row.name;
		request.role = row.role;
		request.project_title = doc.title;
		std::vector<uint8_t> bytes;
		Diagnostic error;
		if (!factory->make(request, bytes, error)) {
			result.diagnostics.push_back(error);
			continue;
		}
		const std::string dir = blank_placement_dir(row.expected_kind);
		const fs::path target = dir.empty() ? fs::path(paths.root) / row.name
		                                    : fs::path(paths.root) / dir / row.name;
		std::string io_error;
		if (!ensure_directory(target.parent_path().generic_string(), io_error) ||
		    !write_file_atomic(target.generic_string(), bytes.data(), bytes.size(), io_error)) {
			result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "create_missing.write",
			                                             io_error, row.name));
			continue;
		}
		result.created.push_back(dir.empty() ? row.name : dir + "/" + row.name);
	}
	return result;
}

} // namespace opennova::editor
