#include <editor/project_build/build_plan.h>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace opennova::editor {

BuildPlan plan_build(const ProjectPaths &paths, const AssetScan &scan, const RequirementReport &requirements,
                     const std::vector<Diagnostic> &document_findings) {
	BuildPlan plan;
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
		if (asset.kind == AssetKind::ImageSource) continue; // its outputs are in the scan
		if (!asset_is_packable(asset)) {
			plan.diagnostics.push_back(make_diagnostic(
			        DiagnosticSeverity::Error, "build.archive_in_project",
			        asset.logical_name + " is an archive; the build packs the project's files itself, so "
			                             "unpack it into the project or remove it.",
			        asset.relative_path));
			continue;
		}
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
			plan.diagnostics.push_back(make_diagnostic(
			        DiagnosticSeverity::Error, "build.name_unstorable",
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
	plan.ok = !diagnostics_have_errors(plan.diagnostics);
	return plan;
}

} // namespace opennova::editor
