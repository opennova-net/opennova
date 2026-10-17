// The hosted data sets' jobs (apps/novaworld_common/hosted/data_set_jobs.h) over a migrated
// database of the test's own and a local object store in a scratch directory, each job run as
// the workers run it (jobs::run_due): a baseline tree uploaded as a packed synthetic install
// (the three archives, two multiplayer missions and a single-player one titled from
// language.pff, version.txt, score.ini) finalized into its manifest, missions, version and
// bytes, trees/<id>/manifest.json (names and sizes) beside its files, the data set's current
// and baseline trees following; a tree holding a file no base tree takes, and one lacking a
// required archive, failed with the reason; an expansion tree finalized through its own pair;
// a 1SFS package opennova-update-tool built from the synthetic baseline and its updated tree
// validated into a draft with its two sections and versions, the object moved under its
// digest, a package whose command file names no data set and a second upload of the same
// bytes refused; a published release applied to the baselines (the release trees' files equal
// the updated tree's: the loose files byte for byte, the archives entry for entry, a file the
// tree does not hold left out, a removed one gone, the version and the missions redone, a run
// again on a building tree continuing on its row), and applied with no baseline failing so;
// and the prune removing an orphaned tree's objects and an abandoned upload, idempotent.

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
#include "package_builder.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <formats/mission/bms.h>
#include <formats/pff/pff.h>
#include <formats/rtxt/rtxt.h>
#include <formats/update/sfs.h>
#include <runtime/mission/mission_catalog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
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
constexpr uint32_t kCtf = static_cast<uint32_t>(bms::AttribFlags::CaptureTheFlag);
constexpr const char *kBaseVersion = "Synthetic Product, V1.0.0.0; English";
constexpr const char *kBaseVersionUpdated = "Synthetic Product, V1.0.0.10; English";
constexpr const char *kExpVersion = "Synthetic Expansion, V1.0.0.0; English";
constexpr const char *kExpVersionUpdated = "Synthetic Expansion, V1.0.0.11; English";

// A base product's tree at `dir` as an install holds it: the boot table's three archives,
// version.txt and score.ini. `updated` is the tree a release takes it to: an entry changed
// and one added in resource.pff, one dropped; a mission added to localres.pff with its title
// in language.pff; version.txt bumped; score.ini gone; _NSTMOUT.TXT added; and readme.txt, a
// loose file no tree holds.
bool make_base_tree(const std::string &dir, bool updated) {
	std::vector<pff::PffEditEntry> resource = { { "keep.def", bytes("kept"), 1100000000 },
		{ "wpn.def", bytes(updated ? "rifle 2" : "rifle 1"),
				updated ? 1700000000u : 1100000000u } };
	if (updated)
		resource.push_back({ "add.def", bytes("new"), 1700000001 });
	else
		resource.push_back({ "drop.def", bytes("x"), 1100000000 });
	std::vector<pff::PffEditEntry> localres = { { "alpha.bms", bms_blob("Alpha Header", kCoop),
														1100000000 },
		{ "bravo.bms", bms_blob("Bravo Header", kTdm), 1100000000 },
		{ "solo.bms", bms_blob("Solo Header", 0), 1100000000 } };
	std::vector<pff::PffEditEntry> language = {
		{ "alpha.bin", info_bin("Alpha Strike", "Take the island."), 1100000000 },
		{ "bravo.bin", info_bin("Bravo Hold", ""), 1100000000 }
	};
	if (updated) {
		localres.push_back({ "charlie.bms", bms_blob("Charlie Header", kCtf), 1700000002 });
		language.push_back({ "charlie.bin", info_bin("Charlie Flag", "Two flags."), 1700000003 });
	}
	bool ok = make_archive(dir + "/resource.pff", resource) &&
			make_archive(dir + "/localres.pff", localres) &&
			make_archive(dir + "/language.pff", language) &&
			write_text(dir + "/version.txt", updated ? kBaseVersionUpdated : kBaseVersion);
	if (updated)
		ok = ok && write_text(dir + "/_NSTMOUT.TXT", "60\r\n") &&
				write_text(dir + "/readme.txt", "not a file of the tree");
	else
		ok = ok && write_text(dir + "/score.ini", "[score]\r\nkills=1\r\n");
	return ok;
}

