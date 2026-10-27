// A release built from a revision of its game's project
// (apps/novaworld_common/products/release_build.h, ADR 0069 d7) on a migrated database of the
// test's own and a local object store, the data program's jobs run on this thread with the real
// tools (opennova-project's export, opennova-update-tool's builder, as child processes). A retail
// game's stock import of a minted install pushed as revision 1 and an edit as revision 2: the
// build exports both and packs the revision whole (every file a row, the stock install's removals
// counted from revision 1, the runtime's own files left out), a package release.validate makes a
// draft of, its one section the game's; published, release.apply builds the game's tree from
// nothing out of it alone, every archive (resource.pff, localres.pff, language.pff) coming back
// equal to the edited revision's export. Our own game's first revision: a full package whose
// tree holds the UPDATE.WIZ naming its product. An account's own game's: its UPDATE.WIZ naming
// the title it runs as too (GAME=jo), and a gt.ssc carrying its own lobby's gate tag under JO's
// key chain, whatever gt.ssc its project held. A retail expansion over its base game's revision
// from folders named neither by the projects' keys nor side by side (the base laid out by its
// key, the expansion's base_project pointed at it). And the refusals: a game that names no
// updater product, a revision that is no project (its export fails, what the tool said kept as
// the build's log), and an expansion whose project names no base game's project. The apps'
// releases from their uploaded builds: the editor's unpacked, its OPENNOVA.WIZ written from its
// row, packed whole with every file of the build (update.exe, revupdat.exe and README.txt among
// them) and validated into a draft of its one section; refused with no update host, and for a
// build that is no zip; the tools' kept as uploaded under their digest, a draft of no section,
// published to their product alone, the same bytes again refused; each upload removed once its
// release is made. The downloads host (products/downloads_job.h) written from the published
// releases into a directory store: Joint Operations' zip the runtime's package applied with the
// served ONRUNTIME.CMD, its UPDATE.WIZ and Escalation's (an account's own mod of it never); an
// account's own public game's the runtime's with its UPDATE.WIZ naming its title (GAME=jo), a
// private one's none; the editor's its package with its ONEDITOR.CMD; the tools' as uploaded; the
// manifest every product of the service's and an account's public game, no private one and no
// account's mod; a retire drops its zip.

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/minted_install.h"

#include "cli_verbs.h"
#include "core/local_object_store.h"
#include "core/zip_files.h"
#include "jobs/worker_pool.h"
#include "products/downloads_job.h"
#include "products/product_jobs.h"
#include "products/product_repository.h"
#include "products/project_repository.h"
#include "products/release_build.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/io/strutil.h>
#include <base/project/project_manifest.h>
#include <formats/pff/pff.h>
#include <formats/update/command_file.h>
#include <formats/update/nlz.h>
#include <formats/update/sfs.h>
#include <net/novacrypto/pubcrypto.h>
#include <net/title/title_identity.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifndef OPENNOVA_PROJECT_TOOL
#error "OPENNOVA_PROJECT_TOOL names opennova-project's binary (tests/novaworld/service_tests.cmake)"
#endif
#ifndef OPENNOVA_UPDATE_TOOL
#error "OPENNOVA_UPDATE_TOOL names opennova-update-tool's binary (tests/novaworld/service_tests.cmake)"
#endif

namespace fs = std::filesystem;

namespace io = opennova::io;
namespace strutil = opennova::strutil;
namespace update = opennova::update;
namespace project = opennova::project;
using namespace opennova::novaworld_server;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;
using test_db::TestDatabase;

namespace {

struct Rig {
	test_temp::TempDir temp;
	TestDatabase scratch;
	ConnectionPool pool;
	Database db;
	LocalObjectStore store;
	jobs::JobRegistry registry;
	int staged = 0;

	explicit Rig(const char *name, const char *update_host = "update.example.com") :
			temp(name), pool(scratch.options()), db(scratch.open()), store(temp.file("store")) {
		hosted::register_product_jobs(registry, store, temp.file("scratch"));
		hosted::ReleaseTools tools;
		tools.project_tool = OPENNOVA_PROJECT_TOOL;
		tools.update_tool = OPENNOVA_UPDATE_TOOL;
		tools.update_host = update_host;
		hosted::register_release_build(registry, store, temp.file("scratch"), tools);
	}
	size_t run_jobs() { return jobs::run_due(pool, registry); }

	// The CLI's verb run in this process (opennova-project's main): its exit code.
	int project(const std::vector<std::string> &args) {
		std::vector<const char *> argv; // argv[0] the verb
		for (const std::string &a : args)
			argv.push_back(a.c_str());
		return project::run_project_command(
				static_cast<int>(argv.size()), argv.data(), stdout, stderr);
	}

	// Every file of the folder but the machine's own and the export folder put in the store as
	// `key`'s, and its manifest pushed as the revision after `parent`: the head after, 0 on
	// failure.
	int64_t push(
			const std::string &key, const std::string &dir, int64_t parent, const char *message) {
		const auto project = hosted::find_product_by_key(db, key);
		if (!project)
			return 0;
		std::vector<opennova::project::ProjectFile> files;
		const fs::path root = io::os_path(dir);
		std::error_code ec;
		for (auto it = fs::recursive_directory_iterator(root, ec);
				!ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
			const std::string relative = io::utf8_generic_path(it->path().lexically_relative(root));
			if (relative == ".opennova" || relative == "build") {
				it.disable_recursion_pending();
				continue;
			}
			std::error_code kind;
			if (!it->is_regular_file(kind))
				continue;
			std::string bytes, error;
			if (!io::read_file_text(io::utf8_path(it->path()), bytes, error))
				return 0;
			opennova::project::ProjectFile file;
			file.path = relative;
			file.sha256 = io::sha256_hex(bytes.data(), bytes.size());
			file.size = bytes.size();
			if (!store.put_file(projects::blob_key(project->id, file.sha256),
						io::utf8_path(it->path()), error))
				return 0;
			files.push_back(file);
		}
		const std::string text = opennova::project::encode_manifest(files);
		const std::string sha = opennova::project::manifest_sha256(text);
		const std::string staged_path = temp.file(("manifest" + std::to_string(++staged)).c_str());
		std::string error;
		if (!io::write_file_atomic(staged_path, text, error) ||
				!store.put_file(projects::manifest_key(project->id, sha), staged_path, error))
			return 0;
		projects::NewRevision revision;
		revision.created_by = "admin taylor";
		revision.message = message;
		revision.manifest_sha256 = sha;
		revision.file_count = static_cast<int64_t>(files.size());
		revision.bytes = static_cast<int64_t>(opennova::project::total_bytes(files));
		int64_t head = 0;
		return projects::commit_revision(db, project->id, parent, revision, head) ==
						projects::CommitOutcome::Ok
				? head
				: 0;
	}

