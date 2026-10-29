// The games' rows (apps/novaworld_common/products/product_repository.h) on a migrated
// database of the test's own: the seeded games (in their seeded order) with their projects' keys,
// a game created, changed and refused (a duplicate login, command file or download, a duplicate
// key, an expansion code another expansion of any game has without case, no base parent, an
// expansion building on it, in use); trees from building to ready (the product's current tree
// follows), what a start may pick (the service's own, every account's public ones, the caller's
// own) and what may be pruned; releases of their games validated (an upload naming none takes
// its base section's), published (the previous one retired), retired, and a built one naming its
// game; explicit lobby selection before release builds and stable shared lobby identities;
// its releases in flight and its starts of the day; the mods a game lists; the servers' pins, the
// owner's update and the admin's update-all; the products' holds on the servers' rows reading a
// server as live exactly when the hosted servers' own rule does.

#include "common/test_database.h"
#include "common/test_expect.h"

#include "hosted/model/hosted_repository.h"
#include "products/product_repository.h"
#include "products/tree_files.h"

#include "program/catalog_repository.h"
#include <net/title/title_identity.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
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
			"INSERT INTO hosted_servers (state, product_id, expansion_id, configuration) VALUES ($1, $2, $3, jsonb_build_object('game_name', 'srv', 'max_players', 32, 'time_limit', 30, 'kill_limit', 50, 'max_score', 5, 'respawn', 5, 'start_delay', 0, 'rotation', jsonb_build_array(jsonb_build_object('file', 'a.bms', 'swap_sides', false)))) RETURNING id;",
			{ BindValue(state), BindValue(product_id),
					expansion_id ? BindValue(*expansion_id) : BindValue(std::monostate{}) });
	return rows.front().as_int(0).value_or(0);
}

