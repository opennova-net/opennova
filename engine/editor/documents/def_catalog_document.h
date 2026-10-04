#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/documents/def_table.h>
#include <editor/model/table_document.h>

namespace opennova::editor {

// The def catalogs (ADR 0046 S5): the first document type, over the catalog's table (def_table.h,
// S13 D10). The rows ARE the format records (`def::DefItemDef` and siblings, ADR 0046 d9), parsed by
// the witnessed family parsers and written by the canonical writers through the file's family row;
// the field schema is `formats/def`'s member inventory projected onto the neutral vocabulary. What a
// family is (its kinds, its parser and writer, its file-wide values) is its row: the document has no
// branch of its own for any family.

// A row: its native record, which owns the arrays its kind's lists are.
struct CatalogRow : TableRow {
	CatalogRecord native;

	explicit CatalogRow(NodeKind kind);
	CatalogRow(NodeKind kind, CatalogRecord record);
	std::shared_ptr<Node> clone() const override { return std::make_shared<CatalogRow>(*this); }
	std::string name() const override;
	RecordHandle record() const override;
	// The record and the arrays it owns (its attachments, actions, sights, effects or ammo rows).
	size_t footprint() const override;
	def::DefRecordKind record_kind() const { return native.kind(); }
};

class DefCatalogDocument : public TableDocument {
public:
	const RecordTable &table() const override { return catalog_table(); }
	// The family that opens this file (null for a kind the catalog does not open).
	const CatalogFamily *family() const { return catalog_family(kind()); }
	// Native access: a record as it stands (null when the document has none), and the vehicle spawn
	// registry the catalog window edits.
	const void *record(const NodeAddress &address) const;
	const std::vector<int> &spawn_ids() const;

	// The file's own kinds, its family's: its records (Add record) and what they hold, and a weapon
	// table's carry limits (Add carry limit), rows of their own.
	const std::vector<RecordKindRow> &kinds() const override;
	// A kind's fields without a document (DocumentType::fields, S13 V3): the table fields()
	// answers, the type's own for the process.
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return catalog_table().fields(kind); }
	// An item's vehicle spawn slots: the bits of the file's registry, each named by its id.
	bool record_choices(const NodeAddress &address, const FieldUse &use,
			std::vector<FieldChoice> &out) const override;
	// An item's symbol carries its TYPE (the DefItemType number, as text): the pool a mission puts a
	// record of the item in is its TYPE's (mission::authoring::entity_kind_for_item_type), which a
	// drop into a mission reads from the graph (ADR 0046 S14). A weapon's carries its loadout name's
	// key, which its words read (documents/name_source.h's definition_words).
	void refine_symbol(const NodeAddress &address, SymbolFacts &facts) const override;
	SerializeResult serialize() const override;
	// A save keeps the records' order and the file's indentation, not its spacing or comments, and
	// leaves out what the game skips: said before saving (the toolbar).
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override {
		return std::make_unique<DefCatalogDocument>(*this);
	}

protected:
	// An item's particle slot names a user point of the item's graphic model: the scope is
	// that model's file, and a record with no graphic names none. An item's vehicle spawn
	// slots offer the registry's ids (record_choices).
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// The family's file-wide state: the same IDs in the same slots (items.def's registry).
	bool same_file_state(const FileState *a, const FileState *b) const override;
	// A new row of one of the family's top kinds: its kind's defaults, named "New_<id>", and what its
	// kind's row adds (an item's marker type and an id no item of the file has).
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id,
	                                const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	bool set_file_value(std::shared_ptr<const FileState> &state, const Edit &edit, Diagnostic &error) override;
	// A copy a Duplicate made of a row: an identity of its own, as an Add gives one (an item's free id, the
	// kind's `duplicated`), and a name no row of its kind has (copy_name: "Dune Buggy (copy)", "WPN_M16_2"),
	// within the characters of a name the game keeps.
	void prepare_duplicate(Node &copy, const Node &original,
	                       const std::vector<std::shared_ptr<const Node>> &rows) const override;
	std::shared_ptr<const FileState> state_after_remove(const std::shared_ptr<const FileState> &state,
	                                                    size_t remaining) const override;
	// What the record's kind derives after any edit (an item's attachment slots).
	void after_edit(Node &row) override;
};

bool is_catalog_kind(AssetKind kind);

} // namespace opennova::editor
