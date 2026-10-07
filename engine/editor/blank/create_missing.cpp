#include <editor/blank/create_missing.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <editor/assets/asset_kinds.h>
#include <editor/assets/project_layout.h>
#include <editor/blank/blank_factory.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

CreateMissingResult create_missing_requirements(const ProjectPaths &paths, const ProjectDocument &doc, const AssetScan &scan,
                                                const RequirementReport &report, const std::vector<std::string> &roles) {
	CreateMissingResult result;
	const auto named = [&roles](const std::string &role) { return std::find(roles.begin(), roles.end(), role) != roles.end(); };
	// A file a made blank names (blank_companion), made where the project keeps a file of its kind
	// (placement_path, as the blank itself) where the project has none of that name and none was made
	// this run, and its target is free; one that is not made leaves the blank made, and says why.
	const auto make_companion = [&](const BlankFactory &made_by, const std::string &made) {
		std::string name;
		const BlankFactory *factory = blank_companion(made_by, made, doc, name);
		if (factory == nullptr || scan.find(name)) return;
		const std::string relative = placement_path(scan, name, factory->kind);
		if (std::find(result.created.begin(), result.created.end(), relative) != result.created.end()) return;
		const fs::path target = path_of(paths.root) / path_of(relative);
		std::error_code ec;
		if (fs::exists(system_path(utf8_of(target)), ec) || ec) return; // the scan is older than the tree
		BlankRequest request;
		request.logical_name = name;
		request.role = factory->role;
		request.project_title = doc.title;
		std::vector<uint8_t> bytes;
		Diagnostic error;
		std::string io_error;
		if (!make_from(*factory, request, bytes, error)) {
			result.diagnostics.push_back(error);
			return;
		}
		if (!ensure_directory(utf8_of(target.parent_path()), io_error) ||
		    !write_file_atomic(utf8_of(target), bytes.data(), bytes.size(), io_error)) {
			result.diagnostics.push_back(make_finding(CoreFinding::CreateMissingWrite, DiagnosticSeverity::Error,
			                                          made + " names " + name + ", which was not made: " + io_error, name));
			return;
		}
		result.created.push_back(relative);
	};
	for (const RequirementRow &row : report.rows) {
		if (!named(row.role)) continue;
		if (row.state == RequirementState::Present) {
			result.diagnostics.push_back(make_finding(CoreFinding::CreateMissingExists, DiagnosticSeverity::Error,
			                                          row.name + " is in the project already (" + row.asset_path +
			                                                  "): nothing was created.",
			                                          row.asset_path));
			continue;
		}
		if (row.state == RequirementState::WrongKind) {
			result.diagnostics.push_back(make_finding(
			        CoreFinding::CreateMissingWrongKind, DiagnosticSeverity::Error,
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
		// Where the project keeps a file of its kind (assets/project_layout.h): the top level of a flat
		// project, beside its files of the kind, else the kind's folder.
		const std::string relative = placement_path(scan, row.name, row.expected_kind);
		const fs::path target = path_of(paths.root) / path_of(relative);
		// The report may be older than the tree: a file that has appeared where this one
		// would go since is left as it is.
		std::error_code ec;
		if (fs::exists(system_path(utf8_of(target)), ec) || ec) { // a project past MAX_PATH too
			result.diagnostics.push_back(make_finding(CoreFinding::CreateMissingExists, DiagnosticSeverity::Error,
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
		if (!make_from(*factory, request, bytes, error)) {
			result.diagnostics.push_back(error);
			continue;
		}
		std::string io_error;
		if (!ensure_directory(utf8_of(target.parent_path()), io_error) ||
		    !write_file_atomic(utf8_of(target), bytes.data(), bytes.size(), io_error)) {
			result.diagnostics.push_back(make_finding(CoreFinding::CreateMissingWrite, DiagnosticSeverity::Error,
			                                          io_error, row.name));
			continue;
		}
		result.created.push_back(relative);
		make_companion(*factory, row.name);
	}
	// A role no row has (none of that name, or a row of a phase the project leaves off),
	// reported once however often it is named.
	for (auto role = roles.begin(); role != roles.end(); ++role) {
		const bool known = std::any_of(report.rows.begin(), report.rows.end(),
		                               [&role](const RequirementRow &row) { return row.role == *role; });
		if (known || std::find(roles.begin(), role, *role) != role) continue;
		result.diagnostics.push_back(make_finding(CoreFinding::CreateMissingUnknown, DiagnosticSeverity::Error,
		                                          "No required file of this project has the role '" + *role + "'."));
	}
	return result;
}

} // namespace opennova::editor
