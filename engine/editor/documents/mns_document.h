#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/document.h>
#include <formats/mns/mns_document.h>

namespace opennova::editor {

// A menu stylesheet (ADR 0046 S9i, ADR 0014): the `.mns` files, one row per line of the
// lossless mns::Document (a variable spanning several lines is one row). A variable's
// value is the value the game reads, as written (a "\\" pair stays doubled, the game
// keeps both). Every change is checked before it commits (accept_change): the rows are
// rendered and read again, and a change the game would read otherwise (a line inside a
// switched-off #if block, a line the variable above would take as its value) is refused.
// The #if / #else / #endif lines, the lines they switch off and a variable whose value
// crosses other lines stay where they are; only their own fields change. Nothing is
// dropped: the document has no source issues, and its diagnostics are the Problems rows
// (validate_styles). Every line ends CR LF, the only line end the game reads without
// stopping [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639c3b]: a file with other line ends loads
// with CR LF ones (the rows hold what Save writes), and the file state remembers its
// first line that did not, which validate_styles reports until Save writes the file.

enum class StyleKind : NodeKind { Variable = 0, Comment = 1, Blank = 2, Conditional = 3, Inactive = 4 };
constexpr NodeKind node_kind(StyleKind kind) { return static_cast<NodeKind>(kind); }

struct StyleRow : Node {
	mns::Node native;

	std::shared_ptr<Node> clone() const override { return std::make_shared<StyleRow>(*this); }
	// A variable's name, a comment's or a directive's text; "" for a blank line.
	std::string name() const override;
};

// What a variable line's value is used as (ADR 0046 S9i; S13 V1: the stylesheet view draws it,
// the document decides it). The menus use the definition the game reads, the last of its name
// in the shell's stylesheets (the graph's binding), each use reading its value as a colour, a
// font or an image. The value is a colour, with a swatch, when a use reads it as one, or when
// none reads it as a font or an image and the game's wcstoul reads it whole; else it is picked
// from the project's files of the kind a use loads, or its own value names (field_on's
// reference). A line that stays where it is (frozen) is neither.
struct StyleValueUse {
	bool winner = false; // the definition the game reads of its name in this file
	bool bound = false;  // and the one of all the shell's stylesheets: the menus' uses are its own
	bool colour = false; // the value is a colour
	bool picks = false;  // the value is picked from the project's files of file.reference's kind
	FieldUse file;       // the value field as the picker takes it (a Font, a MenuTexture, or None)
};

struct StyleFileState : FileState {
	bool has_bom = false;
	// The file as read: its first line that does not end CR LF (0 = every one does), and
	// what the game does with such a file.
	int line_end_line = 0;
	std::string line_end_message;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<StyleFileState>(*this); }
};

class MnsDocument : public Document {
public:
	// One row per line, every kind a row: a variable, a comment and a blank line the outline adds;
	// a directive and a switched-off line, which the file holds as written.
	const std::vector<RecordKindRow> &kinds() const override;
	std::vector<Collection> collections(const Node &, const NodeAddress &) const override { return {}; }
	const std::vector<FieldSchema> &fields(NodeKind kind) const override;
	// The row's own fields (read), and what its place in the document says: its first
	// line (`line`) and whether a later definition overrides it (`overridden`).
	bool get(const NodeAddress &address, const std::string &field, Value &out) const override;
	// A variable's name defines its %NAME%, on its first line: the definition the game reads
	// carries its value; an earlier definition of the name, and one past where the game stops
	// reading, are inert.
	void refine_symbol(const NodeAddress &address, SymbolFacts &facts) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override {
		return std::make_unique<MnsDocument>(*this);
	}

	// The rows as the game's reader sees them, rendered and parsed again: the diagnostics
	// with their current lines, evaluate(), entries().
	mns::Document native() const;
	// A row's first physical line (0 when the row is gone), and the row a line is in.
	int line_of(NodeId row) const;
	NodeId row_at_line(int line) const;
	// True for a row that stays where it is: a directive, a switched-off line, or a
	// variable whose value crosses other lines.
	bool frozen(const Node &row) const;
	// The row of the definition the game reads for `name` (the last one), else 0.
	NodeId winning_row(const std::string &name) const;
	// True for menu_style.mns and brand.mns, the stylesheets the game reads.
	bool read_by_game() const;
	// What a variable line's value is used as over `graph` (none: no use is known), kept for
	// each line while the document's load generation and revision, the graph and `graph_key` (the
	// caller's: it moves whenever the graph may have) stand.
	const StyleValueUse &style_value_use(const NodeAddress &line, const AssetGraph *graph,
	                                     uint64_t graph_key) const;

protected:
	// A variable's value names a font or a texture when its extension says so, on the
	// definition the game reads of a stylesheet it reads: the graph's edge, the badge and
	// the picker. An earlier definition of the same name is ignored.
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	bool read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, std::string &error) override;
	bool set_field(Node &row, const NodeAddress &address, const std::string &field, const Value &value,
	               std::string &error) override;
	bool edit_collection(Node &row, const Edit &edit, const IdAllocator &allocate, NodeId &added,
	                     std::string &error) override;
	bool accept_change(const Change &change, std::string &error) const override;

private:
	// The variables the game reads of the rows as they stand (native().evaluate()), once per
	// load generation and revision.
	const mns::StyleSheet &game_sheet() const;
	struct GameSheet {
		bool made = false;
		uint64_t load_generation = 0, revision = 0;
		mns::StyleSheet sheet;
	};
	mutable GameSheet game_sheet_;
	// What style_value_use made, by row, and what it was made over.
	struct ValueUses {
		bool made = false;
		uint64_t load_generation = 0, revision = 0;
		const AssetGraph *graph = nullptr;
		uint64_t graph_key = 0;
		std::unordered_map<NodeId, StyleValueUse> rows;
	};
	mutable ValueUses value_uses_;
};

bool is_style_kind(AssetKind kind);

// The stylesheet document type's validator (document_types): every .mns in the project,
// open documents standing in for their files. What the game does with each odd line
// (the format's diagnostics: an error where it stops reading or would stop responding,
// or where the document cannot show a line the way the game reads it); a stylesheet the
// game does not read; and on the definition the game reads, through the graph's uses of
// it: a name brand.mns redefines, a value used as a colour that is not one, a value used
// as more than one of colour, font and image, a name no menu uses (style.unused, info),
// markup the game pastes into the menus, a %NAME% inside a value (never expanded), a
// doubled backslash, a value the game reads otherwise than shown.
std::vector<Diagnostic> validate_styles(const ValidationInput &input, const AssetGraph &graph);

} // namespace opennova::editor
