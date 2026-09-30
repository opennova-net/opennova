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

bool asset_is_packable(const AssetEntry &asset) {
	return asset.kind != AssetKind::Archive;
}

ArchiveSlot route_asset(const AssetEntry &asset) {
	return route_asset(asset.kind);
}

ArchiveSlot route_asset(AssetKind kind) {
	return asset_kind_row(kind).archive_slot;
}

} // namespace opennova::editor
