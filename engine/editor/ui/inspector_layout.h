#pragma once

#include <string>
#include <vector>

#include <editor/model/document.h>

namespace opennova::editor {

struct GraphEdge;
struct SessionView;

// --- what a field is called (a field's own words: model/field_text) ---------------------

// The name of the field `id` of a `kind` record of the document (the id when its type has
// no such field), and of the field a graph edge reads, asked of its file's type (the id
// for a native format's slot, "material[2].texture[0]", or a file the editor does not edit).
std::string field_title(const Document &document, NodeKind kind, const std::string &id);
std::string edge_field_title(const SessionView &view, const GraphEdge &edge);

// --- the inspector's plan for one record (ADR 0046 S9h2) ---------------------------------

// A group of the record's fields: those whose dotted ids start with the same step
// ("position.left" and "position.top" under "position"), the block's own yes / no field
// first when it has one (the field named as the step: "string" for STRING), and the
// collections whose kind token starts with the step ("items.item" under "items", the part
// "list_box" under "list_box"); a field of one step that names no group joins the fields
// sharing its heading (FieldSchema::section: a def's physics keys). A collection no group
// claims is a section of its own after the groups. Every field is as it applies to the record (Document::field_on); a field the
// game ignores there is left out while the file does not write it, and a collection it
// ignores while it holds nothing.
struct InspectorSection {
	std::string key;   // the step ("" = the record's general fields); a collection's own: its kind token
	std::string title; // the heading ("" for the general fields)
	bool has_toggle = false;
	FieldUse toggle;              // the block's yes / no field (has_toggle)
	std::vector<FieldUse> fields; // the toggle left out
	std::vector<Document::Collection> collections;
	bool written = false; // a field in it is written, or a collection holds a record (drawn open)
};

// The sections for `record`, whose fields are shown, and `collections_owner`, whose
// collections are (the record itself, or its owner when it holds none: the records beside
// it). A filter keeps the fields whose id, name or key as the file spells it contains it and
// the collections whose name or token does ("" = everything); a section left empty is dropped.
std::vector<InspectorSection> plan_inspector(const Document &document, const NodeAddress &record,
                                             const NodeAddress &collections_owner, const std::string &filter);
// The plan without what a type's own part of the Inspector draws itself (ADR 0046 S15: a mission's
// event, trigger or action in words): the fields of those ids and the collections of those kinds, a
// section left with nothing dropped.
void leave_out(std::vector<InspectorSection> &plan, const std::vector<std::string> &fields,
               const std::vector<NodeKind> &collections);

// --- several records at once (ADR 0046 S9k2) ---------------------------------------------

// Whether records of the kinds `a` and `b` of the document take one shared form: the same fields (their
// ids and types) in the same order (a mission's four entity pools: four kinds over one field table).
bool kinds_alike(const Document &document, NodeKind a, NodeKind b);

// The fields `records` (records of one kind, or of kinds alike, the first the primary) share, grouped as
// plan_inspector groups one record's (a block's own yes / no field first in its group, no
// collections): a field every record has, that the game reads on each of them or that the
// file writes there, neither read-only nor the name that tells the records apart. Each is
// as it applies to the primary. A filter keeps the fields whose id, name or key contains it.
std::vector<InspectorSection> plan_shared_inspector(const Document &document, const std::vector<NodeAddress> &records,
                                                    const std::string &filter);
// How many records are selected, in words: "3 Window records selected" of one kind; of several,
// "5 records selected (2 Item, 3 Building)", the kinds as they first come.
std::string selected_words(const Document &document, const std::vector<NodeAddress> &records);
// True when the records do not all hold the same value in `field`, or an optional field is
// written on some of them and left out on others.
bool field_mixed(const Document &document, const std::vector<NodeAddress> &records, const std::string &field);

// --- a change the Inspector makes --------------------------------------------------------

// A flags field's bit `bit` set (`on`) or cleared on each of `records`, each keeping its own
// other bits (one whose value does not read keeps the first record's): the Sets of one batch,
// one undo step. A signed 32-bit field keeps its word signed.
std::vector<Edit> flag_bit_edits(const Document &document, const std::vector<NodeAddress> &records,
                                 const FieldSchema &field, int64_t bit, bool on);

} // namespace opennova::editor
