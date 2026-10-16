// The hosted data sets' jobs (apps/novaworld_common/hosted/data_set_jobs.h) over a migrated
// database of the test's own and a local object store in a scratch directory, each job run as
// the workers run it (jobs::run_due): a tree uploaded as a packed synthetic install (the three
// archives, two multiplayer missions and a single-player one titled from language.pff,
// version.txt, score.ini) finalized into its manifest, missions, version and bytes,
// trees/<id>/manifest.json beside its files, the data set's current tree following; a tree
// holding a file no base tree takes, and one lacking a required archive, failed with the
// reason; an expansion tree finalized through its own pair; and the prune removing an
// orphaned tree's objects and an abandoned upload, idempotent.

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "update/synthetic_trees.h"

#include "hosted/data_set_files.h"
#include "hosted/data_set_jobs.h"
#include "hosted/data_set_repository.h"
#include "jobs/job_queue.h"
#include "jobs/worker_pool.h"
#include "local_object_store.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <formats/mission/bms.h>
#include <formats/pff/pff.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/mission/mission_catalog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::novaworld_server;
using namespace opennova::novaworld_server::hosted;
namespace jobs = opennova::novaworld_server::jobs;
namespace fs = std::filesystem;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;
using synthetic_trees::bytes;
using synthetic_trees::make_archive;
using synthetic_trees::write_text;
using test_db::TestDatabase;