	// Our own game's project as the seeds key it (its updater product's login) with the game's
	// command file and name, whatever the game is called.
	struct OwnGame {
		std::string key, command_file, name;
	};
	OwnGame own_game() {
		const auto rows = db.query(
				"SELECT key, command_file, display_name FROM products "
				"WHERE kind = 'game' AND updater_login <> '' AND NOT retail AND key IS NOT NULL;");
		OwnGame own;
		if (rows.size() == 1) {
			own.key = rows.front().as_text(0).value_or("");
			own.command_file = rows.front().as_text(1).value_or("");
			own.name = rows.front().as_text(2).value_or("");
		}
		return own;
	}

	// A release built from revision `revision` of `key` (an expansion's over its base game's
	// revision `base_revision`), its build queued: its id.
	int64_t release(const std::string &key, int64_t revision, const char *version,
			int64_t base_revision = 0) {
		const auto project = hosted::find_product_by_key(db, key);
		if (!project)
			return 0;
		hosted::NewBuiltRelease made;
		made.name = key + " " + version;
		made.created_by = "admin taylor";
		made.product_id = project->id;
		made.revision = revision;
		made.base_revision = base_revision;
		made.version = version;
		const int64_t id = hosted::create_built_release(db, made);
		hosted::enqueue_release_build(db, id);
		return id;
	}

	// The app `key`'s release of `version` from the file `build` (its uploaded build, as the
	// release routes leave it once its upload completes: building, its build queued): its id.
	int64_t upload_app(const char *key, const std::string &build, const char *version) {
		const auto app = hosted::find_product_by_key(db, key);
		if (!app)
			return 0;
		const int64_t id = hosted::create_release(
				db, std::string(key) + " " + version, 1, "admin taylor", app->id, version);
		std::string error;
		if (!store.put_file(hosted::release_source_key(id), build, error) ||
				!hosted::set_release_status(db, id, hosted::ReleaseStatus::Uploading,
						hosted::ReleaseStatus::Building))
			return 0;
		hosted::enqueue_release_build(db, id);
		return id;
	}
	bool holds(const std::string &key) {
		bool exists = false;
		uint64_t size = 0;
		std::string error;
		return store.head(key, exists, size, error) && exists;
	}

	// The validated release's package, read back from the store.
	bool package(const hosted::ReleaseRow &release, update::SfsPackage &out) {
		const std::string path = temp.file("package.sfs");
		std::string error;
		std::vector<uint8_t> bytes;
		return store.get_file(hosted::release_package_key(release.sha256), path, error) &&
				io::read_file_bytes(path, bytes, error) &&
				update::sfs_read(bytes.data(), bytes.size(), out, error);
	}
};

// The first file named `name` under `dir`, "" for none.
std::string find_file(const std::string &dir, const char *name) {
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(io::os_path(dir), ec);
			!ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		if (strutil::iequals(io::utf8_path(it->path().filename()), name))
			return io::utf8_path(it->path());
	return std::string();
}

const update::SfsEntry *entry_named(const update::SfsSection &section, const char *name) {
	for (const update::SfsEntry &entry : section.entries)
		if (strutil::iequals(entry.name, name))
			return &entry;
	return nullptr;
}

// An archive's entries by their normalized names: each one's bytes (none when it does not open).
std::map<std::string, std::vector<uint8_t>> archive_contents(const std::string &path) {
	namespace pff = opennova::pff;
	std::map<std::string, std::vector<uint8_t>> out;
	pff::PffArchive archive{};
	if (pff::pff_open(&archive, path.c_str()) != 0)
		return out;
	for (uint32_t i = 0; i < archive.entry_count; ++i) {
		const pff::PffEntry &entry = archive.entries[i];
		std::vector<uint8_t> content(entry.size ? entry.size : 1);
		pff::pff_extract(&archive, &entry, content.data(), content.size());
		content.resize(entry.size);
		out[pff::normalized_logical_name(pff::pff_entry_stored_name(entry))] = std::move(content);
	}
	pff::pff_close(&archive);
	return out;
}

} // namespace

