// The hosted data sets' rows (apps/novaworld_common/hosted/data_set_repository.h, migration
// 0011) on a migrated database of the test's own: the seeded four, a data set created,
// changed and refused (a duplicate expansion, no base parent, in use); trees from uploading
// to ready (the data set's current tree follows), what a start may pick, what may be pruned
// and what holds a tree; the servers' pins, the owner's update and the admin's update-all;
// and the migration's mapping of the old game and expansion columns onto the seeded rows.

#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include "hosted/data_set_files.h"
#include "hosted/data_set_repository.h"

#include <cstdio>
#include <filesystem>
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
	const auto rows = db.query("SELECT id FROM data_sets WHERE display_name = $1;", { BindValue(display_name) });
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
int64_t insert_server(Database &db, int64_t data_set_id, std::optional<int64_t> expansion_id,
		const char *state = "running") {
	const auto rows = db.query(
			"INSERT INTO hosted_servers (state, game_name, max_players, time_limit, kill_limit, "
			"max_score, respawn, start_delay, missions, data_set_id, expansion_id) "
			"VALUES ($1, 'srv', 32, 30, 50, 5, 5, 0, 'a.bms', $2, $3) RETURNING id;",
			{ BindValue(state), BindValue(data_set_id),
					expansion_id ? BindValue(*expansion_id) : BindValue(std::monostate{}) });
	return rows.front().as_int(0).value_or(0);
}

int test_data_sets() {
	TestDatabase tdb;
	Database db = tdb.open();
	// The seeded rows: three bases and Escalation under Joint Operations.
	const std::vector<DataSetRow> all = list_data_sets(db);
	TEST_EXPECT(all.size() == 4);
	const int64_t jo = seeded(db, "Joint Operations"), jox = seeded(db, "Escalation");
	TEST_EXPECT(jo != 0 && jox != 0);
	const auto jo_row = find_data_set(db, jo);
	TEST_EXPECT(jo_row && jo_row->kind == DataSetKind::Base && jo_row->title == "jo" &&
			jo_row->game == "jop_2_consumer" && !jo_row->current_tree_id);
	const auto jox_row = find_data_set(db, jox);
	TEST_EXPECT(jox_row && jox_row->kind == DataSetKind::Expansion && jox_row->parent_id == jo &&
			jox_row->expansion_code == "jox01");
	TEST_EXPECT(list_expansions_of(db, jo).size() == 1);
	TEST_EXPECT(all[0].kind == DataSetKind::Base && all[3].kind == DataSetKind::Expansion);

	// A new base, a new expansion of it, and the refusals.
	NewDataSet bhd;
	bhd.display_name = "Black Hawk Down";
	bhd.kind = DataSetKind::Base;
	bhd.title = "bhd";
	bhd.game = "bhd_consumer";
	bhd.created_by = "admin";
	int64_t bhd_id = 0;
	TEST_EXPECT(create_data_set(db, bhd, bhd_id) == DataSetOutcome::Ok && bhd_id != 0);
	int64_t ignored = 0;
	NewDataSet ts;
	ts.display_name = "Team Sabre";
	ts.kind = DataSetKind::Expansion;
	ts.parent_id = bhd_id;
	ts.expansion_code = "bhx01";
	int64_t ts_id = 0;
	TEST_EXPECT(create_data_set(db, ts, ts_id) == DataSetOutcome::Ok);
	TEST_EXPECT(create_data_set(db, ts, ignored) == DataSetOutcome::DuplicateExpansion);
	NewDataSet orphan = ts;
	orphan.expansion_code = "bhx02";
	orphan.parent_id = ts_id; // an expansion is no parent
	TEST_EXPECT(create_data_set(db, orphan, ignored) == DataSetOutcome::NoSuchParent);
	orphan.parent_id = 99999;
	TEST_EXPECT(create_data_set(db, orphan, ignored) == DataSetOutcome::NoSuchParent);

	DataSetPatch patch;
	patch.display_name = "BHD";
	TEST_EXPECT(update_data_set(db, bhd_id, patch) == DataSetOutcome::Ok);
	TEST_EXPECT(find_data_set(db, bhd_id)->display_name == "BHD");
	DataSetPatch lobby;
	lobby.game = "opennova_0_consumer";
	TEST_EXPECT(update_data_set(db, bhd_id, lobby) == DataSetOutcome::Ok);
	TEST_EXPECT(find_data_set(db, bhd_id)->game == "opennova_0_consumer");
	// An expansion lists in its parent's lobby: a game patched onto it changes nothing.
	TEST_EXPECT(update_data_set(db, ts_id, lobby) == DataSetOutcome::Ok);
	TEST_EXPECT(find_data_set(db, ts_id)->game.empty());
	TEST_EXPECT(update_data_set(db, 99999, patch) == DataSetOutcome::NotFound);

	// A base with an expansion cannot go; the expansion can; then the base.
	TEST_EXPECT(delete_data_set(db, bhd_id) == DataSetOutcome::InUse);
	TEST_EXPECT(delete_data_set(db, ts_id) == DataSetOutcome::Ok);
	TEST_EXPECT(delete_data_set(db, bhd_id) == DataSetOutcome::Ok);
	TEST_EXPECT(delete_data_set(db, bhd_id) == DataSetOutcome::NotFound);
	// One a live server runs cannot go.
	insert_server(db, jo, std::nullopt);
	TEST_EXPECT(count_live_servers_of(db, jo) == 1);
	TEST_EXPECT(delete_data_set(db, jo) == DataSetOutcome::InUse);
	return 0;
}