// The jox01 expansion's tree at `dir`: its pair and version.txt; `updated` adds a mission and
// bumps the version.
bool make_expansion_tree(const std::string &dir, bool updated) {
	std::vector<pff::PffEditEntry> missions = { { "exp.bms", bms_blob("Exp Header", kCoop),
			1100000000 } };
	std::vector<pff::PffEditEntry> texts = { { "exp.bin",
			info_bin("Expansion Op", "From the expansion."), 1100000000 } };
	if (updated) {
		missions.push_back({ "delta.bms", bms_blob("Delta Header", kCoop), 1700000004 });
		texts.push_back({ "delta.bin", info_bin("Delta Op", ""), 1700000005 });
	}
	return make_archive(dir + "/jox01.pff", missions) && make_archive(dir + "/jox01L.pff", texts) &&
			write_text(dir + "/version.txt", updated ? kExpVersionUpdated : kExpVersion);
}

// `from` and `to` under `root`: the base at the root, the expansion under expansion/jox01.
bool make_trees(const std::string &root) {
	return make_base_tree(root + "/from", false) &&
			make_expansion_tree(root + "/from/expansion/jox01", false) &&
			make_base_tree(root + "/to", true) &&
			make_expansion_tree(root + "/to/expansion/jox01", true);
}

std::vector<uint8_t> read_bytes(const std::string &path) {
	std::vector<uint8_t> out;
	std::string error;
	if (!io::read_file_bytes(path, out, error))
		return {};
	return out;
}

// An archive's entries: normalized name -> {content, timestamp}.
struct Entry {
	std::vector<uint8_t> content;
	uint32_t timestamp = 0;
	bool operator==(const Entry &o) const {
		return content == o.content && timestamp == o.timestamp;
	}
};
std::map<std::string, Entry> archive_entries(const std::string &path) {
	std::map<std::string, Entry> out;
	pff::PffArchive archive{};
	if (pff::pff_open(&archive, path.c_str()) != 0)
		return out;
	for (uint32_t i = 0; i < archive.entry_count; ++i) {
		const pff::PffEntry &entry = archive.entries[i];
		Entry item;
		item.timestamp = entry.timestamp;
		item.content.resize(entry.size ? entry.size : 1);
		pff::pff_extract(&archive, &entry, item.content.data(), item.content.size());
		item.content.resize(entry.size);
		out[pff::normalized_logical_name(pff::pff_entry_stored_name(entry))] = std::move(item);
	}
	pff::pff_close(&archive);
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
	// A baseline tree of `data_set_id` from the files at `dir`, finalized by the job: its id,
	// 0 when it did not come out ready.
	int64_t upload_baseline(int64_t data_set_id, const std::string &dir) {
		const int64_t id = create_tree(db, data_set_id, TreeSource::Baseline, TreeState::Uploading,
				std::nullopt, std::nullopt, "admin");
		if (!put_dir(tree_prefix(id), dir))
			return 0;
		if (!set_tree_state(db, id, TreeState::Uploading, TreeState::Finalizing) ||
				enqueue_tree_finalize(db, id) == 0)
			return 0;
		run_jobs();
		const auto tree = find_tree(db, id);
		return tree && tree->state == TreeState::Ready ? id : 0;
	}
	// The synthetic data sets the package's command files name: the base and its expansion.
	bool make_synthetic_data_sets(int64_t &base, int64_t &expansion) {
		NewDataSet b;
		b.display_name = "Synthetic";
		b.kind = DataSetKind::Base;
		b.title = "jo";
		b.game = "jop_2_consumer";
		b.updater_login = "SYNus";
		b.command_file = "SYNPTR.CMD";
		b.created_by = "admin";
		if (create_data_set(db, b, base) != DataSetOutcome::Ok)
			return false;
		NewDataSet e;
		e.display_name = "Synthetic Expansion";
		e.kind = DataSetKind::Expansion;
		e.parent_id = base;
		e.expansion_code = "jox01";
		e.updater_login = "SYNEus";
		e.command_file = "SYNEPTR.CMD";
		e.created_by = "admin";
		return create_data_set(db, e, expansion) == DataSetOutcome::Ok;
	}
	// The package built from `root`'s from and to trees, uploaded as a new release and
	// validated by the job: its id; `package` the package as built.
	int64_t upload_release(const std::string &root, const char *name, update::SfsPackage &package,
			std::vector<update_tool::ProductSpec> products = {}) {
		update_tool::BuildSpec spec;
		spec.from = io::os_path(root + "/from");
		spec.to = io::os_path(root + "/to");
		spec.package_product = "Synthetic Product";
		if (products.empty()) {
			spec.products.push_back({ "", "", "SYNPTR.CMD", "", "", "" });
			spec.products.push_back({ "expansion/jox01", "jox01", "SYNEPTR.CMD", "", "", "" });
		} else {
			spec.products = std::move(products);
		}
		std::vector<update_tool::ProductReport> report;
		std::string error;
		if (!update_tool::build_update_package(spec, package, report, error)) {
			std::fprintf(stderr, "  build: %s\n", error.c_str());
			return 0;
		}
		const std::vector<uint8_t> packed = update::sfs_write(package);
		const int64_t id = create_release(db, name, static_cast<int64_t>(packed.size()), "admin");
		if (!put(release_upload_key(id), packed))
			return 0;
		if (!set_release_status(db, id, ReleaseStatus::Uploading, ReleaseStatus::Validating) ||
				enqueue_release_validate(db, id) == 0)
			return 0;
		run_jobs();
		return id;
	}
};

