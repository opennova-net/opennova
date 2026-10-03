#include <editor/project_build/build_plan.h>

#include <algorithm>

#include <editor/assets/player_files.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/project/expansion_files.h>
#include <editor/project/project_files.h>
#include <formats/rtxt/rtxt.h>


namespace opennova::editor {

namespace {

// Where a file goes in the build (an archive's slot, loose, or nowhere), and, for a loose one, its
// path there.
struct Placement {
	ArchiveSlot slot = ArchiveSlot::None;
	std::string loose_path; // a loose file's, or an archived file's second, loose copy ("" for none)
	bool root_only = false; // an expansion's game reads it only from the install's folder
};

Placement place(const AssetEntry &asset, const BuildTarget &target) {
	Placement out;
	if (!target.is_expansion()) {
		out.slot = route_asset(asset);
		if (out.slot == ArchiveSlot::Loose) out.loose_path = asset.logical_name;
		return out;
	}
	bool also_loose = false;
	const std::string in_folder = expansion_folder(target.expansion) + "/" + asset.logical_name;
	switch (route_for_expansion(asset, target.expansion, also_loose)) {
	case ExpansionPlace::LanguageArchive: out.slot = ArchiveSlot::Language; break;
	case ExpansionPlace::Archive: out.slot = ArchiveSlot::Localres; break; // <b>.pff, the plan's Localres archive
	case ExpansionPlace::Folder:
		out.slot = ArchiveSlot::Loose;
		out.loose_path = in_folder;
		break;
	case ExpansionPlace::RootOnly: out.root_only = true; break;
	case ExpansionPlace::None: break;
	}
	if (also_loose) out.loose_path = in_folder;
	return out;
}

// What the Mods list copies an expansion's [exp_info] EXP_NAME and EXP_DESC into: the 64-byte name and
// the 272-byte description of its ExpansionRecord (stride 596: the name @+0, the folder's name @+0x40,
// the description @+0x144), each copied whole with no bound [orig: Expansion_ScanAndRegister @ 0x4a4598,
// @ 0x4a4612].
constexpr size_t kExpansionRecordName = 64;
constexpr size_t kExpansionRecordDescription = 272;

// The expansion's table (<b>.bin, its row of project/expansion_files) read as the Mods list reads it:
// a name of 64 bytes or more runs over the record's folder name, which choosing the expansion there
// then loads (build.expansion.exp_name, gating); a description of 272 bytes or more spills into the
// next record (build.expansion.exp_desc, listed). A table that does not read is its document's finding.
void check_expansion_table(const ProjectPaths &paths, const AssetScan &scan, const std::string &expansion,
                           std::vector<Diagnostic> &out) {
	for (const AssetEntry &asset : scan.entries) {
		const ExpansionFileRow *row = expansion_file_for(expansion, asset.logical_name);
		if (!row || row->role != ExpansionFileRole::Table) continue;
		rtxt::File table;
		std::string error;
		if (!rtxt::parse_file(join_path(paths.root, asset.relative_path), table, error)) return;
		const rtxt::Entry *name = table.find_in_section("exp_info", "EXP_NAME");
		if (name && name->text.size() >= kExpansionRecordName)
			out.push_back(make_finding(
			        CoreFinding::BuildExpansionExpName, DiagnosticSeverity::Error,
			        "The expansion's name in the Mods list, EXP_NAME, is " + std::to_string(name->text.size()) +
			                " bytes: the game copies it over the expansion's folder name past 63, so choosing it there "
			                "loads the base game. Shorten it to 63 bytes or fewer.",
			        asset.relative_path));
		const rtxt::Entry *description = table.find_in_section("exp_info", "EXP_DESC");
		if (description && description->text.size() >= kExpansionRecordDescription)
			out.push_back(make_finding(
			        CoreFinding::BuildExpansionExpDesc, DiagnosticSeverity::Warning,
			        "The expansion's description in the Mods list, EXP_DESC, is " +
			                std::to_string(description->text.size()) +
			                " bytes: past 271 the game's copy runs into the next expansion's entry, whose title then "
			                "shows in it.",
			        asset.relative_path));
		return;
	}
}

} // namespace

BuildPlan plan_build(const ProjectPaths &paths, const AssetScan &scan, const RequirementReport &requirements,
                     const std::vector<Diagnostic> &document_findings, const BuildTarget &target,
                     const BaseNames *base) {
	BuildPlan plan;
	plan.target = target;
	plan.hash_cache = paths.build_cache_file;
	const bool base_mounts = base && base->sorted && !base->sorted->empty();
	if (target.is_expansion() && !base_mounts) {
		// The expansion plays over the base game, which its build compares its files with and its gate
		// reads (lean packing, BaseNames): no base, no build the editor can vouch for.
		plan.diagnostics.push_back(make_finding(
		        CoreFinding::BuildExpansionBaseMissing, DiagnosticSeverity::Error,
		        target.install.empty()
		                ? "The project builds as the expansion " + target.expansion +
		                          ", which plays over the game install: choose its folder in File > Project settings..."
		                : "The project builds as the expansion " + target.expansion + ", which plays over the game "
		                                                                               "install, and " +
		                          target.install + " does not mount as the game: choose its folder in File > Project "
		                                           "settings..."));
	}
	if (target.is_expansion()) {
		// An expansion's two archives always exist in its build, even empty: the game opens the pair by
		// its name, and a missing <b>.pff is no expansion at all, the base game loading in its place
		// [orig: Expansion_LoadAssets @ 0x4a4767, @ 0x4a4775]. <b>.pff takes the localres and resource
		// slots' kinds alike (route_for_expansion).
		for (const bool language : {true, false}) {
			BuildArchive archive;
			archive.slot = language ? ArchiveSlot::Language : ArchiveSlot::Localres;
			archive.file_name = expansion_archive_path(target.expansion, language);
			plan.archives.push_back(std::move(archive));
		}
	} else {
		// The three boot-table archives always exist in a build, even empty: the boot gate
		// counts archives opened, not entries [orig: fatal check @ 0x4a6f44], and retail
		// ships all three.
		for (const ArchiveSlot slot : {ArchiveSlot::Language, ArchiveSlot::Localres, ArchiveSlot::Resource}) {
			BuildArchive archive;
			archive.slot = slot;
			archive.file_name = archive_slot_file_name(slot);
			plan.archives.push_back(std::move(archive));
		}
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
	// An expansion's name and description as the Mods list copies them (ADR 0046 S16).
	if (target.is_expansion()) check_expansion_table(paths, scan, target.expansion, plan.diagnostics);

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
		const Placement placement = place(asset, target);
		BuildEntry entry;
		entry.logical_name = asset.logical_name;
		entry.source_path = join_path(paths.root, asset.relative_path);
		entry.size_bytes = asset.size_bytes;
		entry.kind = asset.kind;
		if (placement.root_only) {
			// Left out; the build compares it with the base's and says so where they differ
			// (build.expansion.root_only).
			entry.build_path = asset.relative_path;
			plan.root_only.push_back(std::move(entry));
			continue;
		}
		if (!placement.loose_path.empty()) {
			BuildEntry loose = entry;
			loose.build_path = placement.loose_path;
			plan.loose.push_back(std::move(loose));
		}
		const ArchiveSlot slot = placement.slot;
		if (slot == ArchiveSlot::Loose) continue;
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
	// among the plan's findings and refuses nothing (ADR 0046 S14); an expansion's required file or
	// gating reference the base serves refuses nothing either (S16, BaseNames).
	plan.ok = !diagnostics_block_build(plan.diagnostics, target.is_expansion() ? base : nullptr);
	return plan;
}

} // namespace opennova::editor
