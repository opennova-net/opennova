#pragma once

// The hosted data sets' rows (backend/migrations/0011_data_sets.sql and 0013_releases.sql,
// ADR 0064): the data sets an admin created (one product each: a base game, or one expansion
// of a base), their trees (each an immutable version of the data set's files in the object
// store, core/object_store.h), the update releases (1SFS packages) and the sections each carries,
// and the trees each hosted server is pinned to. The routes, the supervisor, the worker's
// jobs and the update listener share it; every write that spans statements is one
// db::Transaction, and each state move a compare-and-set, so two writers never both win.
// Crow-free. Each function throws db::Error on a database failure. Service policy, not a
// port.

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
	std::string updater_login; // "" when the update service does not serve it
	std::string command_file;
	bool retail_updater = false;
	std::optional<int64_t> current_tree_id;
	std::optional<int64_t> baseline_tree_id;
	std::optional<int64_t> published_release_id;
	std::string created_by;
	std::string created_at;
	std::string updated_at;
};

enum class TreeState : uint8_t { Uploading, Finalizing, Building, Ready, Failed, Pruned };
const char *tree_state_name(TreeState state);
std::optional<TreeState> tree_state_from_name(std::string_view name);
enum class TreeSource : uint8_t { Baseline, Release };
const char *tree_source_name(TreeSource source);

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
	TreeSource source = TreeSource::Baseline;
	std::optional<int64_t> release_id;
	std::optional<int64_t> baseline_tree_id;
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

enum class ReleaseStatus : uint8_t { Uploading, Validating, Invalid, Draft, Published, Retired };
const char *release_status_name(ReleaseStatus status);
std::optional<ReleaseStatus> release_status_from_name(std::string_view name);

struct ReleaseRow {
	int64_t id = 0;
	ReleaseStatus status = ReleaseStatus::Uploading;
	std::string sha256;
	int64_t bytes = 0;
	std::string package_product;
	std::string name;
	std::string error;
	std::string created_by;
	std::string created_at;
	std::string published_at;
	std::string retired_at;
};

struct ReleaseSection {
	int64_t release_id = 0;
	int64_t data_set_id = 0;
	std::string expansion; // the PACKAGEBREAK name, "" for the base section
	std::string command_file;
	std::string version;
	int64_t row_count = 0;
	int64_t blob_bytes = 0;
	std::optional<int64_t> tree_id; // the tree its publish built
};

// --- data sets ---

struct NewDataSet {
	std::string display_name;
	DataSetKind kind = DataSetKind::Base;
	std::optional<int64_t> parent_id;
	std::string expansion_code;
	std::string title;
	std::string game;
	std::string updater_login;
	std::string command_file;
	bool retail_updater = false;
	std::string created_by;
};

// What a data set write came to.
enum class DataSetOutcome : uint8_t {
	Ok,
	NotFound,
	DuplicateLogin, // another data set holds the updater login (without case)
	DuplicateExpansion, // the parent has an expansion of that code
	NoSuchParent, // the parent is not a base data set
	InUse, // a live server runs it, or an expansion has it as parent
};

// A new row; `id` its id on Ok. The shapes (the kind's fields, the code, the login) are the
// routes' to check: here only the database's own rules answer.
DataSetOutcome create_data_set(db::Database &db, const NewDataSet &data_set, int64_t &id);

// What an admin may change afterwards.
struct DataSetPatch {
	std::optional<std::string> display_name;
	std::optional<std::string> game; // a base's
	std::optional<std::string> updater_login; // with command_file, both or neither
	std::optional<std::string> command_file;
	std::optional<bool> retail_updater;
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

// A new tree row in `state` (uploading for a baseline the browser uploads, building for a
// release the worker applies); its id.
int64_t create_tree(db::Database &db, int64_t data_set_id, TreeSource source, TreeState state,
		std::optional<int64_t> release_id, std::optional<int64_t> baseline_tree_id,
		const std::string &created_by);
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
// becomes this one; a baseline also becomes its baseline tree and retires the data set's
// published release (its sections no longer describe the baseline: published_release_id
// cleared, the release retired once no data set names it). False when the row left `from`.
bool finish_tree(db::Database &db, int64_t id, TreeState from, const TreeReady &ready);
// The trees whose objects may go: ready or failed ones older than `min_age_s` that are no
// data set's current or baseline tree and no live server's pin, every pruned one whose
// objects are still there (pruned_at NULL), and uploading ones older than
// `abandoned_upload_s`. Each is marked pruned by the caller once its objects are removed.
std::vector<int64_t> prunable_trees(db::Database &db, int64_t min_age_s, int64_t abandoned_upload_s);
// Marks `id` pruned: its state, and pruned_at now (its objects are gone).
void mark_tree_pruned(db::Database &db, int64_t id);
// Marks `id` pruned with its objects still there (pruned_at NULL): the admin's delete of a
// ready or failed tree, whose objects the tree.prune job removes (prunable_trees lists it).
void request_tree_prune(db::Database &db, int64_t id);
// Why a ready tree cannot be deleted now: "current", "baseline", "pinned" (with how many live
// servers), or "" when it can.
std::string tree_hold(db::Database &db, int64_t id, int &pinned_by);
// How many live servers pin tree `id` (as base or expansion).
int count_tree_pins(db::Database &db, int64_t id);

// --- releases ---

int64_t create_release(db::Database &db, const std::string &name, int64_t bytes,
		const std::string &created_by);
std::optional<ReleaseRow> find_release(db::Database &db, int64_t id);
std::optional<int64_t> find_release_by_sha(db::Database &db, std::string_view sha256);
// Every release, newest first.
std::vector<ReleaseRow> list_releases(db::Database &db);
std::vector<ReleaseSection> list_sections(db::Database &db, int64_t release_id);
// `from` -> `to` (with `error` for invalid); false when the row left `from`.
bool set_release_status(db::Database &db, int64_t id, ReleaseStatus from, ReleaseStatus to,
		const std::string &error = "");
struct NewReleaseSection {
	int64_t data_set_id = 0;
	std::string expansion;
	std::string command_file;
	std::string version;
	int64_t row_count = 0;
	int64_t blob_bytes = 0;
};
// validating -> draft with the package's sha, size and product and its sections, in one
// transaction; false when the row left validating.
bool set_release_validated(db::Database &db, int64_t id, const std::string &sha256, int64_t bytes,
		const std::string &package_product, const std::vector<NewReleaseSection> &sections);
// draft -> published: each section's data set's published_release_id becomes `id`, and a
// release no data set names any more is retired. False when the row left draft.
bool publish_release(db::Database &db, int64_t id);
// published -> retired, its sections' data sets' published_release_id cleared. False when
// the row left published.
bool retire_release(db::Database &db, int64_t id);
// The row deleted with its sections (a tree built from it keeps its row, release_id cleared);
// its package object is the caller's to remove first. False when no row has the id.
bool delete_release(db::Database &db, int64_t id);
void set_section_tree(db::Database &db, int64_t release_id, int64_t data_set_id, int64_t tree_id);

// What the update listener serves for a product login: the published release's sha (its
// package's key), the section (by command file and expansion) and where SERVER.LST sends
// the client.
struct PublishedSection {
	int64_t data_set_id = 0;
	int64_t release_id = 0;
	std::string updater_login;
	std::string command_file;
	std::string expansion;
	std::string sha256;
	bool retail_updater = false;
};
// Every data set with a login and a published release, by login.
std::vector<PublishedSection> published_sections(db::Database &db);

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
