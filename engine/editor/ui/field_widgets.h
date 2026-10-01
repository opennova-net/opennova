#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <editor/model/field_use.h>
#include <editor/model/value.h>

// The controls that edit one value of a field (ADR 0046 S12), each drawing what the field's
// schema says of it: a number keeps to its range while it is typed and drags where the field
// is ranged, its unit after it; a list of choices narrows by what is typed into it, and an
// open one takes a typed token; a colour is a swatch that opens a picker; the fields of a
// group share one row. Each draws at the width the caller set and says what changed, which
// the caller turns into the document's Set; the tooltips say what the schema says. A
// control's id is "##value" (a group's members each under its own field id). A text is edited
// in a box over the value itself (ui/text_edit: a value longer than the field holds is never
// cut); what is typed into an open list's box is the caller's (`typed`: one list is open at a
// time, so one string per window, emptied as a list opens), the controls keeping nothing.
namespace opennova::editor::field_widgets {

// What a field's tooltip says of it: its name, its id and the key the file writes when that
// differs, its unit and range, and the format table's note.
std::string field_tip(const FieldSchema &field);
// A table column's heading: the field's name, its unit after it.
std::string column_header(const FieldSchema &field);

// What a control did this frame.
struct Edited {
	bool changed = false;  // the value it was given holds the new one
	bool coalesce = true;  // typed or dragged: one undo step with the rest of the burst (a pick is a step alone)
	bool finished = false; // the control let go after an edit: the burst ends
};

// A number field's control: typed, or dragged where the field is ranged; a typed value past
// the range is clamped as it is typed, so none past it is ever set. `unit`: the field's unit
// after the control, inside the width set. `value` an integer, a real for a Real field (of
// several records, the primary's).
Edited number(const FieldSchema &field, Value &value, bool unit = true);

// A field with choices (the schema's, or the ones its record offers of its own:
// Document::choices_on): a list of them by name (the token the file writes in each one's
// tooltip; each item under its index among the choices, so two of one name are two) under a box
// that narrows it by what is typed (a long list, an open field); an open field takes the typed
// token itself (Enter, or its line at the top), written as typed.
// `mixed`: no choice named. `value` a text field's token, a number's value; `typed` what the
// list's box holds.
Edited choice(const FieldSchema &field, const std::vector<FieldChoice> &choices, Value &value,
		std::string &typed, bool mixed = false);

// A colour field's swatch (`color` the form the field holds a colour in, as it applies to its
// record), a square as high as a control that opens a picker: a HexArgb text (none drawn for a
// %VAR%, which the stylesheet resolves), written AARRGGBB; a PackedRgb integer, written
// 0xRRGGBB.
Edited swatch(FieldColor color, Value &value);
// A red / green / blue group's swatch over its three channels (0..255, in that order).
Edited channel_swatch(std::vector<Value> &values);
// The width a swatch takes on its line, the gap after it included.
float swatch_width();

// One value's control by what the field is as it applies to its record (a flags field's bits
// aside: the caller's, which sets each record's own): a yes / no tick, a list of `choices` (the
// schema's, or the record's own: Document::choices_on), a text (a HexArgb one after its swatch),
// a number (a PackedRgb one after its swatch). `compact`: a table cell (a text on one line, no
// unit: its column's heading has it). `mixed`: the targets differ (a text shows empty with a
// hint, a list names no choice, a number the primary's value). `typed`: what an open list's box
// holds (choice).
Edited value(const FieldUse &field, const std::vector<FieldChoice> &choices, Value &value,
		bool compact, std::string &typed, bool mixed = false);

// The fields of one group on one row (as they apply to the record), each at its share of the
// width set (after a swatch for a Channel group), with its name and what it is in its tooltip.
// `choices` each member's choices as they apply there (Document::choices_on: a record's own list
// where it has one), `values` their values, both in the order of `fields`; `mixed` which of them
// differ across the records edited (empty: none). When one changed, `changed` is its index and
// `values[changed]` its value; a swatch's pick changes every channel (`changed` SIZE_MAX).
// `typed`: what an open list's box holds (choice).
Edited group(const std::vector<FieldUse> &fields,
		const std::vector<const std::vector<FieldChoice> *> &choices, std::vector<Value> &values,
		size_t &changed, std::string &typed, const std::vector<bool> &mixed = {});

} // namespace opennova::editor::field_widgets
