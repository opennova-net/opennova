#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/edit.h>
#include <editor/model/node.h>
#include <editor/model/table_shape.h>
#include <formats/def/def.h>
#include <formats/def/def_schema.h>
#include <formats/def/def_write.h>

namespace opennova::editor {

// The def catalogs' table (ADR 0046 S5; S13 D10 made it rows of the one table shape,
// model/table_shape.h): every family of `.def` the catalog document opens (items.def, weapon.def,
// ammo.def, powerup.def) is a family row, every kind of record they hold a kind row, and every member
// of a record a labelled field projected from the format's own member inventory and line table
// (formats/def/def_schema.h), its value read and written through the member it is, in the units its
// line writes it. A new family of the catalog is rows here and in the format's tables (its member
// inventory, its lines, its rules), its parser and its writer, and its asset kind's row naming the
// catalog: no document and no branch of one.

// The catalog's kinds: every DefRecordKind at its own place (an item 0, a weapon 1, ..., a powerup's
// ammo row 9), then a powerup's two action blocks, each a kind of its own over the action's members
// (pickup 10, respawn 11): a row's two blocks are two lists, which an Add tells apart by the kind.
enum class CatalogKind : NodeKind {
	Item, Weapon, Ammo, Action, Sight, Attachment, Effect, Carry, Powerup, PowerupAmmo, Pickup, Respawn, kCount,
};
inline constexpr size_t kCatalogKindCount = size_t(CatalogKind::kCount);
constexpr NodeKind node_kind(CatalogKind kind) { return static_cast<NodeKind>(kind); }
constexpr NodeKind node_kind(def::DefRecordKind kind) { return static_cast<NodeKind>(kind); }
// The record kind whose members a catalog kind's records are (both action blocks: PowerupAction).
def::DefRecordKind def_kind(NodeKind kind);

// What a catalog kind is beyond its table row: its record kind, what the core calls it, the field
// that names one of its records ("" none), and the rules a kind of its own keeps: what a new row of it
// takes beside its defaults and its name (`made`, over the records of its kind beside it), and what
// its record derives after any edit (`after_edit`).
struct CatalogKindRow {
	CatalogKind kind;
	def::DefRecordKind record;
	const char *token;
	const char *label;
	const char *add_label; // the outline's tool adding a row of it ("" = none)
	bool top;              // a row of its file
	const char *name_field;
	void (*made)(void *record, const std::vector<const void *> &others) = nullptr;
	void (*after_edit)(void *record) = nullptr;
};
const CatalogKindRow &catalog_kind_row(NodeKind kind);
// The field that names a record of a kind: an item's display_name, a weapon's weapon_name, an ammo's
// and a carry limit's name, a powerup's name ("" for a kind no name names).
const char *catalog_name_field(NodeKind kind);

const RecordTable &catalog_table();

// The native record of a row: storage of its record's size, holding the arrays its kind's lists are
// (an item's attachments, a weapon's actions and sights, an ammo's effects, a powerup's ammo rows),
// which a copy of the row copies and the row frees. A row of any family, by its kind row alone.
class CatalogRecord {
public:
	explicit CatalogRecord(def::DefRecordKind kind);
	// The family parser's record, its arrays taken over (the parser's outer array is the caller's).
	CatalogRecord(def::DefRecordKind kind, const void *parsed);
	CatalogRecord(const CatalogRecord &other);
	// The record and its arrays taken over; the record moved from holds nothing.
	CatalogRecord(CatalogRecord &&other) noexcept;
	CatalogRecord &operator=(const CatalogRecord &) = delete;
	~CatalogRecord();
	def::DefRecordKind kind() const { return kind_; }
	void *data() { return storage_.data(); }
	const void *data() const { return storage_.data(); }
	template <class T> T &as() { return *static_cast<T *>(data()); }
	template <class T> const T &as() const { return *static_cast<const T *>(data()); }
	// The record and the arrays it owns.
	size_t footprint() const;

private:
	def::DefRecordKind kind_;
	std::vector<uint64_t> storage_;
};

// One file-wide state a family keeps beside its rows (items.def's vehicle spawn registry), whose
// family row says how a set and a comparison go.
struct ItemsFileState : FileState {
	std::vector<int> spawn_ids;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<ItemsFileState>(*this); }
	size_t footprint() const override { return sizeof(ItemsFileState) + footprint_of(spawn_ids); }
};

// A family of `.def` the catalog opens: its asset kind, the kinds its files hold (a bit per catalog
// kind), the top kind an Add makes by default, how a file reads into rows of records (each with its
// kind) and its file-wide state, and how rows and state write back through the family's writer.
struct CatalogFamily {
	AssetKind asset = AssetKind::Unknown;
	uint32_t kinds = 0;
	CatalogKind first = CatalogKind::Item;
	// The rows a file's bytes read into (each a record of a top kind, in the file's order), the
	// file-wide state, and the parse's findings.
	bool (*parse)(const std::vector<uint8_t> &bytes, std::vector<CatalogRecord> &rows,
	              std::shared_ptr<const FileState> &state, def::DefParseReport &report) = nullptr;
	// The rows and the state as the family's writer writes them.
	def::DefWriteResult (*write)(const std::vector<const CatalogRecord *> &rows, const FileState *state) = nullptr;
	// The family's file-wide values, where it has them: a value set (Edit SetFileValue; null: the
	// family has none), whether two states write the same (null: the same state), and the state once
	// a Remove leaves `remaining` rows (null: as it is).
	bool (*set_file_value)(std::shared_ptr<const FileState> &state, const Edit &edit, std::string &error) = nullptr;
	bool (*same_state)(const FileState *a, const FileState *b) = nullptr;
	std::shared_ptr<const FileState> (*after_remove)(const std::shared_ptr<const FileState> &state,
	                                                 size_t remaining) = nullptr;
	constexpr bool holds(NodeKind kind) const { return kind >= 0 && kind < 32 && (kinds & (uint32_t(1) << kind)) != 0; }
};
// The family that opens an asset kind (null for one the catalog does not open).
const CatalogFamily *catalog_family(AssetKind kind);

} // namespace opennova::editor
