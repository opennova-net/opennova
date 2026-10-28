// The projects' rows (apps/novaworld_common/products/project_repository.h, ADR 0069) on a
// migrated database of the test's own: the five seeded games' projects (the retail ones private,
// each expansion on its base game) and who may list them, the runtime none; the one rule of who
// reads, manages and pushes a project (project_rights); a game's project made, renamed and refused
// through the games' rows (product_repository.h: a duplicate key) and gone with its game; an
// account's own projects (made, listed for their owner, counted to their cap, their stores' bytes
// together, deleted with their purges queued before the account goes, product_jobs.h); the files
// a store holds, each once and at its size; pushes moving the head one at a time, the push that
// lost told the head that won, a game with no project taking none; the history newest first, a
// page of it, and an author's name kept past their account.

#include "common/test_database.h"
#include "common/test_expect.h"

#include "products/product_jobs.h"
#include "products/product_repository.h"
#include "products/project_repository.h"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace opennova;
using namespace opennova::novaworld_server::projects;
namespace hosted = opennova::novaworld_server::hosted;
using opennova::db::BindValue;
using opennova::db::Database;
using test_db::TestDatabase;

namespace {

const std::string kShaA(64, 'a'), kShaB(64, 'b'), kShaC(64, 'c');

// A product as its seeds name it: the id, the updater login and the display name.
struct SeededProduct {
	int64_t id = 0;
	std::string login, name;
};
SeededProduct seeded(Database &db, const std::string &where, const std::vector<BindValue> &binds) {
	const auto rows = db.query(
			"SELECT id, updater_login, display_name FROM products WHERE " + where + ";", binds);
	SeededProduct d;
	if (rows.size() == 1) {
		d.id = rows.front().as_int(0).value_or(0);
		d.login = rows.front().as_text(1).value_or("");
		d.name = rows.front().as_text(2).value_or("");
	}
	return d;
}
// A retail game's, by the command file its stock install ships.
SeededProduct retail(Database &db, const char *command_file) {
	return seeded(db, "command_file = $1 AND retail", { BindValue(command_file) });
}
// Our own game's ("game") or its expansion's: the one of the kind the update service serves that
// is no retail game's.
SeededProduct own(Database &db, const char *kind) {
	return seeded(db, "kind = $1 AND updater_login <> '' AND NOT retail", { BindValue(kind) });
}

// The game whose project's key is `key` (product_repository.h).
std::optional<hosted::ProductRow> project(Database &db, std::string_view key) {
	return hosted::find_product_by_key(db, key);
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
	const SeededProduct game = own(db, "game"), expansion = own(db, "expansion");
	const SeededProduct jo_set = retail(db, "JOPTR.CMD"), jox_set = retail(db, "JOEPTR.CMD"),
						dfx2_set = retail(db, "DFX2.CMD");
	TEST_EXPECT(game.id && expansion.id && jo_set.id && jox_set.id && dfx2_set.id);

	// Every project by key, each byte before the next; our two the public ones.
	const std::vector<hosted::ProductRow> all = hosted::list_projects(db, true, std::nullopt);
	TEST_EXPECT(all.size() == 5);
	for (size_t i = 0; i < all.size(); ++i)
		TEST_EXPECT(all[i].head == 0 && (i == 0 || all[i - 1].key < all[i].key));
	const std::vector<hosted::ProductRow> visible = hosted::list_projects(db, false, std::nullopt);
	TEST_EXPECT(visible.size() == 2 && visible[0].key < visible[1].key);
	for (const hosted::ProductRow &p : visible)
		TEST_EXPECT(p.key == game.login || p.key == expansion.login);

	// Our own game's and its expansion's, keyed by their updater logins and titled as their
	// products are.
	const auto own_game = project(db, game.login),
			   own_expansion = project(db, expansion.login);
	TEST_EXPECT(own_game && own_expansion);
	TEST_EXPECT(own_game->display_name == game.name && project_is_public(*own_game) &&
			!own_game->retail && !own_game->parent_id && own_game->id == game.id &&
			!own_game->owner_id && own_game->owner.empty());
	TEST_EXPECT(own_expansion->display_name == expansion.name &&
			project_is_public(*own_expansion) && !own_expansion->retail &&
			own_expansion->parent_id == own_game->id && own_expansion->id == expansion.id);
	// The retail games', private.
	const auto jo = project(db, "JO"), jox01 = project(db, "JOX01"),
			   dfx2 = project(db, "DFX2");
	TEST_EXPECT(jo && jox01 && dfx2);
	TEST_EXPECT(jo->display_name == "Joint Operations" && jo->visibility == "private" &&
			jo->retail && !jo->parent_id && jo->id == jo_set.id);
	TEST_EXPECT(jox01->visibility == "private" && !project_is_public(*jox01) && jox01->retail &&
			jox01->parent_id == jo->id && jox01->id == jox_set.id);
	TEST_EXPECT(dfx2->visibility == "private" && dfx2->retail && dfx2->id == dfx2_set.id);
	TEST_EXPECT(hosted::find_product(db, jo->id)->key == "JO" &&
			!project(db, "NOPE"));
	// The apps: keyed, with no project.
	TEST_EXPECT(hosted::find_product_by_key(db, "ONRUNTIME") &&
			!project_is_public(*hosted::find_product_by_key(db, "ONRUNTIME")));
	for (const hosted::ProductRow &p : hosted::list_projects(db, true, std::nullopt))
		TEST_EXPECT(p.kind != hosted::ProductKind::App);
	TEST_EXPECT(valid_key("JOX01") && valid_key("A_1") && !valid_key("") && !valid_key("jo") &&
			!valid_key("SEVENTEEN_LETTERS") && !valid_key("A-B"));
	// An own key names its updater product (KEY, KEY.CMD): at most 11, with no QQ.
	TEST_EXPECT(valid_owned_key("ELEVEN_CHAR") && !valid_owned_key("TWELVE_CHARS") &&
			!valid_owned_key("MYQQ") && !valid_owned_key("QQ") && valid_owned_key("Q_Q") &&
			!valid_owned_key("my_game"));
	std::printf(
			"seeded: our two public, the three retail private, each on its game and base, the "
			"runtime none\n");
	return 0;
}

// A game's project is its row's: made with it, renamed and made public by its patch, a duplicate
// key refused, gone with it (its revisions and files too).
static int test_project_writes() {
	TestDatabase tdb;
	Database db = tdb.open();
	hosted::NewProduct mod;
	mod.display_name = "A mod";
	mod.kind = hosted::ProductKind::Expansion;
	mod.parent_id = project(db, "JO")->id;
	mod.expansion_code = "mod";
	mod.key = "MOD";
	mod.visibility = "private";
	mod.created_by = "admin taylor";
	int64_t id = 0;
	TEST_EXPECT(hosted::create_product(db, mod, id) == hosted::ProductOutcome::Ok && id != 0);
	const auto made = project(db, "MOD");
	TEST_EXPECT(made && made->id == id && made->display_name == "A mod" &&
			made->visibility == "private" && made->parent_id == project(db, "JO")->id &&
			made->head == 0 && made->created_by == "admin taylor");
	hosted::NewProduct again = mod;
	again.expansion_code = "mod2";
	int64_t ignored = 0;
	TEST_EXPECT(hosted::create_product(db, again, ignored) == hosted::ProductOutcome::DuplicateKey);

	hosted::ProductPatch patch;
	patch.visibility = "public";
	patch.key = "MODDED";
	patch.display_name = "Renamed";
	TEST_EXPECT(hosted::update_product(db, id, patch) == hosted::ProductOutcome::Ok);
	const auto changed = project(db, "MODDED");
	TEST_EXPECT(!project(db, "MOD") && changed && changed->id == id &&
			project_is_public(*changed) && changed->display_name == "Renamed");
	patch = {};
	patch.key = "JO";
	TEST_EXPECT(hosted::update_product(db, id, patch) == hosted::ProductOutcome::DuplicateKey);

	// Its history and files go with it; Joint Operations, which Escalation builds on, stays.
	int64_t head = 0;
	NewRevision revision;
	revision.created_by = "admin taylor";
	revision.message = "First";
	revision.manifest_sha256 = kShaC;
	TEST_EXPECT(commit_revision(db, id, 0, revision, head) == CommitOutcome::Ok && head == 1);
	record_blobs(db, id, { { kShaA, 10 } });
	TEST_EXPECT(hosted::delete_product(db, project(db, "JO")->id) ==
			hosted::ProductOutcome::HasExpansion);
	TEST_EXPECT(hosted::delete_product(db, id) == hosted::ProductOutcome::Ok &&
			!project(db, "MODDED"));
	TEST_EXPECT(db.query("SELECT 1 FROM project_revisions WHERE product_id = $1 UNION ALL "
						 "SELECT 1 FROM project_blobs WHERE product_id = $1;",
						  { BindValue(id) })
						.empty());
	std::printf("projects: made with their games, renamed, refused, gone with them\n");
	return 0;
}

// The one rule every project route answers by: a public and a private project of the service's
// own, an account's own (private, then public) and an app, as no one, a player, the account that
// owns the one, an admin's session and the admin token (which reads and manages, and never pushes:
// it names no author).
static int test_access_rule() {
	hosted::ProductRow open, closed, owned, app;
	open.kind = closed.kind = owned.kind = hosted::ProductKind::Game;
	open.visibility = "public";
	closed.visibility = "private";
	owned.visibility = "private";
	owned.owner_id = 7;
	app.kind = hosted::ProductKind::App;
	const ProjectCaller anyone{}, player{ 8, false }, owner{ 7, false }, admin{ 9, true },
			token{ std::nullopt, true };
	struct Case {
		const hosted::ProductRow *project;
		const ProjectCaller *caller;
		bool read, manage, push;
	};
	const Case cases[] = {
		{ &open, &anyone, true, false, false },
		{ &open, &player, true, false, false },
		{ &open, &owner, true, false, false },
		{ &open, &admin, true, true, true },
		{ &open, &token, true, true, false },
		{ &closed, &anyone, false, false, false },
		{ &closed, &player, false, false, false },
		{ &closed, &owner, false, false, false },
		{ &closed, &admin, true, true, true },
		{ &closed, &token, true, true, false },
		{ &owned, &anyone, false, false, false },
		{ &owned, &player, false, false, false },
		{ &owned, &owner, true, true, true },
		{ &owned, &admin, true, true, true },
		{ &owned, &token, true, true, false },
		{ &app, &anyone, false, false, false },
		{ &app, &admin, false, false, false },
	};
	for (const Case &c : cases) {
		const ProjectRights rights = project_rights(*c.project, *c.caller);
		TEST_EXPECT(rights.read == c.read && rights.manage == c.manage && rights.push == c.push &&
				rights.release == c.push);
	}
	// Made public, anyone reads it; of the players its owner alone changes and pushes it.
	owned.visibility = "public";
	TEST_EXPECT(project_rights(owned, anyone).read && !project_rights(owned, anyone).push);
	TEST_EXPECT(project_rights(owned, player).read && !project_rights(owned, player).manage &&
			!project_rights(owned, player).push);
	TEST_EXPECT(project_rights(owned, owner).manage && project_rights(owned, owner).push);
	std::printf("access: read, manage and push by the one rule\n");
	return 0;
}

// An account's own projects: a game with no lobby and an expansion of it made, the owner named on
// each; listed for their owner beside the public ones; their stores' bytes counted together; at
// most kOwnedProjectsMax an account; deleted with their purges queued (product_jobs.h
// delete_owned_products), refused (nothing deleted) while another account's expansion builds on
// one, none queued without a store; and gone with the account's row all the same.
static int test_owned_projects() {
	TestDatabase tdb;
	Database db = tdb.open();
	const auto player = [&db](const char *name, const char *pcid) {
		db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
				"VALUES ($1, 'x', $2, '1', $1);",
				{ BindValue(name), BindValue(pcid) });
		const auto rows =
				db.query("SELECT id FROM players WHERE username = $1;", { BindValue(name) });
		return rows.empty() ? int64_t(0) : rows.front().as_int(0).value_or(0);
	};
	const auto keys = [](const std::vector<hosted::ProductRow> &rows) {
		std::vector<std::string> out;
		for (const hosted::ProductRow &r : rows)
			out.push_back(r.key);
		return out;
	};
	const auto purges = [&db] {
		const auto rows = db.query("SELECT count(*) FROM jobs WHERE kind = 'product.purge';");
		return rows.empty() ? int64_t(0) : rows.front().as_int(0).value_or(0);
	};
	const int64_t ann = player("ann", "0a110001"), bob = player("bob", "0a110002"),
				  cal = player("cal", "0a110003");
	TEST_EXPECT(ann && bob && cal);

