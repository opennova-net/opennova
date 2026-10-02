#pragma once

#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_edge.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/model/field_use.h>
#include <editor/model/value.h>

namespace opennova::editor {

// A document's reference queries (ADR 0046 d10, S7; S13 D3: free functions over the graph and the
// scan, which is all they read): the inspector's badge, the picker's names, the fixes of a missing
// value and where "Go to" goes read the same tables the Problems rows come from, each of a field as
// it applies to its record (Document::field_on); and a record found by the symbol it defines, read
// from the same extraction the graph keeps.

// Where "Go to" goes (reference_targets, usage_target): a project file, and the record there that
// defines the name or makes the use (its locator and the field to show), or the file itself.
struct ReferenceTarget {
	std::string label;     // what a choice among several says ("style variable X in menu_style.mns")
	std::string file;      // project-relative
	std::string locator;   // the record there (Document::locator); "" = the file itself
	std::string field;     // the field to show there
	bool editable = false; // the editor opens the file's kind; else Files shows the file
};

// A record of `document` found by the symbol another document names it with (a name, an id; a
// style variable's NAME or its %NAME%): of the symbols the records define (their defining fields,
// as the kind compares names) that `scope` matches (scope_matches: "GAMETEXT.BIN/WepDes" a key of
// that section, "MAIN.MNU/STARTUP" a window of that screen; "" any), the first a lookup finds, a
// row's before a nested record's (a menu's screen before a window of the name). With a scope
// nothing else: a definition no lookup finds there (a shadowed section's key) is not found. With
// none, else the first defined where no lookup looks, else the first record of that name
// (record_name, without case: an animation table's slot, a clip's bone). The definitions are the
// ones the graph read from the document when its slot is current for it (the same instance at the
// same revision), each as the document's own lookup makes it (AssetGraph::for_each_definition),
// else the document's own extraction; never a record of its record sets, which goes by its index
// in its own file and no other file's name.
bool find_definition(const AssetGraph &graph, const Document &document, const std::string &symbol,
		NodeAddress &out, const std::string &scope = std::string());
// Present / Missing / Unverified for a field's value (NotAReference when it names nothing);
// `symbol`, when given, receives the name it looks up.
ReferenceStatus reference_status(const AssetGraph &graph, const FieldUse &field, const Value &value,
                                 std::string *symbol = nullptr);
// What the picker offers a field: the names of its kind in its scope (AssetGraph::choices),
// then what the value may name instead (a menu's font or texture a stylesheet variable, ADR
// 0005; an unchecked text a string id), each with what the field would reference, set to it. A
// Record reference's (S13 D8): the records of its collection in its own file, each by its index,
// those the field can hold and that name a record.
std::vector<ReferenceChoice> reference_choices(const AssetGraph &graph, const FieldUse &field);
// The finding the graph makes of a record's field whose value resolves to nothing
// (AssetGraph::missing_finding), for the fixes Problems offers for it: of the variable itself
// where a %NAME% the stylesheets the game reads do not define stands for a file (the graph
// reports the variable, never a file of that name), else of the reference, a file named
// through a variable by the name its value gives. False when the value resolves or names
// nothing, and for a kind the graph finds none of missing (a Record reference: its file's own
// validation reports an index past its collection).
bool missing_finding(const AssetGraph &graph, const Document &document, const NodeAddress &address,
                     const FieldUse &field, const Value &value, Diagnostic &out);
// The file a field's value loads, where it resolves to one (a symbol's defining file for a symbol
// kind); "" for none.
std::string reference_target_file(
		const AssetGraph &graph, const FieldUse &field, const Value &value);
// Where "Go to" on a reference goes, as the game's lookup reaches it: a symbol's defining
// record (AssetGraph::resolve_symbol: a string id in its own section, a window on its own
// screen, a record of this same file included); a file (the file its kind's loader
// reads), after the style variable that names it where a %NAME% stands (the definition the
// game reads, then the file its value names). None when nothing resolves.
std::vector<ReferenceTarget> reference_targets(
		const AssetGraph &graph, const AssetScan &scan, const FieldUse &field, const Value &value);
// Where "Go to" on a use goes: the record of the edge's file that makes it (its locator, the
// field shown), opened when the editor edits the file's kind, else the file shown in Files.
ReferenceTarget usage_target(const AssetScan &scan, const GraphEdge &edge);
// Where "Go to" on a definition goes: the record that defines a symbol (at its defining field),
// and a file itself; opened, or shown in Files, as usage_target.
ReferenceTarget symbol_target(const AssetScan &scan, const GraphSymbol &symbol);
ReferenceTarget file_target(const AssetScan &scan, const std::string &file);

} // namespace opennova::editor
