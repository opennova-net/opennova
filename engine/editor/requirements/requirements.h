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
// a project file of that exact name and the expected kind. A file an expansion's project lacks that its base
// game serves is Served: the game reads the base's under /exp, so it is no file the project misses.
enum class RequirementState { Present, Missing, WrongKind, Served };

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

// The counts are the Required rows': missing (neither the project nor a base game has it), of the wrong
// kind, and served (the project lacks it, its base game serves it). The findings: an error per unmet
// Required row (`requirement.missing` is the project's, `requirement.wrong_kind` the offending file's), a
// note for one the base serves, and a note per optional row the project lacks
// (`requirement.optional_missing`, what the game does without it), each naming its row's role and file.
struct RequirementReport {
	std::vector<RequirementRow> rows; // manifest order (phase-major)
	int required_total = 0;
	int required_missing = 0;
	int required_wrong_kind = 0;
	int required_served = 0;
	std::vector<Diagnostic> diagnostics;
};

bool requirement_phase_enabled(const ProjectDocument &doc, int phase);
// Whether the manifest's row of `role` is a file the game reads for multiplayer alone, which the project's
// Multiplayer feature puts on the checklist: mp.mnu, the NovaWorld screens the game loads as a player enters
// NovaWorld or comes back to it from a game [orig: UI_EnterNovaWorldMenu @ 0x5588fa, from
// Menu_InitShellResources @ 0x5526c2 and UI_NWMultiPlayer_OnBack @ 0x558cf9]; the mission music GAMEMUS (and
// an expansion's G<n> in its place), which a mission opens only in a multiplayer session (a single-player
// mission stops the music context) [orig: Game_StartMission @ 0x525581..0x5255ae].
bool requirement_multiplayer_only(const std::string &role);
// Whether the project's checklist has the manifest's row: its phase on (requirement_phase_enabled), and a
// multiplayer-only row's Multiplayer feature on.
bool requirement_row_enabled(const ProjectDocument &doc, const gameprofile::RequiredResource &resource);
const char *requirement_phase_label(int phase);

// Beside the rows, a project's expansion (ADR 0046 S16): weighed against `install_expansions` (the
// game install's, vfs_list_expansions; null when no install is set) as listed warnings
// (expansion_install_findings: a name the install has, one to build on it lacks); and the project's
// files the game never reads for its expansion setting (`expansion.file.unread`, a warning on the
// file, its fix a Rename): a music bank by any name but the pair's the game streams (M<n>.sbf and
// G<n>.sbf for an expansion, MENUMUS.SBF and GAMEMUS.SBF for the base game [orig: Expansion_LoadAssets
// @ 0x4a4798/@ 0x4a4906..0x4a4936]), and an expansion's base music scripts, MENUMUS.BIN and GAMEMUS.BIN,
// which the game reads M<n>.bin and G<n>.bin in place of under /exp [orig: @ 0x4a491d, @ 0x4a494a].
// `base_names`: for a project that builds as an expansion, the names its base game serves (sorted as
// reference_kinds.h's BaseNames reads them): a required file the project lacks that the base serves is
// said so (the game reads the base's; the build's gate lets it through), never what the game does
// without it.
RequirementReport evaluate_requirements(const ProjectDocument &doc, const AssetScan &scan,
                                        const std::vector<std::string> *install_expansions = nullptr,
                                        const std::vector<std::string> *base_names = nullptr);

// The roles of the Required rows the project does not meet (missing, or of the wrong
// kind), in manifest order: what "create every missing required file" names (the command
// line's create-missing, the editor's Create all). A wrong-kind row is refused there,
// never overwritten; a row the base game serves is met (named alone, it is made: the project's own
// then stands over the base's).
std::vector<std::string> unmet_required_roles(const RequirementReport &report);

} // namespace opennova::editor
