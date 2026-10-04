#include <editor/project_build/build_plan.h>

#include <algorithm>

#include <base/gameprofile/required_resources.h>
#include <base/io/strutil.h>

#include <editor/assets/player_files.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/model/field_text.h>
#include <editor/project/expansion_files.h>
#include <editor/project/project_files.h>
#include <editor/requirements/requirement_words.h>
#include <formats/rtxt/rtxt.h>


namespace opennova::editor {

namespace {

// Where a file goes in the build (an archive's slot, loose, or nowhere), and, for a loose one, its
// path there.
struct Placement {
	ArchiveSlot slot = ArchiveSlot::None;
	std::string loose_path; // a loose file's ("" for none)
	bool root_only = false; // an expansion's game reads it only from the install's folder
};

Placement place(const AssetEntry &asset, const BuildTarget &target) {
	Placement out;
	if (!target.is_expansion()) {
		out.slot = route_asset(asset);
		if (out.slot == ArchiveSlot::Loose) out.loose_path = asset.logical_name;
		return out;
	}
	switch (route_for_expansion(asset, target.expansion)) {
	case ExpansionPlace::LanguageArchive: out.slot = ArchiveSlot::Language; break;
	case ExpansionPlace::Archive: out.slot = ArchiveSlot::Localres; break; // <b>.pff, the plan's Localres archive
	case ExpansionPlace::Folder:
		out.slot = ArchiveSlot::Loose;
		out.loose_path = expansion_folder(target.expansion) + "/" + asset.logical_name;
		break;
	case ExpansionPlace::RootOnly: out.root_only = true; break;
	case ExpansionPlace::None: break;
	}
	return out;
}


// The plan's own finding about a file of the scan, false for one it packs or leaves out without a word: an
// archive (the build packs the project's files itself); the player's or this machine's own file (a save, a
// configuration, the stored credentials, what the game writes: never packed, ADR 0046 S14,
// assets/player_files.h); a NovaWorld screen of a name no archive holds, which the game never reads (left
// out); a file an archive packs whose name no archive can store (where `target` places it: the standalone
// game's archives, or an expansion's, ADR 0046 S16).
bool own_finding(const AssetEntry &asset, const BuildTarget &target, Diagnostic &out) {
	if (asset.kind == AssetKind::Archive) {
		out = make_finding(CoreFinding::BuildArchiveInProject, DiagnosticSeverity::Error,
		                   asset.logical_name + " is an archive; the build packs the project's files itself, so "
		                                        "unpack it into the project or remove it.",
		                   asset.relative_path);
		return true;
	}
	if (is_player_file(asset.logical_name)) {
		out = make_finding(CoreFinding::BuildPlayerFile, DiagnosticSeverity::Warning,
		                   asset.logical_name + " is " + player_file_words(asset.logical_name) +
		                           ": a build never packs the player's own files, so it is left out.",
		                   asset.relative_path);
		return true;
	}
	// A NovaWorld screen is read through the archives alone (unless /d) by its name: nw_startup.mnx and
	// nw_error.mnx [orig: UI_EnterNovaWorldMenu @ 0x558937; UI_ShowNovaWorldErrorMessage @ 0x558449], and
	// the page an ACTION of type MNX names [orig: CUIWidget_HandleScriptedAction @ 0x649bb2]. A name no
	// archive can store is one the game never reads: such a page (a template, a backup) is left out and
	// said, never gating the build as an archived kind's name would (ADR 0046 S16).
	if (asset.kind == AssetKind::NovaWorldScreen && !logical_name_fits_archive(asset.logical_name)) {
		out = make_finding(CoreFinding::BuildUnread, DiagnosticSeverity::Warning,
		                   "The game reads a NovaWorld screen through its archives alone, and no archive can hold the "
		                   "name " + asset.logical_name + " (it is too long): the build leaves it out.",
		                   asset.relative_path);
		return true;
	}
	const Placement placement = asset_kind_packed(asset.kind) ? place(asset, target) : Placement();
	if (placement.slot != ArchiveSlot::None && placement.slot != ArchiveSlot::Loose &&
	    !logical_name_fits_archive(asset.logical_name)) {
		out = make_finding(CoreFinding::BuildNameUnstorable, DiagnosticSeverity::Error,
		                   "The game cannot store " + asset.logical_name + " in an archive (the name is too long).",
		                   asset.relative_path);
		return true;
	}
	return false;
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

std::vector<Diagnostic> plan_scan_findings(const AssetScan &scan, const std::string &expansion) {
	std::vector<Diagnostic> out;
	BuildTarget target;
	target.expansion = expansion;
	for (const AssetEntry &asset : scan.entries) {
		Diagnostic own;
		if (own_finding(asset, target, own)) out.push_back(std::move(own));
	}
	return out;
}

std::string build_place_words(const AssetEntry &asset, const std::string &expansion) {
	BuildTarget target;
	target.expansion = expansion;
	Diagnostic own;
	if (own_finding(asset, target, own)) return own.message;
	if (!asset_kind_packed(asset.kind))
		return asset.kind == AssetKind::ImportSource
		               ? "A build leaves it out: the files its import makes are packed in its place."
		               : "A build leaves it out: the game never asks for a file of its kind.";
	const Placement placement = place(asset, target);
	if (placement.root_only)
		return "A build of the expansion leaves it out: the game reads it from the install's own folder alone.";
	switch (placement.slot) {
	case ArchiveSlot::Language:
	case ArchiveSlot::Localres:
	case ArchiveSlot::Resource:
		return "A build packs it into " +
		       (target.is_expansion() ? expansion_archive_path(expansion, placement.slot == ArchiveSlot::Language)
		                              : std::string(archive_slot_file_name(placement.slot))) +
		       ".";
	case ArchiveSlot::Loose: return "A build copies it to " + placement.loose_path + ", where the game reads it loose.";
	case ArchiveSlot::None: break;
	}
	return "A build leaves it out: the game never asks for a file of its kind.";
}

bool lists_as_mission(const std::string &name) {
	return strutil::ends_with_icase(name, ".bms") || strutil::ends_with_icase(name, ".npj") ||
	       strutil::ends_with_icase(name, ".npz");
}

BuildPlan plan_build(const ProjectPaths &paths, const AssetScan &scan, const RequirementReport &requirements,
                     const std::vector<Diagnostic> &document_findings, const BuildTarget &target,
                     const BaseNames *base, const ShippedFiles *shipped) {
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
		// The plan's own word on the file (an archive, a player's file, a name no archive stores), once: the
		// gate holds it already when the Problems rows were composed over this scan.
		Diagnostic own;
		if (own_finding(asset, target, own)) {
			if (std::find(document_findings.begin(), document_findings.end(), own) == document_findings.end())
				plan.diagnostics.push_back(std::move(own));
			continue;
		}
		// An import source (its outputs, named after it, are in the scan) and a file of no kind the
		// game knows, which the game never asks for (S13 A8), are left out.
		if (!asset_kind_packed(asset.kind)) continue;
		const Placement placement = place(asset, target);
		BuildEntry entry;
		entry.logical_name = asset.logical_name;
		entry.source_path = join_path(paths.root, asset.relative_path);
		entry.relative_path = asset.relative_path;
		entry.size_bytes = asset.size_bytes;
		entry.kind = asset.kind;
		if (placement.root_only) {
			// Left out; the build compares it with the base's and says so where they differ
			// (build.expansion.root_only).
			entry.build_path = asset.relative_path;
			plan.root_only.push_back(std::move(entry));
			continue;
		}
		const ArchiveSlot slot = placement.slot;
		if (slot == ArchiveSlot::Loose) {
			entry.build_path = placement.loose_path;
			plan.loose.push_back(std::move(entry));
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
	// gating reference the base serves refuses nothing either (S16, BaseNames), nor does a finding that
	// the game's own bytes, packed as stored, do not serialize (ShippedFiles).
	for (const Diagnostic &d : plan.diagnostics)
		if (blocks_build(d, target.is_expansion() ? base : nullptr, shipped)) plan.blockers.push_back(d);
	plan.ok = plan.blockers.empty();
	return plan;
}

std::vector<Diagnostic> build_blockers(const BuildPlan &plan) { return plan.blockers; }

namespace {

// A sentence as the tail of another, its full stop dropped: its first letter lowered only where its first
// word is a word a sentence starts with ("The", "A", "This"), never a name ("RESOURCE.PFF is ...").
std::string clause(std::string sentence) {
	static const char *const kOpeners[] = {"The", "A", "An", "This", "These", "That", "It", "Its",
	                                       "No", "One", "Every", "Some", "Nothing", "Without"};
	const std::string first = sentence.substr(0, sentence.find(' '));
	for (const char *opener : kOpeners)
		if (first == opener) {
			sentence.front() = static_cast<char>(sentence.front() - 'A' + 'a');
			break;
		}
	while (!sentence.empty() && (sentence.back() == '.' || sentence.back() == ' ')) sentence.pop_back();
	return sentence;
}

bool wrong_kind(const Diagnostic &d) { return d.row() == &finding_code(CoreFinding::RequirementWrongKind); }

} // namespace

std::string blocker_words(const Diagnostic &d) {
	if (const RequirementSubject *requirement = requirement_subject(d)) {
		// A file of another kind under a fatal row's name is read as one without a check: no message, no exit.
		if (wrong_kind(d))
			return requirement->target + " is not the kind of file the game reads there: the game reads it as one "
			                             "without checking it, so it may crash or show garbage";
		const std::string without = requirement_without(requirement->role);
		const std::string what = requirement->target + " is missing";
		return without.empty() ? what : what + ": " + clause(without);
	}
	// The plan's own refusals, in its words: what the file is and why it does not pack.
	const size_t slash = d.asset.find_last_of('/');
	const std::string name = slash == std::string::npos ? d.asset : d.asset.substr(slash + 1);
	if (d.row() == &finding_code(CoreFinding::BuildArchiveInProject))
		return name + " is an archive in the project: the build packs the project's files itself";
	if (d.row() == &finding_code(CoreFinding::BuildNameUnstorable))
		return name + "'s name is too long for an archive";
	return clause(d.message);
}

bool blocker_is_the_games(const Diagnostic &d) {
	if (requirement_subject(d)) return true;
	if (const ReferenceSubject *reference = reference_subject(d)) return reference_row(reference->kind).gates_when_missing;
	return false;
}

std::string blocker_reason(const Diagnostic &d) {
	if (const RequirementSubject *requirement = requirement_subject(d)) {
		if (wrong_kind(d))
			return "The game reads it without a check, as the original does: its header's offsets are taken as they "
			       "are [orig: TextResource_FixupPointers @ 0x75d050], so a file of another kind may crash it or show "
			       "garbage.";
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