int test_products() {
	TestDatabase tdb;
	Database db = tdb.open();
	// Three games, their two expansions, and the apps: the runtime and the editor (updatable), the
	// tools and the Blender add-on (download-only).
	const std::vector<ProductRow> all = list_products(db);
	TEST_EXPECT(all.size() == 9);
	const int64_t jo = seeded(db, "Joint Operations"), jox = seeded(db, "Escalation");
	TEST_EXPECT(jo != 0 && jox != 0);
	// The baseline seeds them in one order, so each keeps its id across a reset (the store names
	// a project's files by its game's id: DEPLOY.md "Resetting the service database").
	TEST_EXPECT(jo == 1 && seeded(db, "Delta Force Xtreme 2") == 2 &&
			seeded(db, "OP: Acid Gambit") == 3 && jox == 4 && seeded(db, "OpenNova runtime") == 5 &&
			seeded(db, "OP: Nifty Package") == 6 && seeded(db, "OpenNova Editor") == 7 &&
			seeded(db, "Tools and dedicated server") == 8 && seeded(db, "Blender add-on") == 9);
	const auto jo_row = find_product(db, jo);
	TEST_EXPECT(jo_row && jo_row->kind == ProductKind::Game && jo_row->title == "jo" &&
			jo_row->lobby == "jop_2_consumer" && jo_row->updater_login == "JOTRus" &&
			jo_row->command_file == "JOPTR.CMD" && jo_row->retail && !jo_row->current_tree_id &&
			jo_row->key == "JO" && jo_row->visibility == "private" && jo_row->head == 0 &&
			jo_row->wizard_login == "JOTRqq" && jo_row->download == "jointops-game-windows.zip" &&
			!jo_row->description.empty());
	const auto jox_row = find_product(db, jox);
	TEST_EXPECT(jox_row && jox_row->kind == ProductKind::Expansion && jox_row->parent_id == jo &&
			jox_row->expansion_code == "jox01" && jox_row->updater_login == "JOEus" &&
			jox_row->key == "JOX01" && jox_row->retail && jox_row->wizard_login == "JOEqq" &&
			jox_row->download.empty());
	TEST_EXPECT(list_expansions_of(db, jo).size() == 1);
	TEST_EXPECT(all[0].kind == ProductKind::Game && all[3].kind == ProductKind::Expansion &&
			all[5].kind == ProductKind::App && all[8].kind == ProductKind::App);
	const auto runtime = find_product(db, seeded(db, "OpenNova runtime"));
	TEST_EXPECT(runtime && runtime->kind == ProductKind::App && runtime->lobby.empty() &&
			runtime->title.empty() && !runtime->parent_id && !runtime->current_tree_id &&
			runtime->updater_login == "ONRUNTIME" && runtime->command_file == "ONRUNTIME.CMD" &&
			!runtime->retail && runtime->key == "ONRUNTIME" && runtime->visibility.empty() &&
			runtime->product_name == "OpenNova Runtime" && runtime->product_exe == "opennova.exe" &&
			runtime->download.empty());
	const auto editor = find_product_by_key(db, "ONEDITOR");
	TEST_EXPECT(editor && editor->kind == ProductKind::App && editor->updater_login == "ONEDITOR" &&
			editor->command_file == "ONEDITOR.CMD" && editor->product_exe == "oneditor.exe" &&
			editor->product_name == "OpenNova Editor" &&
			editor->download == "opennova-editor-windows.zip");
	for (const char *key : { "ONTOOLS", "ONBLENDER" }) {
		const auto app = find_product_by_key(db, key);
		TEST_EXPECT(app && app->kind == ProductKind::App && app->updater_login.empty() &&
				app->command_file.empty() && !app->download.empty() && app->visibility.empty());
	}
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
	bhd.title = "jo";
	bhd.lobby = "jop_2_consumer";
	bhd.updater_login = "BHDus";
	bhd.command_file = "BHDPTR.CMD";
	bhd.key = "BHD";
	bhd.visibility = "private";
	bhd.created_by = "admin";
	int64_t bhd_id = 0;
	// An admin's (the admin's route chooses any lobby: catalog::selectable_lobbies).
	TEST_EXPECT(create_product(db, bhd, bhd_id, { std::nullopt, true }) == ProductOutcome::Ok &&
			bhd_id != 0);
	TEST_EXPECT(find_product(db, bhd_id)->key == "BHD" &&
			find_product(db, bhd_id)->visibility == "private" && find_product(db, bhd_id)->head == 0);
	NewProduct dup = bhd;
	dup.updater_login = "bhdus"; // the login is unique without case
	int64_t ignored = 0;
	TEST_EXPECT(create_product(db, dup, ignored) == ProductOutcome::DuplicateLogin);
	dup.updater_login = "BHD2us";
	dup.command_file = "BHD2PTR.CMD";
	TEST_EXPECT(create_product(db, dup, ignored) == ProductOutcome::DuplicateKey);
	NewProduct ts;
	ts.display_name = "Team Sabre";
	ts.kind = ProductKind::Expansion;
	ts.parent_id = bhd_id;
	ts.expansion_code = "bhx01";
	ts.key = "SABRE";
	ts.visibility = "private";
	int64_t ts_id = 0;
	TEST_EXPECT(create_product(db, ts, ts_id) == ProductOutcome::Ok);
	TEST_EXPECT(create_product(db, ts, ignored) == ProductOutcome::DuplicateKey);
	NewProduct again = ts; // the same code under the same game, by another key
	again.key = "SABRE2";
	TEST_EXPECT(create_product(db, again, ignored) == ProductOutcome::DuplicateExpansion);
	// An expansion code names its folder in any game's install: unique across every game, without
	// case, the service's own (jox01, nifty) among them.
	again.parent_id = jo;
	again.expansion_code = "BHX01";
	TEST_EXPECT(create_product(db, again, ignored) == ProductOutcome::DuplicateExpansion);
	again.parent_id = bhd_id;
	again.expansion_code = "Nifty";
	TEST_EXPECT(create_product(db, again, ignored) == ProductOutcome::DuplicateExpansion);
	TEST_EXPECT(db.query("SELECT 1 FROM products WHERE key = 'SABRE2';").empty());
	// So does the database's own index, whatever writes the row.
	{
		bool refused = false;
		try {
			db.exec("INSERT INTO products (key, display_name, kind, parent_id, expansion_code, "
					"visibility) VALUES ('SABRE3', 'X', 'expansion', $1, 'JOX01', 'private');",
					{ BindValue(bhd_id) });
		} catch (const db::Error &) {
			refused = true;
		}
		TEST_EXPECT(refused);
	}
	// A command file and a download are unique too (the command file without case).
	NewProduct named = bhd;
	named.key = "BHDNAMED";
	named.updater_login = "BHDNAMED";
	named.command_file = "joptr.cmd";
	TEST_EXPECT(create_product(db, named, ignored) == ProductOutcome::DuplicateCommandFile);
	named.command_file = "BHDNAMED.CMD";
	named.download = "jointops-game-windows.zip";
	TEST_EXPECT(create_product(db, named, ignored) == ProductOutcome::DuplicateDownload);
	NewProduct orphan = ts;
	orphan.expansion_code = "bhx02";
	orphan.key = "ORPHAN";
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
	steal.updater_login = "BHDus";
	steal.command_file = "DFX2.cmd";
	TEST_EXPECT(update_product(db, bhd_id, steal) == ProductOutcome::DuplicateCommandFile);
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
	TEST_EXPECT(update_product(db, ts_id, moved) == ProductOutcome::InvalidLobby);
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
	TEST_EXPECT(offered_products(db, std::nullopt).empty());

	// A release's tree: building -> ready; the product's current tree is then this one, and the
	// base is offered with its missions.
	const int64_t t1 = create_tree(db, jo, std::nullopt, "worker");
	TEST_EXPECT(t1 != 0);
	const auto fresh = find_tree(db, t1);
	TEST_EXPECT(fresh && fresh->state == TreeState::Building && !fresh->release_id &&
			fresh->files.empty() && fresh->missions.empty() && fresh->created_by == "worker");
	TEST_EXPECT(
			offered_products(db, std::nullopt).empty() && !find_product(db, jo)->current_tree_id);
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
	std::vector<OfferedProduct> offered = offered_products(db, std::nullopt);
	TEST_EXPECT(offered.size() == 1 && offered[0].id == jo && offered[0].tree_id == t1 &&
			offered[0].title == "jo" && offered[0].missions.size() == 2 && offered[0].expansions.empty());
	TEST_EXPECT(find_offered(offered, jo) != nullptr && find_offered(offered, dfx2) == nullptr);

	// The expansion's tree: offered under its base once ready.
	const int64_t tx = create_tree(db, jox, std::nullopt, "worker");
	TEST_EXPECT(finish_tree(db, tx, ready_with("JOX V1.0", { { "x.bms", "Xray" } })));
	offered = offered_products(db, std::nullopt);
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

	// An account's own products: a private game offered to its owner alone, a public one to
	// everyone; an expansion only beside its game, which must be offered too.
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) VALUES "
			"('offer_a', 'x', '0ff00001', '1', 'A'), ('offer_b', 'x', '0ff00002', '1', 'B');");
	const int64_t a =
			db.query("SELECT id FROM players WHERE username = 'offer_a';")[0].as_int(0).value_or(0);
	const int64_t b =
			db.query("SELECT id FROM players WHERE username = 'offer_b';")[0].as_int(0).value_or(0);
	const auto own = [&db](const char *key, int64_t owner, const char *visibility,
							 std::optional<int64_t> parent) -> int64_t {
		NewProduct p;
		p.display_name = key;
		p.kind = parent ? ProductKind::Expansion : ProductKind::Game;
		p.new_lobby_name = parent ? "" : p.display_name;
		p.parent_id = parent;
		p.expansion_code = parent ? "x" + std::string(key) : std::string();
		for (char &c : p.expansion_code)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		p.title = "jo";
		p.key = key;
		p.visibility = visibility;
		p.owner_id = owner;
		int64_t id = 0;
		TEST_EXPECT(create_product(db, p, id) == ProductOutcome::Ok);
		const int64_t tree = create_tree(db, id, std::nullopt, "worker");
		TEST_EXPECT(finish_tree(db, tree, ready_with("1", {})));
		return id;
	};
	const int64_t a_private = own("APRIV", a, "private", std::nullopt);
	const int64_t a_public = own("APUB", a, "public", std::nullopt);
	const int64_t a_mod = own("AMOD", a, "public", a_private);
	const int64_t b_mod = own("BMOD", b, "private", jo);
	const auto ids = [](const std::vector<OfferedProduct> &list) {
		std::vector<int64_t> out;
		for (const OfferedProduct &p : list) {
			out.push_back(p.id);
			for (const OfferedExpansion &e : p.expansions)
				out.push_back(e.id);
		}
		return out;
	};
	TEST_EXPECT(
			(ids(offered_products(db, std::nullopt)) == std::vector<int64_t>{ jo, jox, a_public }));
	TEST_EXPECT((ids(offered_products(db, a)) ==
			std::vector<int64_t>{ jo, jox, a_private, a_mod, a_public }));
	TEST_EXPECT((ids(offered_products(db, b)) == std::vector<int64_t>{ jo, jox, b_mod, a_public }));
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
	TEST_EXPECT(list_releases_of(db, jox).size() == 1 && list_releases_of(db, jox)[0].id == r5);
	TEST_EXPECT(list_releases_of(db, runtime).size() == 1);
	return 0;
}

