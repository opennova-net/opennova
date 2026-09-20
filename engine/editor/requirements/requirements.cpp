#include <editor/requirements/requirements.h>

#include <editor/assets/asset_type_registry.h>

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

		if (row.required) {
			++report.required_total;
			if (row.state == RequirementState::Missing) {
				++report.required_missing;
				report.diagnostics.push_back(make_diagnostic(
				        DiagnosticSeverity::Error, "requirement.missing",
				        "Missing required file " + row.name + " (" + requirement_phase_label(row.phase) +
				                "). Without it: " + resource->failure + ".",
				        row.name));
			} else if (row.state == RequirementState::WrongKind) {
				++report.required_wrong_kind;
				report.diagnostics.push_back(make_diagnostic(
				        DiagnosticSeverity::Error, "requirement.wrong_kind",
				        row.name + " is present but is not a " +
				                std::string(asset_kind_label(row.expected_kind)) + " (found: " +
				                asset_kind_label(row.found_kind) + ").",
				        row.asset_path));
			}
		}
		report.rows.push_back(std::move(row));
	}
	return report;
}

} // namespace opennova::editor
