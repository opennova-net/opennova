#pragma once

// The products' rows (backend/migrations/0001_baseline.sql and 0002_players_projects.sql, ADR
// 0064), each named by its key: the
// games (kind game, a base game, or one expansion of a game; each with its project's visibility
// and head, project_repository.h, and the account it is the own project of, if any), OpenNova's
// apps (kind app: no game tree and no project; products_app_check), their trees (each an
// immutable version of a game's files in the object store, core/object_store.h, built from its
// published release), the update releases (1SFS packages) and the sections each carries, and the
// trees each hosted server is pinned to. The routes, the supervisor, the worker's jobs and the
// update listener share it; every write that spans statements is one db::Transaction, and each
// state move a compare-and-set, so two writers never both win. Crow-free. Each function throws
// db::Error on a database failure. Service policy, not a port.

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

enum class ProductKind : uint8_t { Game, Expansion, App };
const char *product_kind_name(ProductKind kind); // "game", "expansion", "app" (no hosted tree)
std::optional<ProductKind> product_kind_from_name(std::string_view name);

// A products row.
struct ProductRow {
	int64_t id = 0;
	// Every product's: what the routes, its project and its download name it by ("JO",
	// "ONEDITOR").
	std::string key;
	std::string display_name;
	ProductKind kind = ProductKind::Game;
	std::optional<int64_t> parent_id; // an expansion's base game
	std::string expansion_code; // an expansion's /exp code: "jox01"
	std::string title; // a game's gameprofile code: "jo", "dfx2"
	std::string lobby; // a game's games slug (its lobby; "" for none)
	std::string updater_login; // "" when the update service does not serve it
	std::string command_file;
	// The login its stock UPDATE.WIZ names before the territory ("JOTRqq"; "" when it is
	// updater_login itself).
	std::string wizard_login;
	// A retail game (NovaLogic's install): its updater's SERVER.LST names ftp.novaworld.com, and
	// its project's revision 1 is the stock install.
	bool retail = false;
	// Its project (project_repository.h), a game's and an expansion's alone: who may read it
	// ("public", "private"; "" for an app) and its newest revision (0 before the first push).
	std::string visibility;
	int64_t head = 0;
	// The account whose own project it is (a game or an expansion an account made: never an app,
	// never a retail game) and that account's username; none for the service's own.
	std::optional<int64_t> owner_id;
	std::string owner;
	// The updater product as its packages name it (the command file's PRODUCT) and the executable
	// its DONE runs ("" for an expansion): what a release build packs (release_build.h).
	std::string product_name;
	std::string product_exe;
	// Its file's stable name on the downloads host ("" for none) and the words beside it.
	std::string download;
	std::string description;
	std::optional<int64_t> current_tree_id;
	std::optional<int64_t> published_release_id;
	std::string created_by;
	std::string created_at;
	std::string updated_at;
};

enum class TreeState : uint8_t { Building, Ready, Failed, Pruned };
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

// A product_trees row; its manifest and missions decoded.
struct ProductTree {
	int64_t id = 0;
	int64_t product_id = 0;
	TreeState state = TreeState::Building;
	std::optional<int64_t> release_id; // the release it was built from, while that is there
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

// Building: a release the data program builds from a revision of its game's project
// (release_build.h) before it validates it as an uploaded one. AwaitingApproval: a draft of an
// account's own project its owner published, which waits for an admin's approval (ADR 0074);
// nothing serves, applies or lists it.
enum class ReleaseStatus : uint8_t {
	Uploading,
	Validating,
	Invalid,
	Draft,
	Published,
	Retired,
	Building,
	AwaitingApproval,
};
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
	// The game it is a release of: a built one's from the start, an upload's once its validation
	// mapped its base section.
	std::optional<int64_t> product_id;
	// A built release's source (ADR 0069 d7): the revision of its game's project built (0 for an
	// upload) and, for an expansion's, its base game's; the version its package carries; what the
	// build's tools said.
	int64_t revision = 0;
	int64_t base_revision = 0;
	std::string version;
	std::string build_log;
	// Why an admin sent it back to draft from awaiting its approval ("" for none).
	std::string review_note;
};

