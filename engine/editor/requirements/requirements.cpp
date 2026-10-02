#include <editor/requirements/requirements.h>

#include <base/io/strutil.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

using namespace opennova::gameprofile;

bool requirement_phase_enabled(const ProjectDocument &doc, int phase) {
	switch (phase) {
	case BOOT_PHASE_BOOT:
	case BOOT_PHASE_MENU: return true;
	case BOOT_PHASE_MISSION: return doc.features.mission;
	default: return false;
	}
}

const char *requirement_phase_label(int phase) {
	switch (phase) {
	case BOOT_PHASE_BOOT: return "needed to start the game";
	case BOOT_PHASE_MENU: return "needed by the main menu";
	case BOOT_PHASE_MISSION: return "needed to start a mission";
	default: return "";
	}
}

RequirementReport evaluate_requirements(const ProjectDocument &doc, const AssetScan &scan) {
	RequirementReport report;
	const int count = gameprofile_required_resource_count();
	for (int i = 0; i < count; ++i) {
		const RequiredResource *resource = gameprofile_required_resource_at(i);
		if (resource->flags & (RES_F_PATTERN | RES_F_PFF_TABLE_ANY)) continue;
		if (!requirement_phase_enabled(doc, resource->phase)) continue;

		RequirementRow row;
		row.resource = resource;
		row.role = resource->role;
		row.name = resource->name;
		row.phase = resource->phase;
		row.severity = resource->severity;
		row.required = resource->severity != RES_OPTIONAL;
		row.expected_kind = expected_asset_kind_for_required_name(row.name);
		if (const AssetEntry *asset = scan.find(row.name)) {
			row.asset_path = asset->relative_path;
			row.found_kind = asset->kind;
			row.state = asset->kind == row.expected_kind ? RequirementState::Present
			                                             : RequirementState::WrongKind;
		} else {
			row.state = RequirementState::Missing;
		}

		// The finding names its row (the role, the required name); a missing file is the
		// project's finding, since no file of the project is at fault.
		const auto finding = [&row](DiagnosticSeverity severity, CoreFinding code, const std::string &message,
		                            const std::string &asset) {
			Diagnostic d = make_finding(code, severity, message, asset);
			d.subject = RequirementSubject{ row.role, row.name };
			return d;
		};
		if (row.required) {
			++report.required_total;
			if (row.state == RequirementState::Missing) {
				++report.required_missing;
				report.diagnostics.push_back(finding(DiagnosticSeverity::Error, CoreFinding::RequirementMissing,
				                                     "Missing required file " + row.name + " (" +
				                                             requirement_phase_label(row.phase) + "). Without it: " +
				                                             resource->failure + ".",
				                                     std::string()));
			} else if (row.state == RequirementState::WrongKind) {
				++report.required_wrong_kind;
				report.diagnostics.push_back(finding(DiagnosticSeverity::Error, CoreFinding::RequirementWrongKind,
				                                     row.name + " is present but is not a " +
				                                             std::string(asset_kind_label(row.expected_kind)) +
				                                             " (found: " + asset_kind_label(row.found_kind) + ").",
				                                     row.asset_path));
			}
		} else if (row.state == RequirementState::Missing) {
			// An optional file the game does without: a note that says how.
			report.diagnostics.push_back(finding(DiagnosticSeverity::Info, CoreFinding::RequirementOptionalMissing,
			                                     "Optional file " + row.name + " is not in the project (" +
			                                             requirement_phase_label(row.phase) + "). Without it: " +
			                                             resource->failure + ".",
			                                     std::string()));
		}
		report.rows.push_back(std::move(row));
	}
	// A mission in a project whose Missions feature is off (ADR 0046 S14): the files a mission needs
	// when it starts are not on the checklist, so nothing says which the project lacks. A warning on
	// the project, said once, never a build's gate.
	if (!doc.features.mission) {
		for (const AssetEntry &entry : scan.entries) {
			if (entry.kind != AssetKind::Mission || !strutil::ends_with_icase(entry.logical_name, ".bms")) continue;
			report.diagnostics.push_back(make_finding(
					CoreFinding::ProjectMissionFeatureOff, DiagnosticSeverity::Warning,
					"The project holds a mission (" + entry.logical_name +
							") while its Missions feature is off: the files a mission needs when it starts are "
							"not checked. Turn Missions on in File > Project settings..."));
			break;
		}
	}
	return report;
}

std::vector<std::string> unmet_required_roles(const RequirementReport &report) {
	std::vector<std::string> roles;
	for (const RequirementRow &row : report.rows)
		if (row.required && row.state != RequirementState::Present) roles.push_back(row.role);
	return roles;
}

} // namespace opennova::editor
