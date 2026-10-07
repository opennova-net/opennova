#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/diagnostic.h>
#include <editor/model/value.h>

namespace opennova::renderer {
enum class TextureLoader : uint8_t;
}

namespace opennova::editor {

class AssetGraph;
struct GraphEdge;

// What each reference kind is to the asset graph, the resolver, the Problems rows and the
// windows (ADR 0046 S12): one row per ReferenceKind, which everything that asks about a kind
// reads instead of switching on it. A new namespace (a mission's, a sound's) is one
// ReferenceKind value and one row here; so is an index into a collection of the same file (S13
// D8: a mission's entity, waypoint, group, layer or area index, a sound bank's chain tables, a
// dialog bank's def id index), a Record row naming the collection by its record kind's token (and
// the values naming none), which the document core renumbers on an edit that moves the
// collection's records (Document::renumber_references).

// Where a name of the kind resolves.
enum class ReferenceResolution {
	File,          // a project file, by the names the kind's loader reads (reference_file_candidates)
	Symbol,        // a name a project file defines (GraphSymbol), in the reference's scope
	StyleVariable, // a %NAME% the stylesheets the game reads define (AssetGraph::style_binding)
	// A record of the reference's own file (S13 D8), by its index among the file's records of the
	// kind the row's `collection` names (a model's CTRL registers, its MTRX rows): the file's
	// record set, whose records the graph keys by their index in that file (the reference's scope).
	Record,
	Unchecked,     // the editor cannot check it yet (ReferenceStatus::Unverified)
};

// How the game's lookup compares two names of the kind.
enum class NameCase {
	FileName, // as the scan keys a file name (normalized_logical_name)
	NoCase,   // without case (stricmp)
	Exact,    // as written (an item id's number)
	// An animation slot's key: past its first five characters (the `anim_` every retail key and action
	// writes, whatever they are), without case [orig: AnimMap_FindSlotByName @ 0x40cfa0, the stricmp on
	// the name + 5 @ 0x40cfc3; formats/adm adm_slot_key].
	SlotKey,
};

// How a site writes a name of the kind (the graph keys a name by its NameCase alone; a query
// that takes a site's spelling reads it back first: find_definition).
enum class NameSpelling {
	Name,          // the name itself
	StyleVariable, // the whole value one %NAME% (mns::is_variable_reference)
};

// The file names a reference loads, in the order the game probes them, for a kind whose loader
// picks by more than its extensions (reference_file_candidates says how each does). `loader_arg`
// is what the reference's field gives its loader besides the name (FieldUse::loader_arg), which
// the row reads its own way (a texture: a model's texture row's type, or the game's loader of a
// texture no model row names, texture_loader_arg); -1 for none.
using ReferenceFileNames = std::vector<std::string> (*)(const std::string &name, int32_t loader_arg,
                                                        const std::function<bool(const std::string &)> &exists);
// What a finding about a reference nothing resolves says after "<who> names <phrase> '<value>'":
// why, and what the game does instead.
using ReferenceMissingMessage = std::string (*)(const AssetGraph &graph, const GraphEdge &edge);
// The values of a Record reference that name no record, whatever the collection holds (a frame
// byte the pose reads none from).
using RecordNone = bool (*)(int64_t value);
// What a Record reference's index counts (S13 D8). File: every record of its collection's kind in
// the file it is written in, in the file's order (the rows in order, each row's records in the
// walk's pre-order), which must be the format's own table order; the reference resolves in its own
// file (Document::field_on scopes it so) and the document core renumbers it. The one space the core
// numbers: an index into one owner's list alone (a per-owner table) would be a value of its own,
// which the core and the graph learn to number before a row names it (records_well_formed holds
// every Record row to File until then).
enum class RecordIndexSpace {
	None, // no Record reference
	File,
};

struct ReferenceKindRow {
	ReferenceKind kind = ReferenceKind::None;
	const char *token = "";  // the wire form (session JSON, the editor MCP, opennova-project)
	const char *phrase = ""; // a finding's words: "the font"
	const char *label = "";  // a list's and a picker's words: "font"
	ReferenceResolution resolution = ReferenceResolution::Unchecked;
	AssetKind file = AssetKind::Unknown;     // the kind of file a File reference loads
	// A Record reference's collection: the token of the record kind its records are
	// (RecordKindRow::token: "register"), every record of that kind in the file, in the file's
	// order, numbered from 0. A document type whose schema names the kind on a field
	// (FieldSchema::reference) and that holds records of that token makes such references
	// (Document::targeted_collections). "" for any other resolution.
	const char *collection = "";
	// A Record reference's values that name no record (null: every whole number from 0 names one,
	// the index past the collection's end naming one it lacks).
	RecordNone none = nullptr;
	// What a Record reference's index counts (None for any other resolution).
	RecordIndexSpace index_space = RecordIndexSpace::None;
	const char *const *extensions = nullptr; // what its loader appends to the name as written (null-ended)
	ReferenceFileNames file_names = nullptr; // its loader's own rule, in place of the extensions
	NameCase name_case = NameCase::FileName;
	NameSpelling spell = NameSpelling::Name;
	// A Warning where the game tolerates the name missing, an Error where it does not.
	DiagnosticSeverity severity_when_missing = DiagnosticSeverity::Error;
	// Null for a kind the graph never finds missing: one it cannot check, and a Record reference,
	// whose index past its collection its file's own validation reports with what the game makes
	// of it (a model's register_missing, frame_missing), the graph answering Missing for its badge
	// alone.
	ReferenceMissingMessage missing_message = nullptr;
	// The message reads which files the project has (AssetGraph::has_file: a string id's table,
	// the failsafe clip), so the graph words the kind's findings again when the file set changes.
	bool message_reads_files = false;
	// What the picker offers besides the kind's own names: the kind a value may name instead (a
	// menu's font or texture a stylesheet variable, ADR 0005; an unchecked text a string id).
	ReferenceKind also_offers = ReferenceKind::None;
	// A symbol whose scope names the file defining it (the part before its first '/'): a string
	// id's table, the menu a screen or window is looked up in, the model a user point is on.
	bool scope_names_file = false;
	// The picker offers only the symbols the reference's scope matches (scope_matches).
	bool picker_scoped = false;
	// The kind of file a symbol goes in when no file of the project defines one yet (the
	// Problems row's Open fix); Unknown for one the file its scope names holds.
	AssetKind defined_in = AssetKind::Unknown;
	// The witness that the game refuses what names a name of the kind that finds nothing, null where
	// none is witnessed (ADR 0046 S14, the start of severities from witnessed behaviour): a missing
	// reference of a kind with one refuses a build (blocks_build); every other missing reference is
	// listed and gates nothing.
	const char *gates_when_missing = nullptr;
	// The same for a kind whose references refuse only by what each gives its loader
	// (GraphEdge::loader_arg: a texture's role, ADR 0046 S18, the terrain's colour map), the witness for
	// that argument, null where that one is tolerated; null for a kind with no such rule.
	const char *(*gates_when_missing_for)(int32_t loader_arg) = nullptr;

