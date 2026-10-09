#include <editor/project_build/archive_routing.h>

#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <editor/project/expansion_files.h>

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
		case ExpansionLoose::RootOnly:
		case ExpansionLoose::None: return ExpansionPlace::RootOnly;
		}
		return ExpansionPlace::RootOnly;
	case ArchiveSlot::None: return ExpansionPlace::None;
	}
	return ExpansionPlace::None;
}

ExpansionPlace route_for_expansion(const AssetEntry &asset, const std::string &expansion) {
	const ExpansionPlace by_kind = route_for_expansion(asset.kind);
	if (by_kind == ExpansionPlace::None) return by_kind;
	// The expansion's own files where their row puts them (project/expansion_files: <b>.bin, version.txt
	// and the music banks loose in the folder, the rest by kind).
	if (const ExpansionFileRow *row = expansion_file_for(expansion, asset.logical_name))
		if (row->placement == ExpansionPlacement::Folder) return ExpansionPlace::Folder;
	if (strutil::iequals(asset.logical_name, "gt.ssc")) return ExpansionPlace::Folder;
	return by_kind;
}

} // namespace opennova::editor