// A retail game's stock import as revision 1 and an edit as revision 2: the revision packed whole,
// so the jointops zip installs the game into an empty folder from it (ADR 0067 d6 as amended
// 2026-10-25).
static int test_retail_whole() {
	Rig rig("opennova_release_build_retail");
	const std::string install = rig.temp.file("install"), project = rig.temp.file("JO");
	std::string minted;
	TEST_EXPECT(minted_install::mint(install, minted));
	TEST_EXPECT(rig.project({ "new", project, "--title", "Stock", "--game", "jo", "--install",
						install }) == 0);
	TEST_EXPECT(rig.project({ "import", project, "--all", "--install", install }) == 0);
	TEST_EXPECT(rig.push("JO", project, 0, "stock import") == 1);
	const std::string weapon = find_file(project, "weapon.def");
	std::string text, error;
	TEST_EXPECT(!weapon.empty() && io::read_file_text(weapon, text, error));
	const size_t at = text.find("TEST_WEAPON");
	TEST_EXPECT(at != std::string::npos);
	if (at == std::string::npos)
		return 1;
	text.replace(at, 11, "TEST_RIFLE");
	TEST_EXPECT(io::write_file_atomic(weapon, text, error));
	TEST_EXPECT(rig.push("JO", project, 1, "a rifle") == 2);

	const int64_t id = rig.release("JO", 2, "1.0.1");
	TEST_EXPECT(id > 0);
	rig.run_jobs();
	const auto release = hosted::find_release(rig.db, id);
	TEST_EXPECT(release.has_value());
	if (!release)
		return 1;
	if (release->status != hosted::ReleaseStatus::Draft)
		std::fprintf(stderr, "release %s: %s\n%s\n", hosted::release_status_name(release->status),
				release->error.c_str(), release->build_log.c_str());
	TEST_EXPECT(release->status == hosted::ReleaseStatus::Draft);
	TEST_EXPECT(release->build_log.find("$ ") != std::string::npos &&
			release->build_log.find("laid out JO r1") != std::string::npos);
	const std::vector<hosted::ReleaseSection> sections = hosted::list_sections(rig.db, id);
	const auto set = hosted::find_product_by_key(rig.db, "JO");
	TEST_EXPECT(sections.size() == 1 && set && sections[0].product_id == set->id &&
			release->product_id == set->id && sections[0].command_file == "JOPTR.CMD" &&
			sections[0].version == "1.0.1");
	TEST_EXPECT(!sections.empty() && sections[0].row_count >= 1);
	update::SfsPackage package;
	TEST_EXPECT(rig.package(*release, package) && package.product == "Joint Operations" &&
			package.sections.size() == 1);
	const update::SfsEntry *command =
			package.sections.empty() ? nullptr : entry_named(package.sections[0], "JOPTR.CMD");
	TEST_EXPECT(command != nullptr);
	if (!command)
		return 1;
	const auto cmd =
			update::read_command_file(std::string(command->data.begin(), command->data.end()));
	TEST_EXPECT(cmd.has_value());
	// Whole: the edited weapon.def a row beside the files the edit left as they were, and none
	// of the runtime's own files in a game's folder.
	size_t copies = 0;
	bool edited = false, runtime_file = false;
	for (const update::CommandRow &row : cmd ? cmd->rows : std::vector<update::CommandRow>{}) {
		if (row.type == update::CommandType::Copy || row.type == update::CommandType::Loose)
			++copies;
		edited = edited || strutil::iequals(row.name, "weapon.def");
		runtime_file = runtime_file || strutil::iequals(row.name, "update.exe") ||
				strutil::iequals(row.name, "revupdat.exe") || strutil::iequals(row.name, "README.txt");
	}
	TEST_EXPECT(edited && copies > 1 && !runtime_file);

	// Published, the game's tree is built from the release alone (no stock install beside it):
	// every archive back from nothing, entry for entry as the edited revision exports.
	if (!set)
		return 1;
	TEST_EXPECT(hosted::publish_release(rig.db, id) &&
			hosted::enqueue_release_apply(rig.db, id, set->id) > 0);
	rig.run_jobs();
	const auto game = hosted::find_product(rig.db, set->id);
	TEST_EXPECT(game && game->current_tree_id);
	if (!game || !game->current_tree_id) {
		for (const hosted::ReleaseSection &s : hosted::list_sections(rig.db, id))
			if (s.tree_id)
				if (const auto failed = hosted::find_tree(rig.db, *s.tree_id))
					std::fprintf(stderr, "tree %lld: %s\n", static_cast<long long>(failed->id),
							failed->error.c_str());
		return 1;
	}
	const auto tree = hosted::find_tree(rig.db, *game->current_tree_id);
	TEST_EXPECT(tree && tree->state == hosted::TreeState::Ready && tree->release_id == id &&
			tree->version_text == "1.0.1");
	const std::string expected = rig.temp.file("expected");
	TEST_EXPECT(rig.project({ "export", project, "--out", expected }) == 0);
	size_t entries = 0;
	for (const char *archive : { "resource.pff", "localres.pff", "language.pff" }) {
		const auto built = archive_contents(rig.store.path_of(hosted::tree_prefix(tree->id) + archive));
		const auto want = archive_contents(io::utf8_join(expected, archive));
		TEST_EXPECT(!built.empty() && built == want);
		entries += built.size();
	}
	std::printf("retail whole: JO r2, %zu file row(s) of which weapon.def the edit's, a draft; "
				"its tree built from nothing, %zu archive entries back\n",
			copies, entries);
	return 0;
}

// Our own game's first revision: a full package, its tree's UPDATE.WIZ naming the product.
static int test_own_full() {
	Rig rig("opennova_release_build_own");
	const Rig::OwnGame own = rig.own_game();
	TEST_EXPECT(!own.key.empty() && !own.command_file.empty() && !own.name.empty());
	const std::string project = rig.temp.file("Own");
	TEST_EXPECT(rig.project({ "new", project, "--title", "Own" }) == 0);
	TEST_EXPECT(rig.project({ "create-missing", project }) == 0);
	TEST_EXPECT(rig.push(own.key, project, 0, "first") == 1);
	const int64_t id = rig.release(own.key, 1, "0.1.0");
	rig.run_jobs();
	const auto release = hosted::find_release(rig.db, id);
	TEST_EXPECT(release && release->status == hosted::ReleaseStatus::Draft);
	if (!release || release->status != hosted::ReleaseStatus::Draft) {
		if (release)
			std::fprintf(stderr, "%s\n%s\n", release->error.c_str(), release->build_log.c_str());
		return 1;
	}
	update::SfsPackage package;
	TEST_EXPECT(rig.package(*release, package) && package.product == own.name &&
			package.sections.size() == 1);
	const update::SfsEntry *command = package.sections.empty()
			? nullptr
			: entry_named(package.sections[0], own.command_file.c_str());
	TEST_EXPECT(command != nullptr);
	if (!command)
		return 1;
	const auto cmd =
			update::read_command_file(std::string(command->data.begin(), command->data.end()));
	TEST_EXPECT(cmd.has_value());
	bool wizard = false;
	for (const update::CommandRow &row : cmd ? cmd->rows : std::vector<update::CommandRow>{})
		wizard = wizard || strutil::iequals(row.name, "UPDATE.WIZ");
	TEST_EXPECT(wizard);
	std::printf("own game: %s r1 packed whole, its UPDATE.WIZ among its rows\n", own.key.c_str());
	return 0;
}