const TreeFile *file_named(const std::vector<TreeFile> &files, const std::string &name) {
	for (const TreeFile &f : files)
		if (f.name == name)
			return &f;
	return nullptr;
}

std::string sha_of(const std::vector<uint8_t> &content) {
	return io::sha256_hex(content.data(), content.size());
}

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

// A baseline upload of a base data set finalized: the files measured, the version read, the
// multiplayer missions listed with their titles, the manifest beside the files, the data set's
// current and baseline trees this one, a prune queued after.
int test_finalize_baseline() {
	Rig rig("data_set_jobs_finalize");
	const std::string from = rig.temp.file("from");
	TEST_EXPECT(make_base_tree(from, false));
	const int64_t jo = rig.seeded("Joint Operations");
	const int64_t id = create_tree(rig.db, jo, TreeSource::Baseline, TreeState::Uploading,
			std::nullopt, std::nullopt, "admin");
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
	TEST_EXPECT(data_set && data_set->current_tree_id == id && data_set->baseline_tree_id == id);
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
	TEST_EXPECT(make_base_tree(from, false));
	const int64_t jo = rig.seeded("Joint Operations");
	// An extra file.
	const int64_t extra = create_tree(rig.db, jo, TreeSource::Baseline, TreeState::Finalizing,
			std::nullopt, std::nullopt, "admin");
	TEST_EXPECT(rig.put_dir(tree_prefix(extra), from));
	TEST_EXPECT(rig.put(tree_prefix(extra) + "Jointops.exe", bytes("MZ")));
	TEST_EXPECT(enqueue_tree_finalize(rig.db, extra) > 0 && rig.run_jobs() >= 1);
	auto tree = find_tree(rig.db, extra);
	TEST_EXPECT(tree && tree->state == TreeState::Failed &&
			tree->error.find("Jointops.exe") != std::string::npos);
	// A required archive missing.
	const int64_t missing = create_tree(rig.db, jo, TreeSource::Baseline, TreeState::Finalizing,
			std::nullopt, std::nullopt, "admin");
	TEST_EXPECT(rig.put_dir(tree_prefix(missing), from));
	std::string error;
	TEST_EXPECT(rig.store.remove(tree_prefix(missing) + "language.pff", error));
	TEST_EXPECT(enqueue_tree_finalize(rig.db, missing) > 0 && rig.run_jobs() >= 1);
	tree = find_tree(rig.db, missing);
	TEST_EXPECT(tree && tree->state == TreeState::Failed &&
			tree->error.find("language.pff") != std::string::npos);
	// An archive that is none.
	const int64_t corrupt = create_tree(rig.db, jo, TreeSource::Baseline, TreeState::Finalizing,
			std::nullopt, std::nullopt, "admin");
	TEST_EXPECT(rig.put_dir(tree_prefix(corrupt), from));
	TEST_EXPECT(rig.put(tree_prefix(corrupt) + "localres.pff", bytes("not an archive")));
	TEST_EXPECT(enqueue_tree_finalize(rig.db, corrupt) > 0 && rig.run_jobs() >= 1);
	tree = find_tree(rig.db, corrupt);
	TEST_EXPECT(tree && tree->state == TreeState::Failed &&
			tree->error.find("localres.pff") != std::string::npos);
	const auto data_set = find_data_set(rig.db, jo);
	TEST_EXPECT(data_set && !data_set->current_tree_id && !data_set->baseline_tree_id);
	return 0;
}

