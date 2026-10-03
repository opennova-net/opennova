#include <editor/project_build/build_plan.h>

#include <algorithm>

#include <base/gameprofile/required_resources.h>
#include <editor/assets/player_files.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/model/field_text.h>
#include <editor/project/project_files.h>
#include <editor/requirements/requirement_words.h>


namespace opennova::editor {

BuildPlan plan_build(const ProjectPaths &paths, const AssetScan &scan, const RequirementReport &requirements,
                     const std::vector<Diagnostic> &document_findings) {
	BuildPlan plan;
	plan.hash_cache = paths.build_cache_file;
	// The three boot-table archives always exist in a build, even empty: the boot gate
	// counts archives opened, not entries [orig: fatal check @ 0x4a6f44], and retail
	// ships all three.
	for (const ArchiveSlot slot : {ArchiveSlot::Language, ArchiveSlot::Localres, ArchiveSlot::Resource}) {
		BuildArchive archive;
		archive.slot = slot;
		archive.file_name = archive_slot_file_name(slot);
		plan.archives.push_back(std::move(archive));
	}

	// The same document gate CLI validate and the editor's Problems show.
	for (const Diagnostic &d : document_findings) plan.diagnostics.push_back(d);
	// The gate: a broken tree or an unmet requirement never packs.
	for (const Diagnostic &d : scan.diagnostics) {
		if (d.severity == DiagnosticSeverity::Error) plan.diagnostics.push_back(d);
	}
	// An unmet Required row blocks; an optional file the project lacks is a note the game
	// does without, never the build's business.
	for (const Diagnostic &d : requirements.diagnostics)
		if (d.severity == DiagnosticSeverity::Error) plan.diagnostics.push_back(d);

	for (const AssetEntry &asset : scan.entries) {
		if (asset.kind == AssetKind::Archive) {
			plan.diagnostics.push_back(make_finding(
			        CoreFinding::BuildArchiveInProject, DiagnosticSeverity::Error,
			        asset.logical_name + " is an archive; the build packs the project's files itself, so "
			                             "unpack it into the project or remove it.",
			        asset.relative_path));
			continue;
		}
		// The player's or this machine's own file (a save, a configuration, the stored credentials,
		// what the game writes) is never packed: said, and left out (ADR 0046 S14,
		// assets/player_files.h).
		if (is_player_file(asset.logical_name)) {
			plan.diagnostics.push_back(make_finding(
			        CoreFinding::BuildPlayerFile, DiagnosticSeverity::Warning,
			        asset.logical_name + " is " + player_file_words(asset.logical_name) +
			                ": a build never packs the player's own files, so it is left out.",
			        asset.relative_path));
			continue;
		}
		// An import source (its outputs, named after it, are in the scan) and a file of no kind the
		// game knows, which the game never asks for (S13 A8), are left out.
		if (!asset_kind_packed(asset.kind)) continue;
		BuildEntry entry;
		entry.logical_name = asset.logical_name;
		entry.source_path = join_path(paths.root, asset.relative_path);
		entry.size_bytes = asset.size_bytes;
		const ArchiveSlot slot = route_asset(asset);
		if (slot == ArchiveSlot::Loose) {
			plan.loose.push_back(std::move(entry));
			continue;
		}
		if (!logical_name_fits_archive(asset.logical_name)) {
			plan.diagnostics.push_back(make_finding(
			        CoreFinding::BuildNameUnstorable, DiagnosticSeverity::Error,
			        "The game cannot store " + asset.logical_name + " in an archive (the name is too long).",
			        asset.relative_path));
			continue;
		}
		for (BuildArchive &archive : plan.archives) {
			if (archive.slot == slot) archive.entries.push_back(std::move(entry));
		}
	}
	for (BuildArchive &archive : plan.archives) {
		std::sort(archive.entries.begin(), archive.entries.end(), [](const BuildEntry &a, const BuildEntry &b) {
			return normalized_logical_name(a.logical_name) < normalized_logical_name(b.logical_name);
		});
	}
	// An error whose code gates refuses the build (blocks_build): a missing reference is listed
	// among the plan's findings and refuses nothing (ADR 0046 S14).
	plan.ok = !diagnostics_block_build(plan.diagnostics);
	return plan;
}

std::vector<Diagnostic> build_blockers(const BuildPlan &plan) {
	std::vector<Diagnostic> out;
	for (const Diagnostic &d : plan.diagnostics)
		if (blocks_build(d)) out.push_back(d);
	return out;
}

namespace {

// A sentence as the tail of another: its first letter lowered, its full stop dropped.
std::string clause(std::string sentence) {
	if (!sentence.empty() && sentence.front() >= 'A' && sentence.front() <= 'Z')
		sentence.front() = static_cast<char>(sentence.front() - 'A' + 'a');
	while (!sentence.empty() && (sentence.back() == '.' || sentence.back() == ' ')) sentence.pop_back();
	return sentence;
}

} // namespace

std::string blocker_words(const Diagnostic &d) {
	if (const RequirementSubject *requirement = requirement_subject(d)) {
		const std::string without = requirement_without(requirement->role);
		const std::string what = d.row() == &finding_code(CoreFinding::RequirementWrongKind)
		                                 ? requirement->target + " is not the kind of file the game reads there"
		                                 : requirement->target + " is missing";
		return without.empty() ? what : what + ": " + clause(without);
	}
	return clause(d.message);
}

std::string blocker_reason(const Diagnostic &d) {
	if (const RequirementSubject *requirement = requirement_subject(d)) {
		const gameprofile::RequiredResource *row = gameprofile::gameprofile_required_resource_by_role(requirement->role.c_str());
		const std::string without = requirement_without(requirement->role);
		return "The game stops here as the original does: " + (without.empty() ? std::string("it cannot start") : clause(without)) +
		       (row && row->orig ? std::string(" ") + row->orig : std::string()) + ".";
	}
	if (const ReferenceSubject *reference = reference_subject(d))
		if (const char *orig = reference_row(reference->kind).gates_when_missing)
			return std::string("The game refuses to go on without it, as the original does ") + orig + ".";
	return "The editor does not pack what it cannot vouch for: it cannot read, write or store this as it is.";
}

std::string refusal_words(const std::vector<Diagnostic> &blockers) {
	constexpr size_t kNamed = 3;
	std::string out = "The build was refused: " + counted(blockers.size(), "problem") +
	                  (blockers.size() == 1 ? " stops" : " stop") + " it";
	for (size_t i = 0; i < blockers.size() && i < kNamed; ++i) out += (i == 0 ? ": " : "; ") + blocker_words(blockers[i]);
	if (blockers.size() > kNamed) out += "; and " + std::to_string(blockers.size() - kNamed) + " more";
	return out + ". Problems marks them \"Blocks the build\".";
}

} // namespace opennova::editor