int test_trees_and_offered() {
	TestDatabase tdb;
	Database db = tdb.open();
	const int64_t jo = seeded(db, "Joint Operations"), jox = seeded(db, "Escalation");
	const int64_t dfx2 = seeded(db, "Delta Force Xtreme 2");
	TEST_EXPECT(offered_data_sets(db).empty());

	// An upload: uploading -> finalizing -> ready; the data set's current tree is then this
	// one, and the base is offered with its missions.
	const int64_t t1 = create_tree(db, jo, TreeState::Uploading, "admin");
	TEST_EXPECT(t1 != 0);
	const auto fresh = find_tree(db, t1);
	TEST_EXPECT(fresh && fresh->state == TreeState::Uploading && fresh->files.empty() &&
			fresh->missions.empty() && fresh->created_by == "admin");
	TEST_EXPECT(!finish_tree(db, t1, TreeState::Finalizing, ready_with("JO V1.7.5.3", {})));
	TEST_EXPECT(set_tree_state(db, t1, TreeState::Uploading, TreeState::Finalizing));
	TEST_EXPECT(!set_tree_state(db, t1, TreeState::Uploading, TreeState::Finalizing));
	TEST_EXPECT(finish_tree(db, t1, TreeState::Finalizing,
			ready_with("JO V1.7.5.3", { { "a.bms", "Alpha" }, { "b.bms", "Bravo" } })));
	const auto ready = find_tree(db, t1);
	TEST_EXPECT(ready && ready->state == TreeState::Ready && ready->files.size() == 2 &&
			ready->files[0].name == "resource.pff" && ready->files[0].size == 1000 &&
			ready->missions.size() == 2 && ready->missions[1].title == "Bravo" &&
			ready->version_text == "JO V1.7.5.3" && ready->bytes == 1010 && !ready->ready_at.empty());
	const auto jo_row = find_data_set(db, jo);
	TEST_EXPECT(jo_row && jo_row->current_tree_id == t1);
	std::vector<OfferedDataSet> offered = offered_data_sets(db);
	TEST_EXPECT(offered.size() == 1 && offered[0].id == jo && offered[0].tree_id == t1 &&
			offered[0].title == "jo" && offered[0].missions.size() == 2 && offered[0].expansions.empty());
	TEST_EXPECT(find_offered(offered, jo) != nullptr && find_offered(offered, dfx2) == nullptr);

	// The expansion's tree: offered under its base once ready.
	const int64_t tx = create_tree(db, jox, TreeState::Finalizing, "admin");
	TEST_EXPECT(finish_tree(db, tx, TreeState::Finalizing, ready_with("JOX V1.0", { { "x.bms", "Xray" } })));
	offered = offered_data_sets(db);
	TEST_EXPECT(offered.size() == 1 && offered[0].expansions.size() == 1 &&
			offered[0].expansions[0].id == jox && offered[0].expansions[0].code == "jox01" &&
			offered[0].expansions[0].tree_id == tx && offered[0].expansions[0].missions.size() == 1);
	TEST_EXPECT(find_offered_expansion(offered[0], jox) != nullptr && find_offered_expansion(offered[0], jo) == nullptr);
	// An expansion whose base has no data is not offered: DFX2 with nothing, and jox01's
	// readiness alone would not list Joint Operations.
	TEST_EXPECT(list_trees(db, jo).size() == 1 && list_trees(db, jox).size() == 1);

	// A failed tree keeps its reason; a tree's holds.
	const int64_t t2 = create_tree(db, jo, TreeState::Finalizing, "admin");
	TEST_EXPECT(set_tree_state(db, t2, TreeState::Finalizing, TreeState::Failed, "resource.pff is missing"));
	TEST_EXPECT(find_tree(db, t2)->error == "resource.pff is missing");
	int pinned = 0;
	TEST_EXPECT(tree_hold(db, t1, pinned) == "current");
	TEST_EXPECT(tree_hold(db, t2, pinned) == "");
	// Prunable: the failed one once old enough (not yet: created now), never the current.
	TEST_EXPECT(prunable_trees(db, 3600, 7 * 86400).empty());
	TEST_EXPECT(prunable_trees(db, -1, 7 * 86400) == std::vector<int64_t>{ t2 });
	mark_tree_pruned(db, t2);
	TEST_EXPECT(find_tree(db, t2)->state == TreeState::Pruned && !find_tree(db, t2)->pruned_at.empty());
	TEST_EXPECT(prunable_trees(db, -1, 7 * 86400).empty());
	// A newer tree becomes current; the older one, unpinned, is prunable.
	const int64_t t3 = create_tree(db, jo, TreeState::Finalizing, "admin");
	TEST_EXPECT(finish_tree(db, t3, TreeState::Finalizing, ready_with("JO V1.7.5.4", {})));
	TEST_EXPECT(find_data_set(db, jo)->current_tree_id == t3);
	TEST_EXPECT(prunable_trees(db, -1, 7 * 86400) == std::vector<int64_t>{ t1 });
	// Pinned by a live server, it stays.
	const int64_t server = insert_server(db, jo, std::nullopt);
	set_tree_pins(db, server, t1, std::nullopt);
	TEST_EXPECT(prunable_trees(db, -1, 7 * 86400).empty());
	TEST_EXPECT(tree_hold(db, t1, pinned) == "pinned" && pinned == 1);
	// An abandoned upload is prunable past the window.
	const int64_t t4 = create_tree(db, jo, TreeState::Uploading, "admin");
	TEST_EXPECT(prunable_trees(db, -1, -1) == std::vector<int64_t>{ t4 });
	// A pruned tree whose objects are still there (pruned_at NULL) is prunable.
	db.exec("UPDATE data_set_trees SET state = 'pruned', pruned_at = NULL WHERE id = $1;", { BindValue(t4) });
	TEST_EXPECT(prunable_trees(db, 3600, 7 * 86400) == std::vector<int64_t>{ t4 });
	return 0;
}