struct ReleaseSection {
	int64_t release_id = 0;
	int64_t product_id = 0;
	std::string expansion; // the PACKAGEBREAK name, "" for the base section
	std::string command_file;
	std::string version;
	int64_t row_count = 0;
	int64_t blob_bytes = 0;
	std::optional<int64_t> tree_id; // the tree its publish built
};

// --- products ---

// An account's own projects (NewProduct::owner_id): at most kOwnedProjectsMax of them, and at most
// kOwnedBytesMax in their stores and their releases' packages together (project_repository.h
// owned_usage; the project routes refuse an upload or a release past it, whoever pushes it). The
// service's own take no limit, nor an admin's own (owned_limits_apply).
inline constexpr int64_t kOwnedProjectsMax = 3;
inline constexpr int64_t kOwnedBytesMax = int64_t(10) << 30;

// Whether the owned limits bind the account `owner_id`'s own projects: every account's but an
// admin's.
bool owned_limits_apply(db::Database &db, int64_t owner_id);

// What an account's own game gets from its key, in lower case (ADR 0074): the games row its first
// publish adds (publish_release: the lobby "<key>_0_consumer", its gate tag "<key>:0", its .gsb
// "<key>_0.gsb?a=1") and, a public one's, its download on the downloads host
// ("player-<key>-game-windows.zip", given at its create: no download of the service's own
// starts with "player-").
std::string own_lobby_slug(std::string_view key);
std::string own_gate_tag(std::string_view key);
std::string own_download(std::string_view key);

struct NewProduct {
	std::string display_name;
	ProductKind kind = ProductKind::Game;
	std::optional<int64_t> parent_id;
	std::string expansion_code;
	std::string title;
	std::string lobby; // a game's ("" for none: an account's own game, until its first publish)
	std::string updater_login;
	std::string command_file;
	bool retail = false;
	// Its file's name on the downloads host ("" for none; an account's own game's own_download).
	std::string download;
	// Its key and its project's visibility (an admin makes games and expansions alone, each with
	// its project).
	std::string key;
	std::string visibility;
	std::string created_by;
	// The account whose own project it is (a game or an expansion, never a retail one); none for
	// the service's own, an admin's.
	std::optional<int64_t> owner_id;
};

// What a product write came to.
enum class ProductOutcome : uint8_t {
	Ok,
	NotFound,
	DuplicateLogin, // another product holds the updater login (without case)
	DuplicateCommandFile, // another product holds the command file (without case)
	DuplicateDownload, // another product's download has the name
	DuplicateKey, // another product has the key
	DuplicateExpansion, // another expansion, of any game, has the code (without case)
	NoSuchParent, // the parent is not a game (kind game)
	InUse, // a live server runs it
	HasExpansion, // an expansion has it as parent
	TooManyProjects, // the owner has kOwnedProjectsMax projects already
};

// A new row; `id` its id on Ok. The shapes (the kind's fields, the code, the login, the key) are
// the routes' to check: here only the database's own rules answer, and for an owner's the count
// of its projects (its players row held while it is counted, so two creates never both pass it;
// an admin's own are not counted, owned_limits_apply).
ProductOutcome create_product(db::Database &db, const NewProduct &product, int64_t &id);