// An expansion's baseline finalized through its own pair, with no base install beside it.
int test_finalize_expansion() {
	Rig rig("data_set_jobs_expansion");
	const std::string from = rig.temp.file("from");
	TEST_EXPECT(make_expansion_tree(from, false));
	const int64_t jox = rig.seeded("Escalation");
	const int64_t id = rig.upload_baseline(jox, from);
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
	const int64_t spelled = create_tree(rig.db, jox, TreeSource::Baseline, TreeState::Finalizing,
			std::nullopt, std::nullopt, "admin");
	TEST_EXPECT(rig.put_dir(tree_prefix(spelled), from));
	std::string error;
	TEST_EXPECT(rig.put_file(tree_prefix(spelled) + "JOX01L.PFF", from + "/jox01L.pff") &&
			rig.store.remove(tree_prefix(spelled) + "jox01L.pff", error));
	TEST_EXPECT(enqueue_tree_finalize(rig.db, spelled) > 0 && rig.run_jobs() >= 1);
	TEST_EXPECT(find_tree(rig.db, spelled)->state == TreeState::Failed &&
			find_tree(rig.db, spelled)->error.find("JOX01L.PFF") != std::string::npos);
	return 0;
}

// The package validated: a draft with its sections and versions, the object under its digest;
// then a package naming no data set, and the same bytes again, invalid.
int test_validate() {
	Rig rig("data_set_jobs_validate");
	TEST_EXPECT(make_trees(rig.temp.root()));
	int64_t base = 0, expansion = 0;
	TEST_EXPECT(rig.make_synthetic_data_sets(base, expansion));
	update::SfsPackage package;
	const int64_t id = rig.upload_release(rig.temp.root(), "1.0.0.10", package);
	TEST_EXPECT(id != 0);
	if (id == 0)
		return 1;
	const auto release = find_release(rig.db, id);
	TEST_EXPECT(release && release->status == ReleaseStatus::Draft);
	if (!release || release->status != ReleaseStatus::Draft) {
		if (release)
			std::fprintf(stderr, "  release: %s\n", release->error.c_str());
		return 1;
	}
	const std::vector<uint8_t> packed = update::sfs_write(package);
	TEST_EXPECT(release->sha256 == sha_of(packed) &&
			release->bytes == static_cast<int64_t>(packed.size()) &&
			release->package_product == "Synthetic Product" && release->error.empty());
	const std::vector<ReleaseSection> sections = list_sections(rig.db, id);
	TEST_EXPECT(sections.size() == 2);
	if (sections.size() != 2)
		return 1;
	// The base: version.txt, _NSTMOUT.TXT and readme.txt written, score.ini removed; the three
	// archives each a TARGET with their rows (charlie.bin; charlie.bms; wpn.def, add.def,
	// drop.def).
	TEST_EXPECT(sections[0].data_set_id == base && sections[0].expansion.empty() &&
			sections[0].command_file == "SYNPTR.CMD" && sections[0].version == "1.0.0.10" &&
			sections[0].row_count == 12 && sections[0].blob_bytes > 0 && !sections[0].tree_id);
	// The expansion: version.txt; delta.bms and delta.bin under their archives.
	TEST_EXPECT(sections[1].data_set_id == expansion && sections[1].expansion == "jox01" &&
			sections[1].command_file == "SYNEPTR.CMD" && sections[1].version == "1.0.0.11" &&
			sections[1].row_count == 5);
	TEST_EXPECT(rig.has(release_package_key(release->sha256)) && !rig.has(release_upload_key(id)));
	TEST_EXPECT(rig.get(release_package_key(release->sha256)) == packed);

	// The same bytes again: invalid, naming the first.
	update::SfsPackage again;
	const int64_t dup = rig.upload_release(rig.temp.root(), "again", again);
	TEST_EXPECT(dup != 0 && find_release(rig.db, dup)->status == ReleaseStatus::Invalid);
	TEST_EXPECT(find_release(rig.db, dup)->error.find("release " + std::to_string(id)) !=
			std::string::npos);
	TEST_EXPECT(
			rig.has(release_upload_key(dup))); // the upload stays for the delete route to remove

	// A command file no data set takes.
	update::SfsPackage nope;
	const int64_t unmapped = rig.upload_release(
			rig.temp.root(), "nope", nope, { { "", "", "NOPE.CMD", "", "", "" } });
	TEST_EXPECT(unmapped != 0 && find_release(rig.db, unmapped)->status == ReleaseStatus::Invalid);
	TEST_EXPECT(find_release(rig.db, unmapped)->error.find("command file") != std::string::npos);
	TEST_EXPECT(!rig.has(release_package_key(sha_of(update::sfs_write(nope)))));

	// Not a package at all.
	const int64_t junk = create_release(rig.db, "junk", 4, "admin");
	TEST_EXPECT(rig.put(release_upload_key(junk), bytes("junk")));
	TEST_EXPECT(
			set_release_status(rig.db, junk, ReleaseStatus::Uploading, ReleaseStatus::Validating));
	TEST_EXPECT(enqueue_release_validate(rig.db, junk) > 0 && rig.run_jobs() >= 1);
	TEST_EXPECT(find_release(rig.db, junk)->status == ReleaseStatus::Invalid &&
			find_release(rig.db, junk)->error.find("1SFS") != std::string::npos);
	return 0;
}

