#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/diagnostic.h>
#include <editor/model/value.h>

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
// the row reads its own way (a texture: a model's texture row's type); -1 for none.
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

	// A name some file defines, as a symbol or a style variable, or a record of a file's record set
	// (a symbol of the kind named by its index).
	bool names_symbol() const {
		return resolution == ReferenceResolution::Symbol ||
		       resolution == ReferenceResolution::StyleVariable ||
		       resolution == ReferenceResolution::Record;
	}
};

// The number of reference kinds: MissionAction is the last.
inline constexpr size_t kReferenceKindCount = static_cast<size_t>(ReferenceKind::MissionAction) + 1;

// The record a Record reference's value names, by its index in the kind's collection: a whole
// number from 0 that the kind's none does not take (a negative one names none: an index from 0 is
// what the collection holds its records by). False for a value naming no record and for a kind
// that is no Record reference.
bool record_index(ReferenceKind kind, const Value &value, int64_t &index);

// A kind's row (reference_kinds.cpp holds one per kind, in the enum's order; a static_assert
// there checks that and that the tokens are unique).
const ReferenceKindRow &reference_row(ReferenceKind kind);
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