	// A name some file defines, as a symbol or a style variable, or a record of a file's record set
	// (a symbol of the kind named by its index).
	bool names_symbol() const {
		return resolution == ReferenceResolution::Symbol ||
		       resolution == ReferenceResolution::StyleVariable ||
		       resolution == ReferenceResolution::Record;
	}
};

// A texture reference's loader argument when no model row names the texture: the game's loader
// that reads it (renderer::TextureLoader, runtime/renderer/texture_load_rules.h: the sky maps'
// archive loader, a particle's graphic's, the HUD's in its two modes), each a value below -1 so
// that a model texture row's type (>= 0) and none (-1) keep theirs. texture_loader_of reads it
// back; false for a model row's type and for none.
int32_t texture_loader_arg(renderer::TextureLoader loader);
bool texture_loader_of(int32_t loader_arg, renderer::TextureLoader &loader);
// A mission's tile set (its header's terrain_tile): the atlas the terrain draws its tiles from,
// the name with everything from its first '.' replaced by ".TGA" (formats/trn
// trn_mission_tilestrip) through the TGA reader [orig: Terrain_LoadEnvironmentConfig @ 0x610940
// -> Path_ReplaceOrAppendExtension @ 0x53C780]. Below every texture_loader_arg.
inline constexpr int32_t kTileSetTextureArg = -100;

// A model's user point among its first 16 is defined in this section of its model's scope ("GUN.3DI/FIRST16"),
// which an item's particle slot's lookup reads alone [orig: ItemDef_GetBoneMaskByName @ 0x49ea40, the scan end @
// 0x49ea73]; the game's every other lookup of a point by name scans them all [orig: modelgpm_FindUserpointByName
// @ 0x5b2170], a reference naming the model alone, which reaches every section (scope_matches).
inline constexpr const char *kFirstUserPointsSection = "FIRST16";
// The scope a user point of `model` (a model as a field names it, its extension optional: ".3di" then) is
// looked up in: its file name upper case, and the first-16 section where `first_16`. "" for no model.
std::string user_point_scope(const std::string &model, bool first_16);
// The scope an animation map's row is looked up in: `map` (as a field names it, its extension optional:
// ".adm" then) upper case. "" for no map.
std::string animation_map_scope(const std::string &map);

// The number of reference kinds: AvatarPart is the last.
inline constexpr size_t kReferenceKindCount = static_cast<size_t>(ReferenceKind::AvatarPart) + 1;

// The record a Record reference's value names, by its index in the kind's collection: a whole
// number from 0 that the kind's none does not take (a negative one names none: an index from 0 is
// what the collection holds its records by). False for a value naming no record and for a kind
// that is no Record reference.
bool record_index(ReferenceKind kind, const Value &value, int64_t &index);

// A kind's row (reference_kinds.cpp holds one per kind, in the enum's order; a static_assert
// there checks that and that the tokens are unique).
const ReferenceKindRow &reference_row(ReferenceKind kind);

// Whether a finding among the rows a build reads refuses it (ADR 0046 S14, the build follows
// retail): an error whose code gates (FindingCodeRow::gates_build), an error made from no row, or an
// error of a listed code whose subject names the game's refusal: a reference (missing, or naming a
// file of another kind) of a kind the game refuses when the name loads nothing
// (ReferenceKindRow::gates_when_missing), a required file the project lacks whose manifest row is
// the game's refusal to boot (RES_FATAL). Any other listed error blocks nothing.
bool blocks_build(const Diagnostic &d);
bool diagnostics_block_build(const std::vector<Diagnostic> &items);

// The names an expansion's base game serves (ADR 0046 S16): the install mounted as a stock launch of
// the project's game, no expansion (base_install_spec: its archives' files and the loose files it
// ships beside them), each as its listing spells it, sorted by their normalized form. Under /exp the
// game reads what the expansion lacks from the base's archives below the expansion's pair [orig:
// PFF_OpenAllArchives @ 0x4a4310, slots 2..4].
struct BaseNames {
	const std::vector<std::string> *sorted = nullptr;
	// Whether the base serves `name` (compared as the game compares names).
	bool has(const std::string &name) const;
};
// blocks_build over an expansion's base: a required file, or a file a gating reference names, that the
// project lacks and the base serves blocks nothing, the game reading the base's (requirement.missing,
// reference.missing); a file of the name of another kind still blocks (requirement.wrong_kind,
// reference.wrong_kind: the project's file is the one the game reads). Null `base`: blocks_build.
bool blocks_build(const Diagnostic &d, const BaseNames *base);
bool diagnostics_block_build(const std::vector<Diagnostic> &items, const BaseNames *base);

// The files a build packs as the game ships them (ADR 0046 S16): the project's files that are the
// game's own data byte for byte (session/original_bytes.h), less those whose open documents hold
// unsaved edits. The build packs and copies every file's bytes as stored and never runs the editor's
// writer over them (project_build/build_run.h), so a finding that a file does not serialize (a
// blocks_save row's: the writer cannot write back what the editor holds, or holds a value the reader
// refuses) is about a file the build must write: one changed, or one whose edits the Save a build asks
// first must write. Over the game's own bytes, which the game ships and loads, it refuses nothing.
struct ShippedFiles {
	std::set<std::string> original; // project-relative paths (session/original_bytes.h)
	std::set<std::string> unsaved;                    // the open documents with unsaved edits
	bool has(const std::string &asset) const;
};
// blocks_build over the base (above) and the shipped files: a blocks_save row's error about a file
// `shipped` has blocks nothing. Null `shipped`: every such error gates.
bool blocks_build(const Diagnostic &d, const BaseNames *base, const ShippedFiles *shipped);
bool diagnostics_block_build(const std::vector<Diagnostic> &items, const BaseNames *base, const ShippedFiles *shipped);
// The kind a token names; false for none.
bool reference_kind_from_token(const std::string &token, ReferenceKind &out);
// The reference a stylesheet value naming a file of `file`'s kind makes where the game reads it
// (the kinds a style variable may stand for: also_offers); None for any other file.
ReferenceKind style_value_reference(AssetKind file);

// What a style variable is used as where a StyleVar edge names it (GraphEdge::through, what its
// value must be there): a colour (None: an APPEARANCE's, an ITEM's or a FONT's colour), a font's
// file (Font), an image's file (MenuTexture), or none of the three (a string id, a screen's or a
// window's NAME, any other text of a menu), which no stylesheet check reads the value as.
enum class StyleVariableUse { Colour, Font, Image, Other };
StyleVariableUse style_variable_use(ReferenceKind through);

} // namespace opennova::editor
