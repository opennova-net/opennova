#include <editor/assets/asset_type_registry.h>

#include <filesystem>

#include <base/io/strutil.h>
#include <base/resource_index/resource_kind.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

struct KindRow {
	AssetKind kind;
	const char *token;
	const char *label;
};

const KindRow k_kind_rows[] = {
	{ AssetKind::Unknown, "unknown", "Unknown file" },
	{ AssetKind::Archive, "archive", "Archive" },
	{ AssetKind::Model, "model", "Model" },
	{ AssetKind::Animation, "animation", "Animation" },
	{ AssetKind::AnimationMap, "animation_map", "Animation map" },
	{ AssetKind::AiProfile, "ai_profile", "AI profile" },
	{ AssetKind::Texture, "texture", "Texture" },
	{ AssetKind::Font, "font", "Font" },
	{ AssetKind::Strings, "strings", "String table" },
	{ AssetKind::MusicScript, "music_script", "Music script" },
	{ AssetKind::RawBin, "raw_bin", "Binary table" },
	{ AssetKind::Credits, "credits", "Credits" },
	{ AssetKind::Mission, "mission", "Mission" },
	{ AssetKind::Terrain, "terrain", "Terrain" },
	{ AssetKind::TerrainPolyData, "terrain_polydata", "Terrain height data" },
	{ AssetKind::TileInfo, "tile_info", "Tile placement" },
	{ AssetKind::Environment, "environment", "Environment" },
	{ AssetKind::Menu, "menu", "Menu" },
	{ AssetKind::MenuStyle, "menu_style", "Menu style" },
	{ AssetKind::SoundBank, "sound_bank", "Music bank" },
	{ AssetKind::WaveBank, "wave_bank", "Sound bank" },
	{ AssetKind::DialogBank, "dialog_bank", "Dialog bank" },
	{ AssetKind::Particles, "particles", "Particle effects" },
	{ AssetKind::Script, "script", "Script" },
	{ AssetKind::ItemDefs, "item_defs", "Item definitions" },
	{ AssetKind::WeaponDefs, "weapon_defs", "Weapon definitions" },
	{ AssetKind::AmmoDefs, "ammo_defs", "Ammo definitions" },
	{ AssetKind::HudPosDefs, "hudpos_defs", "HUD layout" },
	{ AssetKind::HudFxDefs, "hudfx_defs", "HUD effects" },
	{ AssetKind::AvatarDefs, "avatar_defs", "Avatars" },
	{ AssetKind::SoundProfileDefs, "sound_profile_defs", "Sound profiles" },
	{ AssetKind::CharAttrDefs, "charattr_defs", "Character attributes" },
	{ AssetKind::PowerupDefs, "powerup_defs", "Powerup definitions" },
	{ AssetKind::OtherDefs, "other_defs", "Definitions" },
	{ AssetKind::StringTableCoo, "string_table_coo", "NovaWorld string table" },
	{ AssetKind::Video, "video", "Video" },
	{ AssetKind::PlayerSave, "player_save", "Player save" },
	{ AssetKind::Shader, "shader", "Shader" },
	{ AssetKind::Config, "config", "Configuration" },
	{ AssetKind::Text, "text", "Text" },
};

const KindRow &kind_row(AssetKind kind) {
	for (const KindRow &row : k_kind_rows) {
		if (row.kind == kind) return row;
	}
	return k_kind_rows[0];
}

std::string lower_basename(const std::string &logical_name) {
	return strutil::to_lower(fs::path(logical_name).filename().string());
}