// A validated release of `product_id`, built from a revision (a draft, its one section).
int64_t draft_of(Database &db, int64_t product_id, const char *cmd, const char *sha,
		const std::string &version = "1.0") {
	NewBuiltRelease made;
	made.name = sha;
	made.created_by = "user maker";
	made.product_id = product_id;
	made.revision = 1;
	made.version = version;
	const int64_t id = create_built_release(db, made);
	TEST_EXPECT(set_release_built(db, id, ""));
	TEST_EXPECT(set_release_validated(
			db, id, sha, 100, "x", { { product_id, "", cmd, version, 1, 1 } }));
	return id;
}

// An account's own projects' releases: the configured lobby survives publish and deletion;
// its protocol values copy the title's template. The account's releases in flight and its
// starts of the day; the mods a game lists (the published public ones and the reader's own).
int test_own_releases() {
	TestDatabase tdb;
	Database db = tdb.open();
	const int64_t jo = seeded(db, "Joint Operations");
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) VALUES "
			"('maker', 'x', '0aa00001', '1', 'Maker'), ('other', 'x', '0aa00002', '1', 'Other');");
	const int64_t maker =
			db.query("SELECT id FROM players WHERE username = 'maker';")[0].as_int(0).value_or(0);
	const int64_t other =
			db.query("SELECT id FROM players WHERE username = 'other';")[0].as_int(0).value_or(0);
	const auto own = [&db](const char *key, const char *title, int64_t owner,
							 std::optional<int64_t> parent, const char *code,
							 const char *visibility) -> int64_t {
		NewProduct p;
		p.display_name = std::string(key) + " name";
		p.kind = parent ? ProductKind::Expansion : ProductKind::Game;
		p.new_lobby_name = parent ? "" : p.display_name;
		p.parent_id = parent;
		p.expansion_code = code;
		p.title = title;
		p.key = key;
		p.updater_login = key;
		p.command_file = std::string(key) + ".CMD";
		p.visibility = visibility;
		p.owner_id = owner;
		int64_t id = 0;
		TEST_EXPECT(create_product(db, p, id) == ProductOutcome::Ok);
		return id;
	};
	TEST_EXPECT(own_download("MY_GAME") == "player-my_game-game-windows.zip");

	// Creation selected the lobby; publication preserves it and its protocol template values.
	const int64_t game = own("MYGAME", "jo", maker, std::nullopt, "", "public");
	const int64_t r1 = draft_of(db, game, "MYGAME.CMD", "sha-1");
	const std::string selected = find_product(db, game)->lobby;
	TEST_EXPECT(!selected.empty());
	TEST_EXPECT(publish_release(db, r1));
	TEST_EXPECT(find_product(db, game)->lobby == selected);
	const auto row = db.query(
			"SELECT g.display_name, g.lobby_name, g.gate_tag, g.title, g.gsb_path, g.start_page, "
			"g.ver1, g.exp_bits, g.pfid, g.startup_query FROM games g, games t "
			"WHERE g.slug = $1 AND t.slug = 'acidgambit_0_consumer' "
			"AND g.start_page = t.start_page AND g.host_page = t.host_page AND g.ver1 = t.ver1 "
			"AND g.ver2 = t.ver2 AND g.exp_bits = t.exp_bits AND g.pfid = t.pfid "
			"AND g.startup_query = t.startup_query;",
			{ BindValue(selected) });
	TEST_EXPECT(row.size() == 1 && row[0].as_text(0) == "MYGAME name" &&
			row[0].as_text(1) == selected && row[0].as_text(2) == "on:" + selected.substr(3) &&
			row[0].as_text(3) == "jo" && row[0].as_text(4) == selected + ".gsb?a=1");
	// A second publish adds none; a dfx2 game copies DFX2's row.
	const int64_t r2 = draft_of(db, game, "MYGAME.CMD", "sha-2");
	TEST_EXPECT(publish_release(db, r2));
	TEST_EXPECT(db.query("SELECT 1 FROM games WHERE slug = $1;", { BindValue(selected) }).size() == 1);
	const int64_t dgame = own("MYDFX", "dfx2", maker, std::nullopt, "", "private");
	TEST_EXPECT(publish_release(db, draft_of(db, dgame, "MYDFX.CMD", "sha-3")));
	TEST_EXPECT(
			db.query("SELECT 1 FROM games g, games t WHERE g.slug = $1 "
					 "AND t.slug = 'dfx2_consumer' AND g.title = 'dfx2' "
					 "AND g.join_lan_url = t.join_lan_url AND g.glsvss_request = t.glsvss_request "
					 "AND g.pfid = '38';",
					  { BindValue(find_product(db, dgame)->lobby) })
					.size() == 1);
	// The service's own games keep their seeded lobbies.
	TEST_EXPECT(find_product(db, jo)->lobby == "jop_2_consumer");

	// An owner's publish waits for an admin's approval: served, listed and counted in flight
	// nowhere; a reject sends it back with a note, a submit clears it, an admin's publish
	// publishes it.
	const int64_t pubmod_game = own("AWAITGAME", "jo", maker, std::nullopt, "", "public");
	const int64_t awaiting = draft_of(db, pubmod_game, "AWAITGAME.CMD", "sha-await");
	TEST_EXPECT(!reject_release(db, awaiting, "x") && submit_release(db, awaiting) &&
			!submit_release(db, awaiting));
	TEST_EXPECT(find_release(db, awaiting)->status == ReleaseStatus::AwaitingApproval &&
			release_status_name(ReleaseStatus::AwaitingApproval) ==
					std::string("awaiting_approval") &&
			release_status_from_name("awaiting_approval") == ReleaseStatus::AwaitingApproval);
	TEST_EXPECT(!find_product(db, pubmod_game)->published_release_id &&
			!find_product(db, pubmod_game)->lobby.empty() &&
			count_owned_releases_in_flight(db, maker) == 0);
	for (const PublishedSection &p : published_sections(db))
		TEST_EXPECT(p.release_id != awaiting);
	{
		const std::vector<AwaitingRelease> listed = list_awaiting_releases(db);
		TEST_EXPECT(listed.size() == 1 && listed[0].id == awaiting &&
				listed[0].key == "AWAITGAME" && listed[0].owner == "maker" &&
				listed[0].name == "sha-await" && listed[0].created_at > 0);
	}
	TEST_EXPECT(reject_release(db, awaiting, "Name the maps.") &&
			find_release(db, awaiting)->status == ReleaseStatus::Draft &&
			find_release(db, awaiting)->review_note == "Name the maps." &&
			list_awaiting_releases(db).empty());
	TEST_EXPECT(submit_release(db, awaiting) && find_release(db, awaiting)->review_note.empty());
	TEST_EXPECT(publish_release(db, awaiting) &&
			find_release(db, awaiting)->status == ReleaseStatus::Published &&
			find_product(db, pubmod_game)->published_release_id == awaiting &&
			!find_product(db, pubmod_game)->lobby.empty());
	TEST_EXPECT(!publish_release(db, awaiting));

	// Its releases in flight, and its starts of the UTC day (five, a deleted one still counted).
	TEST_EXPECT(count_owned_releases_in_flight(db, maker) == 0);
	NewBuiltRelease building;
	building.name = "MYGAME 2.0";
	building.product_id = game;
	building.revision = 1;
	building.version = "2.0";
	const int64_t r4 = create_built_release(db, building);
	TEST_EXPECT(count_owned_releases_in_flight(db, maker) == 1 &&
			count_owned_releases_in_flight(db, other) == 0);
	TEST_EXPECT(set_release_built(db, r4, ""));
	TEST_EXPECT(count_owned_releases_in_flight(db, maker) == 1); // validating
	TEST_EXPECT(set_release_status(db, r4, ReleaseStatus::Validating, ReleaseStatus::Invalid, "x"));
	TEST_EXPECT(count_owned_releases_in_flight(db, maker) == 0);
	for (int i = 0; i < 5; ++i)
		TEST_EXPECT(take_release_start(db, maker, 5));
	TEST_EXPECT(!take_release_start(db, maker, 5));
	TEST_EXPECT(delete_release(db, r4) && !take_release_start(db, maker, 5));
	TEST_EXPECT(take_release_start(db, other, 5));
	// A new UTC day starts over at one.
	db.exec("UPDATE release_starts SET day = day - interval '1 day' WHERE player_id = $1;",
			{ BindValue(maker) });
	TEST_EXPECT(take_release_start(db, maker, 5));
	TEST_EXPECT(db.query("SELECT started FROM release_starts WHERE player_id = $1;",
						  { BindValue(maker) })[0]
						.as_int(0) == 1);

	// The mods of a game: the published public ones, and the reader's own private ones (the maker
	// at kOwnedProjectsMax, its two other games go first).
	TEST_EXPECT(delete_product(db, dgame) == ProductOutcome::Ok &&
			delete_product(db, pubmod_game) == ProductOutcome::Ok);
	const int64_t pub = own("PUBMOD", "", maker, jo, "pubmod", "public");
	const int64_t priv = own("PRIVMOD", "", maker, jo, "privmod", "private");
	const int64_t unpublished = own("DRAFTMOD", "", other, jo, "draftmod", "public");
	const int64_t on_own = own("OWNMOD", "", other, game, "ownmod", "public");
	TEST_EXPECT(publish_release(db, draft_of(db, pub, "PUBMOD.CMD", "sha-5", "1.2.3")));
	TEST_EXPECT(publish_release(db, draft_of(db, priv, "PRIVMOD.CMD", "sha-6")));
	draft_of(db, unpublished, "DRAFTMOD.CMD", "sha-7");
	TEST_EXPECT(publish_release(db, draft_of(db, on_own, "OWNMOD.CMD", "sha-8")));
	const auto keys = [](const std::vector<ModRow> &mods) {
		std::vector<std::string> out;
		for (const ModRow &m : mods)
			out.push_back(m.key);
		return out;
	};
	TEST_EXPECT((keys(list_mods(db, "joptr.cmd", std::nullopt)) ==
			std::vector<std::string>{ "PUBMOD" }));
	TEST_EXPECT((keys(list_mods(db, "JOPTR.CMD", other)) == std::vector<std::string>{ "PUBMOD" }));
	TEST_EXPECT((keys(list_mods(db, "JOPTR.CMD", maker)) ==
			std::vector<std::string>{ "PRIVMOD", "PUBMOD" }));
	TEST_EXPECT((keys(list_mods(db, "MYGAME.CMD", std::nullopt)) ==
			std::vector<std::string>{ "OWNMOD" }));
	TEST_EXPECT(list_mods(db, "DFX2.CMD", maker).empty());
	const std::vector<ModRow> mods = list_mods(db, "JOPTR.CMD", std::nullopt);
	TEST_EXPECT(mods.size() == 1 && mods[0].code == "pubmod" && mods[0].name == "PUBMOD name" &&
			mods[0].owner == std::optional<std::string>("maker") && mods[0].version == "1.2.3" &&
			mods[0].updater_login == "PUBMOD" && mods[0].command_file == "PUBMOD.CMD");

	// Deleting a game retains its lobby for other games and existing installs.
	TEST_EXPECT(delete_product(db, on_own) == ProductOutcome::Ok);
	TEST_EXPECT(delete_product(db, game) == ProductOutcome::Ok);
	TEST_EXPECT(db.query("SELECT 1 FROM games WHERE slug = $1;", { BindValue(selected) }).size() == 1);
	TEST_EXPECT(db.query("SELECT 1 FROM games WHERE slug = 'jop_2_consumer';").size() == 1);
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