// What an admin may change afterwards.
struct ProductPatch {
	std::optional<std::string> display_name;
	std::optional<std::string> lobby; // a game's
	std::optional<std::string> updater_login; // with command_file, both or neither
	std::optional<std::string> command_file;
	std::optional<bool> retail;
	// Its key (a rename moves no object: the store names a project's files by the product's id,
	// project_repository.h) and its project's visibility (a game's or an expansion's).
	std::optional<std::string> key;
	std::optional<std::string> visibility;
};
ProductOutcome update_product(db::Database &db, int64_t id, const ProductPatch &patch);
// The row deleted (its trees' rows and its project's revisions and files cascade: their objects
// are the caller's to remove, the trees' prefixes and content/<id>/, product_jobs.h product.purge;
// an account's own game's own lobby, its games row, goes with it); InUse while a live server
// names it, HasExpansion while an expansion has it as parent.
ProductOutcome delete_product(db::Database &db, int64_t id);

std::optional<ProductRow> find_product(db::Database &db, int64_t id);
// Every row: games, expansions, then apps, each by id.
std::vector<ProductRow> list_products(db::Database &db);
// The expansions of the game `parent_id`, by id.
std::vector<ProductRow> list_expansions_of(db::Database &db, int64_t parent_id);
// The games and expansions (each with its project, project_repository.h), by key in byte order:
// every one when `all`, else the public ones and the account `reader`'s own.
std::vector<ProductRow> list_projects(db::Database &db, bool all, std::optional<int64_t> reader);
// The games and expansions the account `owner_id` owns, by key in byte order.
std::vector<ProductRow> list_owned_projects(db::Database &db, int64_t owner_id);
// The product whose key is `key`; none when no product's is.
std::optional<ProductRow> find_product_by_key(db::Database &db, std::string_view key);
// How many live servers run `id` as their product or expansion.
int count_live_servers_of(db::Database &db, int64_t id);

// A product a start may pick: a game with a ready current tree, each of its expansions
// with one beside it. The start form's list and check_settings' `offered`.
struct OfferedExpansion {
	int64_t id = 0;
	std::string code;
	std::string display_name;
	int64_t tree_id = 0;
	std::string version;
	std::vector<TreeMission> missions;
};
struct OfferedProduct {
	int64_t id = 0;
	std::string display_name;
	std::string title;
	std::string lobby;
	int64_t tree_id = 0;
	std::string version;
	std::vector<TreeMission> missions;
	std::vector<OfferedExpansion> expansions;
};
// What the account `caller` may start (none: what any account may): the service's own products,
// every account's public ones and the caller's own, each with a ready current tree (an expansion
// beside its game, which must be offered too).
std::vector<OfferedProduct> offered_products(db::Database &db, std::optional<int64_t> caller);
// Pure lookups over the list (inline: check_settings runs them without the database).
inline const OfferedProduct *find_offered(const std::vector<OfferedProduct> &offered, int64_t id) {
	for (const OfferedProduct &d : offered)
		if (d.id == id)
			return &d;
	return nullptr;
}
inline const OfferedExpansion *find_offered_expansion(const OfferedProduct &product, int64_t id) {
	for (const OfferedExpansion &e : product.expansions)
		if (e.id == id)
			return &e;
	return nullptr;
}

// --- trees ---

// A new tree row, building (the worker applies the release `release_id` to an empty tree); its
// id.
int64_t create_tree(db::Database &db, int64_t product_id, std::optional<int64_t> release_id,
		const std::string &created_by);
std::optional<ProductTree> find_tree(db::Database &db, int64_t id);
// The product's trees, newest first.
std::vector<ProductTree> list_trees(db::Database &db, int64_t product_id);
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
// building -> ready with `ready`, and in the same transaction the product's current tree
// becomes this one. False when the row left building.
bool finish_tree(db::Database &db, int64_t id, const TreeReady &ready);
// The trees whose objects may go: ready or failed ones older than `min_age_s` that are no product's
// current tree and no live server's pin, and every pruned one whose objects are still there
// (pruned_at NULL). Each is marked pruned by the caller once its objects are removed.
std::vector<int64_t> prunable_trees(db::Database &db, int64_t min_age_s);
// Marks `id` pruned: its state, and pruned_at now (its objects are gone).
void mark_tree_pruned(db::Database &db, int64_t id);
// How many live servers pin tree `id` (as base or expansion).
int count_tree_pins(db::Database &db, int64_t id);

