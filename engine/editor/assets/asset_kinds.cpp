#include <editor/assets/asset_kinds.h>


#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_kind.h>

namespace opennova::editor {

namespace {

constexpr const char *kArchive[] = {".pff", nullptr};
constexpr const char *kAnimation[] = {".bad", nullptr};
constexpr const char *kAnimationMap[] = {".adm", nullptr};
constexpr const char *kFaceAnimation[] = {".grm", nullptr};
constexpr const char *kAiProfile[] = {".aip", nullptr};
// A model's normal map made ahead is an .mdt: a TGA the object loader decodes as it does a
// .tga [orig: Texture_LoadByNameWithChannel @ 0x58B66F..0x58B6E6; Texture_LoadAndRegister @
// 0x58B80E..0x58B881], no DDS sibling taken for it [orig: Texture_LoadAsNormalMap @ 0x58C480]
// (renderer::material_texture_source). A PNG is one too: retail's menu loader decodes it [orig:
// CTextureManager_LoadOrFindTexture @ 0x654980 -> load_png_from_file @ 0x6654d0], so one with no
// import record packs as it is (one with its record is an ImportSource: scan_project_assets).
constexpr const char *kTexture[] = {".tga", ".pcx", ".dds", ".mdt", ".png", nullptr};
constexpr const char *kRawBin[] = {".bin", nullptr};
constexpr const char *kMissionText[] = {".mis", nullptr};
constexpr const char *kMapProject[] = {".npj", ".npz", nullptr};
constexpr const char *kTerrainPolyData[] = {".cpt", nullptr};
constexpr const char *kTileInfo[] = {".til", nullptr};
constexpr const char *kWave[] = {".wav", nullptr};
constexpr const char *kDialogBank[] = {".dbf", nullptr};
constexpr const char *kScript[] = {".wac", nullptr};
constexpr const char *kOtherDefs[] = {".def", nullptr};
constexpr const char *kStringTableCoo[] = {".coo", nullptr};
constexpr const char *kNovaWorldScreen[] = {".mnx", nullptr};
constexpr const char *kVideo[] = {".bik", nullptr};
constexpr const char *kPlayerSave[] = {".sav", nullptr};
constexpr const char *kShader[] = {".fx", nullptr};
// assets.cd is read before any archive mounts, as game.cfg is (docs/required-resources.md).
constexpr const char *kConfig[] = {".cfg", ".ini", ".ssc", ".cd", nullptr};
constexpr const char *kText[] = {".txt", nullptr};

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
	constexpr Kind scr(ScrForm form) const {
		Kind out = *this;
		out.row.scr = form;
		return out;
	}
	// The game's reader of it ends a line at CR LF alone (LineReader), cited where the row is.
	constexpr Kind lines(LineReader reader) const {
		Kind out = *this;
		out.row.line_reader = reader;
		return out;
	}
	// A loose kind's place in an expansion build (ExpansionLoose), cited where the row is.
	constexpr Kind expansion(ExpansionLoose place) const {
		Kind out = *this;
		out.row.expansion_loose = place;
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
	Kind(AssetKind::Unknown, "unknown", "Unknown file", ArchiveSlot::None)
	        .about("A file of no kind the game reads: the build leaves it out.")
	        .row,
	Kind(AssetKind::Archive, "archive", "Archive", ArchiveSlot::None).extensions(kArchive)
	        .about("A PFF archive: the editor imports files out of one, and a build makes the project's own.")
	        .row,
	Kind(AssetKind::Model, "model", "Model", ArchiveSlot::Resource)
	        .runtime("object_model")
	        .edited_by(DocumentTypeId::Model)
	        .names_files()
	        .folder("models")
	        .about("A 3D model: the items, vehicles and buildings the game shows name it.")
	        .row,
	Kind(AssetKind::Animation, "animation", "Animation", ArchiveSlot::Resource)
	        .extensions(kAnimation)
	        .edited_by(DocumentTypeId::Animation)
	        .folder("anims")
	        .about("A clip of bone animation: animation maps name it.")
	        .row,
	Kind(AssetKind::AnimationMap, "animation_map", "Animation map", ArchiveSlot::Resource)
	        .extensions(kAnimationMap)
	        .lines(LineReader::AsciiWalk) // [orig: AnimMap_LoadAdmFile @ 0x40ceb4 -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::AnimationMap)
	        .names_files()
	        .folder("anims")
	        .about("An animation map: which clip a soldier or a vehicle plays for each move.")
	        .row,
	// Its base and eye textures by name (formats/grm).
	Kind(AssetKind::FaceAnimation, "face_animation", "Face animation", ArchiveSlot::Resource)
	        .extensions(kFaceAnimation)
	        .names_files()
	        .folder("anims")
	        .about("A face's animation, with the base and eye textures it names.")
	        .row,
	Kind(AssetKind::AiProfile, "ai_profile", "AI profile", ArchiveSlot::Resource)
	        .extensions(kAiProfile)
	        .lines(LineReader::AsciiWalk) // [orig: AIProfile_LoadOrFind @ 0x45fe45 -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::Text)
	        .folder("ai")
	        .about("An AI profile: how a unit the computer runs picks its targets and moves, loaded by the name a placed unit gives.")
	        .row,
	Kind(AssetKind::Texture, "texture", "Texture", ArchiveSlot::Resource)
	        .extensions(kTexture)
	        .edited_by(DocumentTypeId::Texture)
	        .new_name("newtexture.tga")
	        .folder("textures")
	        .about("An image the game draws: a model's surfaces, a menu, the HUD.")
	        .row,
	// No name gives it: a model's chunk row reads the file it names as a chunk container whatever
	// the name [orig: NQ8B @0x58F350; HRZ8 @0x58F470; AOC8 @0x58F590] (renderer::load_material_chunk),
	// so a file no rule types by its name is one when its bytes hold one (classify_asset, the scan's
	// peek at its chunk headers); it packs with the art.
	Kind(AssetKind::MaterialChunk, "material_chunk", "Material chunk", ArchiveSlot::Resource)
	        .folder("textures")
	        .about("A model's material data: a material row of a model reads it as a chunk container.")
	        .row,
	Kind(AssetKind::Font, "font", "Font", ArchiveSlot::Localres)
	        .runtime("font")
	        .folder("fonts")
	        .new_name("newfont.fnt")
	        .about("A font the menus and the HUD write with.")
	        .row,
	// The boot text bins, the menu tables and the per-mission text sidecars.
	Kind(AssetKind::Strings, "strings", "String table", ArchiveSlot::Language)
	        .runtime("strings")
	        .edited_by(DocumentTypeId::Strings)
	        .folder("strings")
	        .new_name("newtable.bin")
	        .about("A string table: the words the menus, the HUD and the missions show, each by its key.")
	        .row,
	Kind(AssetKind::MusicScript, "music_script", "Music script", ArchiveSlot::Localres)
	        .runtime("music_script")
	        .edited_by(DocumentTypeId::MusicScript)
	        .folder("music")
	        .about("A music script: which music plays when.")
	        .row,
	// A raw table read through the archives, fgn2.bin among them: its only reader asks after the
	// archives mount [orig: CEffectSystem_Init @ 0x5f6070 through FileSystem_FileExists @
	// 0x75aa50].
	Kind(AssetKind::RawBin, "raw_bin", "Binary table", ArchiveSlot::Language)
	        .extensions(kRawBin)
	        .about("A table the game reads through the archives as it is.")
	        .row,
	// The country code the boot opens with the C library's fopen, never through the archives,
	// on every read [orig: Game_ReadCCBinFile @ 0x4a5860]: loose, as retail ships it.
	// Its fopen names the bare file ("CC.BIN"), so the game reads it from its working directory, the
	// install's folder, whatever expansion it runs [orig: Game_ReadCCBinFile @ 0x4a5860].
	Kind(AssetKind::CountryCode, "country_code", "Country code", ArchiveSlot::Loose)
	        .file("cc.bin")
	        .expansion(ExpansionLoose::RootOnly)
	        .about("The country code the game reads at boot from its own folder.")
	        .row,
	Kind(AssetKind::Credits, "credits", "Credits", ArchiveSlot::Localres)
	        .runtime("credits")
	        .lines(LineReader::ConfigFile) // [orig: ConfigFile_LoadFromFile @ 0x760a10, its text form]
	        .edited_by(DocumentTypeId::Credits)
	        .folder("menus")
	        .about("The credits the menus scroll.")
	        .row,
	// A .bms in localres: retail's mission list walks only the localres/language volumes [orig:
	// Mission_BuildMapListFromPFF @ 0x562910].
	Kind(AssetKind::Mission, "mission", "Mission", ArchiveSlot::Localres)
	        .runtime("mission")
	        .names_files()
	        .edited_by(DocumentTypeId::Mission)
	        .folder("missions")
	        .new_name("newmission.bms")
	        .about("A mission: what it places, its events and its settings.")
	        .row,
	// The original mission editor's interchange text (dfx2med.exe, docs/mission/mis-format-re.md): the
	// image holds no `.mis` literal and no reader of one, so the game never asks for it and the build
	// leaves it out. It names files (a terrain, items, weapons) no reader of the editor follows: an
	// import takes it and lists it as not followed.
	Kind(AssetKind::MissionText, "mission_text", "Mission text", ArchiveSlot::None)
	        .extensions(kMissionText)
	        .names_files()
	        .folder("missions")
	        .about("The original mission editor's text form of a mission: the game never reads it, and a build leaves it out.")
	        .row,
	// Where retail keeps its own (localres.pff holds ASP_G7.npz): its mission list's archive walk
	// takes a .npj or .npz as it takes a .bms [orig: Mission_BuildMapListFromPFF @ 0x562910]
	// (OpenNova's lists none yet: runtime/mission/mission_catalog.h).
	Kind(AssetKind::MapProject, "map_project", "Map project", ArchiveSlot::Localres)
	        .extensions(kMapProject)
	        .names_files()
	        .folder("missions")
	        .about("A map project, which the game's mission list takes as it takes a mission.")
	        .row,
	Kind(AssetKind::Terrain, "terrain", "Terrain", ArchiveSlot::Resource)
	        .runtime("terrain")
	        .names_files()
	        .folder("terrain")
	        .about("A terrain: the ground a mission is played on.")
	        .row,
	Kind(AssetKind::TerrainPolyData, "terrain_polydata", "Terrain height data",
	     ArchiveSlot::Resource)
	        .extensions(kTerrainPolyData)
	        .folder("terrain")
	        .about("A terrain's height data.")
	        .row,
	// Beside the missions: the game finds a mission's by its name (mission::sidecars).
	Kind(AssetKind::TileInfo, "tile_info", "Tile placement", ArchiveSlot::Resource)
	        .extensions(kTileInfo)
	        .folder("missions")
	        .new_name("newmission.til")
	        .about("A mission's tile placement, which the game finds by the mission's name.")
	        .row,
	Kind(AssetKind::Environment, "environment", "Environment", ArchiveSlot::Resource)
	        .runtime("environment")
	        .lines(LineReader::AsciiWalk) // [orig: Environment_LoadTimeOfDayConfig @ 0x57dbeb -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::Environment)
	        .names_files()
	        .new_name("newenviro.env")
	        .folder("terrain")
	        .about("An environment: a mission's sky, light, fog and water.")
	        .row,
	Kind(AssetKind::Menu, "menu", "Menu", ArchiveSlot::Localres)
	        .runtime("menu")
	        .edited_by(DocumentTypeId::Menu)
	        .names_files()
	        .folder("menus")
	        .new_name("newmenu.mnu")
	        .about("A menu: screens of windows, buttons and lists.")
	        .row,
	Kind(AssetKind::MenuStyle, "menu_style", "Menu style", ArchiveSlot::Localres)
	        .runtime("menu_style")
	        .edited_by(DocumentTypeId::Styles)
	        .names_files()
	        .folder("menus")
	        .about("A menu stylesheet: the fonts and colours the menus name by variable.")
	        .row,
	// Streamed by path, never through the archives (ArchiveSlot). It names no file: its entries are
	// its own chunks of audio (formats/sbf). An expansion's banks, M<name>.sbf and G<name>.sbf, are read
	// by their path in its own folder [orig: Expansion_LoadAssets @ 0x4a4906, @ 0x4a4936].
	Kind(AssetKind::MusicBank, "music_bank", "Music bank", ArchiveSlot::Loose)
	        .runtime("sbf")
	        .expansion(ExpansionLoose::Folder)
	        .folder("music")
	        .about("Music the game streams by its path, never through the archives.")
	        .row,
	// The sound sets, read by SoundBank_OpenFile (formats/lwf), their waves naming the files
	// (documents/sound_bank_document: a wave's file is a field's reference, a set's name a symbol).
	Kind(AssetKind::SoundBank, "sound_bank", "Sound bank", ArchiveSlot::Resource)
	        .runtime("sound")
	        .edited_by(DocumentTypeId::SoundBank)
	        .names_files()
	        .folder("sounds")
	        .new_name("newbank.lwf")
	        .about("A sound bank: sound sets by name, each playing the waves it names.")
	        .row,
	// A wave a sound bank's single names, which the game loads from the archives by name
	// (docs/audio/lwf-dbf-sound-re.md): retail packs its sound waves in localres.pff and its
	// localized voice lines in language.pff, and a name resolves from any mounted archive, so the
	// slot places it and nothing more.
	Kind(AssetKind::Wave, "wave", "Wave", ArchiveSlot::Localres).extensions(kWave)
	        .folder("sounds")
	        .about("A sound: sound banks, dialogs and scripts name it, and the game loads it from the archives by name.")
	        .row,
	Kind(AssetKind::DialogBank, "dialog_bank", "Dialog bank", ArchiveSlot::Localres)
	        .extensions(kDialogBank)
	        .names_files()
	        .folder("missions")
	        .about("A mission's dialog lines and the sounds they play.")
	        .row,
	Kind(AssetKind::Particles, "particles", "Particle effects", ArchiveSlot::Resource)
	        .runtime("particle")
	        .lines(LineReader::AsciiWalk) // [orig: CEffectSystem_Init @ 0x5f62f0 / 0x5f6545 -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::Particles)
	        .names_files()
	        .folder("particles")
	        .about("Particle effects (smoke, fire, sparks) by name, which items, weapons and ammo name.")
	        .row,
	// Its operands' names (S13 D9), the script a RUN names [orig: Script_LoadAndCompileFile @
	// 0x4EE660] and the waves it plays (S14), each an edge the graph reads (documents/script_type).
	Kind(AssetKind::Script, "script", "Script", ArchiveSlot::Localres)
	        .extensions(kScript)
	        .edited_by(DocumentTypeId::Script)
	        .names_files()
	        .folder("missions")
	        .new_name("newscript.wac")
	        .about("A mission's script: the commands its events run.")
	        .row,
	// The .def family by name: the runtime consumes each by its exact name, and browses only
	// Avatars.def and hudpos.def.
	Kind(AssetKind::ItemDefs, "item_defs", "Item definitions", ArchiveSlot::Localres)
	        .file("items.def")
	        .lines(LineReader::AsciiWalk) // [orig: ItemDefs_LoadAndValidate @ 0x4a1e12 -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .about("The item definitions: every soldier, vehicle and object a mission can place.")
	        .row,
	Kind(AssetKind::WeaponDefs, "weapon_defs", "Weapon definitions", ArchiveSlot::Localres)
	        .file("weapon.def")
	        .lines(LineReader::AsciiWalk) // [orig: WeaponDef_LoadAll @ 0x54dd50 -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .about("The weapon definitions.")
	        .row,
	Kind(AssetKind::AmmoDefs, "ammo_defs", "Ammo definitions", ArchiveSlot::Localres)
	        .file("ammo.def")
	        .lines(LineReader::AsciiWalk) // [orig: AmmoDef_LoadAll @ 0x40b0de / 0x40b116 -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .about("The ammunition definitions.")
	        .row,
	Kind(AssetKind::HudPosDefs, "hudpos_defs", "HUD layout", ArchiveSlot::Localres)
	        .runtime("hudpos")
	        .lines(LineReader::AsciiWalk) // [orig: HUD_InitOverlaySystem @ 0x5a4931 -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::HudLayout)
	        .names_files()
	        .folder("defs")
	        .about("Where the HUD draws its parts.")
	        .row,
	Kind(AssetKind::HudFxDefs, "hudfx_defs", "HUD effects", ArchiveSlot::Localres)
	        .file("hudfx.def")
	        .lines(LineReader::AsciiWalk) // [orig: HUD_InitOverlaySystem @ 0x5a4633 -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::Text)
	        .names_files()
	        .folder("defs")
	        .about("The HUD's effects.")
	        .row,
	Kind(AssetKind::AvatarDefs, "avatar_defs", "Avatars", ArchiveSlot::Localres)
	        .runtime("avatar")
	        .lines(LineReader::AsciiWalk) // [orig: CAvatarDefs_Init @ 0x57b1d5 -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::Text)
	        .names_files()
	        .folder("defs")
	        .about("The player's avatars.")
	        .row,
	Kind(AssetKind::SoundProfileDefs, "sound_profile_defs", "Sound profiles", ArchiveSlot::Localres)
	        .file("sndprof.def")
	        .lines(LineReader::AsciiWalk) // [orig: SoundProfile_LoadAll @ 0x5274dd -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::SoundProfiles)
	        .names_files()
	        .folder("defs")
	        .about("The sound profiles: named sets of sounds the game looks up by name.")
	        .row,
	Kind(AssetKind::CharAttrDefs, "charattr_defs", "Character attributes", ArchiveSlot::Localres)
	        .file("charattr.def")
	        .lines(LineReader::ConfigFile) // [orig: CharAttr_LoadFromDef @ 0x412177 -> ConfigFile_LoadFromFile]
	        .edited_by(DocumentTypeId::Text)
	        .names_files()
	        .folder("defs")
	        .about("The characters' attributes.")
	        .row,
	Kind(AssetKind::PowerupDefs, "powerup_defs", "Powerup definitions", ArchiveSlot::Localres)
	        .file("powerup.def")
	        .lines(LineReader::AsciiWalk) // [orig: PowerUpDef_LoadFromFile @ 0x44338e / 0x4433cc -> File_ParseASCIIFile]
	        .edited_by(DocumentTypeId::Catalog)
	        .names_files()
	        .folder("defs")
	        .about("The pickups: what each powerup gives.")
	        .row,
	Kind(AssetKind::OtherDefs, "other_defs", "Definitions", ArchiveSlot::Localres)
	        .extensions(kOtherDefs)
	        .edited_by(DocumentTypeId::Text)
	        .names_files()
	        .folder("defs")
	        .about("A definition table the game reads by its name.")
	        .row,
	// Opened with fopen by its bare name, from the install's folder whatever the expansion [orig:
	// CUIStringTable_OpenAndLoad @ 0x63a500, from @ 0x55262b].
	Kind(AssetKind::StringTableCoo, "string_table_coo", "NovaWorld string table",
	     ArchiveSlot::Loose)
	        .extensions(kStringTableCoo)
	        .folder("strings")
	        .expansion(ExpansionLoose::RootOnly)
	        .about("NovaWorld's string table, which the game opens from its own folder.")
	        .row,
	// The NovaWorld screens' markup: the error page [orig: "nw_error.mnx", UI_ShowNovaWorldErrorMessage
	// @ 0x558449] and the login's start page [orig: "nw_startup.mnx", UI_EnterNovaWorldMenu @ 0x558937],
	// whose STARTUPURL the gate substitutes (docs/net/novaworld-net-re.md D-NET-31) (S13 A8: no kind
	// before, so the build left them out). The menus' scene loader reads them [orig: @ 0x63e1b0 ->
	// FileSystem_LoadFileToBuffer @ 0x63e1c6], through the front door, which reads the archives alone
	// unless /d [orig: FileSystem_OpenFile @ 0x75b1c0, the loose search only when searchLooseFirst @
	// 0x75b1e5, set for the session by /d alone @ 0x4a6fac]. Retail ships them loose in its folder and
	// in no archive, where a launch without /d never reads them; a build packs them with the menus
	// (localres), the one place the game reads them with /d and without, and nowhere else.
	Kind(AssetKind::NovaWorldScreen, "novaworld_screen", "NovaWorld screen", ArchiveSlot::Localres)
	        .extensions(kNovaWorldScreen)
	        .edited_by(DocumentTypeId::Text)
	        .folder("menus")
	        .about("A NovaWorld screen's page, which the menus read.")
	        .row,
	// A video by its name, the expansion's own folder first, then the install's: the menus' [orig:
	// UI_CreateMenuBinkVideos @ 0x54b5ff..0x54b74a] and the intro's [orig: Game_PlayIntroVideos @
	// 0x5637d7..0x563848] (JO:CA's jox01 ships its header, footer and prologue there).
	Kind(AssetKind::Video, "video", "Video", ArchiveSlot::Loose)
	        .extensions(kVideo)
	        .expansion(ExpansionLoose::Folder)
	        .folder("videos")
	        .about("A video the menus or the intro play, read by its name from the game's folder.")
	        .row,
	// An expansion's weapon.sav is the game's beside the expansion's files [orig:
	// PlayerProfile_LoadAllFromDisk @ 0x54f6b7; the save @ 0x54becd]; a build packs no save
	// (assets/player_files.h).
	Kind(AssetKind::PlayerSave, "player_save", "Player save", ArchiveSlot::Loose)
	        .extensions(kPlayerSave)
	        .expansion(ExpansionLoose::Folder)
	        .about("A player's saved settings: a project ships none.")
	        .row,
	// The HLSL effects, which the shader loader takes in the SCR form alone, under its own key
	// [orig: ScriptFile_LoadAndDecrypt @ 0x5AE060].
	Kind(AssetKind::Shader, "shader", "Shader", ArchiveSlot::Resource)
	        .extensions(kShader)
	        .edited_by(DocumentTypeId::Shader)
	        .scr(ScrForm::Shader)
	        .folder("shaders")
	        .new_name("newshader.fx")
	        .about("A shader effect the renderer compiles.")
	        .row,
	// Read from the install's folder before any archive mounts (game.cfg, assets.cd:
	// docs/required-resources.md); gt.ssc, read loose first from the expansion's folder, is its own
	// row of the expansion's files (route_for_expansion).
	Kind(AssetKind::Config, "config", "Configuration", ArchiveSlot::Loose)
	        .extensions(kConfig)
	        .edited_by(DocumentTypeId::Text)
	        .expansion(ExpansionLoose::RootOnly)
	        .about("A configuration the game reads from its own folder before any archive.")
	        .row,
	// Loose in the install root, where retail ships it, opened by its bare name [orig: ScoreConfig_LoadFile
	// @ 0x52d8a0].
	Kind(AssetKind::Score, "score", "Score table", ArchiveSlot::Loose)
	        .file("score.ini")
	        .edited_by(DocumentTypeId::Text)
	        .expansion(ExpansionLoose::RootOnly)
	        .about("The score table, read from the game's own folder.")
	        .row,
	// A text the game reads opens its bare name in the install's folder (earlyerr.txt [orig:
	// Game_ShowEarlyError @ 0x4a68a0 through Game_ReadLineFromFile @ 0x4a59a0]); an expansion's
	// version.txt is its own row of the expansion's files (route_for_expansion).
	Kind(AssetKind::Text, "text", "Text", ArchiveSlot::Loose)
	        .extensions(kText)
	        .edited_by(DocumentTypeId::Text)
	        .expansion(ExpansionLoose::RootOnly)
	        .about("A text the game reads from its own folder.")
	        .row,
	// No name gives it: the scan gives it to a file an importer converts while its import record
	// is there (scan_project_assets), whatever the file's name would make it (a .png a texture).
	// Its outputs, named after it, pack by their own kinds; it never packs.
	Kind(AssetKind::ImportSource, "import_source", "Import source", ArchiveSlot::None)
	        .about("A file the editor turns into the game's form: its import record says how, and the build packs what it made.")
	        .row,
	// No name gives it either: the scan gives it to a file an import record lists among its inputs (S20:
	// a terrain set's heightmap and images), whatever its name would make it. It never packs: its
	// import's outputs do.
	Kind(AssetKind::ImportInput, "import_input", "Import input", ArchiveSlot::None)
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

// Whether a null-ended list holds `name`.
constexpr bool lists(const char *const *names, const char *name) {
	for (; names && *names; ++names)
		if (same_text(*names, name)) return true;
	return false;
}

constexpr size_t text_length(const char *text) {
	size_t length = 0;
	while (text[length]) ++length;
	return length;
}

// Whether `name` ends with `tail`.
constexpr bool ends_with(const char *name, const char *tail) {
	const size_t n = text_length(name), t = text_length(tail);
	return n >= t && same_text(name + (n - t), tail);
}

// A new file's name of a kind that lists its extensions ends with one of them (a kind the
// runtime's classifier types by its bytes has no list to hold it to).
constexpr bool new_name_fits(const AssetKindRow &row) {
	if (!*row.new_name || !row.extensions) return true;
	for (const char *const *extension = row.extensions; *extension; ++extension)
		if (ends_with(row.new_name, *extension)) return true;
	return false;
}

// One row per kind, at the kind's own index; no two rows share a token, a runtime token, a file
// name or an extension (a name gives one kind); an archive, an import source and a file of no
// kind the game knows pack nowhere, every other kind somewhere; no name gives an import source or
// a material chunk (the scan does, by a record beside the file or by its bytes); a kind is edited
// by a type the registry has; a new file's name ends with one of the kind's extensions where it
// lists them; every kind says what it is (about).
constexpr bool rows_well_formed() {
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKindRow &row = kRows[i];
		if (static_cast<size_t>(row.kind) != i || !*row.token || !*row.label) return false;
		const bool left_out = row.kind == AssetKind::Archive || row.kind == AssetKind::ImportSource ||
		                      row.kind == AssetKind::ImportInput || row.kind == AssetKind::Unknown ||
		                      row.kind == AssetKind::MissionText;
		if ((row.archive_slot == ArchiveSlot::None) != left_out) return false;
		// A loose kind says where an expansion's game reads it; no other kind does.
		if ((row.archive_slot == ArchiveSlot::Loose) != (row.expansion_loose != ExpansionLoose::None)) return false;
		const bool by_the_scan = row.kind == AssetKind::ImportSource || row.kind == AssetKind::ImportInput ||
		                         row.kind == AssetKind::MaterialChunk;
		if (by_the_scan && (*row.runtime || row.file_name || row.extensions)) return false;
		if (static_cast<size_t>(row.document) > kDocumentTypeCount) return false;
		if (!row.folder || !row.new_name || !new_name_fits(row) || !row.about || !*row.about) return false;
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
	const std::string file = io::utf8_file_name(logical_name);
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
	const ArchiveSlot slot = asset_kind_row(kind).archive_slot;
	return kind == AssetKind::ImportSource || (slot != ArchiveSlot::Loose && slot != ArchiveSlot::None);
}

} // namespace opennova::editor
