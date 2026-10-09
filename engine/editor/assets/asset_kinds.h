#pragma once

#include <string>

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// What each kind of project file is to the editor (ADR 0046 S13 D5): one row per AssetKind,
// which everything that asks what a kind is reads instead of switching on it (the classifier,
// the build's routing, the name rules, the document registry, the import plan). What the game's
// loaders know of a kind (the names that give it, its archive slot, its place under an expansion,
// its line reader) is the engine's row of facts (base/resource_index/file_kind.h, FileKindFacts),
// which a row reads (AssetKindRow::facts). A new kind is one FileKind value, one row of facts and
// one row in asset_kinds.cpp, which does not build without it.

// Where a build puts a file of a kind (ADR 0046 d8) is its facts' slot (FileKindFacts::archive_slot,
// ArchiveSlot): the boot table's archives, loose, or nowhere; an expansion build's loose places are
// its facts' too (FileKindFacts::expansion_loose, ExpansionLoose). A build leaves out a kind of slot
// None: an archive (a build output, which the build refuses in a project), an import source (its
// outputs, named after it, pack by their own kinds), the project's notes, which the game never
// reads, and a file of no kind the game knows, which the game never asks for (S13 A8: the build
// leaves it out, and says so).
using opennova::ArchiveSlot;
using opennova::ExpansionLoose;
using opennova::LineReader;

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
	SoundBank,     // a .lwf: its waves and its sets, their layers and members (the sound lane)
	SoundProfiles, // SndProf.def: its profiles and each one's 51 slots (the sound lane)
	Particles,   // a .ptl .ptu .ptg: the effect system's text, its reader's findings (ADR 0046 DI-14)
	Environment, // a .env: env::Config's keywords and keyframes (the deep-integration plan's DI-19a)
	HudLayout,   // hudpos.def, held as its text and drawn by the HUD viewport (the plan's DI-20)
	Terrain,     // a .trn: TrnConfig's keys, grid rows and foliage (the deep-integration plan's DI-30)
	DialogBank,  // a .dbf: a mission's dialogs and their lines (the deep-integration plan's DI-32)
	CharAttrs,   // charattr.def, held as its text: its classes' camouflage items as references (DI-09's follow-up)
	FaceAnimation, // a .grm: a person's face, its textures, mesh, gestures and eyes (round S23 lane A)
	kCount, // the number of values, None among them
};

inline constexpr size_t kDocumentTypeCount = static_cast<size_t>(DocumentTypeId::kCount) - 1;

struct AssetKindRow {
	AssetKind kind = AssetKind::Unknown;
	const char *token = ""; // the wire form (session JSON, the editor MCP, opennova-project)
	const char *label = ""; // the windows' words ("Item definitions")
	DocumentTypeId document = DocumentTypeId::None; // the type that edits it; None: packed as it is
	// Its files name other files, or names other files define, that an import brings with them
	// (import_plan's references_unread: those of a kind the graph does not read are not followed).
	bool names_files = false;
	// The kind's folder in a project laid out by kind ("menus", "fonts"; "" for the root), where a
	// file of the kind the editor makes, created or imported, goes when the project has none of the
	// kind yet (assets/project_layout.h: a flat project keeps it at the top level): organization
	// only, the engine sees the flat name (a loose kind's build copy takes the name alone too). Read
	// only by the placement rule: whatever writes a new file into the project asks placement_path. The kinds the game reads from its own folder by a
	// fixed name (a configuration, the score table, a text) stay at the root, as the install keeps them.
	const char *folder = "";
	// The name Files offers a new file of the kind (New > Menu...: "newmenu.mnu"); "" for a kind
	// no New makes (its free-form blank factory's, blank_factory.cpp).
	const char *new_name = "";
	// What a file of the kind is to the game, in a modder's words, a sentence (Files' card for a file,
	// the UX round's project lane): what reads it and how it is found, as the row's own witnesses say.
	const char *about = "";
	// What the game's loaders know of the kind: its names, archive slot, place under an expansion and
	// line reader (base/resource_index/file_kind.h).
	const FileKindFacts &facts() const { return file_kind_facts(kind); }
};

// A kind's row (asset_kinds.cpp holds one per kind, in the enum's order; static_asserts there
// check that, and that no two rows share a token). The Unknown row for a value past the last kind.
// The kind a name gives is the engine's (file_kind_for_name, file_kind_for_file; classify_asset).
const AssetKindRow &asset_kind_row(AssetKind kind);
// Whether a build puts a file of the kind in the build (in an archive, or loose): false for an
// archive, an import source and its inputs, a mission's interchange text, the project's notes and a
// file of no kind the game knows.
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
