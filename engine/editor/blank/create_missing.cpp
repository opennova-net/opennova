#include <editor/blank/create_missing.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <editor/blank/blank_factory.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

CreateMissingResult create_missing_requirements(const ProjectPaths &paths, const ProjectDocument &doc,
                                                const RequirementReport &report, const std::vector<std::string> &roles) {
	CreateMissingResult result;
	const auto named = [&roles](const std::string &role) { return std::find(roles.begin(), roles.end(), role) != roles.end(); };
	for (const RequirementRow &row : report.rows) {
		if (!named(row.role)) continue;
		if (row.state == RequirementState::Present) {
			result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "create_missing.exists",
			                                             row.name + " is in the project already (" + row.asset_path +
			                                                     "): nothing was created.",
			                                             row.asset_path));
			continue;
		}
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
		const std::string dir = blank_placement_dir(row.expected_kind);
		const std::string relative = dir.empty() ? row.name : dir + "/" + row.name;
		const fs::path target = fs::path(paths.root) / relative;
		// The report may be older than the tree: a file that has appeared where this one
		// would go since is left as it is.
		std::error_code ec;
		if (fs::exists(target, ec) || ec) {
			result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "create_missing.exists",
			                                             relative + " is on disk already: nothing was created. Refresh to "
			                                                        "see it.",
			                                             relative));
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
		std::string io_error;
		if (!ensure_directory(target.parent_path().generic_string(), io_error) ||
		    !write_file_atomic(target.generic_string(), bytes.data(), bytes.size(), io_error)) {
			result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "create_missing.write",
			                                             io_error, row.name));
			continue;
		}
		result.created.push_back(relative);
	}
	// A role no row has (none of that name, or a row of a phase the project leaves off),
	// reported once however often it is named.
	for (auto role = roles.begin(); role != roles.end(); ++role) {
		const bool known = std::any_of(report.rows.begin(), report.rows.end(),
		                               [&role](const RequirementRow &row) { return row.role == *role; });
		if (known || std::find(roles.begin(), role, *role) != role) continue;
		result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "create_missing.unknown",
		                                             "No required file of this project has the role '" + *role + "'."));
	}
	return result;
}

} // namespace opennova::editor
