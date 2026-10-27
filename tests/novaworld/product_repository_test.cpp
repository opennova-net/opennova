// The games' rows (apps/novaworld_common/products/product_repository.h) on a migrated
// database of the test's own: the seeded games (in their seeded order) with their projects' keys,
// a game created, changed and refused (a duplicate login, a duplicate key, a duplicate
// expansion, no base parent, an expansion building on it, in use); trees from building to ready
// (the product's current tree follows), what a start may pick and what may be pruned; releases
// of their games validated (an upload naming none takes its base section's), published (the
// previous one retired), retired, and a built one naming its game; the servers' pins, the
// owner's update and the admin's update-all; the products' holds on the servers' rows reading
// a server as live exactly when the hosted servers' own rule does.

#include "common/test_database.h"
#include "common/test_expect.h"

#include "hosted/model/hosted_repository.h"
#include "products/product_repository.h"
#include "products/tree_files.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::novaworld_server::hosted;
using opennova::db::BindValue;
using opennova::db::Database;
using test_db::TestDatabase;

namespace {

int64_t seeded(Database &db, const char *display_name) {
	const auto rows = db.query("SELECT id FROM products WHERE display_name = $1;", { BindValue(display_name) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

TreeReady ready_with(const std::string &version, std::vector<TreeMission> missions) {
	TreeReady r;
	r.files = { { "resource.pff", 1000 }, { "version.txt", 10 } };
	r.missions = std::move(missions);
	r.version_text = version;
	r.bytes = 1010;
	return r;
}

// A hosted server row as a start writes it (the columns the pins need).
int64_t insert_server(Database &db, int64_t product_id, std::optional<int64_t> expansion_id,
		const char *state = "running") {
	const auto rows = db.query(
			"INSERT INTO hosted_servers (state, game_name, max_players, time_limit, kill_limit, "
			"max_score, respawn, start_delay, missions, product_id, expansion_id) "
			"VALUES ($1, 'srv', 32, 30, 50, 5, 5, 0, 'a.bms', $2, $3) RETURNING id;",
			{ BindValue(state), BindValue(product_id),
					expansion_id ? BindValue(*expansion_id) : BindValue(std::monostate{}) });
	return rows.front().as_int(0).value_or(0);
}

int test_products() {
	TestDatabase tdb;
	Database db = tdb.open();
	// Three base games, their two expansions, and the standalone runtime updater.
	const std::vector<ProductRow> all = list_products(db);
	TEST_EXPECT(all.size() == 6);
	const int64_t jo = seeded(db, "Joint Operations"), jox = seeded(db, "Escalation");
	TEST_EXPECT(jo != 0 && jox != 0);
	// The baseline seeds them in one order, so each keeps its id across a reset (the store names
	// a project's files by its game's id: DEPLOY.md "Resetting the service database").
	TEST_EXPECT(jo == 1 && seeded(db, "Delta Force Xtreme 2") == 2 &&
			seeded(db, "OP: Acid Gambit") == 3 && jox == 4 && seeded(db, "OpenNova runtime") == 5 &&
			seeded(db, "OP: Nifty Package") == 6);
	const auto jo_row = find_product(db, jo);
	TEST_EXPECT(jo_row && jo_row->kind == ProductKind::Game && jo_row->title == "jo" &&
			jo_row->lobby == "jop_2_consumer" && jo_row->updater_login == "JOTRus" &&
			jo_row->command_file == "JOPTR.CMD" && jo_row->retail && !jo_row->current_tree_id &&
			jo_row->key == "JO" && jo_row->visibility == "private" && jo_row->head == 0);
	const auto jox_row = find_product(db, jox);
	TEST_EXPECT(jox_row && jox_row->kind == ProductKind::Expansion && jox_row->parent_id == jo &&
			jox_row->expansion_code == "jox01" && jox_row->updater_login == "JOEus" &&
			jox_row->key == "JOX01" && jox_row->retail);
	TEST_EXPECT(list_expansions_of(db, jo).size() == 1);
	TEST_EXPECT(all[0].kind == ProductKind::Game && all[3].kind == ProductKind::Expansion);
	const auto runtime = find_product(db, seeded(db, "OpenNova runtime"));
	TEST_EXPECT(runtime && runtime->kind == ProductKind::App && runtime->lobby.empty() &&
			runtime->title.empty() && !runtime->parent_id && !runtime->current_tree_id &&
			runtime->updater_login == "ONRUNTIME" && runtime->command_file == "ONRUNTIME.CMD" &&
			!runtime->retail && runtime->key.empty() && runtime->visibility.empty());
	const auto native = find_product(db, seeded(db, "OP: Acid Gambit"));
	TEST_EXPECT(native && native->updater_login == "ACIDGAMBIT" &&
			native->command_file == "ACIDGAMBIT.CMD" && !native->retail &&
			native->key == "ACIDGAMBIT" && native->visibility == "public");
	const auto dfx2 = find_product(db, seeded(db, "Delta Force Xtreme 2"));
	TEST_EXPECT(dfx2 && dfx2->updater_login == "DFX2" && dfx2->command_file == "DFX2.CMD" &&
			dfx2->retail && dfx2->key == "DFX2");
	const auto native_expansions = list_expansions_of(db, native->id);
	TEST_EXPECT(native_expansions.size() == 1 && native_expansions[0].expansion_code == "nifty" &&
			native_expansions[0].updater_login == "NIFTY" &&
			native_expansions[0].command_file == "NIFTY.CMD" && native_expansions[0].key == "NIFTY");
	TEST_EXPECT(tree_file_rules(ProductKind::App, "").empty());

	// A new base, a new expansion of it, and the refusals.
	NewProduct bhd;
	bhd.display_name = "Black Hawk Down";
	bhd.kind = ProductKind::Game;
	bhd.title = "bhd";
	bhd.lobby = "bhd_consumer";
	bhd.updater_login = "BHDus";
	bhd.command_file = "BHDPTR.CMD";
	bhd.key = "BHD";
	bhd.visibility = "private";
	bhd.created_by = "admin";
	int64_t bhd_id = 0;
	TEST_EXPECT(create_product(db, bhd, bhd_id) == ProductOutcome::Ok && bhd_id != 0);
	TEST_EXPECT(find_product(db, bhd_id)->key == "BHD" &&
			find_product(db, bhd_id)->visibility == "private" && find_product(db, bhd_id)->head == 0);
	NewProduct dup = bhd;
	dup.updater_login = "bhdus"; // the login is unique without case
	int64_t ignored = 0;
	TEST_EXPECT(create_product(db, dup, ignored) == ProductOutcome::DuplicateLogin);
	dup.updater_login = "BHD2us";
	TEST_EXPECT(create_product(db, dup, ignored) == ProductOutcome::DuplicateKey);
	NewProduct ts;
	ts.display_name = "Team Sabre";
	ts.kind = ProductKind::Expansion;
	ts.parent_id = bhd_id;
	ts.expansion_code = "bhx01";
	int64_t ts_id = 0;
	TEST_EXPECT(create_product(db, ts, ts_id) == ProductOutcome::Ok);
	TEST_EXPECT(create_product(db, ts, ignored) == ProductOutcome::DuplicateExpansion);
	NewProduct orphan = ts;
	orphan.expansion_code = "bhx02";
	orphan.parent_id = ts_id; // an expansion is no parent
	TEST_EXPECT(create_product(db, orphan, ignored) == ProductOutcome::NoSuchParent);
	orphan.parent_id = 99999;
	TEST_EXPECT(create_product(db, orphan, ignored) == ProductOutcome::NoSuchParent);

	ProductPatch patch;
	patch.display_name = "BHD";
	patch.retail = true;
	TEST_EXPECT(update_product(db, bhd_id, patch) == ProductOutcome::Ok);
	TEST_EXPECT(find_product(db, bhd_id)->display_name == "BHD" && find_product(db, bhd_id)->retail);
	ProductPatch steal;
	steal.updater_login = "JOTRUS";
	TEST_EXPECT(update_product(db, bhd_id, steal) == ProductOutcome::DuplicateLogin);
	// Its project renamed and made public; a key another game's project has refused.
	ProductPatch project;
	project.key = "BHAWK";
	project.visibility = "public";
	TEST_EXPECT(update_product(db, bhd_id, project) == ProductOutcome::Ok);
	TEST_EXPECT(find_product(db, bhd_id)->key == "BHAWK" &&
			find_product(db, bhd_id)->visibility == "public");
	project = {};
	project.key = "JO";
	TEST_EXPECT(update_product(db, bhd_id, project) == ProductOutcome::DuplicateKey);
	ProductPatch moved;
	moved.lobby = "acidgambit_0_consumer";
	TEST_EXPECT(update_product(db, bhd_id, moved) == ProductOutcome::Ok);
	TEST_EXPECT(find_product(db, bhd_id)->lobby == "acidgambit_0_consumer");
	// An expansion lists in its parent's lobby: a game patched onto it changes nothing.
	TEST_EXPECT(update_product(db, ts_id, moved) == ProductOutcome::Ok);
	TEST_EXPECT(find_product(db, ts_id)->lobby.empty());
	TEST_EXPECT(update_product(db, 99999, patch) == ProductOutcome::NotFound);

	// A base with an expansion cannot go; the expansion can; then the base.
	TEST_EXPECT(delete_product(db, bhd_id) == ProductOutcome::HasExpansion);
	TEST_EXPECT(delete_product(db, ts_id) == ProductOutcome::Ok);
	TEST_EXPECT(delete_product(db, bhd_id) == ProductOutcome::Ok);
	TEST_EXPECT(delete_product(db, bhd_id) == ProductOutcome::NotFound);
	// One a live server runs cannot go.
	insert_server(db, jo, std::nullopt);
	TEST_EXPECT(count_live_servers_of(db, jo) == 1);
	TEST_EXPECT(delete_product(db, jo) == ProductOutcome::InUse);
	return 0;
}

int test_trees_and_offered() {
	TestDatabase tdb;
	Database db = tdb.open();
	const int64_t jo = seeded(db, "Joint Operations"), jox = seeded(db, "Escalation");
	const int64_t dfx2 = seeded(db, "Delta Force Xtreme 2");
	TEST_EXPECT(offered_products(db).empty());

	// A release's tree: building -> ready; the product's current tree is then this one, and the
	// base is offered with its missions.
	const int64_t t1 = create_tree(db, jo, std::nullopt, "worker");
	TEST_EXPECT(t1 != 0);
	const auto fresh = find_tree(db, t1);
	TEST_EXPECT(fresh && fresh->state == TreeState::Building && !fresh->release_id &&
			fresh->files.empty() && fresh->missions.empty() && fresh->created_by == "worker");
	TEST_EXPECT(offered_products(db).empty() && !find_product(db, jo)->current_tree_id);
	TEST_EXPECT(finish_tree(db, t1,
			ready_with("JO V1.7.5.3", { { "a.bms", "Alpha" }, { "b.bms", "Bravo" } })));
	TEST_EXPECT(!finish_tree(db, t1, ready_with("again", {}))); // ready, no longer building
	const auto ready = find_tree(db, t1);
	TEST_EXPECT(ready && ready->state == TreeState::Ready && ready->files.size() == 2 &&
			ready->files[0].name == "resource.pff" && ready->files[0].size == 1000 &&
			ready->missions.size() == 2 && ready->missions[1].title == "Bravo" &&
			ready->version_text == "JO V1.7.5.3" && ready->bytes == 1010 && !ready->ready_at.empty());
	const auto jo_row = find_product(db, jo);
	TEST_EXPECT(jo_row && jo_row->current_tree_id == t1);
	std::vector<OfferedProduct> offered = offered_products(db);
	TEST_EXPECT(offered.size() == 1 && offered[0].id == jo && offered[0].tree_id == t1 &&
			offered[0].title == "jo" && offered[0].missions.size() == 2 && offered[0].expansions.empty());
	TEST_EXPECT(find_offered(offered, jo) != nullptr && find_offered(offered, dfx2) == nullptr);

	// The expansion's tree: offered under its base once ready.
	const int64_t tx = create_tree(db, jox, std::nullopt, "worker");
	TEST_EXPECT(finish_tree(db, tx, ready_with("JOX V1.0", { { "x.bms", "Xray" } })));
	offered = offered_products(db);
	TEST_EXPECT(offered.size() == 1 && offered[0].expansions.size() == 1 &&
			offered[0].expansions[0].id == jox && offered[0].expansions[0].code == "jox01" &&
			offered[0].expansions[0].tree_id == tx && offered[0].expansions[0].missions.size() == 1);
	TEST_EXPECT(find_offered_expansion(offered[0], jox) != nullptr && find_offered_expansion(offered[0], jo) == nullptr);
	// An expansion whose base has no data is not offered: DFX2 with nothing, and jox01's
	// readiness alone would not list Joint Operations.
	TEST_EXPECT(list_trees(db, jo).size() == 1 && list_trees(db, jox).size() == 1);

	// A failed tree keeps its reason.
	const int64_t t2 = create_tree(db, jo, std::nullopt, "worker");
	TEST_EXPECT(set_tree_state(db, t2, TreeState::Building, TreeState::Failed, "resource.pff is missing"));
	TEST_EXPECT(!set_tree_state(db, t2, TreeState::Building, TreeState::Failed, "again"));
	TEST_EXPECT(find_tree(db, t2)->error == "resource.pff is missing");
	// Prunable: the failed one once old enough (not yet: created now), never the current.
	TEST_EXPECT(prunable_trees(db, 3600).empty());
	TEST_EXPECT(prunable_trees(db, -1) == std::vector<int64_t>{ t2 });
	mark_tree_pruned(db, t2);
	TEST_EXPECT(find_tree(db, t2)->state == TreeState::Pruned && !find_tree(db, t2)->pruned_at.empty());
	TEST_EXPECT(prunable_trees(db, -1).empty());
	// A newer tree becomes current; the older one, unpinned, is prunable.
	const int64_t t3 = create_tree(db, jo, std::nullopt, "worker");
	TEST_EXPECT(finish_tree(db, t3, ready_with("JO V1.7.5.4", {})));
	TEST_EXPECT(find_product(db, jo)->current_tree_id == t3);
	TEST_EXPECT(prunable_trees(db, -1) == std::vector<int64_t>{ t1 });
	// Pinned by a live server, it stays.
	const int64_t server = insert_server(db, jo, std::nullopt);
	set_tree_pins(db, server, t1, std::nullopt);
	TEST_EXPECT(prunable_trees(db, -1).empty());
	TEST_EXPECT(count_tree_pins(db, t1) == 1);
	// A building tree is never prunable; a pruned one whose objects are still there (pruned_at
	// NULL) is.
	const int64_t t4 = create_tree(db, jo, std::nullopt, "worker");
	TEST_EXPECT(prunable_trees(db, -1).empty());
	db.exec("UPDATE product_trees SET state = 'pruned', pruned_at = NULL WHERE id = $1;", { BindValue(t4) });
	TEST_EXPECT(prunable_trees(db, 3600) == std::vector<int64_t>{ t4 });
	return 0;
}

int test_releases() {
	TestDatabase tdb;
	Database db = tdb.open();
	const int64_t jo = seeded(db, "Joint Operations"), jox = seeded(db, "Escalation");
	const int64_t runtime = seeded(db, "OpenNova runtime");

	// An upload that names no game takes its base section's once validated.
	const int64_t r1 = create_release(db, "1.7.5.4", 1234, "admin");
	TEST_EXPECT(r1 != 0 && find_release(db, r1)->status == ReleaseStatus::Uploading &&
			!find_release(db, r1)->product_id);
	TEST_EXPECT(set_release_status(db, r1, ReleaseStatus::Uploading, ReleaseStatus::Validating));
	TEST_EXPECT(set_release_validated(db, r1, "abc123", 1234, "Joint Operations",
			{ { jo, "", "JOPTR.CMD", "1.7.5.4", 3, 999 }, { jox, "jox01", "JOEPTR.CMD", "1.0.0.1", 1, 10 } }));
	TEST_EXPECT(!set_release_validated(db, r1, "abc123", 1234, "x", {}));
	TEST_EXPECT(find_release(db, r1)->status == ReleaseStatus::Draft && find_release(db, r1)->sha256 == "abc123" &&
			find_release(db, r1)->product_id == jo && find_release(db, r1)->revision == 0);
	TEST_EXPECT(find_release_by_sha(db, "abc123") == r1 && !find_release_by_sha(db, "nope"));
	const std::vector<ReleaseSection> sections = list_sections(db, r1);
	TEST_EXPECT(sections.size() == 2 && sections[0].product_id == jo && sections[0].expansion.empty() &&
			sections[1].product_id == jox && sections[1].expansion == "jox01" && !sections[1].tree_id);
	TEST_EXPECT(published_sections(db).empty());

	TEST_EXPECT(publish_release(db, r1));
	TEST_EXPECT(!publish_release(db, r1));
	TEST_EXPECT(find_product(db, jo)->published_release_id == r1 && find_product(db, jox)->published_release_id == r1);
	std::vector<PublishedSection> published = published_sections(db);
	TEST_EXPECT(published.size() == 2 && published[0].updater_login == "JOEus" &&
			published[0].command_file == "JOEPTR.CMD" && published[0].expansion == "jox01" &&
			published[1].updater_login == "JOTRus" && published[1].sha256 == "abc123" && published[1].retail);

	// The release's tree for the base section.
	const int64_t built = create_tree(db, jo, r1, "worker");
	set_section_tree(db, r1, jo, built);
	TEST_EXPECT(find_tree(db, built)->release_id == r1);
	TEST_EXPECT(finish_tree(db, built, ready_with("JO V1.7.5.4", {})));
	TEST_EXPECT(list_sections(db, r1)[0].tree_id == built);
	TEST_EXPECT(find_product(db, jo)->current_tree_id == built);

	// A second release published retires the first (no product names it any more).
	const int64_t r2 = create_release(db, "1.7.5.5", 100, "admin");
	TEST_EXPECT(set_release_status(db, r2, ReleaseStatus::Uploading, ReleaseStatus::Validating));
	TEST_EXPECT(set_release_validated(db, r2, "def456", 100, "Joint Operations",
			{ { jo, "", "JOPTR.CMD", "1.7.5.5", 1, 1 }, { jox, "jox01", "JOEPTR.CMD", "1.0.0.2", 1, 1 } }));
	TEST_EXPECT(publish_release(db, r2));
	TEST_EXPECT(find_release(db, r1)->status == ReleaseStatus::Retired && !find_release(db, r1)->retired_at.empty());
	TEST_EXPECT(find_product(db, jo)->published_release_id == r2);
	TEST_EXPECT(list_releases(db).size() == 2 && list_releases(db)[0].id == r2);

	// Retiring clears the products.
	TEST_EXPECT(retire_release(db, r2) && !retire_release(db, r2));
	TEST_EXPECT(!find_product(db, jo)->published_release_id && published_sections(db).empty());
	// An upload that names its game keeps it (the runtime's, from the Publish workflow).
	const int64_t r3 = create_release(db, "v0.0.21", 100, "token", runtime);
	TEST_EXPECT(find_release(db, r3)->product_id == runtime);
	TEST_EXPECT(set_release_status(db, r3, ReleaseStatus::Uploading, ReleaseStatus::Validating));
	TEST_EXPECT(set_release_validated(db, r3, "ghi789", 100, "OpenNova runtime",
			{ { runtime, "", "ONRUNTIME.CMD", "0.0.21", 1, 1 } }));
	TEST_EXPECT(publish_release(db, r3) && find_product(db, runtime)->published_release_id == r3 &&
			find_release(db, r3)->product_id == runtime);
	// A release built from a revision of its game's project names the game from the start.
	NewBuiltRelease made;
	made.name = "JOX01 1.0.0.3";
	made.created_by = "admin taylor";
	made.product_id = jox;
	made.revision = 2;
	made.base_revision = 1;
	made.version = "1.0.0.3";
	const int64_t r5 = create_built_release(db, made);
	const auto building = find_release(db, r5);
	TEST_EXPECT(building && building->status == ReleaseStatus::Building && building->product_id == jox &&
			building->revision == 2 && building->base_revision == 1 && building->version == "1.0.0.3");
	// An invalid upload keeps its reason; a deleted release leaves its tree's row.
	const int64_t r4 = create_release(db, "bad", 5, "admin");
	TEST_EXPECT(set_release_status(db, r4, ReleaseStatus::Uploading, ReleaseStatus::Validating));
	TEST_EXPECT(set_release_status(db, r4, ReleaseStatus::Validating, ReleaseStatus::Invalid, "no 1SFS trailer"));
	TEST_EXPECT(find_release(db, r4)->status == ReleaseStatus::Invalid && find_release(db, r4)->error == "no 1SFS trailer");
	TEST_EXPECT(delete_release(db, r1) && !delete_release(db, r1));
	TEST_EXPECT(find_tree(db, built) && !find_tree(db, built)->release_id);
	return 0;
}

int test_pins() {
	TestDatabase tdb;
	Database db = tdb.open();
	const int64_t jo = seeded(db, "Joint Operations"), jox = seeded(db, "Escalation");
	const int64_t dfx2 = seeded(db, "Delta Force Xtreme 2");
	TEST_EXPECT(!current_trees_of(db, jo, std::nullopt));
	const int64_t t_jo = create_tree(db, jo, std::nullopt, "a");
	TEST_EXPECT(finish_tree(db, t_jo, ready_with("1", {})));
	TEST_EXPECT(current_trees_of(db, jo, std::nullopt) && current_trees_of(db, jo, std::nullopt)->base_tree_id == t_jo);
	TEST_EXPECT(!current_trees_of(db, jo, jox)); // the expansion has no data yet
	TEST_EXPECT(!current_trees_of(db, jox, std::nullopt)); // an expansion is no base
	const int64_t t_jox = create_tree(db, jox, std::nullopt, "a");
	TEST_EXPECT(finish_tree(db, t_jox, ready_with("x", {})));
	const auto both = current_trees_of(db, jo, jox);
	TEST_EXPECT(both && both->base_tree_id == t_jo && both->expansion_tree_id == t_jox);
	TEST_EXPECT(!current_trees_of(db, dfx2, jox)); // not its expansion

	const int64_t s1 = insert_server(db, jo, jox);
	const int64_t s2 = insert_server(db, jo, std::nullopt);
	const int64_t s3 = insert_server(db, jo, std::nullopt, "stopped");
	// Unpinned (before the first start): behind, by the rule, until pinned.
	TEST_EXPECT(server_outdated(db, s1));
	set_tree_pins(db, s1, t_jo, t_jox);
	set_tree_pins(db, s2, t_jo, std::nullopt);
	set_tree_pins(db, s3, t_jo, std::nullopt);
	TEST_EXPECT(!server_outdated(db, s1) && !server_outdated(db, s2));
	TEST_EXPECT(request_update(db, s1) == UpdateOutcome::UpToDate);
	TEST_EXPECT(request_update(db, s3) == UpdateOutcome::NotLive);
	TEST_EXPECT(request_update(db, 99999) == UpdateOutcome::NotFound);
	// A new expansion tree: s1 is behind, s2 (no expansion) is not.
	const int64_t t_jox2 = create_tree(db, jox, std::nullopt, "a");
	TEST_EXPECT(finish_tree(db, t_jox2, ready_with("x2", {})));
	TEST_EXPECT(server_outdated(db, s1) && !server_outdated(db, s2));
	TEST_EXPECT(request_update(db, s2) == UpdateOutcome::UpToDate);
	TEST_EXPECT(request_update(db, s1) == UpdateOutcome::Requested);
	TEST_EXPECT(request_update(db, s1) == UpdateOutcome::Requested); // idempotent
	TEST_EXPECT(db.query("SELECT 1 FROM hosted_servers WHERE id = $1 AND update_requested;", { BindValue(s1) }).size() == 1);
	// The start that honours it re-pins and clears the flag.
	set_tree_pins(db, s1, t_jo, t_jox2);
	TEST_EXPECT(db.query("SELECT 1 FROM hosted_servers WHERE id = $1 AND update_requested;", { BindValue(s1) }).empty());
	TEST_EXPECT(!server_outdated(db, s1));
	// A new base tree: both are behind; update-all of the product marks both, the stopped one never.
	const int64_t t_jo2 = create_tree(db, jo, std::nullopt, "a");
	TEST_EXPECT(finish_tree(db, t_jo2, ready_with("2", {})));
	TEST_EXPECT(request_update_all(db, dfx2) == 0);
	TEST_EXPECT(request_update_all(db, jo) == 2);
	TEST_EXPECT(request_update_all(db, std::nullopt) == 0);
	TEST_EXPECT(db.query("SELECT count(*) FROM hosted_servers WHERE update_requested;").front().as_int(0) == 2);
	return 0;
}

// The products' holds read the hosted servers' rows with a copy of their live rule
// (product_repository.cpp, which stands below the hosted servers' model): for every state,
// a server pinning a tree holds it exactly while is_live calls the server live.
int test_live_rule() {
	TestDatabase tdb;
	Database db = tdb.open();
	const int64_t jo = seeded(db, "Joint Operations");
	// A tree that is not current (a newer one is), so only its pins hold it.
	const int64_t pinned_tree = create_tree(db, jo, std::nullopt, "a");
	TEST_EXPECT(finish_tree(db, pinned_tree, ready_with("1", {})));
	const int64_t newer = create_tree(db, jo, std::nullopt, "a");
	TEST_EXPECT(finish_tree(db, newer, ready_with("2", {})));
	for (int i = 0; i <= static_cast<int>(State::Failed); ++i) {
		const State state = static_cast<State>(i);
		const int64_t server = insert_server(db, jo, std::nullopt, state_name(state));
		set_tree_pins(db, server, pinned_tree, std::nullopt);
		const std::vector<int64_t> prunable = prunable_trees(db, -1);
		TEST_EXPECT(prunable.empty() == is_live(state));
		TEST_EXPECT((count_tree_pins(db, pinned_tree) == 1) == is_live(state));
		TEST_EXPECT((count_live_servers_of(db, jo) == 1) == is_live(state));
		db.exec("DELETE FROM hosted_servers WHERE id = $1;", { BindValue(server) });
	}
	return 0;
}

// The files a tree holds by kind (tree_files.h): the required ones, the optional ones,
// the canonical spellings and the mission pair.
int test_files() {
	const auto base = tree_file_rules(ProductKind::Game, "");
	TEST_EXPECT(base.size() == 7 && base[0].name == "resource.pff" && base[0].required &&
			base[2].name == "language.pff" && base[2].required && !base[3].required &&
			base[5].name == "_NSTMOUT.TXT");
	const auto exp = tree_file_rules(ProductKind::Expansion, "jox01");
	TEST_EXPECT(exp.size() == 4 && exp[0].name == "jox01.pff" && exp[1].name == "jox01L.pff" &&
			exp[2].name == "version.txt" && exp[2].required && exp[3].name == "gt.ssc" && !exp[3].required);
	TEST_EXPECT(canonical_tree_file(ProductKind::Game, "", "RESOURCE.PFF") == "resource.pff");
	TEST_EXPECT(canonical_tree_file(ProductKind::Game, "", "_nstmout.txt") == "_NSTMOUT.TXT");
	TEST_EXPECT(canonical_tree_file(ProductKind::Game, "", "jox01.pff").empty());
	TEST_EXPECT(canonical_tree_file(ProductKind::Game, "", "Jointops.exe").empty());
	TEST_EXPECT(canonical_tree_file(ProductKind::Expansion, "jox01", "JOX01L.PFF") == "jox01L.pff");
	TEST_EXPECT(canonical_tree_file(ProductKind::Expansion, "jox01", "resource.pff").empty());
	TEST_EXPECT(missing_tree_files(ProductKind::Game, "", { "resource.pff", "localres.pff" }) ==
			std::vector<std::string>{ "language.pff" });
	TEST_EXPECT(missing_tree_files(ProductKind::Game, "", { "RESOURCE.PFF", "localres.pff", "language.pff" }).empty());
	TEST_EXPECT(missing_tree_files(ProductKind::Expansion, "jox01", { "jox01.pff" }) ==
			(std::vector<std::string>{ "jox01L.pff", "version.txt" }));
	TEST_EXPECT(mission_pair_of(ProductKind::Game, "").missions == "localres.pff" &&
			mission_pair_of(ProductKind::Game, "").texts == "language.pff");
	TEST_EXPECT(mission_pair_of(ProductKind::Expansion, "jox01").missions == "jox01.pff" &&
			mission_pair_of(ProductKind::Expansion, "jox01").texts == "jox01L.pff");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	if (int rc = test_files())
		return rc;
	if (int rc = test_products())
		return rc;
	if (int rc = test_trees_and_offered())
		return rc;
	if (int rc = test_releases())
		return rc;
	if (int rc = test_pins())
		return rc;
	if (int rc = test_live_rule())
		return rc;
	std::printf("product_repository: ok\n");
	return 0;
}
