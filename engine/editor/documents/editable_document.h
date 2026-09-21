#pragma once

#include <editor/assets/asset_kind.h>
#include <editor/model/diagnostic.h>
#include <formats/def/def_write.h>

#include <memory>
#include <variant>

namespace opennova::editor {

using CatalogId = uint64_t;
struct CatalogAddress {
	CatalogId row = 0;
	def::DefRecordKind kind = def::DefRecordKind::Item;
	CatalogId child = 0;
};
enum class CatalogOperation { Set, Add, Duplicate, Remove, Move, SetSpawnId };
struct CatalogEdit {
	CatalogOperation operation = CatalogOperation::Set;
	CatalogAddress address;
	std::string field;
	def::DefValue value = int64_t(0);
	size_t position = SIZE_MAX;
	bool coalesce = false;
};

// The format records themselves, with ownership for their C arrays and identities
// for selection/history. Copies clone just this row, including its nested records.
struct CatalogRow {
	CatalogId id = 0;
	def::DefRecordKind kind;
	std::variant<def::DefItemDef, def::DefWeaponDef, def::DefAmmoDef, def::DefAmmoClassCarry> data;
	std::vector<CatalogId> children;
	std::vector<CatalogId> sights;
	explicit CatalogRow(def::DefRecordKind kind);
	CatalogRow(const CatalogRow &);
	CatalogRow &operator=(const CatalogRow &) = delete;
	~CatalogRow();
	void *record();
	const void *record() const;
	std::string name() const;
};

class EditableDocument {
public:
	bool load(const std::string &absolute_path, const std::string &relative_path,
	          AssetKind kind, const std::string &game, Diagnostic &error);
	bool save(Diagnostic &error);
	bool apply(const CatalogEdit &edit, Diagnostic &error);
	void undo();
	void redo();
	void end_edit_group() { coalesce_key_.clear(); }

	const std::string &path() const { return relative_path_; }
	AssetKind kind() const { return kind_; }
	def::DefRecordKind record_kind() const;
	bool dirty() const { return revision_ != saved_revision_; }
	bool can_undo() const { return cursor_ != 0; }
	bool can_redo() const { return cursor_ < history_.size(); }
	bool blocked() const { return !parse_issues_.empty(); }
	uint64_t revision() const { return revision_; }
	CatalogId last_added() const { return last_added_; }
	const std::vector<std::shared_ptr<const CatalogRow>> &rows() const { return rows_; }
	const std::vector<int> &spawn_ids() const { return spawn_ids_; }
	const def::DefParseReport &parse_issues() const { return parse_issues_; }
	const void *record(const CatalogAddress &address) const;
	def::DefWriteResult serialize() const;

private:
	struct Change {
		std::shared_ptr<const CatalogRow> before, after;
		size_t before_position = 0, after_position = 0;
		std::vector<int> before_spawn, after_spawn;
		uint64_t before_revision = 0, after_revision = 0;
	};
	void restore(const Change &change, bool forward);
	void commit(Change change, const std::string &coalesce_key);
	size_t row_index(CatalogId id) const;
	void identify_children(CatalogRow &row);
	std::string absolute_path_, relative_path_;
	AssetKind kind_ = AssetKind::Unknown;
	std::vector<std::shared_ptr<const CatalogRow>> rows_;
	std::vector<int> spawn_ids_;
	def::DefParseReport parse_issues_;
	uint64_t file_fingerprint_ = 0, revision_ = 0, saved_revision_ = 0, next_revision_ = 1;
	CatalogId next_id_ = 1, last_added_ = 0;
	std::vector<Change> history_;
	size_t cursor_ = 0;
	std::string coalesce_key_;
};

bool is_catalog_kind(AssetKind kind);
} // namespace opennova::editor
