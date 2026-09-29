#pragma once

#include <cstddef>
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
// ReferenceKind value and one row here.

// Where a name of the kind resolves.
enum class ReferenceResolution {
	File,          // a project file, by the names the kind's loader reads (reference_file_candidates)
	Symbol,        // a name a project file defines (GraphSymbol), in the reference's scope
	StyleVariable, // a %NAME% the stylesheets the game reads define (AssetGraph::style_binding)
	Unchecked,     // the editor cannot check it yet (ReferenceStatus::Unverified)
};

// How the game's lookup compares two names of the kind.
enum class NameCase {
	FileName, // as the scan keys a file name (normalized_logical_name)
	NoCase,   // without case (stricmp)
	Exact,    // as written (an item id's number)
};

// How a site writes a name of the kind (the graph keys a name by its NameCase alone; a query
// that takes a site's spelling reads it back first: Document::find).
enum class NameSpelling {
	Name,          // the name itself
	StyleVariable, // the whole value one %NAME% (mns::is_variable_reference)
};

// The file names a reference loads, in the order the game probes them, for a kind whose loader
// picks by more than its extensions (reference_file_candidates says how each does).
using ReferenceFileNames = std::vector<std::string> (*)(const std::string &name, int material_type,
                                                        const std::function<bool(const std::string &)> &exists);
// What a finding about a reference nothing resolves says after "<who> names <phrase> '<value>'":
// why, and what the game does instead.
using ReferenceMissingMessage = std::string (*)(const AssetGraph &graph, const GraphEdge &edge);

struct ReferenceKindRow {
	ReferenceKind kind = ReferenceKind::None;
	const char *token = "";  // the wire form (session JSON, the editor MCP, opennova-project)
	const char *phrase = ""; // a finding's words: "the font"
	const char *label = "";  // a list's and a picker's words: "font"
	ReferenceResolution resolution = ReferenceResolution::Unchecked;
	AssetKind file = AssetKind::Unknown;     // the kind of file a File reference loads
	const char *const *extensions = nullptr; // what its loader appends to the name as written (null-ended)
	ReferenceFileNames file_names = nullptr; // its loader's own rule, in place of the extensions
	NameCase name_case = NameCase::FileName;
	NameSpelling spell = NameSpelling::Name;
	// A Warning where the game tolerates the name missing, an Error where it does not.
	DiagnosticSeverity severity_when_missing = DiagnosticSeverity::Error;
	ReferenceMissingMessage missing_message = nullptr; // null for a kind the graph never finds missing
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

	// A name some file defines, as a symbol or a style variable.
	bool names_symbol() const {
		return resolution == ReferenceResolution::Symbol || resolution == ReferenceResolution::StyleVariable;
	}
};

// The number of reference kinds: UserPoint is the last.
inline constexpr size_t kReferenceKindCount = static_cast<size_t>(ReferenceKind::UserPoint) + 1;

// A kind's row (reference_kinds.cpp holds one per kind, in the enum's order; a static_assert
// there checks that and that the tokens are unique).
const ReferenceKindRow &reference_row(ReferenceKind kind);
// The kind a token names; false for none.
bool reference_kind_from_token(const std::string &token, ReferenceKind &out);
// The reference a stylesheet value naming a file of `file`'s kind makes where the game reads it
// (the kinds a style variable may stand for: also_offers); None for any other file.
ReferenceKind style_value_reference(AssetKind file);

} // namespace opennova::editor
