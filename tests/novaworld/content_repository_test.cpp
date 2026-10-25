// The content store's rows (apps/novaworld_common/data_sets/content_repository.h, migration 0025,
// ADR 0069) on a migrated database of the test's own: the five seeded projects on their data sets
// (the retail ones private, each expansion on its base game's project) and who may list them; a
// project created, changed and refused (a duplicate key, a data set another project updates, no
// such data set or base), and one an expansion builds on kept from deletion; the files a store
// holds, each once and at its size; pushes moving the head one at a time, the push that lost told
// the head that won; the history newest first, a page of it, and an author's name kept past their
// account.

#include "common/test_database.h"
#include "common/test_expect.h"

#include "data_sets/content_repository.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::novaworld_server::content;
using opennova::db::BindValue;
using opennova::db::Database;
using test_db::TestDatabase;

namespace {

const std::string kShaA(64, 'a'), kShaB(64, 'b'), kShaC(64, 'c');

// A data set as its seeds name it: the id, the updater login and the display name.
struct SeededDataSet {
	int64_t id = 0;
	std::string login, name;
};
SeededDataSet seeded(Database &db, const std::string &where, const std::vector<BindValue> &binds) {
	const auto rows = db.query(
			"SELECT id, updater_login, display_name FROM data_sets WHERE " + where + ";", binds);
	SeededDataSet d;
	if (rows.size() == 1) {
		d.id = rows.front().as_int(0).value_or(0);
		d.login = rows.front().as_text(1).value_or("");
		d.name = rows.front().as_text(2).value_or("");
	}
	return d;
}
// A retail game's, by the command file its stock install ships.
SeededDataSet retail(Database &db, const char *command_file) {
	return seeded(db, "command_file = $1 AND retail_updater", { BindValue(command_file) });
}
// Our own game's ("base") or its expansion's: the one of the kind the update service serves that
// is no retail game's.
SeededDataSet own(Database &db, const char *kind) {
	return seeded(
			db, "kind = $1 AND updater_login <> '' AND NOT retail_updater", { BindValue(kind) });
}

NewRevision revision(const std::string &message, std::optional<int64_t> author = std::nullopt) {
	NewRevision r;
	r.author_id = author;
	r.created_by = "admin taylor";
	r.message = message;
	r.manifest_sha256 = kShaC;
	r.file_count = 2;
	r.bytes = 30;
	return r;
}

} // namespace

static int test_seeded_projects() {
	TestDatabase tdb;
	Database db = tdb.open();
	const SeededDataSet game = own(db, "base"), expansion = own(db, "expansion");
	const SeededDataSet jo_set = retail(db, "JOPTR.CMD"), jox_set = retail(db, "JOEPTR.CMD"),
						dfx2_set = retail(db, "DFX2.CMD");
	TEST_EXPECT(game.id && expansion.id && jo_set.id && jox_set.id && dfx2_set.id);

	// Every project by key, each byte before the next; our two the public ones.
	const std::vector<ContentProject> all = list_projects(db, true);
	TEST_EXPECT(all.size() == 5);
	for (size_t i = 0; i < all.size(); ++i)
		TEST_EXPECT(all[i].head == 0 && (i == 0 || all[i - 1].key < all[i].key));
	const std::vector<ContentProject> visible = list_projects(db, false);
	TEST_EXPECT(visible.size() == 2 && visible[0].key < visible[1].key);
	for (const ContentProject &p : visible)
		TEST_EXPECT(p.key == game.login || p.key == expansion.login);

	// Our own game's and its expansion's, keyed by their updater logins and titled as their
	// data sets are.
	const auto own_game = find_project(db, game.login),
			   own_expansion = find_project(db, expansion.login);
	TEST_EXPECT(own_game && own_expansion);
	TEST_EXPECT(own_game->title == game.name && own_game->visibility == Visibility::Public &&
			!own_game->retail && !own_game->base_project_id && own_game->data_set_id == game.id);
	TEST_EXPECT(own_expansion->title == expansion.name &&
			own_expansion->visibility == Visibility::Public && !own_expansion->retail &&
			own_expansion->base_project_id == own_game->id &&
			own_expansion->data_set_id == expansion.id);
	// The retail games', private.
	const auto jo = find_project(db, "JO"), jox01 = find_project(db, "JOX01"),
			   dfx2 = find_project(db, "DFX2");
	TEST_EXPECT(jo && jox01 && dfx2);
	TEST_EXPECT(jo->title == "Joint Operations" && jo->visibility == Visibility::Private &&
			jo->retail && !jo->base_project_id && jo->data_set_id == jo_set.id);
	TEST_EXPECT(jox01->visibility == Visibility::Private && jox01->retail &&
			jox01->base_project_id == jo->id && jox01->data_set_id == jox_set.id);
	TEST_EXPECT(dfx2->visibility == Visibility::Private && dfx2->retail &&
			dfx2->data_set_id == dfx2_set.id);
	TEST_EXPECT(find_project_by_id(db, jo->id)->key == "JO" && !find_project(db, "NOPE"));
	std::printf(
			"seeded: our two public, the three retail private, each on its data set and base\n");
	return 0;
}