namespace {

// --- the synthetic install ---

std::vector<uint8_t> bms_blob(const std::string &mission_name, uint32_t attribs) {
	std::vector<uint8_t> out(bms::kHeaderSize, 0);
	out[0] = 'B';
	out[1] = 'M';
	out[2] = 'S';
	out[3] = bms::kMinVersion;
	std::memcpy(out.data() + 4, mission_name.c_str(), std::min<size_t>(mission_name.size(), 31));
	std::memcpy(out.data() + 136, &attribs, sizeof(attribs));
	return out;
}

std::vector<uint8_t> info_bin(const std::string &title, const std::string &briefing) {
	rtxt::File file;
	rtxt::Section info;
	info.name = mission_catalog::kTextInfoSection;
	file.sections.push_back(info);
	rtxt::Entry entry;
	entry.key = mission_catalog::kTextTitleKey;
	entry.text = title;
	entry.section_index = 0;
	file.entries.push_back(entry);
	if (!briefing.empty()) {
		rtxt::Entry b;
		b.key = mission_catalog::kTextBriefingKey;
		b.text = briefing;
		b.section_index = 0;
		file.entries.push_back(b);
	}
	file.normalize_grouping();
	std::vector<uint8_t> out;
	std::string error;
	if (!rtxt::write(file, out, error)) {
		std::fprintf(stderr, "rtxt write failed: %s\n", error.c_str());
		std::exit(1);
	}
	return out;
}

constexpr uint32_t kCoop = static_cast<uint32_t>(bms::AttribFlags::Coop);
constexpr uint32_t kTdm = static_cast<uint32_t>(bms::AttribFlags::TeamDeathmatch);
constexpr const char *kBaseVersion = "Synthetic Product, V1.0.0.0; English";
constexpr const char *kExpVersion = "Synthetic Expansion, V1.0.0.0; English";

// A base product's tree at `dir` as an install holds it: the boot table's three archives
// (two multiplayer missions and a single-player one, their titles in language.pff),
// version.txt and score.ini.
bool make_base_tree(const std::string &dir) {
	const std::vector<pff::PffEditEntry> resource = { { "keep.def", bytes("kept"), 1100000000 },
		{ "wpn.def", bytes("rifle 1"), 1100000000 }, { "drop.def", bytes("x"), 1100000000 } };
	const std::vector<pff::PffEditEntry> localres = { { "alpha.bms",
															  bms_blob("Alpha Header", kCoop),
															  1100000000 },
		{ "bravo.bms", bms_blob("Bravo Header", kTdm), 1100000000 },
		{ "solo.bms", bms_blob("Solo Header", 0), 1100000000 } };
	const std::vector<pff::PffEditEntry> language = {
		{ "alpha.bin", info_bin("Alpha Strike", "Take the island."), 1100000000 },
		{ "bravo.bin", info_bin("Bravo Hold", ""), 1100000000 }
	};
	return make_archive(dir + "/resource.pff", resource) &&
			make_archive(dir + "/localres.pff", localres) &&
			make_archive(dir + "/language.pff", language) &&
			write_text(dir + "/version.txt", kBaseVersion) &&
			write_text(dir + "/score.ini", "[score]\r\nkills=1\r\n");
}

// The jox01 expansion's tree at `dir`: its pair and version.txt.
bool make_expansion_tree(const std::string &dir) {
	const std::vector<pff::PffEditEntry> missions = { { "exp.bms", bms_blob("Exp Header", kCoop),
			1100000000 } };
	const std::vector<pff::PffEditEntry> texts = { { "exp.bin",
			info_bin("Expansion Op", "From the expansion."), 1100000000 } };
	return make_archive(dir + "/jox01.pff", missions) && make_archive(dir + "/jox01L.pff", texts) &&
			write_text(dir + "/version.txt", kExpVersion);
}

std::vector<uint8_t> read_bytes(const std::string &path) {
	std::vector<uint8_t> out;
	std::string error;
	if (!io::read_file_bytes(path, out, error))
		return {};
	return out;
}

// --- the rig ---

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
		register_data_set_jobs(registry, store, temp.file("scratch"));
	}
	// Every job due, run on this thread.
	size_t run_jobs() { return jobs::run_due(pool, registry); }
	std::string job_state(int64_t id) {
		const auto rows = db.query("SELECT state FROM jobs WHERE id = $1;", { BindValue(id) });
		return rows.empty() ? std::string("<gone>") : rows.front().as_text(0).value_or("");
	}
	int64_t seeded(const char *display_name) {
		const auto rows = db.query(
				"SELECT id FROM data_sets WHERE display_name = $1;", { BindValue(display_name) });
		return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
	}
	bool put(const std::string &key, const std::vector<uint8_t> &content) {
		const std::string path = temp.file(("stage" + std::to_string(++staged)).c_str());
		std::string error;
		return io::write_file_atomic(path, content.data(), content.size(), error) &&
				store.put_file(key, path, error);
	}
	bool put_file(const std::string &key, const std::string &path) {
		std::string error;
		return store.put_file(key, path, error);
	}
	// Every file of `dir` put under `prefix` by its own name.
	bool put_dir(const std::string &prefix, const std::string &dir) {
		std::error_code ec;
		for (fs::directory_iterator it(io::os_path(dir), ec), end; !ec && it != end;
				it.increment(ec)) {
			if (!it->is_regular_file(ec))
				continue;
			const std::string name = io::utf8_path(it->path().filename());
			if (!put_file(prefix + name, io::utf8_path(it->path())))
				return false;
		}
		return !ec;
	}
	bool has(const std::string &key) {
		bool exists = false;
		uint64_t size = 0;
		std::string error;
		return store.head(key, exists, size, error) && exists;
	}
	std::vector<uint8_t> get(const std::string &key) {
		const std::string path = temp.file(("fetch" + std::to_string(++staged)).c_str());
		std::string error;
		if (!store.get_file(key, path, error))
			return {};
		return read_bytes(path);
	}
	std::vector<std::string> names_under(const std::string &prefix) {
		std::vector<StoredObject> objects;
		std::string error;
		store.list(prefix, objects, error);
		std::vector<std::string> names;
		for (const StoredObject &o : objects)
			names.push_back(o.key.substr(prefix.size()));
		std::sort(names.begin(), names.end());
		return names;
	}
	// A tree of `data_set_id` from the files at `dir`, finalized by the job: its id, 0 when
	// it did not come out ready.
	int64_t upload_tree(int64_t data_set_id, const std::string &dir) {
		const int64_t id = create_tree(db, data_set_id, TreeState::Uploading, "admin");
		if (!put_dir(tree_prefix(id), dir))
			return 0;
		if (!set_tree_state(db, id, TreeState::Uploading, TreeState::Finalizing) ||
				enqueue_tree_finalize(db, id) == 0)
			return 0;
		run_jobs();
		const auto tree = find_tree(db, id);
		return tree && tree->state == TreeState::Ready ? id : 0;
	}
};

// The manifest object of `tree` read back as the row's files: its tree id, version and files.
bool read_manifest(Rig &rig, int64_t tree, std::string &version, std::vector<TreeFile> &files) {
	const std::vector<uint8_t> text = rig.get(tree_prefix(tree) + kTreeManifestName);
	io::JsonValue root;
	std::string error;
	if (text.empty() || !io::json_parse(std::string(text.begin(), text.end()), root, error))
		return false;
	if (root.get_number("tree", 0) != static_cast<double>(tree))
		return false;
	version = root.get_string("version", "");
	return decode_tree_files(std::string(text.begin(), text.end()), files);
}