// An account's own game (ADR 0074): its updater product its key, its first revision (a project
// holding a gt.ssc of another lobby's) packed whole, its UPDATE.WIZ naming the title it runs as
// (GAME=jo) and its gt.ssc replaced by its own lobby's gate tag, "<key>:0" under JO's key chain.
static int test_player_game() {
	Rig rig("opennova_release_build_player");
	rig.db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
				"VALUES ('maker', 'x', '0bb00001', '1', 'Maker');");
	const auto maker = rig.db.query("SELECT id FROM players WHERE username = 'maker';");
	hosted::NewProduct game;
	game.display_name = "Maker game";
	game.kind = hosted::ProductKind::Game;
	game.title = "jo";
	game.key = "MAKERGAME";
	game.updater_login = "MAKERGAME";
	game.command_file = "MAKERGAME.CMD";
	game.download = hosted::own_download("MAKERGAME");
	game.visibility = "public";
	game.owner_id = maker.empty() ? 0 : maker.front().as_int(0).value_or(0);
	int64_t game_id = 0;
	TEST_EXPECT(hosted::create_product(rig.db, game, game_id) == hosted::ProductOutcome::Ok);
	const std::string project = rig.temp.file("Maker");
	TEST_EXPECT(rig.project({ "new", project, "--title", "Maker" }) == 0);
	TEST_EXPECT(rig.project({ "create-missing", project }) == 0);
	{
		std::string error;
		const opennova::TitleIdentity *jo = opennova::resolve_title_identity("jo", error);
		const std::string other = "acidgambit:0";
		TEST_EXPECT(jo != nullptr &&
				io::write_file_atomic(io::utf8_join(project, "gt.ssc"),
						opennova::encode_key_chain(std::vector<uint8_t>(other.begin(), other.end()),
								jo->gate_tag_keys),
						error));
	}
	TEST_EXPECT(rig.push("MAKERGAME", project, 0, "first") == 1);
	const int64_t id = rig.release("MAKERGAME", 1, "0.1.0");
	rig.run_jobs();
	const auto release = hosted::find_release(rig.db, id);
	TEST_EXPECT(release && release->status == hosted::ReleaseStatus::Draft);
	if (!release || release->status != hosted::ReleaseStatus::Draft) {
		if (release)
			std::fprintf(stderr, "%s\n%s\n", release->error.c_str(), release->build_log.c_str());
		return 1;
	}
	update::SfsPackage package;
	TEST_EXPECT(rig.package(*release, package) && package.sections.size() == 1);
	if (package.sections.size() != 1)
		return 1;
	const auto unpacked = [&package](const char *name) {
		std::vector<uint8_t> content;
		const update::SfsEntry *entry = entry_named(package.sections[0], name);
		if (entry == nullptr ||
				update::nlz_decode(entry->data.data(), entry->data.size(), content) ==
						update::NlzDecode::Corrupt)
			content.clear();
		return content;
	};
	const std::vector<uint8_t> wizard = unpacked("UPDATE.WIZ");
	const std::string wiz(wizard.begin(), wizard.end());
	TEST_EXPECT(wiz.find("CMD=MAKERGAME.CMD\r\n") != std::string::npos &&
			wiz.find("PRODUCT_LOGIN=MAKERGAME\r\n") != std::string::npos &&
			wiz.find("GAME=jo\r\n") != std::string::npos);
	std::string tag, error;
	const opennova::TitleIdentity *jo = opennova::resolve_title_identity("jo", error);
	TEST_EXPECT(jo != nullptr &&
			opennova::decode_gate_tag_file(unpacked("gt.ssc"), jo->gate_tag_keys, tag) &&
			tag == "makergame:0");
	TEST_EXPECT(release->build_log.find("the gate tag makergame:0") != std::string::npos);
	std::printf("player game: MAKERGAME r1 packed whole, its UPDATE.WIZ naming GAME=jo, its gt.ssc "
				"makergame:0\n");
	return 0;
}

// A retail expansion's release over its base game's revision, its author's folders named neither by
// the projects' keys nor side by side as the keys would be ("Joint Operations" and "Escalation"): the
// build lays the base out by its key and points the expansion's base_project at it, the package the
// expansion's section alone. An expansion whose project names no base game's project is refused.
static int test_expansion_folders() {
	Rig rig("opennova_release_build_expansion");
	const std::string install = rig.temp.file("install"), base = rig.temp.file("Joint Operations"),
					  expansion = rig.temp.file("mods/Escalation");
	std::string minted;
	TEST_EXPECT(minted_install::mint(install, minted));
	TEST_EXPECT(rig.project({ "new", base, "--title", "Stock", "--game", "jo", "--install", install }) == 0);
	TEST_EXPECT(rig.project({ "import", base, "--all", "--install", install }) == 0);
	TEST_EXPECT(rig.push("JO", base, 0, "stock import") == 1);
	TEST_EXPECT(rig.project({ "new", expansion, "--title", "Escalation", "--game", "jo", "--expansion", "jox01",
						"--base-project", "../../Joint Operations" }) == 0);
	TEST_EXPECT(rig.project({ "import", expansion, install, "--all", "--expansion", minted_install::kExpansion,
						"--replace" }) == 0);
	TEST_EXPECT(rig.push("JOX01", expansion, 0, "stock import") == 1);
	const int64_t id = rig.release("JOX01", 1, "1.3.0", 1);
	TEST_EXPECT(id > 0);
	rig.run_jobs();
	const auto release = hosted::find_release(rig.db, id);
	TEST_EXPECT(release.has_value());
	if (!release)
		return 1;
	if (release->status != hosted::ReleaseStatus::Draft)
		std::fprintf(stderr, "release %s: %s\n%s\n", hosted::release_status_name(release->status),
				release->error.c_str(), release->build_log.c_str());
	TEST_EXPECT(release->status == hosted::ReleaseStatus::Draft &&
			release->build_log.find("base_project \"../../Joint Operations\" at ../JO") !=
					std::string::npos);
	const std::vector<hosted::ReleaseSection> sections = hosted::list_sections(rig.db, id);
	TEST_EXPECT(sections.size() == 1 && sections[0].command_file == "JOEPTR.CMD");

	// An expansion project on the game install's base game: no base game's project to lay out.
	const std::string loose = rig.temp.file("Loose");
	TEST_EXPECT(rig.project({ "new", loose, "--title", "Loose", "--game", "jo", "--expansion", "jox01",
						"--install", install }) == 0);
	TEST_EXPECT(rig.push("JOX01", loose, 1, "no base project") == 2);
	const int64_t refused = rig.release("JOX01", 2, "1.3.1", 1);
	rig.run_jobs();
	const auto invalid = hosted::find_release(rig.db, refused);
	TEST_EXPECT(invalid && invalid->status == hosted::ReleaseStatus::Invalid &&
			invalid->error.find("names no base game's project") != std::string::npos);
	if (release->status == hosted::ReleaseStatus::Draft)
		std::printf("expansion: JOX01 r1 built over JO r1 from folders of other names; one naming "
					"no base project refused\n");
	return 0;
}

