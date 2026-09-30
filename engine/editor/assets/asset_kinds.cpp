#include <editor/assets/asset_kinds.h>

#include <filesystem>

#include <base/io/strutil.h>
#include <base/resource_index/resource_kind.h>

namespace opennova::editor {

namespace {

constexpr const char *kArchive[] = {".pff", nullptr};
constexpr const char *kAnimation[] = {".bad", nullptr};
constexpr const char *kAnimationMap[] = {".adm", nullptr};
constexpr const char *kFace[] = {".grm", nullptr};
constexpr const char *kAiProfile[] = {".aip", nullptr};
// A model's normal map made ahead is an .mdt: a TGA the object loader decodes as it does a
// .tga [orig: Texture_LoadByNameWithChannel @ 0x58B66F..0x58B6E6; Texture_LoadAndRegister @
// 0x58B80E..0x58B881], no DDS sibling taken for it [orig: Texture_LoadAsNormalMap @ 0x58C480]
// (renderer::material_texture_source).
constexpr const char *kTexture[] = {".tga", ".pcx", ".dds", ".mdt", nullptr};
constexpr const char *kRawBin[] = {".bin", nullptr};
constexpr const char *kMapProject[] = {".npj", ".npz", nullptr};
constexpr const char *kTerrainPolyData[] = {".cpt", nullptr};
constexpr const char *kTileInfo[] = {".til", nullptr};
constexpr const char *kWave[] = {".wav", nullptr};
constexpr const char *kDialogBank[] = {".dbf", nullptr};
constexpr const char *kScript[] = {".wac", nullptr};
constexpr const char *kOtherDefs[] = {".def", nullptr};
constexpr const char *kStringTableCoo[] = {".coo", nullptr};
constexpr const char *kVideo[] = {".bik", nullptr};
constexpr const char *kPlayerSave[] = {".sav", nullptr};
constexpr const char *kShader[] = {".fx", nullptr};
// assets.cd is read before any archive mounts, as game.cfg is (docs/required-resources.md).
constexpr const char *kConfig[] = {".cfg", ".ini", ".ssc", ".cd", nullptr};
constexpr const char *kText[] = {".txt", nullptr};
constexpr const char *kImageSource[] = {".png", nullptr};

// A row built up column by column, so each row names only what it sets; the slot is always
// stated.
struct Kind {
	AssetKindRow row;
	constexpr Kind(AssetKind kind, const char *token, const char *label, ArchiveSlot slot) : row() {
		row.kind = kind;
		row.token = token;
		row.label = label;
		row.archive_slot = slot;
	}
	// A kind the runtime's catalog browses, by its catalog token.
	constexpr Kind runtime(const char *token) const {
		Kind out = *this;
		out.row.runtime = token;
		return out;
	}
	constexpr Kind file(const char *name) const {
		Kind out = *this;
		out.row.file_name = name;
		return out;
	}
	constexpr Kind extensions(const char *const *names) const {
		Kind out = *this;
		out.row.extensions = names;
		return out;
	}
	constexpr Kind edited_by(DocumentTypeId type) const {
		Kind out = *this;
		out.row.document = type;
		return out;
	}
	// An importer's source: its outputs pack, never the source.
	constexpr Kind source() const {
		Kind out = *this;
		out.row.import_source = true;
		return out;
	}
	constexpr Kind names_files() const {
		Kind out = *this;
		out.row.names_files = true;
		return out;
	}
};

constexpr AssetKindRow kRows[] = {
	// A file of no kind the game knows packs into resource.pff with the art (S13 A8 stops
	// packing it).
	Kind(AssetKind::Unknown, "unknown", "Unknown file", ArchiveSlot::Resource).row,
	Kind(AssetKind::Archive, "archive", "Archive", ArchiveSlot::None).extensions(kArchive).row,
	Kind(AssetKind::Model, "model", "Model", ArchiveSlot::Resource)
	        .runtime("object_model")
	        .edited_by(DocumentTypeId::Model)
	        .names_files()
	        .row,
	Kind(AssetKind::Animation, "animation", "Animation", ArchiveSlot::Resource)
	        .extensions(kAnimation)
	        .edited_by(DocumentTypeId::Animation)
	        .row,
	Kind(AssetKind::AnimationMap, "animation_map", "Animation map", ArchiveSlot::Resource)
	        .extensions(kAnimationMap)
	        .edited_by(DocumentTypeId::AnimationMap)
	        .names_files()
	        .row,
	// Its base and eye textures by name (formats/grm).
	Kind(AssetKind::Face, "face", "Face", ArchiveSlot::Resource)
	        .extensions(kFace)
	        .names_files()
	        .row,
	Kind(AssetKind::AiProfile, "ai_profile", "AI profile", ArchiveSlot::Resource)
	        .extensions(kAiProfile)
	        .row,
	Kind(AssetKind::Texture, "texture", "Texture", ArchiveSlot::Resource).extensions(kTexture).row,
	Kind(AssetKind::Font, "font", "Font", ArchiveSlot::Localres).runtime("font").row,
	// The boot text bins, the menu tables and the per-mission text sidecars.
	Kind(AssetKind::Strings, "strings", "String table", ArchiveSlot::Language)
	        .runtime("strings")
	        .edited_by(DocumentTypeId::Strings)
	        .row,
	Kind(AssetKind::MusicScript, "music_script", "Music script", ArchiveSlot::Localres)
	        .runtime("music_script")
	        .row,
	Kind(AssetKind::RawBin, "raw_bin", "Binary table", ArchiveSlot::Language)
	        .extensions(kRawBin)
	        .row,
	Kind(AssetKind::Credits, "credits", "Credits", ArchiveSlot::Localres).runtime("credits").row,
	// A .bms in localres: retail's mission list walks only the localres/language volumes [orig:
	// Mission_BuildMapListFromPFF @ 0x562910].
	Kind(AssetKind::Mission, "mission", "Mission", ArchiveSlot::Localres)
	        .runtime("mission")
	        .names_files()
	        .row,
	// Where retail keeps its own (localres.pff holds ASP_G7.npz), beside the missions the list
	// holds.
	Kind(AssetKind::MapProject, "map_project", "Map project", ArchiveSlot::Localres)
	        .extensions(kMapProject)
	        .names_files()
	        .row,
	Kind(AssetKind::Terrain, "terrain", "Terrain", ArchiveSlot::Resource)
	        .runtime("terrain")
	        .names_files()
	        .row,
	Kind(AssetKind::TerrainPolyData, "terrain_polydata", "Terrain height data",
	     ArchiveSlot::Resource)
	        .extensions(kTerrainPolyData)
	        .row,
	Kind(AssetKind::TileInfo, "tile_info", "Tile placement", ArchiveSlot::Resource)
	        .extensions(kTileInfo)
	        .row,
	Kind(AssetKind::Environment, "environment", "Environment", ArchiveSlot::Resource)
	        .runtime("environment")
	        .names_files()
	        .row,
	Kind(AssetKind::Menu, "menu", "Menu", ArchiveSlot::Localres)
	        .runtime("menu")
	        .edited_by(DocumentTypeId::Menu)
	        .names_files()
	        .row,
	Kind(AssetKind::MenuStyle, "menu_style", "Menu style", ArchiveSlot::Localres)
	        .runtime("menu_style")
	        .edited_by(DocumentTypeId::Styles)
	        .names_files()
	        .row,
	// Streamed by path, never through the archives (ArchiveSlot).
	Kind(AssetKind::MusicBank, "music_bank", "Music bank", ArchiveSlot::Loose)
	        .runtime("sbf")
	        .names_files()
	        .row,
	// The sound sets, read by SoundBank_OpenFile (formats/lwf), their singles naming the waves.
	Kind(AssetKind::SoundBank, "sound_bank", "Sound bank", ArchiveSlot::Resource)
	        .runtime("sound")
	        .names_files()
	        .row,
	// A wave a sound bank's single names, which the game loads from the archives by name
	// (docs/audio/lwf-dbf-sound-re.md): retail packs its sound waves in localres.pff and its
	// localized voice lines in language.pff, and a name resolves from any mounted archive, so the
	// slot places it and nothing more.
	Kind(AssetKind::Wave, "wave", "Wave", ArchiveSlot::Localres).extensions(kWave).row,
	Kind(AssetKind::DialogBank, "dialog_bank", "Dialog bank", ArchiveSlot::Localres)
	        .extensions(kDialogBank)
	        .names_files()
	        .row,
	Kind(AssetKind::Particles, "particles", "Particle effects", ArchiveSlot::Resource)
	        .runtime("particle")
	        .names_files()
	        .row,
	Kind(AssetKind::Script, "script", "Script", ArchiveSlot::Localres)
	        .extensions(kScript)
	        .names_files()
	        .row,
	// The .def family by name: the runtime consumes each by its exact name, and browses only
	// Avatars.def and hudpos.def.
	Kind(AssetKind::ItemDefs, "item_defs", "Item definitions", ArchiveSlot::Localres)
	        .file("items.def")
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .row,
	Kind(AssetKind::WeaponDefs, "weapon_defs", "Weapon definitions", ArchiveSlot::Localres)
	        .file("weapon.def")
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .row,
	Kind(AssetKind::AmmoDefs, "ammo_defs", "Ammo definitions", ArchiveSlot::Localres)
	        .file("ammo.def")
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .row,
	Kind(AssetKind::HudPosDefs, "hudpos_defs", "HUD layout", ArchiveSlot::Localres)
	        .runtime("hudpos")
	        .names_files()
	        .row,
	Kind(AssetKind::HudFxDefs, "hudfx_defs", "HUD effects", ArchiveSlot::Localres)
	        .file("hudfx.def")
	        .names_files()
	        .row,
	Kind(AssetKind::AvatarDefs, "avatar_defs", "Avatars", ArchiveSlot::Localres)
	        .runtime("avatar")
	        .names_files()
	        .row,
	Kind(AssetKind::SoundProfileDefs, "sound_profile_defs", "Sound profiles", ArchiveSlot::Localres)
	        .file("sndprof.def")
	        .names_files()
	        .row,
	Kind(AssetKind::CharAttrDefs, "charattr_defs", "Character attributes", ArchiveSlot::Localres)
	        .file("charattr.def")
	        .names_files()
	        .row,
	Kind(AssetKind::PowerupDefs, "powerup_defs", "Powerup definitions", ArchiveSlot::Localres)
	        .file("powerup.def")
	        .names_files()
	        .row,
	Kind(AssetKind::OtherDefs, "other_defs", "Definitions", ArchiveSlot::Localres)
	        .extensions(kOtherDefs)
	        .names_files()
	        .row,
	Kind(AssetKind::StringTableCoo, "string_table_coo", "NovaWorld string table",
	     ArchiveSlot::Loose)
	        .extensions(kStringTableCoo)
	        .row,
	Kind(AssetKind::Video, "video", "Video", ArchiveSlot::Loose).extensions(kVideo).row,
	Kind(AssetKind::PlayerSave, "player_save", "Player save", ArchiveSlot::Loose)
	        .extensions(kPlayerSave)
	        .row,
	Kind(AssetKind::Shader, "shader", "Shader", ArchiveSlot::Resource).extensions(kShader).row,
	Kind(AssetKind::Config, "config", "Configuration", ArchiveSlot::Loose).extensions(kConfig).row,
	// Loose in the install root, where retail ships it.
	Kind(AssetKind::Score, "score", "Score table", ArchiveSlot::Loose).file("score.ini").row,
	Kind(AssetKind::Text, "text", "Text", ArchiveSlot::Loose).extensions(kText).row,
	// A PNG is a source only while its import record is there (scan_project_assets): its outputs,
	// named after it, land in resource.pff as textures.
	Kind(AssetKind::ImageSource, "image_source", "Image source", ArchiveSlot::None)
	        .extensions(kImageSource)
	        .source()
	        .row,
};

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// Whether a null-ended list holds `name`.
constexpr bool lists(const char *const *names, const char *name) {
	for (; names && *names; ++names)
		if (same_text(*names, name)) return true;
	return false;
}

// One row per kind, at the kind's own index; no two rows share a token, a runtime token, a file
// name or an extension (a name gives one kind); an import source packs nowhere, and every other
// kind but an archive somewhere; a kind is edited by a type the registry has.
constexpr bool rows_well_formed() {
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKindRow &row = kRows[i];
		if (static_cast<size_t>(row.kind) != i || !*row.token || !*row.label) return false;
		const bool packs_nowhere =
		        row.archive_slot == ArchiveSlot::None && row.kind != AssetKind::Archive;
		if (row.import_source != packs_nowhere) return false;
		if (static_cast<size_t>(row.document) > kDocumentTypeCount) return false;
		for (size_t j = 0; j < i; ++j) {
			const AssetKindRow &other = kRows[j];
			if (same_text(row.token, other.token)) return false;
			if (*row.runtime && same_text(row.runtime, other.runtime)) return false;
			if (row.file_name && other.file_name && same_text(row.file_name, other.file_name))
				return false;
			for (const char *const *name = row.extensions; name && *name; ++name)
				if (lists(other.extensions, *name)) return false;
		}
	}
	return true;
}

static_assert(sizeof(kRows) / sizeof(kRows[0]) == kAssetKindCount,
              "every AssetKind has exactly one row");
static_assert(rows_well_formed(),
              "the rows follow AssetKind's order and name each kind, token and file name once");

} // namespace