// The runtime catalog's browsable kinds, by their shared string vocabulary.
AssetKind kind_from_resource_kind(const std::string &resource_kind) {
	if (resource_kind == "avatar") return AssetKind::AvatarDefs;
	if (resource_kind == "mission") return AssetKind::Mission;
	if (resource_kind == "terrain") return AssetKind::Terrain;
	if (resource_kind == "environment") return AssetKind::Environment;
	if (resource_kind == "object_model") return AssetKind::Model;
	if (resource_kind == "credits") return AssetKind::Credits;
	if (resource_kind == "font") return AssetKind::Font;
	if (resource_kind == "particle") return AssetKind::Particles;
	if (resource_kind == "menu") return AssetKind::Menu;
	if (resource_kind == "menu_style") return AssetKind::MenuStyle;
	if (resource_kind == "sbf") return AssetKind::SoundBank;
	if (resource_kind == "sound") return AssetKind::WaveBank;
	if (resource_kind == "hudpos") return AssetKind::HudPosDefs;
	if (resource_kind == "music_script") return AssetKind::MusicScript;
	if (resource_kind == "strings") return AssetKind::Strings;
	return AssetKind::Unknown;
}

// The name-keyed .def family the runtime consumes by exact name.
AssetKind def_kind_for_basename(const std::string &basename) {
	if (basename == "items.def") return AssetKind::ItemDefs;
	if (basename == "weapon.def") return AssetKind::WeaponDefs;
	if (basename == "ammo.def") return AssetKind::AmmoDefs;
	if (basename == "hudpos.def") return AssetKind::HudPosDefs;
	if (basename == "hudfx.def") return AssetKind::HudFxDefs;
	if (basename == "avatars.def") return AssetKind::AvatarDefs;
	if (basename == "sndprof.def") return AssetKind::SoundProfileDefs;
	if (basename == "charattr.def") return AssetKind::CharAttrDefs;
	if (basename == "powerup.def") return AssetKind::PowerupDefs;
	return AssetKind::OtherDefs;
}

AssetKind kind_from_extension(const std::string &extension) {
	if (extension == ".pff") return AssetKind::Archive;
	if (extension == ".bad") return AssetKind::Animation;
	if (extension == ".adm") return AssetKind::AnimationMap;
	if (extension == ".aip") return AssetKind::AiProfile;
	if (extension == ".tga" || extension == ".pcx" || extension == ".dds") return AssetKind::Texture;
	if (extension == ".cpt") return AssetKind::TerrainPolyData;
	if (extension == ".til") return AssetKind::TileInfo;
	if (extension == ".dbf") return AssetKind::DialogBank;
	if (extension == ".wac") return AssetKind::Script;
	if (extension == ".coo") return AssetKind::StringTableCoo;
	if (extension == ".bik") return AssetKind::Video;
	if (extension == ".sav") return AssetKind::PlayerSave;
	if (extension == ".fx") return AssetKind::Shader;
	if (extension == ".cfg" || extension == ".ini" || extension == ".ssc") return AssetKind::Config;
	if (extension == ".txt") return AssetKind::Text;
	if (extension == ".bin") return AssetKind::RawBin;
	return AssetKind::Unknown;
}

} // namespace

const char *asset_kind_token(AssetKind kind) {
	return kind_row(kind).token;
}

const char *asset_kind_label(AssetKind kind) {
	return kind_row(kind).label;
}

bool asset_classification_needs_bytes(const std::string &logical_name) {
	return resource_extension_for_name(logical_name) == ".bin";
}

AssetKind classify_asset(const std::string &logical_name, const std::vector<uint8_t> *bytes) {
	const std::string shared = resource_kind_for_file(logical_name, bytes);
	if (!shared.empty()) return kind_from_resource_kind(shared);
	const std::string extension = resource_extension_for_name(logical_name);
	if (extension == ".def") return def_kind_for_basename(lower_basename(logical_name));
	return kind_from_extension(extension);
}

AssetKind expected_asset_kind_for_required_name(const std::string &name) {
	const std::string extension = resource_extension_for_name(name);
	if (extension != ".bin") return classify_asset(name, nullptr);
	// The witnessed `.bin` rows: the music-script pair (and its expansion forms), the
	// three raw markers/credential stores, and string tables for everything else.
	const std::string basename = lower_basename(name);
	if (basename == "menumus.bin" || basename == "gamemus.bin") return AssetKind::MusicScript;
	if (basename == "fgn2.bin" || basename == "epass.bin" || basename == "passgen.bin")
		return AssetKind::RawBin;
	return AssetKind::Strings;
}

} // namespace opennova::editor
