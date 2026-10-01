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
// archive (a build output, which the build refuses in a project) and an import source (its
// outputs, named after it, pack by their own kinds).
enum class ArchiveSlot { Language, Localres, Resource, Loose, None };

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
	kCount, // the number of values, None among them
};

inline constexpr size_t kDocumentTypeCount = static_cast<size_t>(DocumentTypeId::kCount) - 1;

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
	DocumentTypeId document = DocumentTypeId::None; // the type that edits it; None: packed as it is
	bool import_source = false; // an importer's source: never packed itself, its outputs are
	// Its files name other files, or names other files define, that an import brings with them
	// (import_plan's references_unread: those of a kind the graph does not read are not followed).
	bool names_files = false;
	// Where a file of the kind the editor makes goes inside the project tree, created or imported
	// ("menus", "fonts"; "" for the root): organization only, the engine sees the flat name.
	const char *folder = "";
	// The name Files offers a new file of the kind (New > Menu...: "newmenu.mnu"); "" for a kind
	// no New makes (its free-form blank factory's, blank_factory.cpp).
	const char *new_name = "";
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
// archive and an import source.
bool asset_kind_packed(AssetKind kind);
// Whether the archives' name limit binds a file of the kind: every kind but a loose one (a
// video, a music bank, a config), which the build copies beside the archives under any name. An
// import source is held to it (its importer names its outputs after it), and so is a file of no
// kind the game knows, which the build packs all the same (route_asset).
bool archive_name_limit_binds(AssetKind kind);

} // namespace opennova::editor
