#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/edit.h>
#include <editor/model/node.h>
#include <editor/model/table_shape.h>
#include <formats/def/def.h>
#include <formats/def/def_notes.h>
#include <formats/def/def_schema.h>
#include <formats/def/def_write.h>

namespace opennova::editor {

// The def catalogs' table (ADR 0046 S5; S13 D10 made it rows of the one table shape,
// model/table_shape.h): every family of `.def` the catalog document opens (items.def, weapon.def,
// ammo.def, powerup.def) is a family row, every kind of record they hold a kind row, every list a
// record holds (a C array it owns, a powerup's action block) a list row, and every member of a record
// a labelled field projected from the format's own member inventory and line table
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

// How a copy of a row is named apart from its original: the words " (copy)" after a name that is words
// (an item's display name), "_2" after one a file names it by as a token (a weapon's, an ammo's).
enum class CopyName { None, Words, Token };

// What a catalog kind is beyond its table row: its record kind, what the core calls it, the field
// that names one of its records ("" none), and the rules a kind of its own keeps: what a new row of it
// takes beside its defaults and its name (`made`, over the records of its kind beside it), what its
// record derives after any edit (`after_edit`), what a copy a Duplicate makes takes beside a name of
// its own (`duplicated`: an item's id), how that name is made (`copy_name`), and how many characters of
// a name the game keeps (`name_chars`; 0: the field's width; `name_chars_of`, where it is the record's:
// a weapon's), which the copy's name keeps within. A new row's identity and a copy's keep clear of
// `taken` too: the ids other files of the project name.
struct CatalogKindRow {
	CatalogKind kind;
	def::DefRecordKind record;
	const char *token;
	const char *label;
	const char *add_label; // the outline's tool adding a row of it ("" = none)
	bool top;              // a row of its file
	const char *name_field;
	void (*made)(void *record, const std::vector<const void *> &others, const std::vector<int64_t> &taken) = nullptr;
	void (*after_edit)(void *record) = nullptr;
	void (*duplicated)(void *record, const std::vector<const void *> &others, const std::vector<int64_t> &taken) = nullptr;
	CopyName copy_name = CopyName::None;
	size_t name_chars = 0;
	size_t (*name_chars_of)(const void *record) = nullptr;
};
// The name a copy of a row named `name` takes, none of `taken` (upper case: the game's lookups compare
// names without case) and within `limit` characters (0: none): `name (copy)`, `name (copy 2)`, ... for
// words, `name_2`, `name_3`, ... for a token, the name cut short to leave the suffix room.
std::string copy_name(const std::string &name, CopyName how, size_t limit, const std::vector<std::string> &taken);
const CatalogKindRow &catalog_kind_row(NodeKind kind);
// The name a copy of a row of `kind` named `name` takes beside the names of its kind `taken` (upper
// case): copy_name by its kind's rule, within the characters of a name the game keeps (its row's
// name_chars_of of `record`, the row's own record, else its name_chars, else its name field's width); ""
// for a kind a copy keeps the name of. A Duplicate's copy and a repeated name's fix (catalog.name_duplicate,
// DI-11).
std::string catalog_copy_name(NodeKind kind, const std::string &name, const std::vector<std::string> &taken,
                              const void *record);
// The characters of a weapon's name the game keeps, of a weapon row's record (a def::DefWeaponDef): 32 in a
// block with no `sameas`, 31 in one with it (def::def_weapon_name_chars, the reader's rule the def writer
// caps a name by; D-ITEMDEF-10). The weapon row's name_chars_of.
size_t weapon_name_chars(const void *record);
// The first items.def id from 100000 on that `used` does not hold and the engine keeps for no use
// (def::reserved_item_by_id: a start, a waypoint, a flag, a model it draws; itemdef-re.md, "The ids
// and rows the engine fixes"): a new item's, a copy's, a Use fix's. An item's id is its type_id, which
// a mission names it by.
int free_item_id(const std::function<bool(int)> &used);
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

// What every catalog keeps of its file beside its rows: the indentation it was read with
// (def::DefLayout) and its modeled layout (def::DefTextNotes: each line's blanks, separators, comment and
// ending, the shape of a record's line against the writer's words, the tokens the game skips), which its
// writer generates the file from (each row names its lines by its record's note), so a file read and
// saved again is the file as it was, and one field changed changes that one line. Never shown or edited:
// it is carried for the save alone. The notes are made once, as the file is read, and shared by
// every state of the document: what an undo step holds of a state is not they (the document holds them
// whatever its history holds), so a state's footprint leaves them out (the review: a spawn-slot edit of
// jox01's items.def counted its 4.7 MB of notes twice against the history's budget).
struct CatalogFileState : FileState {
	def::DefLayout layout{};
	std::shared_ptr<const def::DefTextNotes> notes;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<CatalogFileState>(*this); }
	size_t footprint() const override { return sizeof(CatalogFileState); }
};

// items.def's file-wide state beside its layout: the vehicle spawn registry, whose family row says how
// a set and a comparison go.
struct ItemsFileState : CatalogFileState {
	std::vector<int> spawn_ids;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<ItemsFileState>(*this); }
	size_t footprint() const override { return CatalogFileState::footprint() + sizeof(ItemsFileState) -
		                                       sizeof(CatalogFileState) + footprint_of(spawn_ids); }
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
