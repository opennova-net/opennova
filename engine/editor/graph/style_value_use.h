#pragma once

#include <editor/model/field_use.h>
#include <editor/model/value.h>

namespace opennova::editor {

class AssetGraph;
class MnsDocument;

// What a stylesheet line's value is used as (ADR 0046 S9i; S13 V1 took it out of the stylesheet
// view, S13 V3 out of the document: it reads the menus' uses of the definition, which are the
// graph's). The menus use the definition the game reads, the last of its name in the shell's
// stylesheets (the graph's binding), each use reading its value as a colour, a font or an image.
// The value is a colour, with a swatch, when a use reads it as one, or when no use reads it as
// anything (a font, an image, a string id, a name or a shown text) and the game's wcstoul reads it
// whole; else it is picked from the project's files of the kind a use loads, or its own value
// names (the value field's reference as the document's field_on gives it). A line that stays
// where it is (frozen) is neither.
struct StyleValueUse {
	bool winner = false; // the definition the game reads of its name in this file
	bool bound = false;  // and the one of all the shell's stylesheets: the menus' uses are its own
	bool colour = false; // the value is a colour
	bool picks = false;  // the value is picked from the project's files of file.reference's kind
	FieldUse file;       // the value field as the picker takes it (a Font, a MenuTexture, or None)
};

// What the variable `line` of `document` has its value used as over `graph` (null: no use is
// known); none for a line that is no variable. Made anew on each call: the stylesheet view keeps
// each line's while the document's load and revision and the graph stand.
StyleValueUse style_value_use(const MnsDocument &document, const NodeAddress &line,
		const AssetGraph *graph);

} // namespace opennova::editor