// A published release applied: a release tree per section built from its baseline, equal to
// the updated tree; a run again on a building tree continues on its row.
int test_apply() {
	Rig rig("data_set_jobs_apply");
	const std::string root = rig.temp.root();
	TEST_EXPECT(make_trees(root));
	int64_t base = 0, expansion = 0;
	TEST_EXPECT(rig.make_synthetic_data_sets(base, expansion));
	const int64_t base_tree = rig.upload_baseline(base, root + "/from");
	const int64_t exp_tree = rig.upload_baseline(expansion, root + "/from/expansion/jox01");
	TEST_EXPECT(base_tree != 0 && exp_tree != 0);
	if (base_tree == 0 || exp_tree == 0)
		return 1;
	update::SfsPackage package;
	const int64_t release = rig.upload_release(root, "1.0.0.10", package);
	TEST_EXPECT(release != 0 && find_release(rig.db, release)->status == ReleaseStatus::Draft);
	if (release == 0)
		return 1;
	TEST_EXPECT(publish_release(rig.db, release));
	// A worker's earlier run of the base's apply was cut short after making its tree and
	// writing one object: the run again continues on that row and starts its prefix afresh.
	const int64_t building = create_tree(
			rig.db, base, TreeSource::Release, TreeState::Building, release, base_tree, "worker");
	set_section_tree(rig.db, release, base, building);
	TEST_EXPECT(rig.put(tree_prefix(building) + "stray.bin", bytes("left behind")));
	TEST_EXPECT(enqueue_release_apply(rig.db, release, base) > 0);
	TEST_EXPECT(enqueue_release_apply(rig.db, release, base) == 0); // deduped
	TEST_EXPECT(enqueue_release_apply(rig.db, release, expansion) > 0);
	TEST_EXPECT(rig.run_jobs() >= 2);

	// The base's release tree.
	const std::vector<ReleaseSection> sections = list_sections(rig.db, release);
	TEST_EXPECT(sections.size() == 2 && sections[0].tree_id == building && sections[1].tree_id);
	if (sections.size() != 2 || !sections[1].tree_id)
		return 1;
	auto tree = find_tree(rig.db, building);
	TEST_EXPECT(tree && tree->state == TreeState::Ready);
	if (!tree || tree->state != TreeState::Ready) {
		if (tree)
			std::fprintf(stderr, "  tree: %s\n", tree->error.c_str());
		return 1;
	}
	TEST_EXPECT(tree->source == TreeSource::Release && tree->release_id == release &&
			tree->baseline_tree_id == base_tree);
	const char *names[] = { "_NSTMOUT.TXT", "language.pff", "localres.pff", "resource.pff",
		"version.txt" };
	TEST_EXPECT(tree->files.size() == 5);
	for (size_t i = 0; i < 5 && i < tree->files.size(); ++i)
		TEST_EXPECT(tree->files[i].name == names[i]);
	TEST_EXPECT(file_named(tree->files, "score.ini") == nullptr &&
			file_named(tree->files, "readme.txt") == nullptr);
	// The loose files byte for byte, the archives entry for entry, as the updated tree holds
	// them; the stray object gone.
	const std::string to = root + "/to";
	TEST_EXPECT(rig.get(tree_prefix(building) + "version.txt") == read_bytes(to + "/version.txt"));
	TEST_EXPECT(
			rig.get(tree_prefix(building) + "_NSTMOUT.TXT") == read_bytes(to + "/_NSTMOUT.TXT"));
	for (const char *archive : { "resource.pff", "localres.pff", "language.pff" }) {
		const std::string fetched = rig.temp.file((std::string("fetched-") + archive).c_str());
		std::string error;
		TEST_EXPECT(rig.store.get_file(tree_prefix(building) + archive, fetched, error));
		const std::map<std::string, Entry> got = archive_entries(fetched),
										   want = archive_entries(to + "/" + archive);
		TEST_EXPECT(!want.empty() && got == want);
	}
	TEST_EXPECT(!rig.has(tree_prefix(building) + "stray.bin") &&
			!rig.has(tree_prefix(building) + "score.ini") &&
			!rig.has(tree_prefix(building) + "readme.txt"));
	if (int rc = expect_manifest_matches_store(rig, *tree))
		return rc;
	TEST_EXPECT(tree->version_text == kBaseVersionUpdated);
	TEST_EXPECT(tree->missions.size() == 3);
	TEST_EXPECT(tree->missions.size() == 3 && tree->missions[0].file == "alpha.bms" &&
			tree->missions[1].file == "bravo.bms" && tree->missions[2].file == "charlie.bms" &&
			tree->missions[2].title == "Charlie Flag");
	// The data set's current tree is the release's, its baseline the upload's.
	TEST_EXPECT(find_data_set(rig.db, base)->current_tree_id == building &&
			find_data_set(rig.db, base)->baseline_tree_id == base_tree);
	// The baseline's objects are untouched.
	TEST_EXPECT(rig.has(tree_prefix(base_tree) + "score.ini") &&
			rig.get(tree_prefix(base_tree) + "version.txt") ==
					read_bytes(root + "/from/version.txt"));

	// The expansion's.
	tree = find_tree(rig.db, *sections[1].tree_id);
	TEST_EXPECT(tree && tree->state == TreeState::Ready && tree->baseline_tree_id == exp_tree);
	if (!tree || tree->state != TreeState::Ready) {
		if (tree)
			std::fprintf(stderr, "  tree: %s\n", tree->error.c_str());
		return 1;
	}
	TEST_EXPECT(tree->files.size() == 3 && tree->version_text == kExpVersionUpdated);
	TEST_EXPECT(tree->missions.size() == 2 && tree->missions[0].file == "delta.bms" &&
			tree->missions[0].title == "Delta Op" && tree->missions[1].file == "exp.bms");
	for (const char *archive : { "jox01.pff", "jox01L.pff" }) {
		const std::string fetched = rig.temp.file((std::string("fetched-") + archive).c_str());
		std::string error;
		TEST_EXPECT(rig.store.get_file(tree_prefix(tree->id) + archive, fetched, error));
		TEST_EXPECT(
				archive_entries(fetched) == archive_entries(to + "/expansion/jox01/" + archive));
	}
	if (int rc = expect_manifest_matches_store(rig, *tree))
		return rc;
	TEST_EXPECT(find_data_set(rig.db, expansion)->current_tree_id == tree->id);
	TEST_EXPECT(find_release(rig.db, release)->status == ReleaseStatus::Published);

	// Asked again once built: nothing more (the section's tree is ready).
	const size_t trees = list_trees(rig.db, base).size();
	TEST_EXPECT(enqueue_release_apply(rig.db, release, base) > 0 && rig.run_jobs() >= 1);
	TEST_EXPECT(list_trees(rig.db, base).size() == trees);
	// A retired release applies nothing.
	TEST_EXPECT(retire_release(rig.db, release));
	TEST_EXPECT(enqueue_release_apply(rig.db, release, base) > 0 && rig.run_jobs() >= 1);
	TEST_EXPECT(list_trees(rig.db, base).size() == trees);
	return 0;
}

