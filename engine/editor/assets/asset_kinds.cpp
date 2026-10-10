#include <editor/assets/asset_kinds.h>


#include <base/io/strutil.h>

namespace opennova::editor {

namespace {

// A row built up column by column, so each row names only what it sets. What the game's loaders
// know of the kind (its names, slot, expansion place, line reader, each with its witnesses) is its
// row of facts in base/resource_index/file_kind.cpp.
struct Kind {
	AssetKindRow row;
	constexpr Kind(AssetKind kind, const char *token, const char *label) : row() {
		row.kind = kind;
		row.token = token;
		row.label = label;
	}
	constexpr Kind edited_by(DocumentTypeId type) const {
		Kind out = *this;
		out.row.document = type;
		return out;
	}
	constexpr Kind names_files() const {
		Kind out = *this;
		out.row.names_files = true;
		return out;
	}
	constexpr Kind folder(const char *name) const {
		Kind out = *this;
		out.row.folder = name;
		return out;
	}
	constexpr Kind new_name(const char *name) const {
		Kind out = *this;
		out.row.new_name = name;
		return out;
	}
};

constexpr AssetKindRow kRows[] = {
	// A file of no kind the game knows: the game never asks for one, so the build leaves it out
	// (S13 A8; it packed into resource.pff with the art before).
	Kind(AssetKind::Unknown, "unknown", "Unknown file")
	        .row,
	Kind(AssetKind::Archive, "archive", "Archive")
	        .row,
	Kind(AssetKind::Model, "model", "Model")
	        .edited_by(DocumentTypeId::Model)
	        .names_files()
	        .folder("models")
	        .new_name("newmodel.3di")
	        .row,
	Kind(AssetKind::Animation, "animation", "Animation")
	        .edited_by(DocumentTypeId::Animation)
	        .folder("anims")
	        .new_name("newclip.bad")
	        .row,
	Kind(AssetKind::AnimationMap, "animation_map", "Animation map")
	        .edited_by(DocumentTypeId::AnimationMap)
	        .names_files()
	        .folder("anims")
	        .new_name("newmap.adm")
	        .row,
	// Its base and eye textures by name (documents/face_animation_document: each a field's reference).
	Kind(AssetKind::FaceAnimation, "face_animation", "Face animation")
	        .edited_by(DocumentTypeId::FaceAnimation)
	        .names_files()
	        .folder("anims")
	        .new_name("newface.grm")
	        .row,
	Kind(AssetKind::AiProfile, "ai_profile", "AI profile")
	        .edited_by(DocumentTypeId::AiProfile)
	        .folder("ai")
	        .new_name("newprofile.aip")
	        .row,
	Kind(AssetKind::Texture, "texture", "Texture")
	        .edited_by(DocumentTypeId::Texture)
	        .new_name("newtexture.tga")
	        .folder("textures")
	        .row,
	// The scan gives it to a file no rule types by its name whose bytes hold a chunk container
	// (classify_asset, the scan's peek at its chunk headers).
	Kind(AssetKind::MaterialChunk, "material_chunk", "Material chunk")
	        .folder("textures")
	        .row,
	// documents/font_document: its header and its glyphs; its picture the font viewport's (preview/font_viewport).
	Kind(AssetKind::Font, "font", "Font")
	        .edited_by(DocumentTypeId::Font)
	        .folder("fonts")
	        .new_name("newfont.fnt")
	        .row,
	Kind(AssetKind::Strings, "strings", "String table")
	        .edited_by(DocumentTypeId::Strings)
	        .folder("strings")
	        .new_name("newtable.bin")
	        .row,
	Kind(AssetKind::MusicScript, "music_script", "Music script")
	        .edited_by(DocumentTypeId::MusicScript)
	        .folder("music")
	        .row,
	Kind(AssetKind::RawBin, "raw_bin", "Binary table")
	        .row,
	Kind(AssetKind::CountryCode, "country_code", "Country code")
	        .row,
	Kind(AssetKind::Credits, "credits", "Credits")
	        .edited_by(DocumentTypeId::Credits)
	        .names_files()
	        .folder("menus")
	        .new_name("newcredits.kda")
	        .row,
	Kind(AssetKind::Mission, "mission", "Mission")
	        .names_files()
	        .edited_by(DocumentTypeId::Mission)
	        .folder("missions")
	        .new_name("newmission.bms")
	        .row,
	// The game never asks for it, so the build leaves it out. It names files (a terrain, items,
	// weapons) no reader of the editor follows: an import takes it and lists it as not followed.
	Kind(AssetKind::MissionText, "mission_text", "Mission text")
	        .names_files()
	        .folder("missions")
	        .row,
	// OpenNova's mission list lists none yet (runtime/mission/mission_catalog.h).
	Kind(AssetKind::MapProject, "map_project", "Map project")
	        .names_files()
	        .folder("missions")
	        .row,
	Kind(AssetKind::Terrain, "terrain", "Terrain")
	        .edited_by(DocumentTypeId::Terrain)
	        .names_files()
	        .folder("terrain")
	        .row,
	Kind(AssetKind::TerrainPolyData, "terrain_polydata", "Terrain height data")
	        .folder("terrain")
	        .row,
	// Beside the missions: the game finds a mission's by its name (mission::sidecars).
	Kind(AssetKind::TileInfo, "tile_info", "Tile placement")
	        .folder("missions")
	        .new_name("newmission.til")
	        .row,
	Kind(AssetKind::Environment, "environment", "Environment")
	        .edited_by(DocumentTypeId::Environment)
	        .names_files()
	        .new_name("newenviro.env")
	        .folder("terrain")
	        .row,
	Kind(AssetKind::Menu, "menu", "Menu")
	        .edited_by(DocumentTypeId::Menu)
	        .names_files()
	        .folder("menus")
	        .new_name("newmenu.mnu")
	        .row,
	Kind(AssetKind::MenuStyle, "menu_style", "Menu style")
	        .edited_by(DocumentTypeId::Styles)
	        .names_files()
	        .folder("menus")
	        .row,
	// It names no file: its entries are its own chunks of audio (formats/sbf; documents/music_bank_document).
	Kind(AssetKind::MusicBank, "music_bank", "Music bank")
	        .edited_by(DocumentTypeId::MusicBank)
	        .folder("music")
	        .row,
	// Its waves name the files (documents/sound_bank_document: a wave's file is a field's reference, a
	// set's name a symbol).
	Kind(AssetKind::SoundBank, "sound_bank", "Sound bank")
	        .edited_by(DocumentTypeId::SoundBank)
	        .names_files()
	        .folder("sounds")
	        .new_name("newbank.lwf")
	        .row,
	// documents/wave_document: its facts as the game's loader reads it, a trim and a normalise.
	Kind(AssetKind::Wave, "wave", "Wave")
	        .edited_by(DocumentTypeId::Wave)
	        .folder("sounds")
	        .new_name("newwave.wav")
	        .row,
	// documents/dialog_bank_document: a dialog's name a symbol, a line's wave one of the bank's sounds.
	Kind(AssetKind::DialogBank, "dialog_bank", "Dialog bank")
	        .edited_by(DocumentTypeId::DialogBank)
	        .names_files()
	        .folder("missions")
	        .new_name("newmission.dbf")
	        .row,
	Kind(AssetKind::Particles, "particles", "Particle effects")
	        .edited_by(DocumentTypeId::Particles)
	        .names_files()
	        .folder("particles")
	        .new_name("neweffects.ptl")
	        .row,
	// Its operands' names (S13 D9), the script a RUN names [orig: Script_LoadAndCompileFile @
	// 0x4EE660] and the waves it plays (S14), each an edge the graph reads (documents/script_type).
	Kind(AssetKind::Script, "script", "Script")
	        .edited_by(DocumentTypeId::Script)
	        .names_files()
	        .folder("missions")
	        .new_name("newscript.wac")
	        .row,
	Kind(AssetKind::ItemDefs, "item_defs", "Item definitions")
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .row,
	Kind(AssetKind::WeaponDefs, "weapon_defs", "Weapon definitions")
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .row,
	Kind(AssetKind::AmmoDefs, "ammo_defs", "Ammo definitions")
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .row,
	Kind(AssetKind::HudPosDefs, "hudpos_defs", "HUD layout")
	        .edited_by(DocumentTypeId::HudLayout)
	        .names_files()
	        .folder("defs")
	        .row,
	Kind(AssetKind::HudFxDefs, "hudfx_defs", "HUD effects")
	        .edited_by(DocumentTypeId::HudFx)
	        .names_files()
	        .folder("defs")
	        .row,
	Kind(AssetKind::AvatarDefs, "avatar_defs", "Avatars")
	        .edited_by(DocumentTypeId::Avatars)
	        .names_files()
	        .folder("defs")
	        .row,
	Kind(AssetKind::SoundProfileDefs, "sound_profile_defs", "Sound profiles")
	        .edited_by(DocumentTypeId::SoundProfiles)
	        .names_files()
	        .folder("defs")
	        .row,
	Kind(AssetKind::CharAttrDefs, "charattr_defs", "Character attributes")
	        .edited_by(DocumentTypeId::CharAttrs)
	        .names_files()
	        .folder("defs")
	        .row,
	Kind(AssetKind::PowerupDefs, "powerup_defs", "Powerup definitions")
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .row,
	Kind(AssetKind::OtherDefs, "other_defs", "Definitions")
	        .edited_by(DocumentTypeId::Text)
	        .names_files()
	        .folder("defs")
	        .row,
	Kind(AssetKind::StringTableCoo, "string_table_coo", "NovaWorld string table")
	        .folder("strings")
	        .row,
	// S13 A8: no kind before, so the build left the NovaWorld screens out; a build packs them with the
	// menus (S16), the one place the game reads them with /d and without.
	Kind(AssetKind::NovaWorldScreen, "novaworld_screen", "NovaWorld screen")
	        .edited_by(DocumentTypeId::Text)
	        .folder("menus")
	        .row,
	Kind(AssetKind::Video, "video", "Video")
	        .folder("videos")
	        .row,
	// A build packs no save (gameprofile/player_files.h).
	Kind(AssetKind::PlayerSave, "player_save", "Player save")
	        .row,
	// Its loader takes it in the SCR form as stored (vfs_loader_takes_stored): the shader type reads
	// the file itself.
	Kind(AssetKind::Shader, "shader", "Shader")
	        .edited_by(DocumentTypeId::Shader)
	        .folder("shaders")
	        .new_name("newshader.fx")
	        .row,
	// gt.ssc, read loose first from the expansion's folder, is its own row of the expansion's files
	// (route_for_expansion).
	Kind(AssetKind::Config, "config", "Configuration")
	        .edited_by(DocumentTypeId::Text)
	        .row,
	Kind(AssetKind::Score, "score", "Score table")
	        .edited_by(DocumentTypeId::ScoreTable)
	        .row,
	// An expansion's version.txt is its own row of the expansion's files (route_for_expansion).
	Kind(AssetKind::Text, "text", "Text")
	        .edited_by(DocumentTypeId::Text)
	        .row,
	// The build leaves it out, with no word (unlike a file of no kind the game knows, which may be data
	// the game misses); the editor opens it as a text. A file with no extension (a LICENSE) is one when
	// it holds text (classify_asset); one that holds bytes stays of no kind, said, as it may be data.
	Kind(AssetKind::Notes, "notes", "Project notes")
	        .edited_by(DocumentTypeId::Text)
	        .row,
	// The scan gives it to a file an importer converts while its import record is there
	// (scan_project_assets), whatever the file's name would make it (a .png a texture). Its outputs,
	// named after it, pack by their own kinds; it never packs.
	Kind(AssetKind::ImportSource, "import_source", "Import source")
	        .row,
	// The scan gives it to a file an import record lists among its inputs (S20: a terrain set's
	// heightmap and images), whatever its name would make it. It never packs: its import's outputs do.
	Kind(AssetKind::ImportInput, "import_input", "Import input")
	        .row,
};

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// One row per kind, at the kind's own index; no two rows share a token; a kind is edited by a type
// the registry has (what a kind is to the game is assets/asset_kind_words' row). The facts' own shape (names, slots) is the
// engine table's static_asserts' (base/resource_index/file_kind.cpp), and a new file's name ending
// with one of its kind's extensions is the asset kinds test's (the facts are no constant here).
constexpr bool rows_well_formed() {
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKindRow &row = kRows[i];
		if (static_cast<size_t>(row.kind) != i || !*row.token || !*row.label) return false;
		if (static_cast<size_t>(row.document) > kDocumentTypeCount) return false;
		if (!row.folder || !row.new_name) return false;
		for (size_t j = 0; j < i; ++j)
			if (same_text(row.token, kRows[j].token)) return false;
	}
	return true;
}

static_assert(sizeof(kRows) / sizeof(kRows[0]) == kAssetKindCount,
              "every AssetKind has exactly one row");
static_assert(rows_well_formed(), "the rows follow AssetKind's order and name each kind and token once");

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

bool asset_kind_packed(AssetKind kind) {
	return file_kind_facts(kind).archive_slot != ArchiveSlot::None;
}

AssetKind asset_kind_named_by(const std::string &text, bool *only) {
	std::string wanted = strutil::to_lower(text);
	const bool prefixed = wanted.rfind("kind:", 0) == 0;
	if (prefixed) wanted.erase(0, 5);
	while (!wanted.empty() && wanted.front() == ' ') wanted.erase(wanted.begin());
	while (!wanted.empty() && wanted.back() == ' ') wanted.pop_back();
	if (only) *only = prefixed;
	if (wanted.empty()) return AssetKind::kCount;
	// A token spells a space as '_': "sound bank" and "sound_bank" alike.
	for (char &c : wanted)
		if (c == '_') c = ' ';
	const auto spelled = [](const char *name) {
		std::string out = strutil::to_lower(name);
		for (char &c : out)
			if (c == '_') c = ' ';
		return out;
	};
	for (const AssetKindRow &row : kRows) {
		for (const std::string &name : { spelled(row.label), spelled(row.token) })
			if (wanted == name || wanted == name + "s" || (name.size() > 1 && name.back() == 'y' &&
			                                               wanted == name.substr(0, name.size() - 1) + "ies"))
				return row.kind;
	}
	return AssetKind::kCount;
}

bool archive_name_limit_binds(AssetKind kind) {
	// A NovaWorld screen of a name no archive holds is one the game never reads: the build leaves it out
	// and says so (build.unread, plan_build), never refusing the build for it.
	if (kind == AssetKind::NovaWorldScreen) return false;
	const ArchiveSlot slot = file_kind_facts(kind).archive_slot;
	return kind == AssetKind::ImportSource || (slot != ArchiveSlot::Loose && slot != ArchiveSlot::None);
}

} // namespace opennova::editor