// The database owns lobby identity before any package exists. Publication controls discovery,
// and accepting the first game/expansion build fixes the selection permanently. A player never
// selects a retail game's lobby (the original clients browse it); an admin selects any row of the
// game's title, one no product names too. A lobby a game created goes when the game leaves it
// and no other product names it; a system lobby, or one another game still names, stays.
int test_lobby_selection() {
	TestDatabase tdb;
	Database db = tdb.open();
	namespace catalog = opennova::novaworld_server::catalog;
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) VALUES "
			"('lobby_owner', 'x', '0aa01111', '1', 'Lobby owner'), "
			"('lobby_other', 'x', '0aa02222', '1', 'Lobby other');");
	const int64_t owner =
			db.query("SELECT id FROM players WHERE username = 'lobby_owner';")[0].as_int(0).value();
	const int64_t other =
			db.query("SELECT id FROM players WHERE username = 'lobby_other';")[0].as_int(0).value();
	NewProduct game;
	game.key = "LOBBYGAME";
	game.display_name = "Lobby game";
	game.title = "jo";
	game.visibility = "public";
	game.owner_id = owner;
	game.new_lobby_name = "Our lobby";
	int64_t id = 0;
	TEST_EXPECT(create_product(db, game, id) == ProductOutcome::Ok);
	const std::string first = find_product(db, id)->lobby;
	TEST_EXPECT(!find_product(db, id)->lobby_locked && first.find("on_") == 0);
	TEST_EXPECT(catalog::available_games(db).size() == 3 &&
			catalog::available_games(db, owner).size() == 4);
	TEST_EXPECT(catalog::lobby_available(db, first, "jo", { owner }) &&
			!catalog::lobby_available(db, first, "jo", { other }) &&
			!catalog::lobby_available(db, first, "dfx2", { owner }));
	const catalog::GameCatalog all = catalog::GameCatalog::load(db);
	const catalog::GameRow *lobby = all.by_slug(first);
	std::string bytes, why, decoded;
	TEST_EXPECT(lobby && lobby->display_name == "Our lobby" &&
			catalog::gate_tag_file(*lobby, bytes, why) &&
			decode_gate_tag_file(std::vector<uint8_t>(bytes.begin(), bytes.end()),
					resolve_title_identity("jo", why)->gate_tag_keys, decoded) &&
			decoded == "on:" + std::to_string(lobby->id));
	// The player's choices: Acid Gambit's lobby (no retail game's) and its own; neither Joint
	// Operations' nor Delta Force Xtreme 2's, whose lobbies the original Jointops.exe and dfx2.exe
	// browse. An admin's: every row.
	const catalog::LobbyChooser admin{ std::nullopt, true };
	std::vector<std::string> offered;
	for (const catalog::GameRow &row : catalog::selectable_lobbies(db, { owner }))
		offered.push_back(row.slug);
	TEST_EXPECT(offered == (std::vector<std::string>{ "acidgambit_0_consumer", first }));
	TEST_EXPECT(catalog::selectable_lobbies(db, admin).size() == catalog::list_games(db).size());
	ProductPatch patch;
	patch.lobby = "dfx2_consumer";
	TEST_EXPECT(update_product(db, id, patch, { owner }) == ProductOutcome::InvalidLobby);
	patch.lobby = "jop_2_consumer";
	TEST_EXPECT(update_product(db, id, patch, { owner }) == ProductOutcome::InvalidLobby &&
			find_product(db, id)->lobby == first);
	// Its own lobby left for Acid Gambit's goes: no other product names it.
	patch.lobby = "acidgambit_0_consumer";
	TEST_EXPECT(update_product(db, id, patch, { owner }) == ProductOutcome::Ok);
	TEST_EXPECT(find_product(db, id)->lobby == "acidgambit_0_consumer" &&
			!catalog::GameCatalog::load(db).by_slug(first));
	// A system lobby no product names (as a migration adds one): an admin's to select, of its
	// title alone, and never a player's.
	db.exec("INSERT INTO games (slug, display_name, lobby_name, gate_tag, title, start_page, "
			"host_page, gsb_path, join_lan_url, ver1, ver2, exp_bits, pfid, startup_query, "
			"glsvss_request, glsvss_rims, glsvss_agrms) SELECT 'extra_0_consumer', 'Extra', "
			"'extra_0_consumer', 'extra:0', title, start_page, host_page, 'extra_0.gsb?a=1', "
			"join_lan_url, ver1, ver2, exp_bits, pfid, startup_query, glsvss_request, "
			"glsvss_rims, glsvss_agrms FROM games WHERE slug = 'acidgambit_0_consumer';");
	TEST_EXPECT(!catalog::lobby_available(db, "extra_0_consumer", "jo", { owner }) &&
			catalog::lobby_available(db, "extra_0_consumer", "jo", admin) &&
			!catalog::lobby_available(db, "extra_0_consumer", "dfx2", admin));
	patch.lobby = "extra_0_consumer";
	TEST_EXPECT(update_product(db, id, patch, { owner }) == ProductOutcome::InvalidLobby);
	TEST_EXPECT(update_product(db, id, patch, admin) == ProductOutcome::Ok &&
			find_product(db, id)->lobby == "extra_0_consumer");
	// A new lobby; the system lobby it leaves stays.
	patch = {};
	patch.new_lobby_name = "Our second lobby";
	TEST_EXPECT(update_product(db, id, patch, { owner }) == ProductOutcome::Ok);
	const std::string second = find_product(db, id)->lobby;
	TEST_EXPECT(second != first && second.find("on_") == 0 &&
			catalog::GameCatalog::load(db).by_slug("extra_0_consumer"));
	NewProduct shared = game;
	shared.key = "SHARED";
	shared.owner_id = other;
	shared.new_lobby_name.clear();
	shared.lobby = second;
	int64_t shared_id = 0;
	// Unpublished, the lobby is its owner's alone; published, anyone's.
	TEST_EXPECT(create_product(db, shared, shared_id) == ProductOutcome::InvalidLobby);
	const int64_t release = draft_of(db, id, "LOBBY.CMD", "lobby-test-sha");
	TEST_EXPECT(
			publish_release(db, release) && catalog::lobby_available(db, second, "jo", { other }));
	TEST_EXPECT(create_product(db, shared, shared_id) == ProductOutcome::Ok);
	// A lobby another game still names stays when one leaves it.
	patch = {};
	patch.lobby = "acidgambit_0_consumer";
	TEST_EXPECT(update_product(db, shared_id, patch, { other }) == ProductOutcome::Ok &&
			catalog::GameCatalog::load(db).by_slug(second));
	patch.lobby = second;
	TEST_EXPECT(update_product(db, shared_id, patch, { other }) == ProductOutcome::Ok);
	patch = {};
	patch.key = "RENAMED";
	TEST_EXPECT(update_product(db, id, patch, { owner }) == ProductOutcome::Ok &&
			find_product(db, id)->lobby == second);
	NewProduct expansion;
	expansion.key = "LOBBYMOD";
	expansion.display_name = "Lobby mod";
	expansion.kind = ProductKind::Expansion;
	expansion.parent_id = shared_id;
	expansion.expansion_code = "lobbymod";
	expansion.visibility = "private";
	expansion.owner_id = other;
	int64_t expansion_id = 0;
	TEST_EXPECT(create_product(db, expansion, expansion_id) == ProductOutcome::Ok);
	NewBuiltRelease build;
	build.product_id = expansion_id;
	build.name = "First build";
	build.revision = 1;
	build.base_revision = 1;
	build.version = "1.0";
	{
		db::Transaction rejected(db);
		TEST_EXPECT(
				create_built_release(db, build) > 0 && find_product(db, shared_id)->lobby_locked);
	}
	TEST_EXPECT(!find_product(db, shared_id)->lobby_locked);
	const int64_t built = create_built_release(db, build);
	TEST_EXPECT(built > 0 && find_product(db, shared_id)->lobby_locked);
	patch = {};
	patch.lobby = "acidgambit_0_consumer";
	TEST_EXPECT(update_product(db, shared_id, patch, { other }) == ProductOutcome::LobbyLocked);
	TEST_EXPECT(set_release_build_failed(db, built, "failed", "") && delete_release(db, built));
	TEST_EXPECT(update_product(db, shared_id, patch, { other }) == ProductOutcome::LobbyLocked);
	patch.lobby = second;
	TEST_EXPECT(update_product(db, shared_id, patch, { other }) == ProductOutcome::Ok);
	TEST_EXPECT(delete_product(db, expansion_id) == ProductOutcome::Ok &&
			delete_product(db, id) == ProductOutcome::Ok);
	TEST_EXPECT(find_product(db, shared_id)->lobby == second &&
			catalog::GameCatalog::load(db).by_slug(second));
	// A concurrent PATCH waits for the transaction accepting the first build, then sees
	// its durable lock. It cannot commit a different selection behind the worker's back.
	std::future<ProductOutcome> concurrent;
	bool waited = false;
	game.key = "RACEGAME";
	int64_t race_id = 0;
	TEST_EXPECT(create_product(db, game, race_id) == ProductOutcome::Ok);
	const std::string race_lobby = find_product(db, race_id)->lobby;
	{
		db::Transaction accepting(db);
		build.product_id = race_id;
		build.base_revision = 0;
		TEST_EXPECT(create_built_release(db, build) > 0);
		concurrent = std::async(std::launch::async, [&tdb, race_id, owner] {
			Database other_db = tdb.open();
			ProductPatch change;
			change.lobby = "acidgambit_0_consumer";
			return update_product(other_db, race_id, change, { owner });
		});
		waited = concurrent.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
		accepting.commit();
	}
	TEST_EXPECT(waited && concurrent.get() == ProductOutcome::LobbyLocked &&
			find_product(db, race_id)->lobby == race_lobby);
	return 0;
}

