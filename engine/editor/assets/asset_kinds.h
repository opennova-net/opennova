#pragma once

#include <string>

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// What each kind of project file is to the editor (ADR 0046 S13 D5): one row per AssetKind,
// which everything that asks what a kind is reads instead of switching on it (the classifier,
// the build's routing, the name rules, the document registry, the import plan). A new kind is
// one AssetKind value and one row in asset_kinds.cpp, which does not build without it.

// Where a build puts a file of a kind (ADR 0046 d8): one of the three boot-table archives the
// engine opens by fixed name, loose beside them, or nowhere. The boot gate counts only
// archives opened from that table [orig: PFF_OpenAllArchives @ 0x4a4310 over the name table @
// 0x829f90; fatal check @ 0x4a6f44]: an arbitrary-named .pff never mounts
// (docs/vfs/vfs-pff-mount-re.md D-VFS-2), so every packed file goes into language.pff,
// localres.pff or resource.pff. The placement by kind mirrors retail's (witnessed against the
// shipped JO install: the boot text bins in language, menus / defs / missions / fonts / music
// scripts in localres, terrain / env / art in resource), which keeps the output
// retail-bootable; the OpenNova runtime resolves a name from any slot. Two families never
// pack: `.sbf` music banks stream by path and never resolve through the archives [orig:
// AudioVM_InitMenuMusicStreaming @ 0x56aa60], and `earlyerr.txt` is the pre-archive error text
// read before any mount [orig: Game_ShowEarlyError @ 0x4a68a0]; retail's own loose files
// (videos, configs, saves, the machine-keyed NovaWorld cache) stay loose with them. None: an
// archive (a build output, which the build refuses in a project), an import source (its
// outputs, named after it, pack by their own kinds) and a file of no kind the game knows, which
// the game never asks for (S13 A8: the build leaves it out).
enum class ArchiveSlot { Language, Localres, Resource, Loose, None };

// Where the game reads a loose file of a kind when it runs an expansion (`/exp <name>`, ADR 0046
// S16): from the expansion's own folder, `expansion\<name>\`, which an expansion build ships; or only
// from the install's folder, which an expansion cannot change (the build leaves such a file out and
// says so, build.expansion.root_only). None for a kind that is not loose (an archive's, or one no
// build packs). A file the game reads through the file system's front door is no loose kind: the
// front door reads the archives alone unless `/d` [orig: FileSystem_OpenFile @ 0x75b1c0, the loose
// search only when searchLooseFirst @ 0x75b1e5; its one setter for the session, the /d gate
// Game_InitSubsystems @ 0x4a6fa9..0x4a6fac], so such a kind packs (the NovaWorld screens).
enum class ExpansionLoose { None, Folder, RootOnly };

// The document types the editor opens a kind with (ADR 0046 d9): documents/document_types holds
// one DocumentType per value past None, in this order. None: a kind the build packs as it is.
enum class DocumentTypeId {
	None,
	Catalog,
	Strings,
	Menu,
	Styles,
	Model,
	Animation,
	AnimationMap,
	Mission, // a .bms: the mission file's records (ADR 0046 S14)
	// The text documents (ADR 0046 S13 D9): one TextDocument class, a type per behaviour.
	Script,      // a .wac: the WAC compiler's findings, its operands' names as references
	MusicScript, // a music script's SCR0 bytecode, held as its MUS text
	Credits,     // a .kda: a CBIN form held as its ConfigFile text
	Shader,      // a .fx: the SCR form the shader loader takes, held as its text
	Text,        // any other text, as the file stores it: a configuration, a text, and each text kind no
	             // structured type edits yet, its engine reader's findings its own (DI-06)
	Texture,     // a .tga .mdt .pcx .dds .png: its texels as the game reads them (ADR 0046 S18)
	kCount, // the number of values, None among them
};

inline constexpr size_t kDocumentTypeCount = static_cast<size_t>(DocumentTypeId::kCount) - 1;