	hosted::NewProduct game;
	game.display_name = "Ann's game";
	game.kind = hosted::ProductKind::Game;
	game.title = "jo";
	game.new_lobby_name = "Ann's lobby";
	game.key = "ANNGAME";
	game.visibility = "private";
	game.created_by = "player ann";
	game.owner_id = ann;
	int64_t game_id = 0, mod_id = 0;
	TEST_EXPECT(hosted::create_product(db, game, game_id) == hosted::ProductOutcome::Ok);
	hosted::NewProduct mod;
	mod.display_name = "Ann's mod";
	mod.kind = hosted::ProductKind::Expansion;
	mod.parent_id = game_id;
	mod.expansion_code = "annmod";
	mod.key = "ANNMOD";
	mod.visibility = "public";
	mod.created_by = "player ann";
	mod.owner_id = ann;
	TEST_EXPECT(hosted::create_product(db, mod, mod_id) == hosted::ProductOutcome::Ok);
	const auto made = hosted::find_product(db, game_id);
	TEST_EXPECT(made && made->owner_id == ann && made->owner == "ann" && !made->lobby.empty() &&
			!made->retail && made->updater_login.empty() && made->visibility == "private");
	TEST_EXPECT(hosted::find_product(db, mod_id)->owner == "ann");

	// Listed: the public ones and the reader's own; every one for an admin.
	TEST_EXPECT((keys(hosted::list_projects(db, false, ann)) ==
			std::vector<std::string>{ "ACIDGAMBIT", "ANNGAME", "ANNMOD", "NIFTY" }));
	TEST_EXPECT((keys(hosted::list_projects(db, false, bob)) ==
			std::vector<std::string>{ "ACIDGAMBIT", "ANNMOD", "NIFTY" }));
	TEST_EXPECT((keys(hosted::list_projects(db, false, std::nullopt)) ==
			std::vector<std::string>{ "ACIDGAMBIT", "ANNMOD", "NIFTY" }));
	TEST_EXPECT(hosted::list_projects(db, true, std::nullopt).size() == 7);
	TEST_EXPECT((keys(hosted::list_owned_projects(db, ann)) ==
			std::vector<std::string>{ "ANNGAME", "ANNMOD" }));
	TEST_EXPECT(hosted::list_owned_projects(db, bob).empty());