// --- releases ---

// A release's version: one to four numbers between dots ("0.2.0", "1.7.5.8"), as a command
// file's VERSION reads.
bool valid_release_version(std::string_view version);

// An uploaded release (uploading): an app's build of version `version` when `product_id` names
// the app (the release routes'), else a package that names its product once validated.
int64_t create_release(db::Database &db, const std::string &name, int64_t bytes,
		const std::string &created_by, std::optional<int64_t> product_id = std::nullopt,
		const std::string &version = std::string());
// A release the data program builds (building), from `revision` of the project of the game
// `product_id` (an expansion's on `base_revision` of its base game's), its package's version
// `version`, named `name`. The caller enqueues its release.build in the same transaction. (An
// app's release is an upload, create_release, built once its build is in.)
struct NewBuiltRelease {
	std::string name;
	std::string created_by;
	int64_t product_id = 0;
	int64_t revision = 0;
	int64_t base_revision = 0; // 0 for a base game's
	std::string version;
};
int64_t create_built_release(db::Database &db, const NewBuiltRelease &release);
// building -> validating, its package uploaded (release_upload_key) and `build_log` kept; false
// when the row left building.
bool set_release_built(db::Database &db, int64_t id, const std::string &build_log);
// building -> invalid with `error` and `build_log`; false when the row left building.
bool set_release_build_failed(
		db::Database &db, int64_t id, const std::string &error, const std::string &build_log);
// building -> draft with the download's sha and size and no section: a download-only app's
// release, its build kept as uploaded (product_jobs.h release_download_key). False when the row
// left building.
bool set_release_stored(db::Database &db, int64_t id, const std::string &sha256, int64_t bytes);
std::optional<ReleaseRow> find_release(db::Database &db, int64_t id);
std::optional<int64_t> find_release_by_sha(db::Database &db, std::string_view sha256);
// Every release, newest first.
std::vector<ReleaseRow> list_releases(db::Database &db);
// The releases of the product `product_id` (ReleaseRow::product_id), newest first.
std::vector<ReleaseRow> list_releases_of(db::Database &db, int64_t product_id);
// How many releases of the account `owner_id`'s own projects are building or validating.
int64_t count_owned_releases_in_flight(db::Database &db, int64_t owner_id);
// One more release counted as started by the account `player_id` on this UTC day
// (release_starts), while it has started fewer than `max` that day; false, nothing counted, once
// it has. A release deleted since still counts.
bool take_release_start(db::Database &db, int64_t player_id, int64_t max);
std::vector<ReleaseSection> list_sections(db::Database &db, int64_t release_id);
// `from` -> `to` (with `error` for invalid); false when the row left `from`.
bool set_release_status(db::Database &db, int64_t id, ReleaseStatus from, ReleaseStatus to,
		const std::string &error = "");
struct NewReleaseSection {
	int64_t product_id = 0;
	std::string expansion;
	std::string command_file;
	std::string version;
	int64_t row_count = 0;
	int64_t blob_bytes = 0;
};
// validating -> draft with the package's sha, size and product and its sections (an upload that
// named no game becomes its first section's, the base one), in one transaction; false when the
// row left validating.
bool set_release_validated(db::Database &db, int64_t id, const std::string &sha256, int64_t bytes,
		const std::string &package_product, const std::vector<NewReleaseSection> &sections);
