#pragma once

// The hosted data sets' rows (backend/migrations/0011_data_sets.sql, ADR 0064): the data sets
// an admin created (one product each: a base game, or one expansion of a base), their trees
// (each an immutable version of the data set's files in the object store, object_store.h),
// and the trees each hosted server is pinned to. The routes, the supervisor and the worker's
// jobs share it; every write that spans statements is one db::Transaction, and each state
// move a compare-and-set, so two writers never both win. Crow-free. Each function throws
// db::Error on a database failure. Service policy, not a port.

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

enum class DataSetKind : uint8_t { Base, Expansion };
const char *data_set_kind_name(DataSetKind kind); // "base", "expansion"
std::optional<DataSetKind> data_set_kind_from_name(std::string_view name);

// A data_sets row.
struct DataSetRow {
	int64_t id = 0;
	std::string display_name;
	DataSetKind kind = DataSetKind::Base;
	std::optional<int64_t> parent_id; // an expansion's base
	std::string expansion_code; // an expansion's /exp code: "jox01"
	std::string title; // a base's gameprofile code: "jo", "dfx2"
	std::string game; // a base's games slug (its lobby)
	std::optional<int64_t> current_tree_id;
	std::string created_by;
	std::string created_at;
	std::string updated_at;
};

enum class TreeState : uint8_t { Uploading, Finalizing, Ready, Failed, Pruned };
const char *tree_state_name(TreeState state);
std::optional<TreeState> tree_state_from_name(std::string_view name);

// One file of a tree, as its manifest lists it: what hostd checks a fetch by.
struct TreeFile {
	std::string name;
	uint64_t size = 0;
};
struct TreeMission {
	std::string file; // the .bms name
	std::string title;
};

// A data_set_trees row; its manifest and missions decoded.
struct DataSetTree {
	int64_t id = 0;
	int64_t data_set_id = 0;
	TreeState state = TreeState::Uploading;
	std::string version_text;
	std::vector<TreeFile> files;
	std::vector<TreeMission> missions;
	int64_t bytes = 0;
	std::string error;
	std::string created_by;
	std::string created_at;
	std::string ready_at;
	std::string pruned_at;
};

// --- data sets ---

struct NewDataSet {
	std::string display_name;
	DataSetKind kind = DataSetKind::Base;
	std::optional<int64_t> parent_id;
	std::string expansion_code;
	std::string title;
	std::string game;
	std::string created_by;
};

// What a data set write came to.
enum class DataSetOutcome : uint8_t {
	Ok,
	NotFound,
	DuplicateExpansion, // the parent has an expansion of that code
	NoSuchParent, // the parent is not a base data set
	InUse, // a live server runs it, or an expansion has it as parent
};

// A new row; `id` its id on Ok. The shapes (the kind's fields, the code) are the routes' to
// check: here only the database's own rules answer.
DataSetOutcome create_data_set(db::Database &db, const NewDataSet &data_set, int64_t &id);

// What an admin may change afterwards.
struct DataSetPatch {
	std::optional<std::string> display_name;
	std::optional<std::string> game; // a base's
};
DataSetOutcome update_data_set(db::Database &db, int64_t id, const DataSetPatch &patch);
// The row deleted (its trees' rows cascade: their objects are the caller's to remove first,
// tree.prune); InUse while a live server names it or an expansion has it as parent.
DataSetOutcome delete_data_set(db::Database &db, int64_t id);

std::optional<DataSetRow> find_data_set(db::Database &db, int64_t id);
// Every row, bases first, each by id.
std::vector<DataSetRow> list_data_sets(db::Database &db);
// The expansions of base `parent_id`, by id.
std::vector<DataSetRow> list_expansions_of(db::Database &db, int64_t parent_id);
// How many live servers run `id` as their data set or expansion.
int count_live_servers_of(db::Database &db, int64_t id);
// How many of them are behind its current tree (server_outdated's rule, over `id` alone).
int count_outdated_servers_of(db::Database &db, int64_t id);