// A release published before any baseline: each section's tree fails "no baseline uploaded",
// the release stays published.
int test_apply_without_baseline() {
	Rig rig("data_set_jobs_no_baseline");
	TEST_EXPECT(make_trees(rig.temp.root()));
	int64_t base = 0, expansion = 0;
	TEST_EXPECT(rig.make_synthetic_data_sets(base, expansion));
	update::SfsPackage package;
	const int64_t release = rig.upload_release(rig.temp.root(), "1.0.0.10", package);
	TEST_EXPECT(release != 0 && publish_release(rig.db, release));
	TEST_EXPECT(enqueue_release_apply(rig.db, release, base) > 0 &&
			enqueue_release_apply(rig.db, release, expansion) > 0 && rig.run_jobs() >= 2);
	for (const ReleaseSection &s : list_sections(rig.db, release)) {
		TEST_EXPECT(s.tree_id.has_value());
		if (!s.tree_id)
			continue;
		const auto tree = find_tree(rig.db, *s.tree_id);
		TEST_EXPECT(tree && tree->state == TreeState::Failed &&
				tree->error == "no baseline uploaded" && tree->source == TreeSource::Release &&
				!tree->baseline_tree_id);
		TEST_EXPECT(rig.names_under(tree_prefix(*s.tree_id)).empty());
	}
	TEST_EXPECT(find_release(rig.db, release)->status == ReleaseStatus::Published);
	TEST_EXPECT(!find_data_set(rig.db, base)->current_tree_id);
	return 0;
}

