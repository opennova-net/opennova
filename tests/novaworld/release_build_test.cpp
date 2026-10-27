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
// tree holds the UPDATE.WIZ naming its product. A retail expansion over its base game's revision
// from folders named neither by the projects' keys nor side by side (the base laid out by its
// key, the expansion's base_project pointed at it). And the refusals: a game that names no
// updater product, a revision that is no project (its export fails, what the tool said kept as
// the build's log), and an expansion whose project names no base game's project.

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/minted_install.h"

#include "cli_verbs.h"
#include "core/local_object_store.h"
#include "jobs/worker_pool.h"
#include "products/product_jobs.h"
#include "products/product_repository.h"
#include "products/project_repository.h"
#include "products/release_build.h"

#include <base/content/content_manifest.h>
#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/io/strutil.h>
#include <formats/pff/pff.h>
#include <formats/update/command_file.h>
#include <formats/update/sfs.h>

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

	explicit Rig(const char *name) :
			temp(name), pool(scratch.options()), db(scratch.open()), store(temp.file("store")) {
		hosted::register_product_jobs(registry, store, temp.file("scratch"));
		hosted::ReleaseTools tools;
		tools.project_tool = OPENNOVA_PROJECT_TOOL;
		tools.update_tool = OPENNOVA_UPDATE_TOOL;
		tools.update_host = "update.example.com";
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
		std::vector<opennova::content::ContentFile> files;
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
			opennova::content::ContentFile file;
			file.path = relative;
			file.sha256 = io::sha256_hex(bytes.data(), bytes.size());
			file.size = bytes.size();
			if (!store.put_file(content::blob_key(project->id, file.sha256),
						io::utf8_path(it->path()), error))
				return 0;
			files.push_back(file);
		}
		const std::string text = opennova::content::encode_manifest(files);
		const std::string sha = opennova::content::manifest_sha256(text);
		const std::string staged_path = temp.file(("manifest" + std::to_string(++staged)).c_str());
		std::string error;
		if (!io::write_file_atomic(staged_path, text, error) ||
				!store.put_file(content::manifest_key(project->id, sha), staged_path, error))
			return 0;
		content::NewRevision revision;
		revision.created_by = "admin taylor";
		revision.message = message;
		revision.manifest_sha256 = sha;
		revision.file_count = static_cast<int64_t>(files.size());
		revision.bytes = static_cast<int64_t>(opennova::content::total_bytes(files));
		int64_t head = 0;
		return content::commit_revision(db, project->id, parent, revision, head) ==
						content::CommitOutcome::Ok
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
				"WHERE kind = 'base' AND updater_login <> '' AND NOT retail AND key IS NOT NULL;");
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
		made.content_revision = revision;
		made.base_revision = base_revision;
		made.version = version;
		const int64_t id = hosted::create_built_release(db, made);
		hosted::enqueue_release_build(db, id);
		return id;
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
	loose.kind = hosted::ProductKind::Base;
	loose.title = "jo";
	loose.game = "jop_2_consumer";
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

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	int failures = 0;
	failures += test_retail_whole();
	failures += test_own_full();
	failures += test_expansion_folders();
	failures += test_refusals();
	if (failures == 0)
		std::printf("release_build: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