	// What their stores hold together: a file each holds counts in each.
	record_blobs(db, game_id, { { kShaA, 100 } });
	record_blobs(db, mod_id, { { kShaA, 100 }, { kShaB, 50 } });
	OwnedUsage usage = owned_usage(db, ann);
	TEST_EXPECT(usage.projects == 2 && usage.bytes == 250 && usage.release_bytes == 0);
	usage = owned_usage(db, bob);
	TEST_EXPECT(usage.projects == 0 && usage.bytes == 0);
	// And their releases' packages, every release not deleted.
	db.exec("INSERT INTO releases (status, product_id, sha256, bytes) VALUES "
			"('draft', $1, 'ann-sha-1', 40), ('retired', $1, 'ann-sha-2', 2);",
			{ BindValue(game_id) });
	usage = owned_usage(db, ann);
	TEST_EXPECT(usage.bytes == 292 && usage.release_bytes == 42);

	// At most kOwnedProjectsMax an account.
	for (int64_t i = 2; i < hosted::kOwnedProjectsMax; ++i) {
		hosted::NewProduct more = game;
		more.key = "ANN" + std::to_string(i);
		int64_t id = 0;
		TEST_EXPECT(hosted::create_product(db, more, id) == hosted::ProductOutcome::Ok);
	}
	hosted::NewProduct past = game;
	past.key = "ANNPAST";
	int64_t ignored = 0;
	TEST_EXPECT(
			hosted::create_product(db, past, ignored) == hosted::ProductOutcome::TooManyProjects);
	TEST_EXPECT(owned_usage(db, ann).projects == hosted::kOwnedProjectsMax &&
			!hosted::find_product_by_key(db, "ANNPAST"));
	// An admin's own take no limit.
	TEST_EXPECT(hosted::owned_limits_apply(db, ann));
	db.exec("UPDATE players SET role = 'admin' WHERE id = $1;", { BindValue(ann) });
	TEST_EXPECT(!hosted::owned_limits_apply(db, ann));
	TEST_EXPECT(hosted::create_product(db, past, ignored) == hosted::ProductOutcome::Ok);
	TEST_EXPECT(hosted::delete_product(db, ignored) == hosted::ProductOutcome::Ok);
	db.exec("UPDATE players SET role = 'player' WHERE id = $1;", { BindValue(ann) });