// A game that names no updater product, and a revision that is no project: each release invalid
// with its reason, the second with what the export said.
static int test_refusals() {
	Rig rig("opennova_release_build_refusals");
	hosted::NewProduct loose;
	loose.display_name = "Loose";
	loose.kind = hosted::ProductKind::Game;
	loose.title = "jo";
	loose.lobby = "jop_2_consumer";
	loose.key = "LOOSE";
	loose.visibility = "private";
	int64_t loose_id = 0;
	TEST_EXPECT(hosted::create_product(rig.db, loose, loose_id) == hosted::ProductOutcome::Ok);
	const std::string notes = rig.temp.file("notes");
	std::string error;
	TEST_EXPECT(io::ensure_directory(notes, error) &&
			io::write_file_atomic(notes + "/README.txt", std::string("no project here\n"), error));
	TEST_EXPECT(rig.push("LOOSE", notes, 0, "notes") == 1);
	const int64_t no_set = rig.release("LOOSE", 1, "1.0");
	const Rig::OwnGame own = rig.own_game();
	TEST_EXPECT(rig.push(own.key, notes, 0, "notes") == 1);
	const int64_t no_project = rig.release(own.key, 1, "1.0");
	rig.run_jobs();
	const auto first = hosted::find_release(rig.db, no_set);
	TEST_EXPECT(first && first->status == hosted::ReleaseStatus::Invalid &&
			first->error.find("names no updater product") != std::string::npos);
	const auto second = hosted::find_release(rig.db, no_project);
	TEST_EXPECT(second && second->status == hosted::ReleaseStatus::Invalid &&
			second->error.find("the export of " + own.key + " r1") != std::string::npos &&
			second->build_log.find("$ ") != std::string::npos);
	if (second && second->status == hosted::ReleaseStatus::Invalid)
		std::printf("refusals: no updater product; a revision that is no project (%s)\n",
				second->error.c_str());
	return 0;
}

// The files `names` written into `dir` (each its own name's text), zipped as `zip`: false when
// either fails.
static bool zip_of(
		const std::string &dir, const std::vector<const char *> &names, const std::string &zip) {
	std::string error;
	if (!io::ensure_directory(dir, error))
		return false;
	for (const char *name : names)
		if (!io::write_file_atomic(
					io::utf8_join(dir, name), std::string("synthetic ") + name, error))
			return false;
	return zip_directory(dir, zip, error);
}

// The editor's build zipped and built: its package's one section the editor's, every file of the
// build a row beside the OPENNOVA.WIZ the build wrote (CMD, the update host, the login), its
// upload removed. With no update host, and from a file that is no zip: refused.
static int test_app_build() {
	Rig rig("opennova_release_build_app");
	const std::string zip = rig.temp.file("editor.zip");
	TEST_EXPECT(zip_of(rig.temp.file("editor"),
			{ "oneditor.exe", "oneditor.dll", "onplay.exe", "opennova.dll", "update.exe",
					"revupdat.exe", "README.txt" },
			zip));
	const int64_t id = rig.upload_app("ONEDITOR", zip, "0.0.22");
	TEST_EXPECT(id > 0);
	rig.run_jobs();
	const auto release = hosted::find_release(rig.db, id);
	TEST_EXPECT(release && release->status == hosted::ReleaseStatus::Draft);
	if (!release || release->status != hosted::ReleaseStatus::Draft) {
		if (release)
			std::fprintf(stderr, "%s\n%s\n", release->error.c_str(), release->build_log.c_str());
		return 1;
	}
	TEST_EXPECT(!rig.holds(hosted::release_source_key(id)));
	const std::vector<hosted::ReleaseSection> sections = hosted::list_sections(rig.db, id);
	TEST_EXPECT(sections.size() == 1 && sections[0].command_file == "ONEDITOR.CMD" &&
			sections[0].version == "0.0.22");
	update::SfsPackage package;
	TEST_EXPECT(rig.package(*release, package) && package.product == "OpenNova Editor" &&
			package.sections.size() == 1);
	if (package.sections.size() != 1)
		return 1;
	const update::SfsEntry *command = entry_named(package.sections[0], "ONEDITOR.CMD");
	const auto cmd = command ? update::read_command_file(
									   std::string(command->data.begin(), command->data.end()))
							 : std::nullopt;
	TEST_EXPECT(cmd.has_value());
	std::vector<std::string> rows;
	for (const update::CommandRow &row : cmd ? cmd->rows : std::vector<update::CommandRow>{})
		rows.push_back(strutil::to_lower(row.name));
	for (const char *name : { "oneditor.exe", "onplay.exe", "update.exe", "revupdat.exe",
				 "readme.txt", "opennova.wiz" })
		TEST_EXPECT(std::find(rows.begin(), rows.end(), name) != rows.end());
	const update::SfsEntry *wizard = entry_named(package.sections[0], "OPENNOVA.WIZ");
	std::vector<uint8_t> text;
	TEST_EXPECT(wizard && update::nlz_decode(wizard->data.data(), wizard->data.size(), text) !=
					update::NlzDecode::Corrupt);
	const std::string wiz(text.begin(), text.end());
	TEST_EXPECT(wiz.find("CMD=ONEDITOR.CMD\r\n") != std::string::npos &&
			wiz.find("URL=update.example.com\r\n") != std::string::npos &&
			wiz.find("PRODUCT_LOGIN=ONEDITOR\r\n") != std::string::npos);

	// No update host to name, and a build that is no zip: each refused, its upload kept for the
	// release's delete.
	Rig hostless("opennova_release_build_app_hostless", "");
	const int64_t no_host = hostless.upload_app("ONEDITOR", zip, "0.0.23");
	const std::string junk = hostless.temp.file("junk.zip");
	std::string error;
	TEST_EXPECT(io::write_file_atomic(junk, std::string("no zip"), error));
	hostless.run_jobs();
	const auto refused_host = hosted::find_release(hostless.db, no_host);
	TEST_EXPECT(refused_host && refused_host->status == hosted::ReleaseStatus::Invalid &&
			refused_host->error.find("ONNET_UPDATE_HOST") != std::string::npos);
	const int64_t not_zip = rig.upload_app("ONRUNTIME", junk, "0.0.23");
	rig.run_jobs();
	const auto refused_zip = hosted::find_release(rig.db, not_zip);
	TEST_EXPECT(refused_zip && refused_zip->status == hosted::ReleaseStatus::Invalid &&
			refused_zip->error.find("does not unpack") != std::string::npos &&
			rig.holds(hosted::release_source_key(not_zip)));
	std::printf("app build: ONEDITOR 0.0.22 packed whole with its OPENNOVA.WIZ, a draft; no update "
				"host and no zip refused\n");
	return 0;
}

