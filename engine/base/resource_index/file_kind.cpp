#include <base/resource_index/file_kind.h>

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_kind.h>
#include <base/vfs/vfs.h>

namespace opennova {

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
// import record is a texture as it is (one with its record beside it is an import source).
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
// The texts the game reads, each by its bare name from its folder (no other .txt: every other .txt name its
// program holds is one it only writes, a log or a marker, but the menu cache's own cache\mru.txt, kept in the
// cache folder [orig: CUICache_ScanCacheDirectory @ 0x64d6b0], a player's file; docs/correspondence.md, "The
// texts the game reads by name"): earlyerr.txt [orig: Game_ShowEarlyError @
// 0x4a68a0 -> Game_ReadLineFromFile @ 0x4a59a0]; an expansion's version.txt [orig: Expansion_LoadAssets @
// 0x4a4730, "expansion\%s\version.txt" @ 0x4a4852]; the chat filter [orig: ChatFilter_LoadFromFile @
// 0x4fd640]; the high scores [orig: HUD_LoadHighScoreText @ 0x5630e0]; the banned addresses [orig:
// Server_InitNewRoundState @ 0x51c8e0, the read @ 0x51cb3b]; a NovaWorld host's ban list [orig:
// BanList_InitFromMission @ 0x5098f0]; the session timeout, read when it is there [orig: CNapiNetwork_Init @
// 0x4ca4a0, "_NSTMOUT.TXT" @ 0x4ca9e1]; and four the game reads by their being there: the development gate,
// no stack trace and the network log [orig: Game_ParseCommandLineAndInit @ 0x4a7310, "_devnova.txt" @
// 0x4a7c7b, "_NOSTACKTRACE.TXT" @ 0x4a7caa, "_DONETLOG.TXT" @ 0x4a7cc1], and no video test [orig:
// Game_RunVideoTestDialog @ 0x53ec10, "_VIDTEST.TXT" @ 0x53ec4b].
constexpr const char *kTextNames[] = {"earlyerr.txt",  "version.txt",  "filter.txt",        "hiscore.txt",
                                      "banned.txt",    "banlist.txt",  "_nstmout.txt",      "_devnova.txt",
                                      "_nostacktrace.txt", "_donetlog.txt", "_vidtest.txt", nullptr};
// What a project keeps for its people and the game never reads: Markdown (no reader of it in the game's
// program), and a .txt of any name but those above.
constexpr const char *kNotes[] = {".md", ".txt", nullptr};

// A row built up column by column, so each row names only what it sets; the slot is always
// stated.
struct Kind {
	FileKindFacts row;
	constexpr Kind(FileKind kind, ArchiveSlot slot) : row() {
		row.kind = kind;
		row.archive_slot = slot;
	}
	// A kind the runtime's catalog browses, by its catalog token.
	constexpr Kind resource_kind(const char *token) const {
		Kind out = *this;
		out.row.resource_kind = token;
		return out;
	}
	constexpr Kind file(const char *name) const {
		Kind out = *this;
		out.row.file_name = name;
		return out;
	}
	constexpr Kind files(const char *const *names) const {
		Kind out = *this;
		out.row.file_names = names;
		return out;
	}
	constexpr Kind extensions(const char *const *names) const {
		Kind out = *this;
		out.row.extensions = names;
		return out;
	}
	// The game's reader of it ends a line at CR LF alone (LineReader), cited where the row is.
	constexpr Kind lines(LineReader reader) const {
		Kind out = *this;
		out.row.line_reader = reader;
		return out;
	}
	// A loose kind's place under an expansion (ExpansionLoose), cited where the row is.
	constexpr Kind expansion(ExpansionLoose place) const {
		Kind out = *this;
		out.row.expansion_loose = place;
		return out;
	}
};

constexpr FileKindFacts kRows[] = {
	// A file of no kind the game knows: the game never asks for one.
	Kind(FileKind::Unknown, ArchiveSlot::None).row,
	Kind(FileKind::Archive, ArchiveSlot::None).extensions(kArchive).row,
	Kind(FileKind::Model, ArchiveSlot::Resource).resource_kind("object_model").row,
	// Retail packs its animations, their maps and the AI profiles in localres.pff, every one (JO:CA's,
	// file_kind_test's retail leg).
	Kind(FileKind::Animation, ArchiveSlot::Localres).extensions(kAnimation).row,
	Kind(FileKind::AnimationMap, ArchiveSlot::Localres)
	        .extensions(kAnimationMap)
	        .lines(LineReader::AsciiWalk) // [orig: AnimMap_LoadAdmFile @ 0x40ceb4 -> File_ParseASCIIFile]
	        .row,
	// Its base and eye textures by name (formats/grm).
	Kind(FileKind::FaceAnimation, ArchiveSlot::Resource).extensions(kFaceAnimation).row,
	Kind(FileKind::AiProfile, ArchiveSlot::Localres)
	        .extensions(kAiProfile)
	        .lines(LineReader::AsciiWalk) // [orig: AIProfile_LoadOrFind @ 0x45fe45 -> File_ParseASCIIFile]
	        .row,
	Kind(FileKind::Texture, ArchiveSlot::Resource).extensions(kTexture).row,
	// No name gives it: a model's chunk row reads the file it names as a chunk container whatever
	// the name [orig: NQ8B @0x58F350; HRZ8 @0x58F470; AOC8 @0x58F590] (renderer::load_material_chunk),
	// so a file no rule types by its name is one when its bytes hold one (renderer::
	// is_material_chunk_container, a peek at its chunk headers); it packs with the art.
	Kind(FileKind::MaterialChunk, ArchiveSlot::Resource).row,
	Kind(FileKind::Font, ArchiveSlot::Localres).resource_kind("font").row,
	// The boot text bins, the menu tables and the per-mission text sidecars.
	Kind(FileKind::Strings, ArchiveSlot::Language).resource_kind("strings").row,
	Kind(FileKind::MusicScript, ArchiveSlot::Localres).resource_kind("music_script").row,
	// A raw table read through the archives, fgn2.bin among them: its only reader asks after the
	// archives mount [orig: CEffectSystem_Init @ 0x5f6070 through FileSystem_FileExists @
	// 0x75aa50].
	Kind(FileKind::RawBin, ArchiveSlot::Language).extensions(kRawBin).row,
	// The country code the boot opens with the C library's fopen, never through the archives,
	// on every read [orig: Game_ReadCCBinFile @ 0x4a5860]: loose, as retail ships it.
	// Its fopen names the bare file ("CC.BIN"), so the game reads it from its working directory, the
	// install's folder, whatever expansion it runs [orig: Game_ReadCCBinFile @ 0x4a5860].
	Kind(FileKind::CountryCode, ArchiveSlot::Loose).file("cc.bin").expansion(ExpansionLoose::RootOnly).row,
	// Retail's (NLIST.KDA) in language.pff, an expansion's in its language archive (jox01L.pff).
	Kind(FileKind::Credits, ArchiveSlot::Language)
	        .resource_kind("credits")
	        .lines(LineReader::ConfigFile) // [orig: ConfigFile_LoadFromFile @ 0x760a10, its text form]
	        .row,
	// A .bms in localres: retail's mission list walks only the localres/language volumes [orig:
	// Mission_BuildMapListFromPFF @ 0x562910] (runtime/mission/mission_catalog's walk pairs).
	Kind(FileKind::Mission, ArchiveSlot::Localres).resource_kind("mission").row,
	// The original mission editor's interchange text (dfx2med.exe, docs/mission/mis-format-re.md): the
	// image holds no `.mis` literal and no reader of one, so the game never asks for it.
	Kind(FileKind::MissionText, ArchiveSlot::None).extensions(kMissionText).row,
	// Where retail keeps its own (localres.pff holds ASP_G7.npz): its mission list's archive walk
	// takes a .npj or .npz as it takes a .bms [orig: Mission_BuildMapListFromPFF @ 0x562910]
	// (runtime/mission/mission_catalog.h).
	Kind(FileKind::MapProject, ArchiveSlot::Localres).extensions(kMapProject).row,
	Kind(FileKind::Terrain, ArchiveSlot::Resource)
	        .resource_kind("terrain")
	        // The time-of-day load's .trn pass, whose hook reads the terrain's keys [orig:
	        // Environment_LoadTimeOfDayConfig @ 0x57dbeb -> File_ParseASCIIFile; Terrain_ParseConfigCallback @ 0x60F330].
	        .lines(LineReader::AsciiWalk)
	        .row,
	Kind(FileKind::TerrainPolyData, ArchiveSlot::Resource).extensions(kTerrainPolyData).row,
	// Beside the missions, as retail packs every one: the game finds a mission's by its name
	// (runtime/mission/mission_sidecars).
	Kind(FileKind::TileInfo, ArchiveSlot::Localres).extensions(kTileInfo).row,
	Kind(FileKind::Environment, ArchiveSlot::Resource)
	        .resource_kind("environment")
	        .lines(LineReader::AsciiWalk) // [orig: Environment_LoadTimeOfDayConfig @ 0x57dbeb -> File_ParseASCIIFile]
	        .row,
	Kind(FileKind::Menu, ArchiveSlot::Localres).resource_kind("menu").row,
	Kind(FileKind::MenuStyle, ArchiveSlot::Localres).resource_kind("menu_style").row,
	// Streamed by path, never through the archives (ArchiveSlot). It names no file: its entries are
	// its own chunks of audio (formats/sbf). An expansion's banks, M<name>.sbf and G<name>.sbf, are read
	// by their path in its own folder [orig: Expansion_LoadAssets @ 0x4a4906, @ 0x4a4936].
	Kind(FileKind::MusicBank, ArchiveSlot::Loose).resource_kind("sbf").expansion(ExpansionLoose::Folder).row,
	// The sound sets, read by SoundBank_OpenFile (formats/lwf), their waves naming the files. Retail
	// packs a mission's bank in language.pff with the voice lines its waves name, and the game's own two,
	// game.lwf and menu.LWF, in localres.pff with the sounds theirs name; an expansion splits its own
	// alike (jox01L.pff holds five, jox01.pff JOx01.LWF). A bank resolves from any mounted archive, so the
	// slot places a project's banks, a mission's dialog banks the ones a project makes.
	Kind(FileKind::SoundBank, ArchiveSlot::Language).resource_kind("sound").row,
	// A wave a sound bank's single names, which the game loads from the archives by name
	// (docs/audio/lwf-dbf-sound-re.md): retail packs its sound waves in localres.pff and its
	// localized voice lines in language.pff, and a name resolves from any mounted archive, so the
	// slot places it and nothing more.
	Kind(FileKind::Wave, ArchiveSlot::Localres).extensions(kWave).row,
	// A mission's dialogs, read by DialogManager_LoadFromFile (formats/dbf).
	Kind(FileKind::DialogBank, ArchiveSlot::Localres).extensions(kDialogBank).row,
	// Retail packs every one in localres.pff.
	Kind(FileKind::Particles, ArchiveSlot::Localres)
	        .resource_kind("particle")
	        .lines(LineReader::AsciiWalk) // [orig: CEffectSystem_Init @ 0x5f62f0 / 0x5f6545 -> File_ParseASCIIFile]
	        .row,
	Kind(FileKind::Script, ArchiveSlot::Localres).extensions(kScript).row,
	// The .def family by name: the runtime consumes each by its exact name, and browses only
	// Avatars.def and hudpos.def.
	Kind(FileKind::ItemDefs, ArchiveSlot::Localres)
	        .file("items.def")
	        .lines(LineReader::AsciiWalk) // [orig: ItemDefs_LoadAndValidate @ 0x4a1e12 -> File_ParseASCIIFile]
	        .row,
	Kind(FileKind::WeaponDefs, ArchiveSlot::Localres)
	        .file("weapon.def")
	        .lines(LineReader::AsciiWalk) // [orig: WeaponDef_LoadAll @ 0x54dd50 -> File_ParseASCIIFile]
	        .row,
	Kind(FileKind::AmmoDefs, ArchiveSlot::Localres)
	        .file("ammo.def")
	        .lines(LineReader::AsciiWalk) // [orig: AmmoDef_LoadAll @ 0x40b0de / 0x40b116 -> File_ParseASCIIFile]
	        .row,
	Kind(FileKind::HudPosDefs, ArchiveSlot::Localres)
	        .resource_kind("hudpos")
	        .lines(LineReader::AsciiWalk) // [orig: HUD_InitOverlaySystem @ 0x5a4931 -> File_ParseASCIIFile]
	        .row,
	Kind(FileKind::HudFxDefs, ArchiveSlot::Localres)
	        .file("hudfx.def")
	        .lines(LineReader::AsciiWalk) // [orig: HUD_InitOverlaySystem @ 0x5a4633 -> File_ParseASCIIFile]
	        .row,
	Kind(FileKind::AvatarDefs, ArchiveSlot::Localres)
	        .resource_kind("avatar")
	        .lines(LineReader::AsciiWalk) // [orig: CAvatarDefs_Init @ 0x57b1d5 -> File_ParseASCIIFile]
	        .row,
	Kind(FileKind::SoundProfileDefs, ArchiveSlot::Localres)
	        .file("sndprof.def")
	        .lines(LineReader::AsciiWalk) // [orig: SoundProfile_LoadAll @ 0x5274dd -> File_ParseASCIIFile]
	        .row,
	Kind(FileKind::CharAttrDefs, ArchiveSlot::Localres)
	        .file("charattr.def")
	        .lines(LineReader::ConfigFile) // [orig: CharAttr_LoadFromDef @ 0x412177 -> ConfigFile_LoadFromFile]
	        .row,
	Kind(FileKind::PowerupDefs, ArchiveSlot::Localres)
	        .file("powerup.def")
	        .lines(LineReader::AsciiWalk) // [orig: PowerUpDef_LoadFromFile @ 0x44338e / 0x4433cc -> File_ParseASCIIFile]
	        .row,
	Kind(FileKind::OtherDefs, ArchiveSlot::Localres).extensions(kOtherDefs).row,
	// Opened with fopen by its bare name, from the install's folder whatever the expansion [orig:
	// CUIStringTable_OpenAndLoad @ 0x63a500, from @ 0x55262b].
	Kind(FileKind::StringTableCoo, ArchiveSlot::Loose)
	        .extensions(kStringTableCoo)
	        .expansion(ExpansionLoose::RootOnly)
	        .row,
	// The NovaWorld screens' markup: the error page [orig: "nw_error.mnx", UI_ShowNovaWorldErrorMessage
	// @ 0x558449] and the login's start page [orig: "nw_startup.mnx", UI_EnterNovaWorldMenu @ 0x558937],
	// whose STARTUPURL the gate substitutes (docs/net/novaworld-net-re.md D-NET-31). The menus' scene
	// loader reads them [orig: @ 0x63e1b0 -> FileSystem_LoadFileToBuffer @ 0x63e1c6], through the front
	// door, which reads the archives alone unless /d [orig: FileSystem_OpenFile @ 0x75b1c0, the loose
	// search only when searchLooseFirst @ 0x75b1e5, set for the session by /d alone @ 0x4a6fac]. Retail
	// ships them loose in its folder and in no archive, where a launch without /d never reads them; a
	// packed set holds them with the menus (localres), the one place the game reads them with /d and
	// without, and nowhere else.
	Kind(FileKind::NovaWorldScreen, ArchiveSlot::Localres).extensions(kNovaWorldScreen).row,
	// A video by its name, the expansion's own folder first, then the install's: the menus' [orig:
	// UI_CreateMenuBinkVideos @ 0x54b5ff..0x54b74a] and the intro's [orig: Game_PlayIntroVideos @
	// 0x5637d7..0x563848] (JO:CA's jox01 ships its header, footer and prologue there).
	Kind(FileKind::Video, ArchiveSlot::Loose).extensions(kVideo).expansion(ExpansionLoose::Folder).row,
	// An expansion's weapon.sav is the game's beside the expansion's files [orig:
	// PlayerProfile_LoadAllFromDisk @ 0x54f6b7; the save @ 0x54becd]; a player's file, which no game's
	// set ships (gameprofile/player_files.h).
	Kind(FileKind::PlayerSave, ArchiveSlot::Loose).extensions(kPlayerSave).expansion(ExpansionLoose::Folder).row,
	// The HLSL effects, which the shader loader takes in the SCR form alone, under its own key
	// [orig: ScriptFile_LoadAndDecrypt @ 0x5AE060] (vfs_loader_takes_stored). The loader reads the
	// working directory's loose ones and then walks every mounted archive's entries for them [orig:
	// HLSLEffect_InitAndLoadAll @ 0x5b0080, the slots 0..14 @ 0x5b0141, through
	// HLSLEffect_LoadAllFromPFFArchive @ 0x5afed0], where it is given no override folder, which
	// would be read alone [orig: @ 0x5b0112..0x5b0118]; both its callers give none [orig:
	// Render_InitAllSubsystems @ 0x58642f; sub_586480 @ 0x5864a6]. So any slot serves one; retail
	// packs its 44 in localres.pff (JO:CA, measured 2026-10-09: none in resource.pff, language.pff or
	// jox01's), the slot that places them.
	Kind(FileKind::Shader, ArchiveSlot::Localres).extensions(kShader).row,
	// Read from the install's folder before any archive mounts (game.cfg, assets.cd:
	// docs/required-resources.md); gt.ssc, read loose first from the expansion's folder, is the
	// name's own rule (vfs_read_from_expansion_folder).
	Kind(FileKind::Config, ArchiveSlot::Loose).extensions(kConfig).expansion(ExpansionLoose::RootOnly).row,
	// Loose in the install root, where retail ships it, opened by its bare name [orig: ScoreConfig_LoadFile
	// @ 0x52d8a0].
	Kind(FileKind::Score, ArchiveSlot::Loose).file("score.ini").expansion(ExpansionLoose::RootOnly).row,
	// A text the game reads opens its bare name in the install's folder, by the names it knows (kTextNames,
	// earlyerr.txt [orig: Game_ShowEarlyError @ 0x4a68a0 through Game_ReadLineFromFile @ 0x4a59a0]); an
	// expansion's version.txt is read from the expansion's folder by its path [orig: Expansion_LoadAssets @
	// 0x4a4852], the expansion's own file's rule.
	Kind(FileKind::Text, ArchiveSlot::Loose).files(kTextNames).expansion(ExpansionLoose::RootOnly).row,
	// What the game never reads: a project's README, its licence, its list of sources, a note of any kind.
	// A file with no extension (a LICENSE) is one when it holds text; one that holds bytes stays of no kind,
	// as it may be data.
	Kind(FileKind::Notes, ArchiveSlot::None).extensions(kNotes).row,
	// No name gives it: it is a file an importer converts while its import record is there, whatever
	// the file's name would make it (a .png a texture). Its outputs, named after it, are read by their
	// own kinds; the game never reads it.
	Kind(FileKind::ImportSource, ArchiveSlot::None).row,
	// No name gives it either: it is a file an import record lists among its inputs (a terrain set's
	// heightmap and images), whatever its name would make it. The game never reads it: its import's
	// outputs it does.
	Kind(FileKind::ImportInput, ArchiveSlot::None).row,
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

// Whether row `a` and row `b` give a whole name both: one's file name or a name of its list the other's.
constexpr bool share_a_name(const FileKindFacts &a, const FileKindFacts &b) {
	if (a.file_name && (lists(b.file_names, a.file_name) || (b.file_name && same_text(a.file_name, b.file_name))))
		return true;
	for (const char *const *name = a.file_names; name && *name; ++name)
		if (lists(b.file_names, *name) || (b.file_name && same_text(*name, b.file_name))) return true;
	return false;
}

// One row per kind, at the kind's own index; no two rows share a catalog token, a whole name or an
// extension (a name gives one kind); an archive, an import source and an import's input, the
// original mission editor's text, a project's notes and a file of no kind the game knows are read
// from nowhere, every other kind from somewhere; a loose kind, and no other, says where an
// expansion's game reads it; no name gives an import source, an import's input or a material chunk
// (a record beside the file does, or its bytes).
constexpr bool rows_well_formed() {
	for (size_t i = 0; i < kFileKindCount; ++i) {
		const FileKindFacts &row = kRows[i];
		if (static_cast<size_t>(row.kind) != i || !row.resource_kind) return false;
		const bool read_from_nowhere = row.kind == FileKind::Archive || row.kind == FileKind::ImportSource ||
		                               row.kind == FileKind::ImportInput || row.kind == FileKind::Unknown ||
		                               row.kind == FileKind::MissionText || row.kind == FileKind::Notes;
		if ((row.archive_slot == ArchiveSlot::None) != read_from_nowhere) return false;
		if ((row.archive_slot == ArchiveSlot::Loose) != (row.expansion_loose != ExpansionLoose::None)) return false;
		const bool by_content = row.kind == FileKind::ImportSource || row.kind == FileKind::ImportInput ||
		                        row.kind == FileKind::MaterialChunk;
		if (by_content && (*row.resource_kind || row.file_name || row.file_names || row.extensions)) return false;
		for (size_t j = 0; j < i; ++j) {
			const FileKindFacts &other = kRows[j];
			if (*row.resource_kind && same_text(row.resource_kind, other.resource_kind)) return false;
			if (share_a_name(row, other)) return false;
			for (const char *const *name = row.extensions; name && *name; ++name)
				if (lists(other.extensions, *name)) return false;
		}
	}
	return true;
}

static_assert(sizeof(kRows) / sizeof(kRows[0]) == kFileKindCount, "every FileKind has exactly one row");
static_assert(rows_well_formed(),
              "the rows follow FileKind's order and name each catalog token, file name and extension once");

// The boot table's three archives, in the slots' order.
static_assert(sizeof(kBootArchiveTable) / sizeof(kBootArchiveTable[0]) == 3, "language, localres, resource");
static_assert(static_cast<int>(ArchiveSlot::Language) == 0 && static_cast<int>(ArchiveSlot::Localres) == 1 &&
                      static_cast<int>(ArchiveSlot::Resource) == 2,
              "an archive slot indexes the boot table");

} // namespace

const FileKindFacts &file_kind_facts(FileKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return index < kFileKindCount ? kRows[index] : kRows[0];
}

FileKind file_kind_for_resource_kind(const std::string &resource_kind) {
	if (resource_kind.empty()) return FileKind::Unknown;
	for (const FileKindFacts &row : kRows)
		if (resource_kind == row.resource_kind) return row.kind;
	return FileKind::Unknown;
}

FileKind file_kind_for_file(const std::string &logical_name, const std::vector<uint8_t> *bytes) {
	// The runtime catalog's kinds by its own classifier, then the rows' own names.
	const std::string shared = resource_kind_for_file(logical_name, bytes);
	if (!shared.empty()) return file_kind_for_resource_kind(shared);
	const std::string name = strutil::to_lower(io::utf8_file_name(logical_name));
	for (const FileKindFacts &row : kRows)
		if ((row.file_name && name == row.file_name) || lists(row.file_names, name.c_str())) return row.kind;
	const std::string extension = resource_extension_for_name(logical_name);
	if (extension.empty()) return FileKind::Unknown;
	for (const FileKindFacts &row : kRows)
		if (lists(row.extensions, extension.c_str())) return row.kind;
	return FileKind::Unknown;
}

FileKind file_kind_for_name(const std::string &logical_name) { return file_kind_for_file(logical_name, nullptr); }

FileKind file_kind_for_required_name(const std::string &name) {
	const std::string extension = resource_extension_for_name(name);
	if (extension != ".bin") return file_kind_for_name(name);
	// The witnessed `.bin` rows: a kind a row knows by its whole name (CC.BIN, the country
	// code), the music-script pair (and its expansion forms), the three raw markers/credential
	// stores, and string tables for everything else.
	const FileKind named = file_kind_for_name(name);
	if (named != FileKind::RawBin && named != FileKind::Unknown) return named;
	const std::string basename = strutil::to_lower(io::utf8_file_name(name));
	if (basename == "menumus.bin" || basename == "gamemus.bin") return FileKind::MusicScript;
	if (basename == "fgn2.bin" || basename == "epass.bin" || basename == "passgen.bin") return FileKind::RawBin;
	return FileKind::Strings;
}

const char *archive_slot_file_name(ArchiveSlot slot) {
	switch (slot) {
	case ArchiveSlot::Language:
	case ArchiveSlot::Localres:
	case ArchiveSlot::Resource: return kBootArchiveTable[static_cast<size_t>(slot)];
	case ArchiveSlot::Loose:
	case ArchiveSlot::None: return "";
	}
	return "";
}

} // namespace opennova