const AssetKindRow &asset_kind_row(AssetKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return index < kAssetKindCount ? kRows[index] : kRows[0];
}

const char *asset_kind_token(AssetKind kind) { return asset_kind_row(kind).token; }

const char *asset_kind_label(AssetKind kind) { return asset_kind_row(kind).label; }

AssetKind asset_kind_from_token(const std::string &token) {
	for (const AssetKindRow &row : kRows)
		if (token == row.token) return row.kind;
	return AssetKind::Unknown;
}

AssetKind asset_kind_for_runtime(const std::string &runtime_kind) {
	if (runtime_kind.empty()) return AssetKind::Unknown;
	for (const AssetKindRow &row : kRows)
		if (runtime_kind == row.runtime) return row.kind;
	return AssetKind::Unknown;
}

AssetKind asset_kind_for_name(const std::string &logical_name) {
	const std::string file = std::filesystem::path(logical_name).filename().string();
	const std::string name = strutil::to_lower(file);
	for (const AssetKindRow &row : kRows)
		if (row.file_name && name == row.file_name) return row.kind;
	const std::string extension = resource_extension_for_name(logical_name);
	if (extension.empty()) return AssetKind::Unknown;
	for (const AssetKindRow &row : kRows)
		if (lists(row.extensions, extension.c_str())) return row.kind;
	return AssetKind::Unknown;
}

bool asset_kind_packed(AssetKind kind) {
	return asset_kind_row(kind).archive_slot != ArchiveSlot::None;
}

bool archive_name_limit_binds(AssetKind kind) {
	return asset_kind_row(kind).archive_slot != ArchiveSlot::Loose;
}

} // namespace opennova::editor
