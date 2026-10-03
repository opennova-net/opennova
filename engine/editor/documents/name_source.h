#pragma once

#include <cstdint>
#include <string>

#include <editor/graph/graph_edge.h>
#include <editor/model/value.h>

namespace opennova::editor {

// The display-name service (ADR 0046 S15, Names): what a value a document holds reads as to a modder
// where it stands for something with a name (an item id, an SSN, a zone id, a path's number, a group,
// an event's index, a text key), and the value as the file holds it beside the words. A document
// type words its own records and values (DocumentType::record_label, value_label: the mission's,
// documents/mission_labels.h); the generic words and the dispatch are graph/display_names.h's.
struct DisplayName {
	// The words: an item by its catalog's name ("Ammo crate"), an entity by its item and its SSN
	// ("Ranger #12"), an area by its zone ("Zone 3"), a path with its stops ("Path 5 (4 stops)"), an
	// event by its first trigger, a text key by its string. A value naming nothing says so in words
	// ("No item 123456 in the project"). "" where the editor has no words of its own for the value (the
	// value is shown as it is).
	std::string text;
	// The value as the file holds it ("123456"), shown muted beside the words or in their tooltip; ""
	// where the words are the value.
	std::string raw;
	// Where the words come from, for a tooltip ("items.def", "WALK.BIN/PeopleNames"); "" for none.
	std::string source;
	// The value names nothing the document or the project has, which the words say.
	bool dangling = false;
	bool empty() const { return text.empty(); }
};

// What the display names read beyond the one document (ADR 0046 S15): the names the project's files
// define, as the game's lookup reaches them. The session's is the asset graph's (graph/display_names.h,
// GraphNameSource); with none (no project open, a test of one document) a record and a value read in
// the document's own words.
class NameSource {
public:
	virtual ~NameSource() = default;
	// The definition a name of `kind` reaches in `scope` (AssetGraph::resolve_symbol); null for none.
	virtual const GraphSymbol *symbol(ReferenceKind kind, const std::string &name,
	                                  const std::string &scope = std::string()) const = 0;
	// The definition an edge reaches, its scopes tried in their order (AssetGraph::symbol_reached: a
	// mission's text key in the mission's own table, else medmssn.bin); null for none.
	virtual const GraphSymbol *reached(const GraphEdge &edge) const = 0;
	// Moves whenever what the source answers may (the graph's generation): what a cache of words keys on.
	virtual uint64_t generation() const = 0;
};

// A heading a type's outline groups a row under (ui/outline_model's OutlineSpec::groups: a mission's
// "Organics", "Team 1", "Group 3"): its key among its siblings, which orders them, and its words.
// A value a picker offers that names no definition because the game resolves it itself (DocumentType::
// game_choices: a mission's SSN 10000, the player): the value as the file writes it and its words.
struct GameChoice {
	std::string name;
	std::string label;
};

struct RowHeading {
	std::string key;
	std::string text;
};

// The words a definition reads as (a value naming it, a picker's row): an item by its catalog's name
// (the record defining its id: an item's display name), a string id by its text where its table's
// symbol carries it (StringsDocument::refine_symbol), a record of a record set by its own name where it
// has one; any other by its name as defined.
std::string symbol_words(const GraphSymbol &symbol);

} // namespace opennova::editor