// A game with no lobby has no selection to fix: neither a build of it (or of its expansion) nor a
// validated upload naming it locks it, and one locked without a lobby (as the lobby migration
// leaves a game it found with none) still takes one, existing or new. Once it has one, the lock
// holds.
int test_lock_needs_a_lobby() {
	TestDatabase tdb;
	Database db = tdb.open();
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) VALUES "
			"('lockless', 'x', '0aa03333', '1', 'Lockless');");
	const int64_t owner =
			db.query("SELECT id FROM players WHERE username = 'lockless';")[0].as_int(0).value();
	NewProduct game;
	game.key = "LOCKLESS";
	game.display_name = "Lockless";
	game.title = "jo";
	game.visibility = "private";
	game.owner_id = owner;
	game.lobby = "acidgambit_0_consumer";
	int64_t id = 0;
	TEST_EXPECT(create_product(db, game, id) == ProductOutcome::Ok);
	NewProduct mod;
	mod.key = "LOCKMOD";
	mod.display_name = "Lockless mod";
	mod.kind = ProductKind::Expansion;
	mod.parent_id = id;
	mod.expansion_code = "lockmod";
	mod.visibility = "private";
	mod.owner_id = owner;
	int64_t mod_id = 0;
	TEST_EXPECT(create_product(db, mod, mod_id) == ProductOutcome::Ok);
	db.exec("UPDATE products SET lobby = '' WHERE id = $1;", { BindValue(id) });
	NewBuiltRelease build;
	build.product_id = id;
	build.name = "Lockless build";
	build.revision = 1;
	build.version = "1.0";
	TEST_EXPECT(create_built_release(db, build) > 0 && !find_product(db, id)->lobby_locked);
	build.product_id = mod_id;
	build.base_revision = 1;
	TEST_EXPECT(create_built_release(db, build) > 0 && !find_product(db, id)->lobby_locked);
	draft_of(db, id, "LOCKLESS.CMD", "lockless-sha");
	TEST_EXPECT(!find_product(db, id)->lobby_locked);
	// Locked with none: it takes a new lobby, and that one is fixed.
	db.exec("UPDATE products SET lobby_locked = true WHERE id = $1;", { BindValue(id) });
	ProductPatch patch;
	patch.new_lobby_name = "Found lobby";
	TEST_EXPECT(update_product(db, id, patch, { owner }) == ProductOutcome::Ok);
	const std::string made = find_product(db, id)->lobby;
	TEST_EXPECT(made.find("on_") == 0 && find_product(db, id)->lobby_locked);
	patch = {};
	patch.lobby = "acidgambit_0_consumer";
	TEST_EXPECT(update_product(db, id, patch, { owner }) == ProductOutcome::LobbyLocked &&
			find_product(db, id)->lobby == made);
	patch = {};
	patch.new_lobby_name = "Another";
	TEST_EXPECT(update_product(db, id, patch, { owner }) == ProductOutcome::LobbyLocked);
	// Locked with none again: it takes an existing one.
	db.exec("UPDATE products SET lobby = '' WHERE id = $1;", { BindValue(id) });
	patch = {};
	patch.lobby = "acidgambit_0_consumer";
	TEST_EXPECT(update_product(db, id, patch, { owner }) == ProductOutcome::Ok &&
			find_product(db, id)->lobby == "acidgambit_0_consumer" &&
			find_product(db, id)->lobby_locked);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	if (int rc = test_files())
		return rc;
	if (int rc = test_lobby_selection())
		return rc;
	if (int rc = test_lock_needs_a_lobby())
		return rc;
	if (int rc = test_products())
		return rc;
	if (int rc = test_trees_and_offered())
		return rc;
	if (int rc = test_releases())
		return rc;
	if (int rc = test_own_releases())
		return rc;
	if (int rc = test_pins())
		return rc;
	if (int rc = test_live_rule())
		return rc;
	std::printf("product_repository: ok\n");
	return 0;
}