// The tools' build kept as uploaded: its digest names it in the store, the release a draft of no
// section, published to the tools alone (nothing for the updater); the same bytes again refused.
static int test_download_only() {
	Rig rig("opennova_release_build_download");
	const std::string zip = rig.temp.file("tools.zip");
	TEST_EXPECT(zip_of(rig.temp.file("tools"), { "opennova-serve", "opennova-project" }, zip));
	uint64_t size = 0;
	std::string sha256, error;
	TEST_EXPECT(hosted::digest_file(zip, size, sha256, error));
	const int64_t id = rig.upload_app("ONTOOLS", zip, "0.0.22");
	TEST_EXPECT(id > 0);
	rig.run_jobs();
	const auto release = hosted::find_release(rig.db, id);
	TEST_EXPECT(release && release->status == hosted::ReleaseStatus::Draft &&
			release->sha256 == sha256 && release->bytes == static_cast<int64_t>(size) &&
			release->version == "0.0.22" && hosted::list_sections(rig.db, id).empty());
	TEST_EXPECT(rig.holds(hosted::release_download_key(sha256)) &&
			!rig.holds(hosted::release_source_key(id)));
	TEST_EXPECT(hosted::publish_release(rig.db, id));
	const auto tools = hosted::find_product_by_key(rig.db, "ONTOOLS");
	TEST_EXPECT(tools && tools->published_release_id == id);
	TEST_EXPECT(hosted::published_sections(rig.db).empty());
	const int64_t again = rig.upload_app("ONTOOLS", zip, "0.0.23");
	rig.run_jobs();
	const auto refused = hosted::find_release(rig.db, again);
	TEST_EXPECT(refused && refused->status == hosted::ReleaseStatus::Invalid &&
			refused->error.find("already uploaded as release " + std::to_string(id)) !=
					std::string::npos);
	std::printf("download only: ONTOOLS kept as uploaded, published to itself; the same bytes "
				"refused\n");
	return 0;
}

// `path`'s text, "" when it does not read.
static std::string text_of(const std::string &path) {
	std::string text, error;
	return io::read_file_text(path, text, error) ? text : std::string();
}

// The manifest's entry for `key`, null when it lists none.
static const io::JsonValue *entry_of(const io::JsonValue &manifest, const char *key) {
	const io::JsonValue *products = manifest.get("products");
	if (products == nullptr || !products->is_array())
		return nullptr;
	for (const io::JsonValue &item : products->array)
		if (item.get_string("key", "") == key)
			return &item;
	return nullptr;
}

// Whether the manifest lists `key` as offering no download: its file's fields null.
static bool unoffered(const io::JsonValue &manifest, const char *key) {
	const io::JsonValue *entry = entry_of(manifest, key);
	return entry != nullptr && entry->get("sha256") && entry->get("sha256")->is_null() &&
			entry->get("size")->is_null() && entry->get("release")->is_null() &&
			entry->get("published")->is_null();
}