int test_pins() {
	TestDatabase tdb;
	Database db = tdb.open();
	const int64_t jo = seeded(db, "Joint Operations"), jox = seeded(db, "Escalation");
	const int64_t dfx2 = seeded(db, "Delta Force Xtreme 2");
	TEST_EXPECT(!current_trees_of(db, jo, std::nullopt));
	const int64_t t_jo = create_tree(db, jo, TreeState::Finalizing, "a");
	TEST_EXPECT(finish_tree(db, t_jo, TreeState::Finalizing, ready_with("1", {})));
	TEST_EXPECT(current_trees_of(db, jo, std::nullopt) && current_trees_of(db, jo, std::nullopt)->base_tree_id == t_jo);
	TEST_EXPECT(!current_trees_of(db, jo, jox)); // the expansion has no data yet
	TEST_EXPECT(!current_trees_of(db, jox, std::nullopt)); // an expansion is no base
	const int64_t t_jox = create_tree(db, jox, TreeState::Finalizing, "a");
	TEST_EXPECT(finish_tree(db, t_jox, TreeState::Finalizing, ready_with("x", {})));
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
	const int64_t t_jox2 = create_tree(db, jox, TreeState::Finalizing, "a");
	TEST_EXPECT(finish_tree(db, t_jox2, TreeState::Finalizing, ready_with("x2", {})));
	TEST_EXPECT(server_outdated(db, s1) && !server_outdated(db, s2));
	TEST_EXPECT(request_update(db, s2) == UpdateOutcome::UpToDate);
	TEST_EXPECT(request_update(db, s1) == UpdateOutcome::Requested);
	TEST_EXPECT(request_update(db, s1) == UpdateOutcome::Requested); // idempotent
	TEST_EXPECT(db.query("SELECT 1 FROM hosted_servers WHERE id = $1 AND update_requested;", { BindValue(s1) }).size() == 1);
	// The start that honours it re-pins and clears the flag.
	set_tree_pins(db, s1, t_jo, t_jox2);
	TEST_EXPECT(db.query("SELECT 1 FROM hosted_servers WHERE id = $1 AND update_requested;", { BindValue(s1) }).empty());
	TEST_EXPECT(!server_outdated(db, s1));
	// A new base tree: both are behind; update-all of the data set marks both, the stopped one never.
	const int64_t t_jo2 = create_tree(db, jo, TreeState::Finalizing, "a");
	TEST_EXPECT(finish_tree(db, t_jo2, TreeState::Finalizing, ready_with("2", {})));
	TEST_EXPECT(request_update_all(db, dfx2) == 0);
	TEST_EXPECT(request_update_all(db, jo) == 2);
	TEST_EXPECT(request_update_all(db, std::nullopt) == 0);
	TEST_EXPECT(db.query("SELECT count(*) FROM hosted_servers WHERE update_requested;").front().as_int(0) == 2);
	return 0;
}

