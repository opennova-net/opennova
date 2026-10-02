#pragma once

#include <cstdint>
#include <string>

#include <editor/model/value.h>

namespace opennova::editor {

// A field as it applies to one record (Document::field_on, ADR 0046 S13 D2): the kind's schema,
// pointed at and never copied (its words, its range and its choices stay in the type's table),
// and what the record makes of it, which the base seeds from the schema and the type refines
// (Document::refine_field). Small enough to make for every field of every record the graph
// reads and every field the Inspector draws on every frame.
struct FieldUse {
	// The kind's field: an entry of Document::fields(kind), which outlives the document.
	const FieldSchema *schema = nullptr;
	// Whether the game reads the field on this record.
	Applicability applies = Applicability::Reads;
	// The reference it makes given the record's other fields, and the symbol it defines.
	ReferenceKind reference = ReferenceKind::None;
	ReferenceKind defines = ReferenceKind::None;
	// What a value that is one %NAME% stands for where the field makes no reference of its own: a
	// menu's text as the game reads it (ReferenceKind::MenuText: any text of a menu, the game
	// expanding its whole text first), which the stylesheet variable's value replaces as it is;
	// the graph reads it as a StyleVar edge through that kind (value_reference). None: a %NAME%
	// there is text like any other.
	ReferenceKind variable_through = ReferenceKind::None;
	// The namespace the name it references or defines lives in ("" = any).
	std::string scope;
	// What the record calls the field where its record's type names it otherwise than the schema (a
	// mission trigger's parameter: "Zone", "Waypoint list"); null: the schema's label (field_title).
	const char *label = nullptr;
	// What the reference's loader picks the file by, as its kind's row reads it (a model's
	// texture row: the row's type, reference_file_candidates; another texture: the game's loader
	// of it, texture_loader_arg); -1 for none.
	int32_t loader_arg = -1;
	FieldColor color = FieldColor::None;
	// Never less than the schema's: the base puts the schema's back after the type's refinement.
	bool read_only = false;
	// The record offers choices of its own in place of the schema's (Document::record_choices):
	// a model's LOD 0 parts by index, a clip's bones, an item's spawn slots. The widgets, the JSON
	// and the find ask for them; the graph's extraction never does. (A model's CTRL registers and
	// MTRX rows by index are Record references instead, S13 D8: the picker offers them.)
	bool own_choices = false;
	// The record holding the records those choices name by their index (a clip's bones: the clip
	// row); empty where they name no record (a part of LOD 0, a spawn slot).
	NodeAddress record_owner;
};

// A field as its schema declares it, on any record: what Document::field_on seeds before the
// type refines it.
inline FieldUse field_use(const FieldSchema &schema) {
	FieldUse use;
	use.schema = &schema;
	use.applies = schema.applies;
	use.reference = schema.reference;
	use.defines = schema.defines;
	use.scope = schema.scope;
	use.color = schema.color;
	use.read_only = schema.read_only;
	return use;
}

} // namespace opennova::editor
