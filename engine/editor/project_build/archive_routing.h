#pragma once

#include <string>

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>

namespace opennova::editor {

// Where a project asset lands in a Play or Export build (ADR 0046 d8): the slot its kind's row
// names (ArchiveSlot, assets/asset_kinds.h).

// "language.pff", "localres.pff", "resource.pff"; "" for Loose and None.
const char *archive_slot_file_name(ArchiveSlot slot);

// The slot for an asset, which its kind decides (AssetKindRow::archive_slot). Three kinds have
// none, so no build packs them (asset_kind_packed): an Archive-kind file (a .pff inside the
// project), which the build reports and leaves out; an import source, which never packs itself,
// its outputs, named after it, landing where their own kinds' rows say; and a file of no kind the
// game knows, which the game never asks for (S13 A8).
ArchiveSlot route_asset(AssetKind kind);
ArchiveSlot route_asset(const AssetEntry &asset);

// Where a build of the project as the expansion `<b>` (ADR 0046 S16) puts a file: an expansion is
// the folder `expansion/<b>/` of an install, whose two archives the game opens under `/exp <b>` in
// the boot table's first two slots, `expansion\<b>\<b>L.pff` then `expansion\<b>\<b>.pff`, over the
// base game's three [orig: PFF_OpenAllArchives @ 0x4a4310, the name table @ 0x829f90; Expansion_LoadAssets
// @ 0x4a4730]. The mission list walks that pair as it walks the base's localres and language pair
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170, @ 0x5635a5..0x5635bb], so the kinds of the
// language slot go to `<b>L.pff` and those of the localres and resource slots to `<b>.pff`, as JO:CA's
// jox01 keeps them. A loose kind goes into the expansion's folder where its reader looks there, and
// nowhere where the game reads it only from the install's folder (AssetKindRow::expansion_loose).
enum class ExpansionPlace {
	LanguageArchive, // <b>L.pff
	Archive,         // <b>.pff
	Folder,          // loose, in expansion/<b>/
	RootOnly,        // the game reads it from the install's folder alone: an expansion cannot ship it
	None,            // no build packs it
};
ExpansionPlace route_for_expansion(AssetKind kind);

// The expansion's folder in an install, or in its build: "expansion/<b>".
std::string expansion_folder(const std::string &expansion);
// Its archives' paths there: "expansion/<b>/<b>L.pff" (language) or "expansion/<b>/<b>.pff".
std::string expansion_archive_path(const std::string &expansion, bool language);

// Where a build puts a file of the expansion `<b>`: its kind's place, a file the front door reads
// (ExpansionLoose::FrontDoor) packed in <b>.pff and loose in the folder alike (`also_loose`); and the
// files the game reads by their path in the expansion's folder, whatever their kind would say, loose
// there alone: the expansion's own files whose row says so (project/expansion_files.h,
// ExpansionPlacement::Folder: `<b>.bin`, the override table, whose path-qualified query no archive
// entry matches, so only the loose file serves it [orig: TextResource_LoadOverrideTable @ 0x4a49de
// through File_LoadResource @ 0x75b540], and which serves the Mods list's name and description too,
// loose first [orig: Expansion_ScanAndRegister @ 0x4a43d0, the search path @ 0x4a4492]; `version.txt`;
// the music banks), and `gt.ssc`, read loose first from the folder [orig: Mission_LoadEncryptedConfig
// @ 0x4cdcf4].
ExpansionPlace route_for_expansion(const AssetEntry &asset, const std::string &expansion, bool &also_loose);

} // namespace opennova::editor
