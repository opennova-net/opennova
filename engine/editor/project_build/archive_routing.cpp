#include <editor/project_build/archive_routing.h>

#include <base/io/strutil.h>
#include <editor/project/expansion_files.h>

namespace opennova::editor {

ArchiveSlot route_asset(const AssetEntry &asset) {
	return route_asset(asset.kind);
}

ArchiveSlot route_asset(AssetKind kind) {
	return file_kind_facts(kind).archive_slot;
}

ExpansionPlace route_for_expansion(AssetKind kind) {
	const FileKindFacts &row = file_kind_facts(kind);
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
