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

} // namespace opennova::editor