// The downloads host written from the published releases (products/downloads_job.h) into a
// directory store: the runtime, the editor and the tools built from their uploads and published,
// Joint Operations given a published release. JO's zip is the runtime's package applied with the
// served ONRUNTIME.CMD beside it, its UPDATE.WIZ and Escalation's naming the update host and
// their stock logins; the editor's its package applied with its ONEDITOR.CMD; the tools' their
// upload byte for byte. The manifest lists every product, those offering no download (no file,
// or no published release) with null fields; an object of no product's removed. The editor's
// release retired: its zip gone, its entry null.
static int test_downloads() {
	Rig rig("opennova_release_build_downloads");
	LocalObjectStore downloads(rig.temp.file("downloads"));
	hosted::register_downloads_publish(
			rig.registry, rig.store, &downloads, rig.temp.file("scratch"), "update.example.com");
	const std::string runtime_zip = rig.temp.file("runtime.zip");
	const std::string editor_zip = rig.temp.file("editor.zip");
	const std::string tools_zip = rig.temp.file("tools.zip");
	TEST_EXPECT(zip_of(rig.temp.file("runtime"),
			{ "opennova.exe", "opennova.dll", "update.exe", "revupdat.exe", "README.txt" },
			runtime_zip));
	TEST_EXPECT(zip_of(
			rig.temp.file("editor"), { "oneditor.exe", "onplay.exe", "update.exe" }, editor_zip));
	TEST_EXPECT(zip_of(rig.temp.file("tools"), { "opennova-serve" }, tools_zip));
	const int64_t runtime = rig.upload_app("ONRUNTIME", runtime_zip, "0.0.22");
	const int64_t editor = rig.upload_app("ONEDITOR", editor_zip, "0.0.22");
	const int64_t tools = rig.upload_app("ONTOOLS", tools_zip, "0.0.22");
	rig.run_jobs();
	for (const int64_t id : { runtime, editor, tools })
		TEST_EXPECT(hosted::publish_release(rig.db, id));
	// Joint Operations' published release, as a publish leaves it (the downloads read no game's
	// package: its zip is the runtime's with its wizards).
	const auto jo = hosted::find_product_by_key(rig.db, "JO");
	TEST_EXPECT(jo.has_value());
	if (!jo)
		return 1;
	const int64_t game = hosted::create_release(
			rig.db, "Joint Operations 1.0", 1, "admin taylor", jo->id, "1.0.0");
	rig.db.exec("UPDATE releases SET status = 'published', published_at = now() WHERE id = $1;",
			{ BindValue(game) });
	rig.db.exec("UPDATE products SET published_release_id = $1 WHERE id = $2;",
			{ BindValue(game), BindValue(jo->id) });
	// An account's own: a public game and a private one, each with a published release, and a
	// public mod of Joint Operations.
	rig.db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
				"VALUES ('maker', 'x', '0bb00002', '1', 'Maker');");
	const auto maker = rig.db.query("SELECT id FROM players WHERE username = 'maker';");
	const int64_t maker_id = maker.empty() ? 0 : maker.front().as_int(0).value_or(0);
	const auto own = [&rig, maker_id, &jo](
							 const char *key, const char *visibility, bool expansion) -> int64_t {
		hosted::NewProduct p;
		p.display_name = std::string(key) + " name";
		p.kind = expansion ? hosted::ProductKind::Expansion : hosted::ProductKind::Game;
		p.parent_id = expansion ? std::optional<int64_t>(jo->id) : std::nullopt;
		p.expansion_code = expansion ? "mymod" : "";
		p.title = "jo";
		p.key = key;
		p.updater_login = key;
		p.command_file = std::string(key) + ".CMD";
		p.download = expansion ? std::string() : hosted::own_download(key);
		p.visibility = visibility;
		p.owner_id = maker_id;
		int64_t id = 0;
		if (hosted::create_product(rig.db, p, id) != hosted::ProductOutcome::Ok)
			return 0;
		const int64_t release = hosted::create_release(
				rig.db, std::string(key) + " 1.0", 1, "user maker", id, "1.0");
		rig.db.exec("UPDATE releases SET status = 'published', published_at = now() WHERE id = $1;",
				{ BindValue(release) });
		rig.db.exec("UPDATE products SET published_release_id = $1 WHERE id = $2;",
				{ BindValue(release), BindValue(id) });
		return id;
	};
	TEST_EXPECT(own("PUBGAME", "public", false) && own("PRIVGAME", "private", false) &&
			own("MYMOD", "public", true));
	// An object no product names, a set before this one's.
	const std::string stale = rig.temp.file("stale.zip");
	std::string error;
	TEST_EXPECT(io::write_file_atomic(stale, std::string("old"), error) &&
			downloads.put_file("opennova-old-windows.zip", stale, error));
	TEST_EXPECT(hosted::enqueue_downloads_publish(rig.db) > 0);
	rig.run_jobs();

	const std::string host = downloads.root();
	io::JsonValue manifest;
	TEST_EXPECT(io::json_parse(text_of(io::utf8_join(host, "manifest.json")), manifest, error));
	TEST_EXPECT(manifest.get_string("written", "").size() == 20 &&
			!fs::exists(io::os_path(io::utf8_join(host, "opennova-old-windows.zip"))));
	const io::JsonValue *jo_entry = entry_of(manifest, "JO");
	const std::string jo_zip = io::utf8_join(host, "jointops-game-windows.zip");
	uint64_t size = 0;
	std::string sha256;
	TEST_EXPECT(jo_entry && hosted::digest_file(jo_zip, size, sha256, error) &&
			jo_entry->get_string("file", "") == "jointops-game-windows.zip" &&
			jo_entry->get_string("sha256", "") == sha256 &&
			jo_entry->get("size")->number == static_cast<double>(size) &&
			jo_entry->get_string("kind", "") == "game" &&
			jo_entry->get_string("version", "") == "1.0.0" &&
			jo_entry->get_string("release", "") == "Joint Operations 1.0" &&
			jo_entry->get_string("published", "").size() == 20 &&
			!jo_entry->get_string("description", "").empty());
	TEST_EXPECT(unoffered(manifest, "DFX2") && unoffered(manifest, "ACIDGAMBIT") &&
			unoffered(manifest, "ONBLENDER") && unoffered(manifest, "ONRUNTIME") &&
			entry_of(manifest, "ONRUNTIME")->get("file")->is_null() &&
			unoffered(manifest, "JOX01") &&
			!fs::exists(io::os_path(io::utf8_join(host, "dfx2-game-windows.zip"))));

	// JO's zip: the runtime's package applied, the served command file beside it, the wizards.
	const std::string jo_dir = rig.temp.file("jo-unzipped");
	TEST_EXPECT(unzip_files(jo_zip, jo_dir, error));
	const auto runtime_release = hosted::find_release(rig.db, runtime);
	update::SfsPackage package;
	TEST_EXPECT(runtime_release && rig.package(*runtime_release, package) &&
			package.sections.size() == 1);
	const update::SfsEntry *served =
			package.sections.empty() ? nullptr : entry_named(package.sections[0], "ONRUNTIME.CMD");
	TEST_EXPECT(served &&
			text_of(io::utf8_join(jo_dir, "ONRUNTIME.CMD")) ==
					std::string(served->data.begin(), served->data.end()));
	TEST_EXPECT(text_of(find_file(jo_dir, "opennova.exe")) == "synthetic opennova.exe" &&
			text_of(find_file(jo_dir, "OPENNOVA.WIZ")).find("CMD=ONRUNTIME.CMD\r\n") !=
					std::string::npos);
	const std::string jo_wiz = text_of(io::utf8_join(jo_dir, "UPDATE.WIZ"));
	TEST_EXPECT(jo_wiz.find("CMD=JOPTR.CMD\r\n") != std::string::npos &&
			jo_wiz.find("URL=update.example.com\r\n") != std::string::npos &&
			jo_wiz.find("PRODUCT_LOGIN=JOTRqq\r\n") != std::string::npos);
	const std::string jox_wiz = text_of(io::utf8_join(jo_dir, "expansion/jox01/UPDATE.WIZ"));
	TEST_EXPECT(jox_wiz.find("CMD=JOEPTR.CMD\r\n") != std::string::npos &&
			jox_wiz.find("URL=update.example.com\r\n") != std::string::npos &&
			jox_wiz.find("PRODUCT_LOGIN=JOEqq\r\n") != std::string::npos);
	// An account's own mod is installed in game: no wizard of its in the game's zip, no entry of
	// its in the manifest.
	TEST_EXPECT(!fs::exists(io::os_path(io::utf8_join(jo_dir, "expansion/mymod"))) &&
			entry_of(manifest, "MYMOD") == nullptr && jo_wiz.find("GAME=") == std::string::npos);
	// An account's own public game: its zip the runtime's with its UPDATE.WIZ naming its title; a
	// private one's none, and no entry.
	const std::string pub_zip = io::utf8_join(host, "player-pubgame-game-windows.zip");
	const std::string pub_dir = rig.temp.file("pub-unzipped");
	TEST_EXPECT(unzip_files(pub_zip, pub_dir, error));
	const std::string pub_wiz = text_of(io::utf8_join(pub_dir, "UPDATE.WIZ"));
	TEST_EXPECT(pub_wiz.find("CMD=PUBGAME.CMD\r\n") != std::string::npos &&
			pub_wiz.find("URL=update.example.com\r\n") != std::string::npos &&
			pub_wiz.find("PRODUCT_LOGIN=PUBGAME\r\n") != std::string::npos &&
			pub_wiz.find("GAME=jo\r\n") != std::string::npos &&
			text_of(find_file(pub_dir, "opennova.exe")) == "synthetic opennova.exe" &&
			!fs::exists(io::os_path(io::utf8_join(pub_dir, "expansion"))));
	const io::JsonValue *pub_entry = entry_of(manifest, "PUBGAME");
	TEST_EXPECT(pub_entry &&
			pub_entry->get_string("file", "") == "player-pubgame-game-windows.zip" &&
			pub_entry->get_string("version", "") == "1.0" &&
			pub_entry->get_string("kind", "") == "game");
	TEST_EXPECT(entry_of(manifest, "PRIVGAME") == nullptr &&
			!fs::exists(io::os_path(io::utf8_join(host, "player-privgame-game-windows.zip"))));

	// The editor's: its package applied with its command file; the tools': their upload.
	const std::string editor_dir = rig.temp.file("editor-unzipped");
	TEST_EXPECT(unzip_files(io::utf8_join(host, "opennova-editor-windows.zip"), editor_dir, error));
	TEST_EXPECT(text_of(find_file(editor_dir, "oneditor.exe")) == "synthetic oneditor.exe" &&
			!text_of(io::utf8_join(editor_dir, "ONEDITOR.CMD")).empty() &&
			text_of(find_file(editor_dir, "OPENNOVA.WIZ")).find("PRODUCT_LOGIN=ONEDITOR\r\n") !=
					std::string::npos);
	const io::JsonValue *editor_entry = entry_of(manifest, "ONEDITOR");
	TEST_EXPECT(editor_entry && editor_entry->get_string("release", "") == "ONEDITOR 0.0.22" &&
			editor_entry->get_string("version", "") == "0.0.22" &&
			editor_entry->get_string("kind", "") == "app");
	std::string uploaded, offered;
	TEST_EXPECT(hosted::digest_file(tools_zip, size, uploaded, error) &&
			hosted::digest_file(
					io::utf8_join(host, "opennova-tools-windows.zip"), size, offered, error) &&
			uploaded == offered && entry_of(manifest, "ONTOOLS") &&
			entry_of(manifest, "ONTOOLS")->get_string("sha256", "") == uploaded);

	// The editor's release retired: the host drops its zip, the manifest its fields.
	TEST_EXPECT(hosted::retire_release(rig.db, editor));
	TEST_EXPECT(hosted::enqueue_downloads_publish(rig.db) > 0);
	rig.run_jobs();
	TEST_EXPECT(io::json_parse(text_of(io::utf8_join(host, "manifest.json")), manifest, error));
	TEST_EXPECT(unoffered(manifest, "ONEDITOR") &&
			!fs::exists(io::os_path(io::utf8_join(host, "opennova-editor-windows.zip"))) &&
			fs::exists(io::os_path(jo_zip)));
	std::printf("downloads: JO's zip the runtime's package with its wizards, the editor's its "
				"package, the tools' as uploaded, the manifest every product; a retire drops "
				"its zip\n");
	return 0;
}

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	int failures = 0;
	failures += test_retail_whole();
	failures += test_own_full();
	failures += test_player_game();
	failures += test_expansion_folders();
	failures += test_refusals();
	failures += test_app_build();
	failures += test_download_only();
	failures += test_downloads();
	if (failures == 0)
		std::printf("release_build: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
