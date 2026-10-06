#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/model/finding_code_row.h>
#include <formats/mns/mns_document.h>

namespace opennova::editor {

// A menu stylesheet (ADR 0046 S9i, ADR 0014): the `.mns` files, one row per line of the
// lossless mns::Document (a variable spanning several lines is one row). A variable's
// value is the value the game reads, as written (a "\\" pair stays doubled, the game
// keeps both). Every step is checked before it commits (accept_step): the rows are
// rendered and read again, and a change the game would read otherwise (a line inside a
// switched-off #if block, a line the variable above would take as its value) is refused.
// The #if / #else / #endif lines, the lines they switch off and a variable whose value
// crosses other lines stay where they are; only their own fields change. Nothing is
// dropped: the document has no source issues, and its diagnostics are the Problems rows
// (validate_styles_file). Every line ends CR LF, the only line end the game reads without
// stopping [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639c3b]: a file with other line ends loads
// with CR LF ones (the rows hold what Save writes), and the file state remembers its
// first line that did not, which validate_styles_file reports until Save writes the file.

enum class StyleKind : NodeKind { Variable = 0, Comment = 1, Blank = 2, Conditional = 3, Inactive = 4 };
constexpr NodeKind node_kind(StyleKind kind) { return static_cast<NodeKind>(kind); }

struct StyleRow : Node {
	mns::Node native;

	std::shared_ptr<Node> clone() const override { return std::make_shared<StyleRow>(*this); }
	// A variable's name, a comment's or a directive's text; "" for a blank line.
	std::string name() const override;
	// The line's text and each physical line a variable spans.
	size_t footprint() const override;
};

struct StyleFileState : FileState {
	bool has_bom = false;
	// The file as read: its first line that does not end CR LF (0 = every one does), and
	// what the game does with such a file.
	int line_end_line = 0;
	std::string line_end_message;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<StyleFileState>(*this); }
	size_t footprint() const override {
		return sizeof(StyleFileState) + footprint_of(line_end_message);
	}
};

class MnsDocument : public Document {
public:
	// One row per line, every kind a row: a variable, a comment and a blank line the outline adds;
	// a directive and a switched-off line, which the file holds as written.
	const std::vector<RecordKindRow> &kinds() const override;
	std::vector<Collection> collections(const Node &, const NodeAddress &) const override { return {}; }
	const std::vector<FieldSchema> &fields(NodeKind kind) const override { return schema(kind); }
	// A kind's fields without a document (DocumentType::fields, S13 V3): the table fields()
	// answers, the type's own for the process.
	static const std::vector<FieldSchema> &schema(NodeKind kind);
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
	// True for menu_style.mns and brand.mns, the stylesheets the game reads. What a line's value
	// is used as, which reads the menus' uses of it too, is graph/style_value_use's.
	bool read_by_game() const;

protected:
	// A variable's value names a font or a texture when its extension says so, on the
	// definition the game reads of a stylesheet it reads: the graph's edge, the badge and
	// the picker. An earlier definition of the same name is ignored.
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	bool read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id,
	                                const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	bool set_field(Node &row, const NodeAddress &address, const std::string &field, const Value &value,
	               std::string &error) override;
	bool edit_collection(Node &row, const Edit &edit, const IdAllocator &allocate, NodeId &added,
	                     std::string &error) override;
	bool accept_step(const EditStep &step, const StagedRows &rows,
	                 StepRefusal &refusal) const override;

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
};

bool is_style_kind(AssetKind kind);

// The stylesheet document type's validator over one .mns (DocumentType::validate_file), an
// open document standing in for its file, in one pass over its rows. What the game does with
// each odd line (the format's diagnostics: an error where it stops reading or would stop
// responding, or where the document cannot show a line the way the game reads it); a
// stylesheet the game does not read; and on the definition the game reads of each name:
// markup the game pastes into the menus, a %NAME% inside a value (never expanded), a doubled
// backslash, a value the game reads otherwise than shown. What the menus make of a definition
// (a name brand.mns redefines, a value used as a colour that is not one, a value used as more
// than one of colour, font and image, a name no menu uses) is graph/use_checks'.
std::vector<Diagnostic> validate_styles_file(const DocumentBase &document);
// A style variable no stylesheet the game reads defines, added to one it reads (ADR 0046 DI-15,
// DocumentType::define_symbol): the line Add variable makes, named as the menu names it, where the game reads it.
bool define_style_variable(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out);

// The stylesheet type's own finding codes (DocumentType::findings), each a row of its table
// (mns_document.cpp, static_asserted into this order): its validator's (a line end the game does
// not read, which a rewrite ends CR LF; a stylesheet the game does not read; a value's markup, a
// %NAME% inside it, a doubled backslash, a value the game reads otherwise than shown), then the
// stylesheet reader's, one per mns::DiagnosticCode (finding_code(mns::DiagnosticCode): style. and
// the code's token, '_' for '-'; the reader's line-ending is LineEnding), then the use checks' of
// its variables (graph/use_checks.h).
enum class StyleFinding {
	LineEnding,
	NotLoaded,
	XmlChar,
	NestedVar,
	Backslash,
	ReadDifferently,
	// the stylesheet reader's (mns::DiagnosticCode)
	DirectiveForm,
	IfWithoutArgument,
	NoncanonicalIfArg,
	UnbalancedElse,
	DuplicateElse,
	UnbalancedEndif,
	UnknownDirective,
	DirectiveTail,
	LoneBackslash,
	ValueIsDirective,
	ValueStartsWithHash,
	ValueOnNextLine,
	DuplicateName,
	ContinuedDuplicate,
	NulByte,
	InvalidNameChar,
	MissingValueDelimiter,
	NoValue,
	ContinuationAtEof,
	UnterminatedIf,
	Hangs,
	Stops,
	// the use checks'
	OverriddenByBrand,
	Unused,
	NotAColor,
	MixedUse,
	kCount
};
const FindingCodeRow &finding_code(StyleFinding code);
// The row of what the stylesheet reader says of a line.
const FindingCodeRow &finding_code(mns::DiagnosticCode code);
FindingTable style_finding_codes();

} // namespace opennova::editor