// The prune: an orphaned ready tree's objects removed and its row marked, an abandoned
// upload's parts aborted, the current tree kept; a second run finds nothing.
int test_prune() {
	Rig rig("data_set_jobs_prune");
	const std::string from = rig.temp.file("from");
	TEST_EXPECT(make_base_tree(from, false));
	const int64_t jo = rig.seeded("Joint Operations");
	const int64_t first = rig.upload_baseline(jo, from);
	const int64_t second = rig.upload_baseline(jo, from);
	TEST_EXPECT(first != 0 && second != 0);
	if (first == 0 || second == 0)
		return 1;
	// The first is neither current nor baseline now, and old enough.
	rig.db.exec("UPDATE data_set_trees SET created_at = now() - interval '2 hours' WHERE id = $1;",
			{ BindValue(first) });
	// An upload begun eight days ago and never finished.
	const int64_t abandoned = create_tree(rig.db, jo, TreeSource::Baseline, TreeState::Uploading,
			std::nullopt, std::nullopt, "admin");
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
	const int64_t fresh = create_tree(rig.db, jo, TreeSource::Baseline, TreeState::Finalizing,
			std::nullopt, std::nullopt, "admin");
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
		{ "finalize_baseline", test_finalize_baseline },
		{ "finalize_refusals", test_finalize_refusals },
		{ "finalize_expansion", test_finalize_expansion },
		{ "validate", test_validate },
		{ "apply", test_apply },
		{ "apply_without_baseline", test_apply_without_baseline },
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