static int test_project_writes() {
	TestDatabase tdb;
	Database db = tdb.open();
	NewProject mod;
	mod.key = "MOD";
	mod.title = "A mod";
	mod.visibility = Visibility::Public;
	mod.created_by = "admin taylor";
	int64_t id = 0;
	TEST_EXPECT(create_project(db, mod, id) == ProjectOutcome::Ok && id != 0);
	TEST_EXPECT(create_project(db, mod, id) == ProjectOutcome::DuplicateKey);
	NewProject other = mod;
	other.key = "OTHER";
	other.data_set_id = find_project(db, "JO")->data_set_id;
	TEST_EXPECT(create_project(db, other, id) == ProjectOutcome::DataSetTaken);
	other.data_set_id = 999999;
	TEST_EXPECT(create_project(db, other, id) == ProjectOutcome::NoSuchDataSet);
	other.data_set_id.reset();
	other.base_project_id = 999999;
	TEST_EXPECT(create_project(db, other, id) == ProjectOutcome::NoSuchBase);

	const int64_t mod_id = find_project(db, "MOD")->id;
	ProjectPatch patch;
	patch.visibility = Visibility::Private;
	TEST_EXPECT(update_project(db, mod_id, patch) == ProjectOutcome::Ok);
	const auto changed = find_project(db, "MOD");
	TEST_EXPECT(changed->visibility == Visibility::Private && changed->title == "A mod");
	patch = {};
	patch.title = "Renamed";
	TEST_EXPECT(update_project(db, mod_id, patch) == ProjectOutcome::Ok &&
			find_project(db, "MOD")->title == "Renamed");
	TEST_EXPECT(update_project(db, 999999, patch) == ProjectOutcome::NotFound);

	// Joint Operations' project is Escalation's base; the mod's is no one's.
	TEST_EXPECT(delete_project(db, find_project(db, "JO")->id) == ProjectOutcome::InUse);
	TEST_EXPECT(delete_project(db, mod_id) == ProjectOutcome::Ok && !find_project(db, "MOD"));
	TEST_EXPECT(delete_project(db, mod_id) == ProjectOutcome::NotFound);
	std::printf("projects: created, changed, refused, and a base game's kept from deletion\n");
	return 0;
}

