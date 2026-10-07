#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_edge.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document.h>

namespace opennova::editor {

// The jumps a keystroke or a right-click makes (ADR 0046 DI-18, "Shortcuts and context jumps"), over the
// graph and the scan alone: what Go to file (Ctrl+P) and Go to name (Ctrl+T) list as a name is typed, what
// Find usages (Shift+F12) lists of a file or of a record, and where Go to definition (F12) leads from a
// record. Each is what the project's files say: no kind of file or record is named here.

// What a search lists (the project_search query's `scope`, the workspace's project_find): every file and name
// (Find in project), the files alone (Go to file) or the names the files define alone (Go to name).
enum class SearchScope : uint8_t { All, Files, Names };
// all, files, names.
const char *search_scope_token(SearchScope scope);
bool search_scope_from_token(const std::string &token, SearchScope &out);

// AssetGraph::search over one scope. All as the graph lists them (files first, then the names); Files the
// files alone and Names the names alone, each ranked by how it holds the text, without case: its name the
// text itself (a file's name also without its extension), then a name the text starts, then a name holding
// it anywhere, then one found by its words, its record's title or a record naming it; the graph's order kept
// within a rank.
std::vector<GraphSearchHit> search_project(const AssetGraph &graph, const std::string &text, SearchScope scope);

// The uses Find usages lists. Of the file `file` where `locator` is empty: who names the file or what it
// defines (AssetGraph::usages_of). Of the record at `locator` in it otherwise (a document record's locator,
// or the record's path in a file the graph reads through the engine's own parser, as a Go to names it; any
// record's path where no locator is it, as a finding names its record): the
// uses of each name the record defines that a lookup of the game finds (record_users); none for a record that
// defines no name.
std::vector<const GraphEdge *> usages_at(const AssetGraph &graph, const std::string &file, const std::string &locator);
// What Find usages lists the uses of, in words: the file's name, or the name the record defines and its file
// ("item 100300 in items.def"); the record's own place where it defines none.
std::string usages_subject_words(const AssetGraph &graph, const std::string &file, const std::string &locator);

// Where Go to definition leads from a record (F12 with a record selected and no reference field under the
// pointer or with the keyboard): what the first of its fields, in its type's order, names that the project
// defines or holds (reference_targets of the first written field whose value resolves): a mission entity's
// item, an item's model. False, `out` empty, for a record naming nothing that resolves.
bool record_definition(const AssetGraph &graph, const AssetScan &scan, const Document &document, const NodeAddress &record,
                       std::vector<ReferenceTarget> &out);

} // namespace opennova::editor
