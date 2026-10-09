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
	constexpr Kind about(const char *words) const {
		Kind out = *this;
		out.row.about = words;
		return out;
	}
};

constexpr AssetKindRow kRows[] = {
	// A file of no kind the game knows: the game never asks for one, so the build leaves it out
	// (S13 A8; it packed into resource.pff with the art before).
	Kind(AssetKind::Unknown, "unknown", "Unknown file")
	        .about("A file of no kind the game reads: the build leaves it out.")
	        .row,
	Kind(AssetKind::Archive, "archive", "Archive")
	        .about("A PFF archive: the editor imports files out of one, and a build makes the project's own.")
	        .row,
	Kind(AssetKind::Model, "model", "Model")
	        .edited_by(DocumentTypeId::Model)
	        .names_files()
	        .folder("models")
	        .new_name("newmodel.3di")
	        .about("A 3D model: the items, vehicles and buildings the game shows name it.")
	        .row,
	Kind(AssetKind::Animation, "animation", "Animation")
	        .edited_by(DocumentTypeId::Animation)
	        .folder("anims")
	        .new_name("newclip.bad")
	        .about("A clip of bone animation: animation maps name it.")
	        .row,
	Kind(AssetKind::AnimationMap, "animation_map", "Animation map")
	        .edited_by(DocumentTypeId::AnimationMap)
	        .names_files()
	        .folder("anims")
	        .new_name("newmap.adm")
	        .about("An animation map: which clip a soldier or a vehicle plays for each move.")
	        .row,
	// Its base and eye textures by name (formats/grm).
	Kind(AssetKind::FaceAnimation, "face_animation", "Face animation")
	        .names_files()
	        .folder("anims")
	        .about("A face's animation, with the base and eye textures it names.")
	        .row,
	Kind(AssetKind::AiProfile, "ai_profile", "AI profile")
	        .edited_by(DocumentTypeId::Text)
	        .folder("ai")
	        .new_name("newprofile.aip")
	        .about("An AI profile: how a unit the computer runs picks its targets and moves, loaded by the name a placed unit gives.")
	        .row,
	Kind(AssetKind::Texture, "texture", "Texture")
	        .edited_by(DocumentTypeId::Texture)
	        .new_name("newtexture.tga")
	        .folder("textures")
	        .about("An image the game draws: a model's surfaces, a menu, the HUD.")
	        .row,
	// The scan gives it to a file no rule types by its name whose bytes hold a chunk container
	// (classify_asset, the scan's peek at its chunk headers).
	Kind(AssetKind::MaterialChunk, "material_chunk", "Material chunk")
	        .folder("textures")
	        .about("A model's material data: a material row of a model reads it as a chunk container.")
	        .row,
	Kind(AssetKind::Font, "font", "Font")
	        .folder("fonts")
	        .new_name("newfont.fnt")
	        .about("A font the menus and the HUD write with.")
	        .row,
	Kind(AssetKind::Strings, "strings", "String table")
	        .edited_by(DocumentTypeId::Strings)
	        .folder("strings")
	        .new_name("newtable.bin")
	        .about("A string table: the words the menus, the HUD and the missions show, each by its key.")
	        .row,
	Kind(AssetKind::MusicScript, "music_script", "Music script")
	        .edited_by(DocumentTypeId::MusicScript)
	        .folder("music")
	        .about("A music script: which music plays when.")
	        .row,
	Kind(AssetKind::RawBin, "raw_bin", "Binary table")
	        .about("A table the game reads through the archives as it is.")
	        .row,
	Kind(AssetKind::CountryCode, "country_code", "Country code")
	        .about("The country code the game reads at boot from its own folder.")
	        .row,
	Kind(AssetKind::Credits, "credits", "Credits")
	        .edited_by(DocumentTypeId::Credits)
	        .folder("menus")
	        .new_name("newcredits.kda")
	        .about("The credits the menus scroll.")
	        .row,
	Kind(AssetKind::Mission, "mission", "Mission")
	        .names_files()
	        .edited_by(DocumentTypeId::Mission)
	        .folder("missions")
	        .new_name("newmission.bms")
	        .about("A mission: what it places, its events and its settings.")
	        .row,
	// The game never asks for it, so the build leaves it out. It names files (a terrain, items,
	// weapons) no reader of the editor follows: an import takes it and lists it as not followed.
	Kind(AssetKind::MissionText, "mission_text", "Mission text")
	        .names_files()
	        .folder("missions")
	        .about("The original mission editor's text form of a mission: the game never reads it, and a build leaves it out.")
	        .row,
	// OpenNova's mission list lists none yet (runtime/mission/mission_catalog.h).
	Kind(AssetKind::MapProject, "map_project", "Map project")
	        .names_files()
	        .folder("missions")
	        .about("A map project, which the game's mission list takes as it takes a mission.")
	        .row,
	Kind(AssetKind::Terrain, "terrain", "Terrain")
	        .edited_by(DocumentTypeId::Terrain)
	        .names_files()
	        .folder("terrain")
	        .about("A terrain: the ground a mission is played on.")
	        .row,
	Kind(AssetKind::TerrainPolyData, "terrain_polydata", "Terrain height data")
	        .folder("terrain")
	        .about("A terrain's height data.")
	        .row,
	// Beside the missions: the game finds a mission's by its name (mission::sidecars).
	Kind(AssetKind::TileInfo, "tile_info", "Tile placement")
	        .folder("missions")
	        .new_name("newmission.til")
	        .about("A mission's tile placement, which the game finds by the mission's name.")
	        .row,
	Kind(AssetKind::Environment, "environment", "Environment")
	        .edited_by(DocumentTypeId::Environment)
	        .names_files()
	        .new_name("newenviro.env")
	        .folder("terrain")
	        .about("An environment: a mission's sky, light, fog and water.")
	        .row,
	Kind(AssetKind::Menu, "menu", "Menu")
	        .edited_by(DocumentTypeId::Menu)
	        .names_files()
	        .folder("menus")
	        .new_name("newmenu.mnu")
	        .about("A menu: screens of windows, buttons and lists.")
	        .row,
	Kind(AssetKind::MenuStyle, "menu_style", "Menu style")
	        .edited_by(DocumentTypeId::Styles)
	        .names_files()
	        .folder("menus")
	        .about("A menu stylesheet: the fonts and colours the menus name by variable.")
	        .row,
	// It names no file: its entries are its own chunks of audio (formats/sbf).
	Kind(AssetKind::MusicBank, "music_bank", "Music bank")
	        .folder("music")
	        .about("Music the game streams by its path, never through the archives.")
	        .row,
	// Its waves name the files (documents/sound_bank_document: a wave's file is a field's reference, a
	// set's name a symbol).
	Kind(AssetKind::SoundBank, "sound_bank", "Sound bank")
	        .edited_by(DocumentTypeId::SoundBank)
	        .names_files()
	        .folder("sounds")
	        .new_name("newbank.lwf")
	        .about("A sound bank: sound sets by name, each playing the waves it names.")
	        .row,
	Kind(AssetKind::Wave, "wave", "Wave")
	        .folder("sounds")
	        .new_name("newwave.wav")
	        .about("A sound: sound banks, dialogs and scripts name it, and the game loads it from the archives by name.")
	        .row,
	// documents/dialog_bank_document: a dialog's name a symbol, a line's wave one of the bank's sounds.
	Kind(AssetKind::DialogBank, "dialog_bank", "Dialog bank")
	        .edited_by(DocumentTypeId::DialogBank)
	        .names_files()
	        .folder("missions")
	        .new_name("newmission.dbf")
	        .about("A mission's dialogs: each a list of lines, each line a wave of the dialog bank's sounds.")
	        .row,
	Kind(AssetKind::Particles, "particles", "Particle effects")
	        .edited_by(DocumentTypeId::Particles)
	        .names_files()
	        .folder("particles")
	        .new_name("neweffects.ptl")
	        .about("Particle effects (smoke, fire, sparks) by name, which items, weapons and ammo name.")
	        .row,
	// Its operands' names (S13 D9), the script a RUN names [orig: Script_LoadAndCompileFile @
	// 0x4EE660] and the waves it plays (S14), each an edge the graph reads (documents/script_type).
	Kind(AssetKind::Script, "script", "Script")
	        .edited_by(DocumentTypeId::Script)
	        .names_files()
	        .folder("missions")
	        .new_name("newscript.wac")
	        .about("A mission's script: the commands its events run.")
	        .row,
	Kind(AssetKind::ItemDefs, "item_defs", "Item definitions")
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .about("The item definitions: every soldier, vehicle and object a mission can place.")
	        .row,
	Kind(AssetKind::WeaponDefs, "weapon_defs", "Weapon definitions")
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .about("The weapon definitions.")
	        .row,
	Kind(AssetKind::AmmoDefs, "ammo_defs", "Ammo definitions")
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .about("The ammunition definitions.")
	        .row,
	Kind(AssetKind::HudPosDefs, "hudpos_defs", "HUD layout")
	        .edited_by(DocumentTypeId::HudLayout)
	        .names_files()
	        .folder("defs")
	        .about("Where the HUD draws its parts.")
	        .row,
	Kind(AssetKind::HudFxDefs, "hudfx_defs", "HUD effects")
	        .edited_by(DocumentTypeId::Text)
	        .names_files()
	        .folder("defs")
	        .about("The HUD's effects.")
	        .row,
	Kind(AssetKind::AvatarDefs, "avatar_defs", "Avatars")
	        .edited_by(DocumentTypeId::Text)
	        .names_files()
	        .folder("defs")
	        .about("The player's avatars.")
	        .row,
	Kind(AssetKind::SoundProfileDefs, "sound_profile_defs", "Sound profiles")
	        .edited_by(DocumentTypeId::SoundProfiles)
	        .names_files()
	        .folder("defs")
	        .about("The sound profiles: named sets of sounds the game looks up by name.")
	        .row,
	Kind(AssetKind::CharAttrDefs, "charattr_defs", "Character attributes")
	        .edited_by(DocumentTypeId::CharAttrs)
	        .names_files()
	        .folder("defs")
	        .about("The characters' attributes.")
	        .row,
	Kind(AssetKind::PowerupDefs, "powerup_defs", "Powerup definitions")
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .about("The pickups: what each powerup gives.")
	        .row,
	Kind(AssetKind::OtherDefs, "other_defs", "Definitions")
	        .edited_by(DocumentTypeId::Text)
	        .names_files()
	        .folder("defs")
	        .about("A definition table the game reads by its name.")
	        .row,
	Kind(AssetKind::StringTableCoo, "string_table_coo", "NovaWorld string table")
	        .folder("strings")
	        .about("NovaWorld's string table, which the game opens from its own folder.")
	        .row,
	// S13 A8: no kind before, so the build left the NovaWorld screens out; a build packs them with the
	// menus (S16), the one place the game reads them with /d and without.
	Kind(AssetKind::NovaWorldScreen, "novaworld_screen", "NovaWorld screen")
	        .edited_by(DocumentTypeId::Text)
	        .folder("menus")
	        .about("A NovaWorld screen's page, which the menus read.")
	        .row,
	Kind(AssetKind::Video, "video", "Video")
	        .folder("videos")
	        .about("A video the menus or the intro play, read by its name from the game's folder.")
	        .row,
	// A build packs no save (gameprofile/player_files.h).
	Kind(AssetKind::PlayerSave, "player_save", "Player save")
	        .about("A player's saved settings: a project ships none.")
	        .row,
	// Its loader takes it in the SCR form as stored (vfs_loader_takes_stored): the shader type reads
	// the file itself.
	Kind(AssetKind::Shader, "shader", "Shader")
	        .edited_by(DocumentTypeId::Shader)
	        .folder("shaders")
	        .new_name("newshader.fx")
	        .about("A shader effect the renderer compiles.")
	        .row,
	// gt.ssc, read loose first from the expansion's folder, is its own row of the expansion's files
	// (route_for_expansion).
	Kind(AssetKind::Config, "config", "Configuration")
	        .edited_by(DocumentTypeId::Text)
	        .about("A configuration the game reads from its own folder before any archive.")
	        .row,
	Kind(AssetKind::Score, "score", "Score table")
	        .edited_by(DocumentTypeId::Text)
	        .about("The score table, read from the game's own folder.")
	        .row,
	// An expansion's version.txt is its own row of the expansion's files (route_for_expansion).
	Kind(AssetKind::Text, "text", "Text")
	        .edited_by(DocumentTypeId::Text)
	        .about("A text the game reads from its own folder.")
	        .row,
	// The build leaves it out, with no word (unlike a file of no kind the game knows, which may be data
	// the game misses); the editor opens it as a text. A file with no extension (a LICENSE) is one when
	// it holds text (classify_asset); one that holds bytes stays of no kind, said, as it may be data.
	Kind(AssetKind::Notes, "notes", "Project notes")
	        .edited_by(DocumentTypeId::Text)
	        .about("A note for the people who make the project (a README, a licence, a list of sources): the game "
	               "never reads it, so the build leaves it out.")
	        .row,
	// The scan gives it to a file an importer converts while its import record is there
	// (scan_project_assets), whatever the file's name would make it (a .png a texture). Its outputs,
	// named after it, pack by their own kinds; it never packs.
	Kind(AssetKind::ImportSource, "import_source", "Import source")
	        .about("A file the editor turns into the game's form: its import record says how, and the build packs what it made.")
	        .row,
	// The scan gives it to a file an import record lists among its inputs (S20: a terrain set's
	// heightmap and images), whatever its name would make it. It never packs: its import's outputs do.
	Kind(AssetKind::ImportInput, "import_input", "Import input")
	        .about("A file an import reads to make the game's files, such as a terrain's heightmap: the build packs what the import made, never it.")
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
// the registry has; every kind says what it is (about). The facts' own shape (names, slots) is the
// engine table's static_asserts' (base/resource_index/file_kind.cpp), and a new file's name ending
// with one of its kind's extensions is the asset kinds test's (the facts are no constant here).
constexpr bool rows_well_formed() {
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKindRow &row = kRows[i];
		if (static_cast<size_t>(row.kind) != i || !*row.token || !*row.label) return false;
		if (static_cast<size_t>(row.document) > kDocumentTypeCount) return false;
		if (!row.folder || !row.new_name || !row.about || !*row.about) return false;
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
