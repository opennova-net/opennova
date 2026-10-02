#include <editor/graph/style_value_use.h>

#include <string>

#include <editor/documents/mns_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <formats/mnu/mnu_layout.h>

namespace opennova::editor {

StyleValueUse style_value_use(const MnsDocument &document, const NodeAddress &line,
		const AssetGraph *graph) {
	constexpr NodeKind kVariable = node_kind(StyleKind::Variable);
	StyleValueUse out;
	const Node *row = document.row(line.row);
	if (!row || row->kind != kVariable) return out;
	// The uses of the definition the game reads, by what its value must be there.
	out.winner = document.winning_row(row->name()) == row->id;
	bool colour = false, font = false, image = false, other = false;
	if (out.winner && document.read_by_game() && graph) {
		const GraphSymbol *binding = graph->style_binding(row->name());
		out.bound = binding && binding->file == document.path();
	}
	if (out.bound)
		for (const GraphEdge *edge : graph->referrers_of(ReferenceKind::StyleVar, row->name())) {
			const StyleVariableUse use = style_variable_use(edge->through);
			colour = colour || use == StyleVariableUse::Colour;
			font = font || use == StyleVariableUse::Font;
			image = image || use == StyleVariableUse::Image;
			other = other || use == StyleVariableUse::Other;
		}
	const NodeAddress address{row->id, row->kind, 0};
	FieldUse value;
	for (const FieldSchema &schema : document.fields(kVariable))
		if (schema.id == "value") value = document.field_on(address, schema);
	Value text;
	const std::string shown =
			document.get(address, "value", text) ? std::get<std::string>(text) : std::string();
	const bool fixed = document.frozen(*row);
	// The guess from the value alone only where no use says what it is: a string id's, a name's
	// or a shown text's value is none of a colour, a font and an image, hex digits or not.
	out.colour = !fixed && (colour || (!font && !image && !other && mnu::color_reads_whole(shown)));
	out.file = value;
	if (font || value.reference == ReferenceKind::Font) out.file.reference = ReferenceKind::Font;
	else if (image || value.reference == ReferenceKind::MenuTexture)
		out.file.reference = ReferenceKind::MenuTexture;
	else out.file.reference = ReferenceKind::None;
	out.picks = !fixed && !out.colour && graph && out.file.reference != ReferenceKind::None;
	return out;
}

} // namespace opennova::editor