static int test_blobs() {
	TestDatabase tdb;
	Database db = tdb.open();
	const int64_t jo = find_project(db, "JO")->id, dfx2 = find_project(db, "DFX2")->id;
	const StoredBlob a{ kShaA, 10 }, b{ kShaB, 20 };
	const auto shas = [](const std::vector<StoredBlob> &blobs) {
		std::vector<std::string> out;
		for (const StoredBlob &blob : blobs)
			out.push_back(blob.sha256);
		return out;
	};
	TEST_EXPECT(missing_blobs(db, jo, {}).empty());
	TEST_EXPECT(
			(shas(missing_blobs(db, jo, { a, b, a })) == std::vector<std::string>{ kShaA, kShaB }));
	record_blobs(db, jo, { a });
	TEST_EXPECT((shas(missing_blobs(db, jo, { b, a })) == std::vector<std::string>{ kShaB }));
	// Held, but at another size: no file a manifest may name.
	TEST_EXPECT(
			(shas(missing_blobs(db, jo, { { kShaA, 11 } })) == std::vector<std::string>{ kShaA }));
	record_blobs(db, jo, { a, b }); // the one held already kept
	TEST_EXPECT(missing_blobs(db, jo, { a, b }).empty());
	TEST_EXPECT(missing_blobs(db, dfx2, { a }).size() == 1); // each project's store its own
	const ProjectStorage storage = project_storage(db, jo);
	TEST_EXPECT(storage.blobs == 2 && storage.bytes == 30);
	TEST_EXPECT(project_storage(db, dfx2).blobs == 0 && project_storage(db, dfx2).bytes == 0);
	std::printf("blobs: each held once, each project's own\n");
	return 0;
}

static int test_pushes() {
	TestDatabase tdb;
	Database db = tdb.open();
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle, role) "
			"VALUES ('taylor', 'x', '0000aaaa', '1', 'Taylor', 'admin');");
	const int64_t author = db.query("SELECT id FROM players WHERE username = 'taylor';")
								   .front()
								   .as_int(0)
								   .value_or(0);
	const int64_t jox01 = find_project(db, "JOX01")->id;

	int64_t head = -1;
	TEST_EXPECT(
			commit_revision(db, jox01, 0, revision("First", author), head) == CommitOutcome::Ok &&
			head == 1);
	// A second push on the same parent loses, told the head that won; nothing of it is written.
	TEST_EXPECT(
			commit_revision(db, jox01, 0, revision("Also first"), head) == CommitOutcome::Stale &&
			head == 1);
	TEST_EXPECT(commit_revision(db, jox01, 5, revision("Ahead"), head) == CommitOutcome::Stale &&
			head == 1);
	TEST_EXPECT(
			commit_revision(db, jox01, 1, revision("Second", author), head) == CommitOutcome::Ok &&
			head == 2);
	TEST_EXPECT(commit_revision(db, jox01, 2, revision("Third"), head) == CommitOutcome::Ok &&
			head == 3);
	TEST_EXPECT(
			commit_revision(db, 999999, 0, revision("Nowhere"), head) == CommitOutcome::NotFound);
	TEST_EXPECT(find_project(db, "JOX01")->head == 3 && find_project(db, "JO")->head == 0);

	const std::vector<ContentRevision> history = list_revisions(db, jox01, 0, 10);
	TEST_EXPECT(history.size() == 3 && history[0].number == 3 && history[2].number == 1);
	TEST_EXPECT(history[2].message == "First" && history[2].author_id == author &&
			history[2].created_by == "admin taylor" && history[2].manifest_sha256 == kShaC &&
			history[2].file_count == 2 && history[2].bytes == 30 && !history[2].created_at.empty());
	const std::vector<ContentRevision> page = list_revisions(db, jox01, 3, 1);
	TEST_EXPECT(page.size() == 1 && page[0].number == 2 && page[0].message == "Second");
	TEST_EXPECT(list_revisions(db, jox01, 1, 10).empty());
	TEST_EXPECT(find_revision(db, jox01, 2)->message == "Second" && !find_revision(db, jox01, 4));

	// The author's account gone: the revision keeps their name.
	db.exec("DELETE FROM players WHERE id = $1;", { BindValue(author) });
	const auto first = find_revision(db, jox01, 1);
	TEST_EXPECT(first && !first->author_id && first->created_by == "admin taylor");
	std::printf(
			"pushes: the head moved one at a time, the loser told the winner, the history paged\n");
	return 0;
}

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	int failures = 0;
	failures += test_seeded_projects();
	failures += test_project_writes();
	failures += test_blobs();
	failures += test_pushes();
	if (failures == 0)
		std::printf("content_repository: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