// The migration's mapping: a server on the old game slug and expansion code lands on the
// seeded rows. Migrations 0001-0010 first, a row written as the old service wrote it, then
// 0011 over it.
int test_migration_mapping() {
	test_db::Options options;
	options.migrate = false;
	TestDatabase tdb(options);
	Database db = tdb.open();
	namespace fs = std::filesystem;
	const fs::path all = test_db::source_dir() / "backend" / "migrations";
	const fs::path staged = fs::temp_directory_path() / test_paths_unique("opennova_ds_migrations");
	fs::remove_all(staged);
	fs::create_directories(staged);
	for (const auto &entry : fs::directory_iterator(all)) {
		const std::string name = entry.path().filename().string();
		if (name.rfind("0011", 0) != 0)
			fs::copy_file(entry.path(), staged / name);
	}
	db::run_migrations(db, staged);
	db.exec("INSERT INTO hosted_servers (state, game_name, max_players, time_limit, kill_limit, "
			"max_score, respawn, start_delay, missions, game, expansion) "
			"VALUES ('running', 'old', 32, 30, 50, 5, 5, 0, 'a.bms', 'jop_2_consumer', 'jox01');");
	db.exec("INSERT INTO hosted_servers (state, game_name, max_players, time_limit, kill_limit, "
			"max_score, respawn, start_delay, missions, game, expansion) "
			"VALUES ('stopped', 'old2', 32, 30, 50, 5, 5, 0, 'a.bms', 'dfx2_consumer', '');");
	fs::copy_file(all / "0011_data_sets.sql", staged / "0011_data_sets.sql");
	db::run_migrations(db, staged);
	fs::remove_all(staged);
	const int64_t jo = seeded(db, "Joint Operations"), jox = seeded(db, "Escalation");
	const int64_t dfx2 = seeded(db, "Delta Force Xtreme 2");
	const auto rows = db.query("SELECT game_name, data_set_id, expansion_id, base_tree_id, update_requested "
							   "FROM hosted_servers ORDER BY id;");
	TEST_EXPECT(rows.size() == 2);
	TEST_EXPECT(rows[0].as_int(1) == jo && rows[0].as_int(2) == jox && rows[0].is_null(3) && rows[0].as_int(4) == 0);
	TEST_EXPECT(rows[1].as_int(1) == dfx2 && rows[1].is_null(2));
	TEST_EXPECT(db.query("SELECT 1 FROM information_schema.columns WHERE table_name = 'hosted_servers' "
						 "AND column_name IN ('game', 'expansion');")
					.empty());
	return 0;
}

