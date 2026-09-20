#include <editor/project_build/archive_routing.h>

#include <base/io/strutil.h>

namespace opennova::editor {

const char *archive_slot_file_name(ArchiveSlot slot) {
	switch (slot) {
	case ArchiveSlot::Language: return "language.pff";
	case ArchiveSlot::Localres: return "localres.pff";
	case ArchiveSlot::Resource: return "resource.pff";
	case ArchiveSlot::Loose: return "";
	}
	return "";
}

const char *archive_slot_label(ArchiveSlot slot) {
	switch (slot) {
	case ArchiveSlot::Language: return "language archive";
	case ArchiveSlot::Localres: return "local resource archive";
	case ArchiveSlot::Resource: return "resource archive";
	case ArchiveSlot::Loose: return "loose file";
	}
	return "";
}

bool asset_is_packable(const AssetEntry &asset) {
	return asset.kind != AssetKind::Archive;
}

ArchiveSlot route_asset(const AssetEntry &asset) {
	switch (asset.kind) {
	// The boot text bins, the menu tables and the per-mission text sidecars.
	case AssetKind::Strings:
	case AssetKind::RawBin: return ArchiveSlot::Language;
	// Menus, stylesheets, definitions, missions, fonts, music scripts, dialog banks,
	// credits and scripts: retail's localres set. The `.bms` placement also matters
	// for retail's mission list, which walks only the localres/language volumes
	// [orig: Mission_BuildMapListFromPFF @ 0x562910].
	case AssetKind::Menu:
	case AssetKind::MenuStyle:
	case AssetKind::ItemDefs:
	case AssetKind::WeaponDefs:
	case AssetKind::AmmoDefs:
	case AssetKind::HudPosDefs:
	case AssetKind::HudFxDefs:
	case AssetKind::AvatarDefs:
	case AssetKind::SoundProfileDefs:
	case AssetKind::CharAttrDefs:
	case AssetKind::PowerupDefs:
	case AssetKind::OtherDefs:
	case AssetKind::Mission:
	case AssetKind::Font:
	case AssetKind::MusicScript:
	case AssetKind::DialogBank:
	case AssetKind::Credits:
	case AssetKind::Script: return ArchiveSlot::Localres;
	// Streams by path, read before any mount, or retail's own loose files.
	case AssetKind::SoundBank:
	case AssetKind::Video:
	case AssetKind::Text:
	case AssetKind::Config:
	case AssetKind::PlayerSave:
	case AssetKind::StringTableCoo: return ArchiveSlot::Loose;
	// Models, animations, textures, terrain, environments, wave banks, effects,
	// shaders, and anything the engine does not name: retail's resource set.
	case AssetKind::Model:
	case AssetKind::Animation:
	case AssetKind::AnimationMap:
	case AssetKind::AiProfile:
	case AssetKind::Texture:
	case AssetKind::Terrain:
	case AssetKind::TerrainPolyData:
	case AssetKind::TileInfo:
	case AssetKind::Environment:
	case AssetKind::WaveBank:
	case AssetKind::Particles:
	case AssetKind::Shader:
	case AssetKind::Unknown:
	case AssetKind::Archive: return ArchiveSlot::Resource;
	}
	return ArchiveSlot::Resource;
}

} // namespace opennova::editor
