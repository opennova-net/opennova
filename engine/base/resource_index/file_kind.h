#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// What a file IS to the game (ADR 0046 d6/d7), typed: the one kind vocabulary. The runtime
// catalog's string kinds (resource_kind.h, `resource_kind_for_name_and_magic`) are the shared
// source for every kind the runtime browses, each the kind whose facts name its catalog token;
// this enum adds the kinds the catalog does not browse (the name-keyed .def family, scripts,
// textures, banks, videos, plain text) so a required file and a file found on disk compare by
// one type. What the game's loaders know of each kind (the names that give it, the archive it
// lives in, where an expansion's game reads it loose, which reader cuts its text into lines) is
// its row of facts (file_kind_facts): a new kind is one value here, before kCount, and one row in
// file_kind.cpp, which does not build without it.
enum class FileKind {
	Unknown = 0,
	Archive,        // .pff (never a file inside a game's set: an archive holds them)
	Model,          // .3di
	Animation,      // .bad
	AnimationMap,   // .adm
	FaceAnimation,  // .grm (a character's facial texture meshes and gesture offsets)
	AiProfile,      // .aip
	Texture,        // .tga .pcx .dds .mdt .png
	MaterialChunk,  // a model's material chunk container, by its content under a name no rule types
	Font,           // .fnt
	Strings,        // RTXT .bin
	MusicScript,    // SCR0 .bin
	RawBin,         // .bin with neither magic (raw tables, exp_info style)
	CountryCode,    // CC.BIN (the country code the boot reads loose)
	Credits,        // .kda
	Mission,        // .bms
	MissionText,    // .mis (the original mission editor's interchange text, which the game never reads)
	MapProject,     // .npj .npz (the mission editor's project, the mission list's other scan)
	Terrain,        // .trn
	TerrainPolyData, // .cpt
	TileInfo,       // .til
	Environment,    // .env
	Menu,           // .mnu
	MenuStyle,      // .mns
	MusicBank,      // .sbf (the music a music script plays, streamed by path)
	SoundBank,      // .lwf (the sound sets, each naming its waves)
	Wave,           // .wav (a sound bank's single, loaded from the archives by name)
	DialogBank,     // .dbf
	Particles,      // .ptl .ptu .ptg
	Script,         // .wac
	ItemDefs,       // items.def
	WeaponDefs,     // weapon.def
	AmmoDefs,       // ammo.def
	HudPosDefs,     // hudpos.def
	HudFxDefs,      // hudfx.def
	AvatarDefs,     // Avatars.def
	SoundProfileDefs, // SndProf.def
	CharAttrDefs,   // charattr.def
	PowerupDefs,    // powerup.def
	OtherDefs,      // any other .def
	StringTableCoo, // .coo (the NovaWorld UI string table)
	NovaWorldScreen, // .mnx (a NovaWorld screen's markup; retail ships it loose, a packed set holds it)
	Video,          // .bik
	PlayerSave,     // .sav
	Shader,         // .fx
	Config,         // .cfg .ini .ssc .cd
	Score,          // score.ini (the scoring table per game type)
	Text,           // a .txt the game reads by its name (earlyerr.txt, an expansion's version.txt, ...)
	Notes,          // notes the game never reads: a .md, any other .txt, a text with no extension
	// A file a tool converts into the game's files, whatever its name, while its import record is
	// beside it (a .png the image importer turns into a texture): the game never reads it, its
	// outputs, named after it, are read by their own kinds.
	ImportSource,
	// A file an import reads besides its source (a terrain set's images), whatever its name, while a
	// record lists it among its inputs: the game never reads it, its import's outputs are.
	ImportInput,
	kCount,         // the number of kinds, not a kind
};

inline constexpr size_t kFileKindCount = static_cast<size_t>(FileKind::kCount);

// Where a file of a kind lives in a game's set (ADR 0046 d8): one of the three boot-table archives
// the engine opens by fixed name, loose beside them, or nowhere. The boot gate counts only
// archives opened from that table [orig: PFF_OpenAllArchives @ 0x4a4310 over the name table @
// 0x829f90; fatal check @ 0x4a6f44]: an arbitrary-named .pff never mounts
// (docs/vfs/vfs-pff-mount-re.md D-VFS-2), so every packed file goes into language.pff,
// localres.pff or resource.pff (archive_slot_file_name, base/vfs kBootArchiveTable). The placement
// by kind mirrors retail's (witnessed against the shipped JO install, file_kind_test's retail leg:
// the boot text bins, the credits and the missions' sound banks in language; menus / defs / missions
// and their tiles / fonts / music scripts / animations / AI profiles / particles / effects in
// localres; models / terrain / env / most textures in resource; the few files retail splits from
// their kind's archive by what they are for, as the voice lines in language, recorded there), which
// keeps a set packed by it retail-bootable; the OpenNova runtime resolves a name from any slot. Two
// families never pack: `.sbf` music banks stream by path and never resolve through the archives
// [orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60], and `earlyerr.txt` is
// the pre-archive error text read before any mount [orig: Game_ShowEarlyError @ 0x4a68a0];
// retail's own loose files (videos, configs, saves, the machine-keyed NovaWorld cache) stay loose
// with them. None: a kind the game reads from nowhere: an archive (it holds files, it is no file
// of one), an import source and an import's input (their outputs, named after them, are read by
// their own kinds), the original mission editor's text and a project's notes, which the game never
// reads, and a file of no kind the game knows, which the game never asks for.
enum class ArchiveSlot { Language, Localres, Resource, Loose, None };

