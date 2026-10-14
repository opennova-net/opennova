#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/model/finding_code_row.h>
#include <formats/mns/mns.h>

namespace opennova::editor {

// A menu stylesheet (ADR 0046 S9i): the `.mns` files, one row per variable of the list the game's reader makes of
// the file (mns::KeyValueList [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639870]: its directives decided, its
// comments, blank lines and switched-off lines read as nothing, the first spelling and the last value of each name;
// the S26 ruling that superseded ADR 0014's lossless document). A variable's value is the value the game reads. A
// save writes the list alone (mns::write: `name<TAB>value` and CR LF a line, the only line end the game reads without
// stopping [orig: @ 0x639c3b]); what the stored file holds besides (its comments, blank lines and directives, the
// lines past where the game stops reading it, other line ends) the file state remembers, which
// validate_styles_file reports until a save writes the file.

enum class StyleKind : NodeKind { Variable = 0 };
constexpr NodeKind node_kind(StyleKind kind) { return static_cast<NodeKind>(kind); }

struct StyleRow : Node {
	mns::KeyValue native;

	std::shared_ptr<Node> clone() const override { return std::make_shared<StyleRow>(*this); }
	std::string name() const override { return native.name; }
	size_t footprint() const override;
};

// What the stored file holds that the list does not write, until a save writes the file.
struct StyleFileState : FileState {
	// Its first line that does not end CR LF (0 = every one does).
	int line_end_line = 0;
	// Where the game's reader hangs on the file as stored (0 = it does not), and where it stops reading it (0 = it
	// reads to the end): mns::parse_key_value_buffer's stop, its line.
	int stored_hang_line = 0;
	int stored_stop_line = 0;
	// The stored text, its layout aside (comments, blank lines, spacing), is not what a save writes of the list:
	// directives and the lines they switch off, a name defined again (the list keeps its first spelling and last
	// value), a value continued onto another line.
	bool dropped = false;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<StyleFileState>(*this); }
	size_t footprint() const override { return sizeof(StyleFileState); }
};

class MnsDocument : public Document {
public:
	// One row per variable.
	const std::vector<RecordKindRow> &kinds() const override;
	std::vector<Collection> collections(const Node &, const NodeAddress &) const override { return {}; }
	const std::vector<FieldSchema> &fields(NodeKind kind) const override { return schema(kind); }
	// A kind's fields without a document (DocumentType::fields, S13 V3): the table fields()
	// answers, the type's own for the process.
	static const std::vector<FieldSchema> &schema(NodeKind kind);
	// The row's own fields (read), and its line in the file a save writes (`line`).
	bool get(const NodeAddress &address, const std::string &field, Value &out) const override;
	// A variable's name defines its %NAME%, its value what the game reads.
	void refine_symbol(const NodeAddress &address, SymbolFacts &facts) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override {
		return std::make_unique<MnsDocument>(*this);
	}

	// The list the rows hold, the game's reading of the file a save writes.
	mns::KeyValueList list() const;
	// A row's line in the file a save writes (0 when the row is gone), and the row a line is in.
	int line_of(NodeId row) const;
	NodeId row_at_line(int line) const;
	// The row of `name` (a %NAME% or the bare name), else 0: the list holds one of each name.
	NodeId winning_row(const std::string &name) const;
	// True for menu_style.mns and brand.mns, the stylesheets the game reads. What a line's value
	// is used as, which reads the menus' uses of it too, is graph/style_value_use's.
	bool read_by_game() const;

protected:
	// A variable's value names a font or a texture when its extension says so, in a stylesheet the game reads: the
	// graph's edge, the badge and the picker.
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
	// A copy is a variable of a name of its own, NAME_2 (or the first free NAME_n): the list holds one of each name.
	void prepare_duplicate(Node &copy, const Node &original,
	                       const std::vector<std::shared_ptr<const Node>> &rows) const override;
	// A step whose list the writer refuses (a name the reader takes for another row's) is refused.
	bool accept_step(const EditStep &step, const StagedRows &rows,
	                 StepRefusal &refusal) const override;
};

// The stylesheet document type's validator over one .mns (DocumentType::validate_file), an open document standing
// in for its file: what the stored file holds that a save leaves out (its other line ends, its comments, directives
// and switched-off lines, what follows where the game stops reading it) and where the game's reader hangs on it; a
// stylesheet the game does not read; and on each variable: markup the game pastes into the menus, a %NAME% inside a
// value (never expanded), a doubled backslash. What the menus make of a definition (a name brand.mns redefines, a
// value used as a colour that is not one, a value used as more than one of colour, font and image, a name no menu
// uses) is graph/use_checks'.
std::vector<Diagnostic> validate_styles_file(const DocumentBase &document);
// A style variable no stylesheet the game reads defines, added to one it reads (ADR 0046 DI-15,
// DocumentType::define_symbol): the line Add variable makes, named as the menu names it, where the game reads it.
bool define_style_variable(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out);

// The stylesheet type's own finding codes (DocumentType::findings), each a row of its table (mns_document.cpp,
// static_asserted into this order): its validator's (a line end the game does not read, which a save ends CR LF;
// the input a save leaves out; a stylesheet the game does not read; a value's markup, a %NAME% inside it, a doubled
// backslash; where the game's reader hangs on the stored file or stops reading it), then the use checks' of its
// variables (graph/use_checks.h).
enum class StyleFinding {
	LineEnding,
	IgnoredInput,
	NotLoaded,
	XmlChar,
	NestedVar,
	Backslash,
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
FindingTable style_finding_codes();

} // namespace opennova::editor
