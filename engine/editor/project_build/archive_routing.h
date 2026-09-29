#pragma once

#include <string>

#include <editor/assets/asset_registry.h>

namespace opennova::editor {

// Where a project asset lands in a Play or Export build (ADR 0046 d8): one of the three
// boot-table archives the engine opens by fixed name, or loose beside them. The boot gate
// counts only archives opened from that table [orig: PFF_OpenAllArchives @ 0x4a4310 over
// the name table @ 0x829f90; fatal check @ 0x4a6f44]: an arbitrary-named .pff never
// mounts (docs/vfs/vfs-pff-mount-re.md D-VFS-2), so every packed file goes into
// language.pff, localres.pff or resource.pff. The placement by kind mirrors retail's
// (witnessed against the shipped JO install: the boot text bins in language, menus /
// defs / missions / fonts / music scripts in localres, terrain / env / art in resource),
// which keeps the output retail-bootable; the OpenNova runtime resolves a name from
// any slot. Two families never pack: `.sbf` music banks stream by path and never
// resolve through the archives [orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60], and
// `earlyerr.txt` is the pre-archive error text read before any mount [orig:
// Game_ShowEarlyError @ 0x4a68a0]; retail's own loose files (videos, configs, saves,
// the machine-keyed NovaWorld cache) stay loose with them.
enum class ArchiveSlot { Language, Localres, Resource, Loose };

// "language.pff", "localres.pff", "resource.pff"; "" for Loose.
const char *archive_slot_file_name(ArchiveSlot slot);
const char *archive_slot_label(ArchiveSlot slot);

// The slot for an asset, which its kind decides. An Archive-kind file (a .pff inside
// the project) has no slot: the build reports it and leaves it out. A PNG source never
// packs itself; its outputs, named after it, land in the resource archive.
ArchiveSlot route_asset(AssetKind kind);
ArchiveSlot route_asset(const AssetEntry &asset);
bool asset_is_packable(const AssetEntry &asset);

} // namespace opennova::editor