// draft -> published: each section's product's published_release_id becomes `id` (a release with
// no section, a download-only app's, its own product's), and a release no product names any
// more is retired. An account's own game with no lobby gets its own in the same transaction: a
// games row of own_lobby_slug (its lobby name too), gate tag own_gate_tag, .gsb
// "<key>_0.gsb?a=1", its name and title, its other columns its title's template row's (OP: Acid
// Gambit's for jo, Delta Force Xtreme 2's for dfx2, kOwnLobbyTemplates; none while that row is
// missing), and its products.lobby that slug. A release awaiting approval publishes as a draft
// does (an admin's approval). Its review note is cleared. False when the row is neither.
bool publish_release(db::Database &db, int64_t id);
// draft -> awaiting_approval, its review note cleared: an account's own project's release its
// owner published, nothing served until an admin publishes it. False when the row left draft.
bool submit_release(db::Database &db, int64_t id);
// awaiting_approval -> draft with `note` (an admin's reject: why). False when the row was not
// awaiting approval.
bool reject_release(db::Database &db, int64_t id, const std::string &note);
// A release awaiting an admin's approval, what the admin's attention list names: its name, its
// product's key and its owner's username ("" for none), and when it was made (Unix seconds).
struct AwaitingRelease {
	int64_t id = 0;
	std::string name;
	std::string key;
	std::string owner;
	int64_t created_at = 0;
};
// Every release awaiting approval, oldest first.
std::vector<AwaitingRelease> list_awaiting_releases(db::Database &db);
// published -> retired, its sections' products' published_release_id cleared. False when
// the row left published.
bool retire_release(db::Database &db, int64_t id);
// The row deleted with its sections (a tree built from it keeps its row, release_id cleared);
// its package object is the caller's to remove first. False when no row has the id.
bool delete_release(db::Database &db, int64_t id);
void set_section_tree(db::Database &db, int64_t release_id, int64_t product_id, int64_t tree_id);

// What the update listener serves for a product login: the published release's sha (its
// package's key), the section (by command file and expansion) and where SERVER.LST sends
// the client.
struct PublishedSection {
	int64_t product_id = 0;
	int64_t release_id = 0;
	std::string updater_login;
	std::string command_file;
	std::string expansion;
	std::string sha256;
	bool retail = false;
};
// Every product with a login and a published release, by login.
std::vector<PublishedSection> published_sections(db::Database &db);

// The games rows an account's own game's lobby copies (publish_release), by its title.
struct OwnLobbyTemplate {
	const char *title;
	const char *slug;
};
inline constexpr OwnLobbyTemplate kOwnLobbyTemplates[] = {
	{ "jo", "acidgambit_0_consumer" },
	{ "dfx2", "dfx2_consumer" },
};

// A mod a game's players may install (GET /api/mods): an expansion with an updater product and a
// published release.
struct ModRow {
	std::string key;
	std::string code; // its expansion code: its expansion/<code>/ folder
	std::string name;
	std::optional<std::string> owner; // its owner's username; none for the service's own
	std::string version; // its published release's (its section's VERSION when it names none)
	std::string updater_login;
	std::string command_file;
};
// The mods of the game whose command file is `game_command_file` (without case): the public ones
// and the account `reader`'s own, by key in byte order.
std::vector<ModRow> list_mods(
		db::Database &db, std::string_view game_command_file, std::optional<int64_t> reader);

// --- the hosted servers' pins ---

// The current trees of a base and (when set) an expansion: nullopt when either has no ready
// current tree.
struct CurrentTrees {
	int64_t base_tree_id = 0;
	std::optional<int64_t> expansion_tree_id;
};
std::optional<CurrentTrees> current_trees_of(db::Database &db, int64_t product_id,
		std::optional<int64_t> expansion_id);

// The manifest JSON a tree's files are stored as, and read back (`{"files": [{"name",
// "size"}]}`).
std::string encode_tree_files(const std::vector<TreeFile> &files);
bool decode_tree_files(std::string_view json, std::vector<TreeFile> &out);
std::string encode_tree_missions(const std::vector<TreeMission> &missions);
bool decode_tree_missions(std::string_view json, std::vector<TreeMission> &out);

} // namespace opennova::novaworld_server::hosted
