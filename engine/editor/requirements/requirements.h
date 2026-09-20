#pragma once

#include <string>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// The requirements checklist (ADR 0046 d7): the files the engine demands by name, from
// the witnessed manifest (engine/base/gameprofile/required_resources), evaluated against
// the project's assets. BOOT and MENU rows always apply; MISSION rows when the project
// enables missions. Archive-table rows are build outputs and pattern rows are not files,
// so neither appears. The engine's names stay fixed (ADR 0046 d5): a row is satisfied by
// a project file of that exact name and the expected kind.
enum class RequirementState { Present, Missing, WrongKind };

struct RequirementRow {
	const gameprofile::RequiredResource *resource = nullptr; // the manifest row
	std::string role;      // the row's stable token
	std::string name;      // the file the engine requires
	int phase = 0;         // gameprofile::BootPhase
	int severity = 0;      // gameprofile::ResourceSeverity
	bool required = false; // every severity but OPTIONAL
	AssetKind expected_kind = AssetKind::Unknown;
	RequirementState state = RequirementState::Missing;
	std::string asset_path;                // the matching project file when one exists
	AssetKind found_kind = AssetKind::Unknown;
};

struct RequirementReport {
	std::vector<RequirementRow> rows; // manifest order (phase-major)
	int required_total = 0;
	int required_missing = 0;
	int required_wrong_kind = 0;
	std::vector<Diagnostic> diagnostics; // one error per unmet Required row
};

bool requirement_phase_enabled(const ProjectDocument &doc, int phase);
const char *requirement_phase_label(int phase);

RequirementReport evaluate_requirements(const ProjectDocument &doc, const AssetScan &scan);

} // namespace opennova::editor
