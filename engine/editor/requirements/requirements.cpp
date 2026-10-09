#include <editor/requirements/requirements.h>

#include <base/io/strutil.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/requirements/requirement_words.h>
#include <editor/project/expansion_files.h>
#include <editor/project/expansion_name.h>

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
	// When the game reads it, not that it cannot go on without it: what it does then is the row's
	// own words (requirement_without), and only the manifest's fatal rows stop it.
	case BOOT_PHASE_BOOT: return "read as the game starts";
	case BOOT_PHASE_MENU: return "read by the main menu";
	case BOOT_PHASE_MISSION: return "read as a mission starts";
	default: return "";
	}
}

namespace {

// The project's files the game never reads for its expansion setting (ADR 0046 S16): a music bank but
// the pair the game streams, and under an expansion the base game's music scripts.
void unread_expansion_files(const ProjectDocument &doc, const AssetScan &scan, std::vector<Diagnostic> &out) {
	const bool expansion = !doc.expansion.standalone();
	const std::string menu = expansion ? expansion_file_name(expansion_file_row(ExpansionFileRole::MenuMusicBank), doc.expansion.name)
	                                   : expansion_file_row(ExpansionFileRole::MenuMusicBank).replaces();
	const std::string game = expansion ? expansion_file_name(expansion_file_row(ExpansionFileRole::GameMusicBank), doc.expansion.name)
	                                   : expansion_file_row(ExpansionFileRole::GameMusicBank).replaces();
	for (const AssetEntry &entry : scan.entries) {
		std::string read_instead;
		if (entry.kind == AssetKind::MusicBank && !strutil::iequals(entry.logical_name, menu) &&
		    !strutil::iequals(entry.logical_name, game))
			read_instead = "the game streams music from " + menu + " and " + game + " alone";
		else if (expansion && entry.kind == AssetKind::MusicScript)
			for (const ExpansionFileRole role : { ExpansionFileRole::MenuMusicScript, ExpansionFileRole::GameMusicScript }) {
				const ExpansionFileRow &row = expansion_file_row(role);
				if (strutil::iequals(entry.logical_name, row.replaces()))
					read_instead = "under /exp " + doc.expansion.name + " the game reads " +
					               expansion_file_name(row, doc.expansion.name) + " in its place";
			}
		if (read_instead.empty()) continue;
		out.push_back(make_finding(CoreFinding::ExpansionFileUnread, DiagnosticSeverity::Warning,
		                           "The game never reads " + entry.logical_name + ": " + read_instead +
		                                   ". Rename it to be read, or remove it.",
		                           entry.relative_path));
	}
}

} // namespace

RequirementReport evaluate_requirements(const ProjectDocument &doc, const AssetScan &scan,
                                        const std::vector<std::string> *install_expansions,
                                        const std::vector<std::string> *base_names) {
	RequirementReport report;
	const BaseNames base{base_names && !base_names->empty() ? base_names : nullptr};
	const int count = gameprofile_required_resource_count();
	for (int i = 0; i < count; ++i) {
		const RequiredResource *resource = gameprofile_required_resource_at(i);
		// An expansion's own file is a row of a project that builds as one, by the name its expansion
		// forms (ADR 0046 S16, expansion_files.h); a project of the base game has none.
		const ExpansionFileRow *expansion_file = nullptr;
		if (resource->flags & RES_F_EXPANSION) {
			if (doc.expansion.standalone()) continue;
			expansion_file = expansion_file_row_for_manifest_role(resource->role);
			if (!expansion_file) continue;
		} else if (resource->flags & (RES_F_PATTERN | RES_F_PFF_TABLE_ANY | RES_F_PLAYER_FILE)) {
			// A pattern, a boot archive, or the player's own file (a save, a configuration: never a
			// project's, ADR 0046 S14) is no row of the checklist.
			continue;
		} else if (!doc.expansion.standalone()) {
			// A file the game reads in place of the base's under /exp (MENUMUS.SBF/.BIN, GAMEMUS.SBF/.BIN:
			// M<n>.* and G<n>.* take their place [orig: Expansion_LoadAssets @ 0x4a4906..0x4a494a]) is no
			// row of an expansion's checklist: the game never reads it there.
			if (gameprofile::gameprofile_replaced_under_expansion(resource->name)) continue;
		}
		if (!requirement_phase_enabled(doc, resource->phase)) continue;

		RequirementRow row;
		row.resource = resource;
		row.role = resource->role;
		row.name = expansion_file ? expansion_file_name(*expansion_file, doc.expansion.name) : resource->name;
		row.phase = resource->phase;
		row.severity = resource->severity;
		row.required = resource->severity != RES_OPTIONAL;
		row.expected_kind = expansion_file ? expansion_file->kind : file_kind_for_required_name(row.name);
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
		// What the game does without it, in plain words (requirement_words.h; the manifest's own record
		// of it is the cited detail, requirement_witness).
		const std::string without = requirement_without(row.role);
		// A file an expansion's base game serves: the game reads the base's under /exp [orig:
		// PFF_OpenAllArchives @ 0x4a4310, slots 2..4], as the build's gate lets it through (blocks_build
		// over BaseNames): said so, never what the game does with no file (the demo round's bug 6).
		const bool served = row.state == RequirementState::Missing && base.has(row.name);
		const std::string then = served ? " The game reads the base game's, which the expansion builds on."
		                         : without.empty() ? std::string(" The game reads it by name.")
		                                           : " " + without;
		if (row.required) {
			++report.required_total;
			if (row.state == RequirementState::Missing) {
				++report.required_missing;
				// One the base game serves is no file the game goes without: a note, never an error (the
				// game reads the base's, and the build's gate lets it through).
				report.diagnostics.push_back(finding(served ? DiagnosticSeverity::Info : DiagnosticSeverity::Error,
				                                     CoreFinding::RequirementMissing,
				                                     row.name + " is missing (" + requirement_phase_label(row.phase) + ")." +
				                                             then,
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
			                                     "Optional file " + row.name + " is not in the project." + then,
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
	// The project's expansion (ADR 0046 S16): against the game install's, listed; and the files its
	// setting leaves the game never reading.
	if (install_expansions)
		expansion_install_findings(doc.expansion, *install_expansions, DiagnosticSeverity::Warning, report.diagnostics);
	unread_expansion_files(doc, scan, report.diagnostics);
	return report;
}

std::vector<std::string> unmet_required_roles(const RequirementReport &report) {
	std::vector<std::string> roles;
	for (const RequirementRow &row : report.rows)
		if (row.required && row.state != RequirementState::Present) roles.push_back(row.role);
	return roles;
}

} // namespace opennova::editor