// The tree's stored files equal the manifest's names and sizes, and the manifest beside them
// says the same as the row.
int expect_manifest_matches_store(Rig &rig, const DataSetTree &tree) {
	std::vector<std::string> names;
	for (const TreeFile &f : tree.files) {
		const std::vector<uint8_t> content = rig.get(tree_prefix(tree.id) + f.name);
		TEST_EXPECT(!content.empty() && content.size() == f.size);
		names.push_back(f.name);
	}
	names.push_back(kTreeManifestName);
	std::sort(names.begin(), names.end());
	TEST_EXPECT(rig.names_under(tree_prefix(tree.id)) == names);
	std::string version;
	std::vector<TreeFile> files;
	TEST_EXPECT(read_manifest(rig, tree.id, version, files));
	TEST_EXPECT(version == tree.version_text);
	TEST_EXPECT(files.size() == tree.files.size());
	for (size_t i = 0; i < files.size() && i < tree.files.size(); ++i)
		TEST_EXPECT(files[i].name == tree.files[i].name && files[i].size == tree.files[i].size);
	return 0;
}

// --- the cases ---

// An upload of a base data set finalized: the files measured, the version read, the
// multiplayer missions listed with their titles, the manifest beside the files, the data set's
// current tree this one, a prune queued after.
int test_finalize_base() {
	Rig rig("data_set_jobs_finalize");
	const std::string from = rig.temp.file("from");
	TEST_EXPECT(make_base_tree(from));
	const int64_t jo = rig.seeded("Joint Operations");
	const int64_t id = create_tree(rig.db, jo, TreeState::Uploading, "admin");
	TEST_EXPECT(rig.put_dir(tree_prefix(id), from));
	// Not finalizing yet: the job has nothing to do and changes nothing.
	const int64_t early = enqueue_tree_finalize(rig.db, id);
	TEST_EXPECT(early > 0 && rig.run_jobs() == 1);
	TEST_EXPECT(
			find_tree(rig.db, id)->state == TreeState::Uploading && rig.job_state(early) == "done");
	TEST_EXPECT(set_tree_state(rig.db, id, TreeState::Uploading, TreeState::Finalizing));
	const int64_t job = enqueue_tree_finalize(rig.db, id);
	TEST_EXPECT(job > 0 && enqueue_tree_finalize(rig.db, id) == 0); // deduped while queued
	TEST_EXPECT(!jobs::job_pending(rig.db, kJobTreePrune));
	TEST_EXPECT(rig.run_jobs() >= 1);
	TEST_EXPECT(rig.job_state(job) == "done");
	const auto tree = find_tree(rig.db, id);
	TEST_EXPECT(tree && tree->state == TreeState::Ready && tree->error.empty() &&
			!tree->ready_at.empty());
	if (!tree || tree->state != TreeState::Ready) {
		if (tree)
			std::fprintf(stderr, "  tree: %s\n", tree->error.c_str());
		return 1;
	}
	TEST_EXPECT(tree->files.size() == 5);
	const char *names[] = { "language.pff", "localres.pff", "resource.pff", "score.ini",
		"version.txt" };
	int64_t bytes = 0;
	for (size_t i = 0; i < 5 && i < tree->files.size(); ++i) {
		TEST_EXPECT(tree->files[i].name == names[i]);
		const std::vector<uint8_t> content = read_bytes(from + "/" + names[i]);
		TEST_EXPECT(!content.empty() && tree->files[i].size == content.size());
		bytes += static_cast<int64_t>(content.size());
	}
	TEST_EXPECT(tree->bytes == bytes && tree->version_text == kBaseVersion);
	// The catalog's multiplayer rows, titled from language.pff, in its order; the
	// single-player one left out.
	TEST_EXPECT(tree->missions.size() == 2);
	TEST_EXPECT(tree->missions.size() == 2 && tree->missions[0].file == "alpha.bms" &&
			tree->missions[0].title == "Alpha Strike" && tree->missions[1].file == "bravo.bms" &&
			tree->missions[1].title == "Bravo Hold");
	if (int rc = expect_manifest_matches_store(rig, *tree))
		return rc;
	const auto data_set = find_data_set(rig.db, jo);
	TEST_EXPECT(data_set && data_set->current_tree_id == id);
	// The prune ran after it (nothing to prune yet: the job is done, the tree stays).
	TEST_EXPECT(!jobs::job_pending(rig.db, kJobTreePrune));
	TEST_EXPECT(find_tree(rig.db, id)->state == TreeState::Ready);
	// The scratch directory is left clean.
	std::error_code ec;
	TEST_EXPECT(!fs::exists(
			io::os_path(rig.temp.file(("scratch/tree-" + std::to_string(id)).c_str())), ec));
	return 0;
}