// A data set a start may pick: a base with a ready current tree, each of its expansions
// with one beside it. The start form's list and check_settings' `offered`.
struct OfferedExpansion {
	int64_t id = 0;
	std::string code;
	std::string display_name;
	int64_t tree_id = 0;
	std::string version;
	std::vector<TreeMission> missions;
};
struct OfferedDataSet {
	int64_t id = 0;
	std::string display_name;
	std::string title;
	std::string game;
	int64_t tree_id = 0;
	std::string version;
	std::vector<TreeMission> missions;
	std::vector<OfferedExpansion> expansions;
};
std::vector<OfferedDataSet> offered_data_sets(db::Database &db);
// Pure lookups over the list (inline: check_settings runs them without the database).
inline const OfferedDataSet *find_offered(const std::vector<OfferedDataSet> &offered, int64_t id) {
	for (const OfferedDataSet &d : offered)
		if (d.id == id)
			return &d;
	return nullptr;
}
inline const OfferedExpansion *find_offered_expansion(const OfferedDataSet &data_set, int64_t id) {
	for (const OfferedExpansion &e : data_set.expansions)
		if (e.id == id)
			return &e;
	return nullptr;
}

// --- trees ---

// A new tree row in `state` (uploading while the browser's parts are in flight); its id.
int64_t create_tree(
		db::Database &db, int64_t data_set_id, TreeState state, const std::string &created_by);
std::optional<DataSetTree> find_tree(db::Database &db, int64_t id);
// The data set's trees, newest first.
std::vector<DataSetTree> list_trees(db::Database &db, int64_t data_set_id);
// `from` -> `to` with `error` (Failed) when the row is still in `from`; false otherwise.
bool set_tree_state(db::Database &db, int64_t id, TreeState from, TreeState to,
		const std::string &error = "");

// What a tree that is ready carries.
struct TreeReady {
	std::vector<TreeFile> files;
	std::vector<TreeMission> missions;
	std::string version_text;
	int64_t bytes = 0;
};
// `from` -> ready with `ready`, and in the same transaction the data set's current tree
// becomes this one. False when the row left `from`.
bool finish_tree(db::Database &db, int64_t id, TreeState from, const TreeReady &ready);
// The trees whose objects may go: ready or failed ones older than `min_age_s` that are no
// data set's current tree and no live server's pin, every pruned one whose objects are
// still there (pruned_at NULL), and uploading ones older than `abandoned_upload_s`. Each is
// marked pruned by the caller once its objects are removed.
std::vector<int64_t> prunable_trees(db::Database &db, int64_t min_age_s, int64_t abandoned_upload_s);
// Marks `id` pruned: its state, and pruned_at now (its objects are gone).
void mark_tree_pruned(db::Database &db, int64_t id);
// Marks `id` pruned with its objects still there (pruned_at NULL): the admin's delete of a
// ready or failed tree, whose objects the tree.prune job removes (prunable_trees lists it).
void request_tree_prune(db::Database &db, int64_t id);
// Why a ready tree cannot be deleted now: "current", "pinned" (with how many live servers),
// or "" when it can.
std::string tree_hold(db::Database &db, int64_t id, int &pinned_by);
// How many live servers pin tree `id` (as base or expansion).
int count_tree_pins(db::Database &db, int64_t id);

// --- the hosted servers' pins ---

// The current trees of a base and (when set) an expansion: nullopt when either has no ready
// current tree.
struct CurrentTrees {
	int64_t base_tree_id = 0;
	std::optional<int64_t> expansion_tree_id;
};
std::optional<CurrentTrees> current_trees_of(db::Database &db, int64_t data_set_id,
		std::optional<int64_t> expansion_id);
// The server's pins, and update_requested cleared.
void set_tree_pins(db::Database &db, int64_t server_id, int64_t base_tree_id,
		std::optional<int64_t> expansion_tree_id);
enum class UpdateOutcome : uint8_t { Requested, NotFound, NotLive, UpToDate };
// The owner's update: a live server whose pins differ from its data sets' current trees
// gets update_requested (idempotent: one already asked answers Requested).
UpdateOutcome request_update(db::Database &db, int64_t server_id);
// Every live server whose pins are behind its data sets' current trees (of data set
// `data_set_id`, as base or expansion, when set); how many were marked.
size_t request_update_all(db::Database &db, std::optional<int64_t> data_set_id);
// Whether a live server's pins are behind its data sets' current trees.
bool server_outdated(db::Database &db, int64_t server_id);

// The manifest JSON a tree's files are stored as, and read back (`{"files": [{"name",
// "size"}]}`).
std::string encode_tree_files(const std::vector<TreeFile> &files);
bool decode_tree_files(std::string_view json, std::vector<TreeFile> &out);
std::string encode_tree_missions(const std::vector<TreeMission> &missions);
bool decode_tree_missions(std::string_view json, std::vector<TreeMission> &out);

} // namespace opennova::novaworld_server::hosted