// The files a tree holds by kind (data_set_files.h): the required ones, the optional ones,
// the canonical spellings and the mission pair.
int test_files() {
	const auto base = tree_file_rules(DataSetKind::Base, "");
	TEST_EXPECT(base.size() == 7 && base[0].name == "resource.pff" && base[0].required &&
			base[2].name == "language.pff" && base[2].required && !base[3].required &&
			base[5].name == "_NSTMOUT.TXT");
	const auto exp = tree_file_rules(DataSetKind::Expansion, "jox01");
	TEST_EXPECT(exp.size() == 4 && exp[0].name == "jox01.pff" && exp[1].name == "jox01L.pff" &&
			exp[2].name == "version.txt" && exp[2].required && exp[3].name == "gt.ssc" && !exp[3].required);
	TEST_EXPECT(canonical_tree_file(DataSetKind::Base, "", "RESOURCE.PFF") == "resource.pff");
	TEST_EXPECT(canonical_tree_file(DataSetKind::Base, "", "_nstmout.txt") == "_NSTMOUT.TXT");
	TEST_EXPECT(canonical_tree_file(DataSetKind::Base, "", "jox01.pff").empty());
	TEST_EXPECT(canonical_tree_file(DataSetKind::Base, "", "Jointops.exe").empty());
	TEST_EXPECT(canonical_tree_file(DataSetKind::Expansion, "jox01", "JOX01L.PFF") == "jox01L.pff");
	TEST_EXPECT(canonical_tree_file(DataSetKind::Expansion, "jox01", "resource.pff").empty());
	TEST_EXPECT(missing_tree_files(DataSetKind::Base, "", { "resource.pff", "localres.pff" }) ==
			std::vector<std::string>{ "language.pff" });
	TEST_EXPECT(missing_tree_files(DataSetKind::Base, "", { "RESOURCE.PFF", "localres.pff", "language.pff" }).empty());
	TEST_EXPECT(missing_tree_files(DataSetKind::Expansion, "jox01", { "jox01.pff" }) ==
			(std::vector<std::string>{ "jox01L.pff", "version.txt" }));
	TEST_EXPECT(mission_pair_of(DataSetKind::Base, "").missions == "localres.pff" &&
			mission_pair_of(DataSetKind::Base, "").texts == "language.pff");
	TEST_EXPECT(mission_pair_of(DataSetKind::Expansion, "jox01").missions == "jox01.pff" &&
			mission_pair_of(DataSetKind::Expansion, "jox01").texts == "jox01L.pff");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	if (int rc = test_files())
		return rc;
	if (int rc = test_data_sets())
		return rc;
	if (int rc = test_trees_and_offered())
		return rc;
	if (int rc = test_pins())
		return rc;
	if (int rc = test_migration_mapping())
		return rc;
	std::printf("data_set_repository: ok\n");
	return 0;
}
