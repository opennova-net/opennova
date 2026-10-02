#include <editor/project_build/build_plan.h>

#include <algorithm>
#include <filesystem>

#include <editor/assets/player_files.h>
#include <editor/model/diagnostic.h>

namespace fs = std::filesystem;

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
		entry.source_path = (fs::path(paths.root) / asset.relative_path).generic_string();
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

} // namespace opennova::editor