// How the game's loader of a kind takes a file in the SCR form (formats/scr): as the game's text
// readers do, the form optional and unwrapped under the game's key [orig: File_ParseASCIIFile @
// 0x53D860, its sniff for "SCR" and version 1], which a document's load undoes before its type reads
// the file; or as the shader loader does, the form required and unwrapped under a key of its own
// [orig: ScriptFile_LoadAndDecrypt @ 0x5AE060, the key at 0x5AE0C0], which the type reads itself
// from the bytes as stored (a file not in the form is one the loader rejects).
enum class ScrForm { Optional, Shader };

struct AssetKindRow {
	AssetKind kind = AssetKind::Unknown;
	const char *token = ""; // the wire form (session JSON, the editor MCP, opennova-project)
	const char *label = ""; // the windows' words ("Item definitions")
	// A kind the runtime's catalog browses is named by its catalog token ("object_model"): the
	// runtime's classifier types such a file (resource_kind_for_name_and_magic), the one
	// implementation of that fact; "" for any other kind.
	const char *runtime = "";
	// What else names a file of the kind: its whole name, lower case ("items.def"), which is
	// looked for before any extension; its extensions, lower case with the dot (null-ended).
	const char *file_name = nullptr;
	const char *const *extensions = nullptr;
	ArchiveSlot archive_slot = ArchiveSlot::Resource;
	// A loose kind's place in an expansion build (ExpansionLoose): set exactly on the Loose rows.
	ExpansionLoose expansion_loose = ExpansionLoose::None;
	DocumentTypeId document = DocumentTypeId::None; // the type that edits it; None: packed as it is
	// Its files name other files, or names other files define, that an import brings with them
	// (import_plan's references_unread: those of a kind the graph does not read are not followed).
	bool names_files = false;
	// Where a file of the kind the editor makes goes inside the project tree, created or imported
	// ("menus", "fonts"; "" for the root): organization only, the engine sees the flat name (a loose
	// kind's build copy takes the name alone too). The kinds the game reads from its own folder by a
	// fixed name (a configuration, the score table, a text) stay at the root, as the install keeps them.
	const char *folder = "";
	// The name Files offers a new file of the kind (New > Menu...: "newmenu.mnu"); "" for a kind
	// no New makes (its free-form blank factory's, blank_factory.cpp).
	const char *new_name = "";
	// How its loader takes the SCR form (ScrForm).
	ScrForm scr = ScrForm::Optional;
	// What a file of the kind is to the game, in a modder's words, a sentence (Files' card for a file,
	// the UX round's project lane): what reads it and how it is found, as the row's own witnesses say.
	const char *about = "";
};

// A kind's row (asset_kinds.cpp holds one per kind, in the enum's order; static_asserts there
// check that, and that no two rows share a token, a runtime token, a file name or an extension).
// The Unknown row for a value past the last kind.
const AssetKindRow &asset_kind_row(AssetKind kind);
// The kind the runtime catalog's token names; Unknown for none.
AssetKind asset_kind_for_runtime(const std::string &runtime_kind);
// The kind a file name gives by the rows' own names: its whole name (without case), else its
// extension; Unknown for none. The runtime's classifier is asked first (classify_asset).
AssetKind asset_kind_for_name(const std::string &logical_name);
// Whether a build puts a file of the kind in the build (in an archive, or loose): false for an
// archive, an import source and a file of no kind the game knows.
bool asset_kind_packed(AssetKind kind);
// The kind a text names as a modder writes it in a filter (the UX round's project lane: Files, the files
// query): a kind's label or its token, in any case, singular or plural ("texture", "Textures",
// "sound bank", "sound_bank", "waves"), or either after "kind:"; kCount for none. `only`, when given, says
// whether the text was "kind:..." (the files of the kind alone, no name matched by the text).
AssetKind asset_kind_named_by(const std::string &text, bool *only = nullptr);
// Whether the archives' name limit binds a file of the kind: a kind the build packs into an
// archive, and an import source (its importer names its outputs after it). Not a loose one (a
// video, a music bank, a config), which the build copies beside the archives under any name, nor
// one the build leaves out (an archive, a file of no kind the game knows), nor a NovaWorld screen,
// which the game reads by the few names its menus give (a longer one the build leaves out, said).
bool archive_name_limit_binds(AssetKind kind);

} // namespace opennova::editor
