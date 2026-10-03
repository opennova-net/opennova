#include <editor/project_build/archive_routing.h>

#include <base/io/strutil.h>
#include <base/vfs/vfs.h>

namespace opennova::editor {

// The boot table's slots, in its order (base/vfs kBootArchiveTable).
static_assert(sizeof(kBootArchiveTable) / sizeof(kBootArchiveTable[0]) == 3, "language, localres, resource");

const char *archive_slot_file_name(ArchiveSlot slot) {
	switch (slot) {
	case ArchiveSlot::Language: return kBootArchiveTable[0];
	case ArchiveSlot::Localres: return kBootArchiveTable[1];
	case ArchiveSlot::Resource: return kBootArchiveTable[2];
	case ArchiveSlot::Loose:
	case ArchiveSlot::None: return "";
	}
	return "";
}

ArchiveSlot route_asset(const AssetEntry &asset) {
	return route_asset(asset.kind);
}

ArchiveSlot route_asset(AssetKind kind) {
	return asset_kind_row(kind).archive_slot;
}

ExpansionPlace route_for_expansion(AssetKind kind) {
	const AssetKindRow &row = asset_kind_row(kind);
	switch (row.archive_slot) {
	case ArchiveSlot::Language: return ExpansionPlace::LanguageArchive;
	case ArchiveSlot::Localres:
	case ArchiveSlot::Resource: return ExpansionPlace::Archive;
	case ArchiveSlot::Loose:
		switch (row.expansion_loose) {
		case ExpansionLoose::Folder: return ExpansionPlace::Folder;
		case ExpansionLoose::FrontDoor: return ExpansionPlace::Archive; // and loose in the folder
		case ExpansionLoose::RootOnly:
		case ExpansionLoose::None: return ExpansionPlace::RootOnly;
		}
		return ExpansionPlace::RootOnly;
	case ArchiveSlot::None: return ExpansionPlace::None;
	}
	return ExpansionPlace::None;
}

std::string expansion_folder(const std::string &expansion) {
	return "expansion/" + expansion;
}

std::string expansion_archive_path(const std::string &expansion, bool language) {
	return expansion_folder(expansion) + "/" + expansion + (language ? "L.pff" : ".pff");
}

ExpansionPlace route_for_expansion(const AssetEntry &asset, const std::string &expansion, bool &also_loose) {
	also_loose = false;
	const ExpansionPlace by_kind = route_for_expansion(asset.kind);
	if (by_kind == ExpansionPlace::None) return by_kind;
	for (const char *name : {"version.txt", "gt.ssc"})
		if (strutil::iequals(asset.logical_name, name)) return ExpansionPlace::Folder;
	if (strutil::iequals(asset.logical_name, expansion + ".bin")) return ExpansionPlace::Folder;
	also_loose = asset_kind_row(asset.kind).expansion_loose == ExpansionLoose::FrontDoor;
	return by_kind;
}

} // namespace opennova::editor