// A tree holding what a base tree does not, and one lacking a required archive: failed with
// the reason, the data set untouched.
int test_finalize_refusals() {
	Rig rig("data_set_jobs_refusals");
	const std::string from = rig.temp.file("from");
	TEST_EXPECT(make_base_tree(from));
	const int64_t jo = rig.seeded("Joint Operations");
	// An extra file.
	const int64_t extra = create_tree(rig.db, jo, TreeState::Finalizing, "admin");
	TEST_EXPECT(rig.put_dir(tree_prefix(extra), from));
	TEST_EXPECT(rig.put(tree_prefix(extra) + "Jointops.exe", bytes("MZ")));
	TEST_EXPECT(enqueue_tree_finalize(rig.db, extra) > 0 && rig.run_jobs() >= 1);
	auto tree = find_tree(rig.db, extra);
	TEST_EXPECT(tree && tree->state == TreeState::Failed &&
			tree->error.find("Jointops.exe") != std::string::npos);
	// A required archive missing.
	const int64_t missing = create_tree(rig.db, jo, TreeState::Finalizing, "admin");
	TEST_EXPECT(rig.put_dir(tree_prefix(missing), from));
	std::string error;
	TEST_EXPECT(rig.store.remove(tree_prefix(missing) + "language.pff", error));
	TEST_EXPECT(enqueue_tree_finalize(rig.db, missing) > 0 && rig.run_jobs() >= 1);
	tree = find_tree(rig.db, missing);
	TEST_EXPECT(tree && tree->state == TreeState::Failed &&
			tree->error.find("language.pff") != std::string::npos);
	// An archive that is none.
	const int64_t corrupt = create_tree(rig.db, jo, TreeState::Finalizing, "admin");
	TEST_EXPECT(rig.put_dir(tree_prefix(corrupt), from));
	TEST_EXPECT(rig.put(tree_prefix(corrupt) + "localres.pff", bytes("not an archive")));
	TEST_EXPECT(enqueue_tree_finalize(rig.db, corrupt) > 0 && rig.run_jobs() >= 1);
	tree = find_tree(rig.db, corrupt);
	TEST_EXPECT(tree && tree->state == TreeState::Failed &&
			tree->error.find("localres.pff") != std::string::npos);
	const auto data_set = find_data_set(rig.db, jo);
	TEST_EXPECT(data_set && !data_set->current_tree_id);
	return 0;
}

// An expansion's tree finalized through its own pair, with no base install beside it.
int test_finalize_expansion() {
	Rig rig("data_set_jobs_expansion");
	const std::string from = rig.temp.file("from");
	TEST_EXPECT(make_expansion_tree(from));
	const int64_t jox = rig.seeded("Escalation");
	const int64_t id = rig.upload_tree(jox, from);
	TEST_EXPECT(id != 0);
	if (id == 0)
		return 1;
	const auto tree = find_tree(rig.db, id);
	TEST_EXPECT(tree->files.size() == 3 && tree->files[0].name == "jox01.pff" &&
			tree->files[1].name == "jox01L.pff" && tree->files[2].name == "version.txt");
	TEST_EXPECT(tree->version_text == kExpVersion);
	TEST_EXPECT(tree->missions.size() == 1 && tree->missions[0].file == "exp.bms" &&
			tree->missions[0].title == "Expansion Op");
	TEST_EXPECT(find_data_set(rig.db, jox)->current_tree_id == id);
	if (int rc = expect_manifest_matches_store(rig, *tree))
		return rc;
	// A spelling that is not the canonical one is refused (hostd fetches by the manifest's).
	const int64_t spelled = create_tree(rig.db, jox, TreeState::Finalizing, "admin");
	TEST_EXPECT(rig.put_dir(tree_prefix(spelled), from));
	std::string error;
	TEST_EXPECT(rig.put_file(tree_prefix(spelled) + "JOX01L.PFF", from + "/jox01L.pff") &&
			rig.store.remove(tree_prefix(spelled) + "jox01L.pff", error));
	TEST_EXPECT(enqueue_tree_finalize(rig.db, spelled) > 0 && rig.run_jobs() >= 1);
	TEST_EXPECT(find_tree(rig.db, spelled)->state == TreeState::Failed &&
			find_tree(rig.db, spelled)->error.find("JOX01L.PFF") != std::string::npos);
	return 0;
}

