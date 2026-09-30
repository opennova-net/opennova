#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/document.h>
#include <editor/project/project_document.h>
#include <formats/rtxt/rtxt.h>

namespace opennova::editor {

// A string table (ADR 0046 S6b): the RTXT `.bin` files the game reads text from
// (gametext.bin, gameerr.bin, ...). Rows are the table's sections in file order;
// a section's strings are its nested collection, kept contiguous the way the
// engine's lookup derives an entry's index [orig: TextResource_FindEntryBySectionAndKey
// @ 0x75D250]. The bytes are cp1252 (docs/interface/rtxt-strings-re.md): the
// document shows UTF-8 and stores an edit back as cp1252, refusing a character
// cp1252 has no byte for (utf8_to_cp1252 names it), so a table stays game-readable.
// A string's text runs over several lines. A section duplicates under a name of its
// own. `rtxt::write` is byte-exact for an untouched table, so saving without edits
// rewrites the same bytes.

enum class StringsKind : NodeKind { Section = 0, String = 1 };
constexpr NodeKind node_kind(StringsKind kind) { return static_cast<NodeKind>(kind); }

struct StringsSection : Node {
	std::string section_name;
	std::vector<rtxt::Entry> entries; // collections[0] carries their identities

	StringsSection();
	std::shared_ptr<Node> clone() const override { return std::make_shared<StringsSection>(*this); }
	std::string name() const override { return section_name; }
};

class StringsDocument : public Document {
public:
	// A section, the file's row (Add section), and its strings.
	const std::vector<RecordKindRow> &kinds() const override;
	std::vector<Collection> collections(const Node &row, const NodeAddress &owner) const override;
	const std::vector<FieldSchema> &fields(NodeKind kind) const override;
	// The ids of a section a first section of the same name shadows are inert.
	void refine_symbol(const NodeAddress &address, SymbolFacts &facts) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<Document> snapshot() const override {
		return std::make_unique<StringsDocument>(*this);
	}
	// The table as the engine reads it, rebuilt from the rows.
	rtxt::File table() const;

protected:
	// A string's key defines its string id in "TABLE.BIN/Section" (the table's file name, the
	// section as written).
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
	// A duplicated section is named anew: under its original's name no lookup would find it.
	void prepare_duplicate(Node &copy) const override;

private:
	// A string's index in its section as the document's index places it (SIZE_MAX for none):
	// where read and set_field look first.
	size_t place_of(const NodeAddress &address) const;
};

bool is_strings_kind(AssetKind kind);

// The strings document type's validator over one table (DocumentType::validate_file), an
// open document standing in for its file. An empty key is an error; a duplicate key inside
// one section is a warning (retail tables carry them, D-RTXT-5). A section is checked by the
// reader's own rule (rtxt::File::section_index, the first section of a name in any case): an
// empty name is an error, a name an earlier section has a warning (a section lookup never
// reaches it).
std::vector<Diagnostic> validate_strings_file(const Document &document);

} // namespace opennova::editor
