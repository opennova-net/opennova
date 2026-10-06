#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/documents/def_table.h>
#include <editor/model/diagnostic.h>
#include <editor/model/table_document.h>
#include <formats/def/reserved_items.h>

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
	// A save generates the file in the form it was read in, but for the lines an edit changed (the file's
	// modeled layout, def_notes.h: its spacing, its comments, its spellings and what the game skips): said
	// before saving (the toolbar).
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override {
		return std::make_unique<DefCatalogDocument>(*this);
	}
	// The item ids other files of the project name (Document::set_names_used_elsewhere): a new item's
	// id and a copy's keep clear of them.
	void set_names_used_elsewhere(ReferenceKind kind, const std::vector<std::string> &names) override;

protected:
	// A step a Duplicate's name could not be set in is refused, saying why (prepare_duplicate).
	bool accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const override;
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
	// A record a Duplicate copies within its row (a sight, an action block, an attachment, an effect row, a
	// powerup's ammo row) is its own: the copy names none of the file's layout (def_clear_notes), so the
	// writer puts it down in its own form where the row's order has it.
	void prepare_record(const Node &row, const ListChange &change, DetachedRecord &record) const override;
	std::shared_ptr<const FileState> state_after_remove(const std::shared_ptr<const FileState> &state,
	                                                    size_t remaining) const override;
	// What the record's kind derives after any edit (an item's attachment slots).
	void after_edit(Node &row) override;

private:
	std::vector<int64_t> item_ids_elsewhere_;
	// Why the last copy's name could not be set ("" none), which its step's veto reports.
	mutable std::string duplicate_refusal_;
};

bool is_catalog_kind(AssetKind kind);

// The items.def ids the engine fixes (formats/def/reserved_items.h; itemdef-re.md, "The ids and rows the
// engine fixes"), as the catalog keeps them. Whether an item's change from `before` (null: a row the
// step adds) to `after` keeps them: a place or an objective the engine finds by its id
// (ReservedItemRule::Refuse) stays on its id, and its id takes no item of another kind; a mismatch the
// file already had is left as it was. False with why, and the field it is about ("id" or "type").
bool reserved_change_allowed(const def::DefItemDef *before, const def::DefItemDef &after, std::string &why,
                             std::string &field);
// The place or objective an item's name names, by retail's own row name or the editor's words for it
// (an item named "Insertion point" or "start, primary, player"; null for any other name).
const def::ReservedItem *reserved_item_named(const std::string &name);
// An item's type in words with its article ("a marker", "an effect", "an item of no type"), and a
// reserved row as a message names it ("the Insertion point, a marker"; "the Parachute").
std::string item_kind_words(int type);
std::string reserved_item_words(const def::ReservedItem &row);
// A batch adding the engine's row to an items.def, as the engine looks for it: an item named by the
// row's words, on its id, of its kind (an Add and the Sets naming what it made: one undo step).
std::vector<Edit> reserved_item_add_edits(const def::ReservedItem &row);
// A weapon, an ammo, an item or a powerup row another file names and no file defines, added to a catalog whose
// family holds that kind (ADR 0046 DI-15, DocumentType::define_symbol): named as referenced (an item on the id
// it names, by the reserved-id rule: the engine's own row where the engine keeps the id), born with the values
// the game's reader gives a new row of its kind. False for another kind, a family without it, or an item id
// that is none.
bool define_catalog_symbol(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out);

} // namespace opennova::editor
