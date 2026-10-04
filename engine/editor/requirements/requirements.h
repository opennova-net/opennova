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

// The counts are the Required rows'. The findings: an error per unmet Required row
// (`requirement.missing` is the project's, `requirement.wrong_kind` the offending file's)
// and a note per optional row the project lacks (`requirement.optional_missing`, what the
// game does without it), each naming its row's role and file.
struct RequirementReport {
	std::vector<RequirementRow> rows; // manifest order (phase-major)
	int required_total = 0;
	int required_missing = 0;
	int required_wrong_kind = 0;
	std::vector<Diagnostic> diagnostics;
};

bool requirement_phase_enabled(const ProjectDocument &doc, int phase);
const char *requirement_phase_label(int phase);

// Beside the rows, a project's expansion (ADR 0046 S16): weighed against `install_expansions` (the
// game install's, vfs_list_expansions; null when no install is set) as listed warnings
// (expansion_install_findings: a name the install has, one to build on it lacks); and the project's
// files the game never reads for its expansion setting (`expansion.file.unread`, a warning on the
// file, its fix a Rename): a music bank by any name but the pair's the game streams (M<n>.sbf and
// G<n>.sbf for an expansion, MENUMUS.SBF and GAMEMUS.SBF for the base game [orig: Expansion_LoadAssets
// @ 0x4a4798/@ 0x4a4906..0x4a4936]), and an expansion's base music scripts, MENUMUS.BIN and GAMEMUS.BIN,
// which the game reads M<n>.bin and G<n>.bin in place of under /exp [orig: @ 0x4a491d, @ 0x4a494a].
RequirementReport evaluate_requirements(const ProjectDocument &doc, const AssetScan &scan,
                                        const std::vector<std::string> *install_expansions = nullptr);

// The roles of the Required rows the project does not meet (missing, or of the wrong
// kind), in manifest order: what "create every missing required file" names (the command
// line's create-missing, the editor's Create all). A wrong-kind row is refused there,
// never overwritten.
std::vector<std::string> unmet_required_roles(const RequirementReport &report);

} // namespace opennova::editor