	// Bob's mod of Ann's game keeps Ann's projects from going, none of them deleted and no
	// purge queued; Bob's own go first, each purge queued.
	hosted::NewProduct bobs = mod;
	bobs.display_name = "Bob's mod";
	bobs.expansion_code = "bobmod";
	bobs.key = "BOBMOD";
	bobs.created_by = "player bob";
	bobs.owner_id = bob;
	int64_t bobs_id = 0;
	TEST_EXPECT(hosted::create_product(db, bobs, bobs_id) == hosted::ProductOutcome::Ok);
	TEST_EXPECT(
			hosted::delete_owned_products(db, ann, true) == hosted::ProductOutcome::HasExpansion);
	TEST_EXPECT(owned_usage(db, ann).projects == hosted::kOwnedProjectsMax && purges() == 0);
	TEST_EXPECT(hosted::delete_owned_products(db, bob, true) == hosted::ProductOutcome::Ok &&
			!hosted::find_product(db, bobs_id) && purges() == 1);
	// Then Ann's: the expansion before the game it builds on, each purge queued with its id.
	TEST_EXPECT(hosted::delete_owned_products(db, ann, true) == hosted::ProductOutcome::Ok);
	TEST_EXPECT(owned_usage(db, ann).projects == 0 && !hosted::find_product(db, game_id) &&
			!hosted::find_product(db, mod_id) && purges() == 1 + hosted::kOwnedProjectsMax);
	TEST_EXPECT(db.query("SELECT 1 FROM jobs WHERE kind = 'product.purge' AND "
						 "(payload->>'product_id')::bigint = $1;",
						  { BindValue(game_id) })
						.size() == 1);
	// Its releases go with it, their packages among its purge's objects.
	TEST_EXPECT(db.query("SELECT 1 FROM releases WHERE sha256 LIKE 'ann-sha-%';").empty());
	TEST_EXPECT(db.query("SELECT 1 FROM jobs WHERE kind = 'product.purge' AND "
						 "(payload->>'product_id')::bigint = $1 AND "
						 "payload->'objects' ? 'updates/ann-sha-1.sfs';",
						  { BindValue(game_id) })
						.size() == 1);
	TEST_EXPECT(db.query("SELECT 1 FROM project_blobs WHERE product_id IN ($1, $2);",
						  { BindValue(game_id), BindValue(mod_id) })
					.empty());
	// Without a store (no objects) none is queued.
	hosted::NewProduct cals = game;
	cals.key = "CALGAME";
	cals.owner_id = cal;
	int64_t cals_id = 0;
	TEST_EXPECT(hosted::create_product(db, cals, cals_id) == hosted::ProductOutcome::Ok);
	TEST_EXPECT(hosted::delete_owned_products(db, cal, false) == hosted::ProductOutcome::Ok &&
			!hosted::find_product(db, cals_id) && purges() == 1 + hosted::kOwnedProjectsMax);