// Where the game reads a loose file of a kind when it runs an expansion (`/exp <name>`, ADR 0046
// S16): from the expansion's own folder, `expansion\<name>\`, which an expansion ships; or only from
// the install's folder, which an expansion cannot change (a build of an expansion leaves such a
// file out). None for a kind that is not loose (an archive's, or one the game reads from nowhere). A
// file the game reads through the file system's front door is no loose kind: the front door reads
// the archives alone unless `/d` [orig: FileSystem_OpenFile @ 0x75b1c0, the loose search only when
// searchLooseFirst @ 0x75b1e5; its one setter for the session, the /d gate Game_InitSubsystems @
// 0x4a6fa9..0x4a6fac], so such a kind packs (the NovaWorld screens).
enum class ExpansionLoose { None, Folder, RootOnly };

// How the game's reader of a kind cuts its text into lines where it ends a line at CR LF and nowhere
// else, an LF alone or a CR alone a byte of the line: the kinds whose text a line-end check holds
// to CR LF (the editor's line-ends rule). AsciiWalk: the shared ASCII walk, which ends a line only
// where a CR is followed by an LF [orig: File_ParseASCIIFile @ 0x53D810, the test @ 0x53D8DE],
// tokenizes the line's first 1000 characters and skips a line with no word or whose first word
// starts with '/' [orig: @ 0x53D908..0x53D91E; Terrain_TokenizeConfigLine @ 0x53CB60, the clamp @
// 0x53CBBB] (base/io/ascii_config.h). ConfigFile: the ConfigFile text reader, where CR LF ends a line
// and a lone CR or LF does not [orig: ConfigFile_LoadFromFile @ 0x760a10 -> ConfigFile_ParseText @
// 0x7608a0] (formats/configfile), whose pool of text values such a kind's files are held to
// (configfile::data_strings_pool). None: any other kind, among them two whose readers end lines
// otherwise: a stylesheet's reader, which stops at a line end other than CR LF, and a script's,
// which ends a line at a CR.
enum class LineReader { None, AsciiWalk, ConfigFile };

// What the game's loaders know of a kind. A shader's loader takes its file in the SCR form under a
// key of its own, as stored: that is a name's rule (base/vfs/vfs_decode.h, vfs_loader_takes_stored),
// not a column here.
struct FileKindFacts {
	FileKind kind = FileKind::Unknown;
	// A kind the runtime's catalog browses is named by its catalog token ("object_model"): the
	// runtime's classifier types such a file (resource_kind_for_name_and_magic), the one
	// implementation of that fact; "" for any other kind.
	const char *resource_kind = "";
	// What else names a file of the kind: its whole name, lower case ("items.def"), or the whole
	// names it has (null-ended: the texts the game reads, "earlyerr.txt"), which are looked for
	// before any extension; its extensions, lower case with the dot (null-ended).
	const char *file_name = nullptr;
	const char *const *file_names = nullptr;
	const char *const *extensions = nullptr;
	ArchiveSlot archive_slot = ArchiveSlot::Resource;
	// A loose kind's place under an expansion (ExpansionLoose): set exactly on the Loose rows.
	ExpansionLoose expansion_loose = ExpansionLoose::None;
	// How the game's reader of it ends a line, where at CR LF alone (LineReader): each such row cites
	// its reader.
	LineReader line_reader = LineReader::None;
};

// A kind's facts (file_kind.cpp holds one row per kind, in the enum's order; static_asserts there
// check that, and that no two rows share a catalog token, a whole name or an extension). The
// Unknown row for a value past the last kind.
const FileKindFacts &file_kind_facts(FileKind kind);

// The kind the runtime catalog's token names (a FileKindFacts::resource_kind); Unknown for none.
FileKind file_kind_for_resource_kind(const std::string &resource_kind);

// The kind of a file by its name and, for a `.bin`, its content: the runtime catalog's kind by its
// own classifier (resource_kind_for_file, a `.bin` by its magic when `bytes` is given), else the
// facts' own names: its whole name (without case), else its extension; Unknown for none. `bytes`
// may be null: a `.bin` is then a raw table, or the kind a row knows by its whole name (CC.BIN's).
// A file no name types may still be a material chunk by its content (renderer::
// is_material_chunk_container), which a caller holding the bytes asks.
FileKind file_kind_for_file(const std::string &logical_name, const std::vector<uint8_t> *bytes);

// file_kind_for_file by the name alone.
FileKind file_kind_for_name(const std::string &logical_name);

// The kind a required-resource row's file name implies without reading anything (base/gameprofile
// required_resources): the `.bin` rows are string tables except the music scripts and the raw
// markers, which are named. What a requirement compares a file found on disk against.
FileKind file_kind_for_required_name(const std::string &name);

// The boot table's archive of a slot: "language.pff", "localres.pff", "resource.pff" (base/vfs
// kBootArchiveTable, in its order); "" for Loose and None.
const char *archive_slot_file_name(ArchiveSlot slot);

} // namespace opennova