// The prune: an orphaned ready tree's objects removed and its row marked, an abandoned
// upload's parts aborted, the current tree kept; a second run finds nothing.
int test_prune() {
	Rig rig("data_set_jobs_prune");
	const std::string from = rig.temp.file("from");
	TEST_EXPECT(make_base_tree(from));
	const int64_t jo = rig.seeded("Joint Operations");
	const int64_t first = rig.upload_tree(jo, from);
	const int64_t second = rig.upload_tree(jo, from);
	TEST_EXPECT(first != 0 && second != 0);
	if (first == 0 || second == 0)
		return 1;
	// The first is not current now, and old enough.
	rig.db.exec("UPDATE data_set_trees SET created_at = now() - interval '2 hours' WHERE id = $1;",
			{ BindValue(first) });
	// An upload begun eight days ago and never finished.
	const int64_t abandoned = create_tree(rig.db, jo, TreeState::Uploading, "admin");
	rig.db.exec("UPDATE data_set_trees SET created_at = now() - interval '8 days' WHERE id = $1;",
			{ BindValue(abandoned) });
	TEST_EXPECT(rig.put(tree_prefix(abandoned) + "version.txt", bytes("partial")));
	std::string upload_id, error;
	TEST_EXPECT(
			rig.store.multipart_begin(tree_prefix(abandoned) + "resource.pff", upload_id, error));
	const std::string upload_dir = rig.temp.file(("store/.uploads/" + upload_id).c_str());
	std::error_code ec;
	TEST_EXPECT(fs::is_directory(io::os_path(upload_dir), ec));
	// A fresh failed tree is not old enough.
	const int64_t fresh = create_tree(rig.db, jo, TreeState::Finalizing, "admin");
	TEST_EXPECT(set_tree_state(rig.db, fresh, TreeState::Finalizing, TreeState::Failed, "x"));
	TEST_EXPECT(rig.put(tree_prefix(fresh) + "version.txt", bytes("fresh")));

	const int64_t job = enqueue_tree_prune(rig.db);
	TEST_EXPECT(job > 0 && enqueue_tree_prune(rig.db) == 0);
	TEST_EXPECT(rig.run_jobs() == 1 && rig.job_state(job) == "done");
	TEST_EXPECT(rig.names_under(tree_prefix(first)).empty() &&
			rig.names_under(tree_prefix(abandoned)).empty());
	TEST_EXPECT(!fs::exists(io::os_path(upload_dir), ec));
	TEST_EXPECT(rig.names_under(tree_prefix(second)).size() == 6 &&
			rig.has(tree_prefix(fresh) + "version.txt"));
	for (const int64_t id : { first, abandoned }) {
		const auto tree = find_tree(rig.db, id);
		TEST_EXPECT(tree && tree->state == TreeState::Pruned && !tree->pruned_at.empty());
	}
	TEST_EXPECT(find_tree(rig.db, second)->state == TreeState::Ready &&
			find_tree(rig.db, fresh)->state == TreeState::Failed);
	// Again: nothing to prune, nothing changes.
	TEST_EXPECT(enqueue_tree_prune(rig.db) > 0 && rig.run_jobs() == 1);
	TEST_EXPECT(rig.names_under(tree_prefix(second)).size() == 6);
	// The hourly schedule.
	const std::vector<jobs::PeriodicJob> schedule = data_set_schedule();
	TEST_EXPECT(schedule.size() == 1 && schedule[0].kind == kJobTreePrune &&
			schedule[0].period_ms == 60 * 60 * 1000);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "finalize_base", test_finalize_base },
		{ "finalize_refusals", test_finalize_refusals },
		{ "finalize_expansion", test_finalize_expansion },
		{ "prune", test_prune },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the data sets' jobs\n");
	return 0;
}