	// An account's row going takes its projects with it all the same, a game and its expansion in
	// the one delete.
	game.key = "CALGAME2";
	game.owner_id = cal;
	TEST_EXPECT(hosted::create_product(db, game, game_id) == hosted::ProductOutcome::Ok);
	mod.parent_id = game_id;
	mod.key = "CALMOD";
	mod.owner_id = cal;
	TEST_EXPECT(hosted::create_product(db, mod, mod_id) == hosted::ProductOutcome::Ok);
	db.exec("DELETE FROM players WHERE id = $1;", { BindValue(cal) });
	TEST_EXPECT(!hosted::find_product(db, game_id) && !hosted::find_product(db, mod_id));
	std::printf("owned: listed for their owner, counted, capped, deleted with their purges\n");
	return 0;
}

static int test_blobs() {
	TestDatabase tdb;
	Database db = tdb.open();
	const int64_t jo = project(db, "JO")->id, dfx2 = project(db, "DFX2")->id;
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
	const int64_t jox01 = project(db, "JOX01")->id;

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
	// An app has no project and takes no revision.
	const int64_t runtime = hosted::find_product_by_key(db, "ONRUNTIME")->id;
	TEST_EXPECT(
			commit_revision(db, runtime, 0, revision("Runtime"), head) == CommitOutcome::NotFound);
	TEST_EXPECT(project(db, "JOX01")->head == 3 && project(db, "JO")->head == 0);

	const std::vector<ProjectRevision> history = list_revisions(db, jox01, 0, 10);
	TEST_EXPECT(history.size() == 3 && history[0].number == 3 && history[2].number == 1);
	TEST_EXPECT(history[2].message == "First" && history[2].author_id == author &&
			history[2].created_by == "admin taylor" && history[2].manifest_sha256 == kShaC &&
			history[2].file_count == 2 && history[2].bytes == 30 && !history[2].created_at.empty());
	const std::vector<ProjectRevision> page = list_revisions(db, jox01, 3, 1);
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
	failures += test_access_rule();
	failures += test_project_writes();
	failures += test_owned_projects();
	failures += test_blobs();
	failures += test_pushes();
	if (failures == 0)
		std::printf("project_repository: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
