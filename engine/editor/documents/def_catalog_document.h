#pragma once

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <editor/model/document.h>
#include <formats/def/def_write.h>

namespace opennova::editor {

// The item / weapon / ammo catalogs (ADR 0046 S5): the first document type. The
// rows ARE the format records (`def::DefItemDef` and siblings, ADR 0046 d9), parsed
// by the witnessed family parsers and written by the canonical writers; the field
// schema is `formats/def`'s member inventory projected onto the neutral vocabulary.

constexpr NodeKind node_kind(def::DefRecordKind kind) { return static_cast<NodeKind>(kind); }
constexpr def::DefRecordKind def_kind(NodeKind kind) { return static_cast<def::DefRecordKind>(kind); }
// The field that names a top-level record: an item's display_name, a weapon's weapon_name,
// an ammo's name.
const char *catalog_name_field(def::DefRecordKind kind);

// A row: the native record with ownership for its C arrays. Collection slot 0 holds
// the attachments / actions / effects, slot 1 a weapon's sights.
struct CatalogRow : Node {
	std::variant<def::DefItemDef, def::DefWeaponDef, def::DefAmmoDef, def::DefAmmoClassCarry> data;

	explicit CatalogRow(def::DefRecordKind kind);
	CatalogRow(const CatalogRow &other);
	CatalogRow &operator=(const CatalogRow &) = delete;
	~CatalogRow() override;

	std::shared_ptr<Node> clone() const override { return std::make_shared<CatalogRow>(*this); }
	std::string name() const override;
	// The record and the arrays it owns (its attachments, actions, sights or effects).
	size_t footprint() const override;
	def::DefRecordKind record_kind() const { return def_kind(kind); }
	void *record();
	const void *record() const;
};

// items.def's file-wide vehicle spawn registry.
struct ItemsFileState : FileState {
	std::vector<int> spawn_ids;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<ItemsFileState>(*this); }
	size_t footprint() const override { return sizeof(ItemsFileState) + footprint_of(spawn_ids); }
};

class DefCatalogDocument : public Document {
public:
	// The top-level record kind of this file.
	def::DefRecordKind record_kind() const;
	// Native access: a record as it stands (null when the document has none), and the
	// vehicle spawn registry the catalog window edits.
	const void *record(const NodeAddress &address) const;
	const std::vector<int> &spawn_ids() const;

	// The file's own kinds: its records (Add record) and what they hold, and a weapon table's carry
	// limits (Add carry limit), rows of their own.
	const std::vector<RecordKindRow> &kinds() const override;
	std::vector<Collection> collections(const Node &row, const NodeAddress &owner) const override;
	const std::vector<FieldSchema> &fields(NodeKind kind) const override;
	// An item's vehicle spawn slots: the bits of the file's registry, each named by its id.
	bool record_choices(const NodeAddress &address, const FieldUse &use,
			std::vector<FieldChoice> &out) const override;
	SerializeResult serialize() const override;
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
	// A member its line writes as a number of its own reads in the units the file writes it
	// (def_authored_get); the others as stored.
	bool read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const override;
	// A field whose line has a present flag (DefProperty::present_field) is written while the
	// flag is set: Clear clears it, keeping the value.
	bool read_present(const Node &row, const NodeAddress &address, const std::string &field) const override;
	bool set_present(Node &row, const NodeAddress &address, const std::string &field, bool present,
	                 std::string &error) override;
	// The vehicle spawn registry: the same IDs in the same slots.
	bool same_file_state(const FileState *a, const FileState *b) const override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id,
	                                const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	bool set_field(Node &row, const NodeAddress &address, const std::string &field, const Value &value,
	               std::string &error) override;
	bool edit_collection(Node &row, const Edit &edit, const IdAllocator &allocate, NodeId &added,
	                     std::string &error) override;
	bool set_file_value(std::shared_ptr<const FileState> &state, const Edit &edit, Diagnostic &error) override;
	std::shared_ptr<const FileState> state_after_remove(const std::shared_ptr<const FileState> &state,
	                                                    size_t remaining) const override;
	void after_edit(Node &row) override;
};

bool is_catalog_kind(AssetKind kind);

} // namespace opennova::editor
