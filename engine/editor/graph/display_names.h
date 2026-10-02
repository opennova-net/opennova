#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/documents/name_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/document.h>
#include <editor/model/field_use.h>

namespace opennova::editor {

// The display-name service (ADR 0046 S15, Names): the one place a value a document holds is turned
// into what a modder reads, which the outline's rows, the Inspector's values and reference fields, the
// pickers, Problems, the viewport's labels and tooltips, the status line and the wire's answers all
// call. A document type words its own records and values (DocumentType::record_label, value_label: the
// mission's, documents/mission_labels.h); what names a definition of the project is worded from the
// graph (an item by its catalog's name, a string id by its text). Pure functions of the document as it
// stands and of the names (the graph, through GraphNameSource); a caller drawing many rows a frame keeps
// a DisplayNameCache.

// The asset graph as the names the display names read (NameSource): its definitions as the game's
// lookup reaches them. Holds the graph by reference: made where it is used (a frame, a query).
class GraphNameSource final : public NameSource {
public:
	explicit GraphNameSource(const AssetGraph &graph) : graph_(graph) {}
	const GraphSymbol *symbol(ReferenceKind kind, const std::string &name, const std::string &scope) const override {
		return graph_.resolve_symbol(kind, name, scope);
	}
	const GraphSymbol *reached(const GraphEdge &edge) const override { return graph_.symbol_reached(edge); }
	uint64_t generation() const override { return graph_.generation(); }
	const AssetGraph &graph() const { return graph_; }

private:
	const AssetGraph &graph_;
};

// A record's title as the windows show it (the outline's rows, the breadcrumb, the viewport's labels,
// the Inspector's heading, the place of a Problems row): its type's words with the project's names
// (DocumentType::record_label: a mission's entity by its item's name and its SSN), else the document's
// own title (Document::record_title). `names` null: the document's own words. "" for a record the
// document does not have.
std::string record_display(const Document &document, const NodeAddress &address, const NameSource *names);

// A field's value as the windows show it beside the value itself (the Inspector, the wire's `display`):
// its type's words (DocumentType::value_label: a mission's SSN, zone, event, group, path, stop, item,
// text key), else a choice's name (field_text: a flags field's bits by name), else what the reference
// names (an item by its catalog's name, a string id by its text, a record of a record set by its own
// name; one the project lacks said in words, `dangling`). Empty words for a value that stands for
// nothing with a name (a plain number, a text, a file by its name: the badge says whether it is
// found).
DisplayName value_display(const Document &document, const NodeAddress &address, const FieldUse &field, const Value &value,
                          const NameSource *names);

// The picker's names worded (ADR 0046 S15): each choice whose label is empty given what the field's
// value would read as set to it (value_display), so a list of ids reads as names ("Ranger #12" for
// SSN 12, "Ammo crate" for item 106101) with the id muted beside it; the filter matches either.
void word_choices(const Document &document, const NodeAddress &address, const FieldUse &field, const NameSource *names,
                  std::vector<ReferenceChoice> &choices);

// What a definition the picker offers points at, for its tooltip, made only while it shows (cheap
// enough per hover, not per row): an item's model (the file its graphic loads) and its catalog, a string
// id's text and table, a record set's record by its own name; "" for nothing more than its name.
std::string symbol_preview(const AssetGraph &graph, ReferenceKind kind, const std::string &name, const std::string &scope);

// The record titles of one document with one source of names, kept while both stand (the document's
// identity, load and revision, the names' generation): what a view drawing many rows a frame (the
// viewport's labels, a list) reads, each record worded once.
class DisplayNameCache {
public:
	const std::string &record(const Document &document, const NodeAddress &address, const NameSource *names);
	// How many titles were worded (made, not read from the cache): a test's measure.
	size_t made() const { return made_; }

private:
	struct Key {
		uint64_t document = 0, load = 0, revision = 0, names = 0;
		bool has_names = false;
		bool operator==(const Key &other) const {
			return document == other.document && load == other.load && revision == other.revision && names == other.names &&
			       has_names == other.has_names;
		}
	};
	struct AddressHash {
		size_t operator()(const NodeAddress &address) const {
			return std::hash<uint64_t>()(address.row * 1000003u ^ address.child * 31u ^ uint64_t(address.kind));
		}
	};
	Key key_;
	std::unordered_map<NodeAddress, std::string, AddressHash> titles_;
	size_t made_ = 0;
};

} // namespace opennova::editor
