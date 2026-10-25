#include <editor/project_build/archive_routing.h>

#include <base/vfs/vfs.h>
#include <editor/project/expansion_files.h>
#include <formats/pff/pff.h>

namespace opennova::editor {

namespace {

// The archive `record` names for the file, None where it names none.
ArchiveSlot recorded(const AssetEntry &asset, const ArchiveRecord *record) {
	if (!record || record->empty()) return ArchiveSlot::None;
	const auto found = record->find(asset.key.empty() ? pff::normalized_logical_name(asset.logical_name) : asset.key);
	return found == record->end() ? ArchiveSlot::None : found->second;
}

bool archive_slot(ArchiveSlot slot) {
	return slot == ArchiveSlot::Language || slot == ArchiveSlot::Localres || slot == ArchiveSlot::Resource;
}

} // namespace

ArchiveSlot route_asset(const AssetEntry &asset, const ArchiveRecord *record) {
	const ArchiveSlot by_kind = route_asset(asset.kind);
	const ArchiveSlot kept = recorded(asset, record);
	return archive_slot(by_kind) && archive_slot(kept) ? kept : by_kind;
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

ExpansionPlace route_for_expansion(const AssetEntry &asset, const std::string &expansion, const ArchiveRecord *record) {
	const ExpansionPlace by_kind = route_for_expansion(asset.kind);
	if (by_kind == ExpansionPlace::None) return by_kind;
	// The archive the install held it in, of the pair, ahead of the folder the rows below would copy it
	// to: an install that archives its <e>.bin never loads it as the override table, which the game reads
	// from the folder alone [orig: TextResource_LoadOverrideTable @ 0x4a49de through File_LoadResource
	// @ 0x75b540] (JO:CA's jox01L.pff holds jox01.bin), and a project of that install plays as it does.
	const ArchiveSlot kept = recorded(asset, record);
	if ((by_kind == ExpansionPlace::LanguageArchive || by_kind == ExpansionPlace::Archive) && archive_slot(kept))
		return kept == ArchiveSlot::Language ? ExpansionPlace::LanguageArchive : ExpansionPlace::Archive;
	// The expansion's own files where their row puts them (project/expansion_files: <b>.bin, version.txt
	// and the music banks loose in the folder, the rest by kind).
	if (const ExpansionFileRow *row = expansion_file_for(expansion, asset.logical_name))
		if (row->placement == ExpansionPlacement::Folder) return ExpansionPlace::Folder;
	// What the game reads from the expansion's folder by path (gt.ssc, the videos: base/vfs
	// vfs_read_from_expansion_folder).
	if (vfs_read_from_expansion_folder(asset.logical_name, expansion)) return ExpansionPlace::Folder;
	return by_kind;
}

} // namespace opennova::editor
