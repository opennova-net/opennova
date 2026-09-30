#pragma once

#include <string>

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>

namespace opennova::editor {

// Where a project asset lands in a Play or Export build (ADR 0046 d8): the slot its kind's row
// names (ArchiveSlot, assets/asset_kinds.h).

// "language.pff", "localres.pff", "resource.pff"; "" for Loose and None.
const char *archive_slot_file_name(ArchiveSlot slot);

// The slot for an asset, which its kind decides (AssetKindRow::archive_slot). An Archive-kind
// file (a .pff inside the project) has none: the build reports it and leaves it out. An import
// source has none either: a PNG never packs itself, and its outputs, named after it, land in the
// resource archive as textures.
ArchiveSlot route_asset(AssetKind kind);
ArchiveSlot route_asset(const AssetEntry &asset);
bool asset_is_packable(const AssetEntry &asset);

} // namespace opennova::editor
