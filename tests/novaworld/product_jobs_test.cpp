// The hosted products' jobs (apps/novaworld_common/products/product_jobs.h) over a migrated
// database of the test's own and a local object store in a scratch directory, each job run as
// the workers run it (jobs::run_due), over a minted retail-shaped install (the boot table's three
// archives, two multiplayer missions and a single-player one titled from language.pff,
// version.txt, score.ini) and the tree a release takes it to: a 1SFS package
// opennova-update-tool built from them validated into a draft with its two sections and
// versions, the object moved under its digest, and the refusals (a package whose command file
// names no product, a second upload of the same bytes); our own game's first release and the
// runtime's (a second of the
// same version taken too); a whole retail-shaped release built into each game's tree from
// nothing (the trees' files equal the updated tree's: the loose files byte for byte, every
// archive made anew entry for entry, the unchanged entries with the rest, a file the tree does
// not hold left out, the removals since the stock install nothing to do, the version and the
// missions read, trees/<id>/manifest.json beside the files, a run again on a building tree
// continuing on its row afresh); a release that leaves a tree without a required archive
// failing it; the prune removing an orphaned tree's objects, idempotent; and a deleted game's
// purge removing its trees' prefixes and its project's files (content/<id>/), another game's left,
// idempotent.

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "update/synthetic_trees.h"

#include "core/local_object_store.h"
#include "jobs/job_queue.h"
#include "jobs/worker_pool.h"
#include "package_builder.h"
#include "products/product_jobs.h"
#include "products/product_repository.h"
#include "products/project_repository.h"
#include "products/tree_files.h"

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
// in language.pff; version.txt bumped; score.ini gone; _NSTMOUT.TXT added; and readme.txt and
// loose.bms, loose files no tree holds (the second a mission the loose leg of an install's
// catalog lists, D-MNU-25, which the tree's mission list must not).
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
	if (updated) {
		const std::vector<uint8_t> loose = bms_blob("Loose Header", kCoop);
		const std::string_view loose_bytes(
				reinterpret_cast<const char *>(loose.data()), loose.size());
		ok = ok && write_text(dir + "/_NSTMOUT.TXT", "60\r\n") &&
				write_text(dir + "/readme.txt", "not a file of the tree") &&
				write_text(dir + "/loose.bms", loose_bytes);
	} else {
		ok = ok && write_text(dir + "/score.ini", "[score]\r\nkills=1\r\n");
	}
	return ok;
}

// The jox01 expansion's tree at `dir`: its pair and version.txt; `updated` adds a mission and
// bumps the version.
bool make_expansion_tree(const std::string &dir, bool updated, const std::string &code = "jox01") {
	std::vector<pff::PffEditEntry> missions = { { "exp.bms", bms_blob("Exp Header", kCoop),
			1100000000 } };
	std::vector<pff::PffEditEntry> texts = { { "exp.bin",
			info_bin("Expansion Op", "From the expansion."), 1100000000 } };
	if (updated) {
		missions.push_back({ "delta.bms", bms_blob("Delta Header", kCoop), 1700000004 });
		texts.push_back({ "delta.bin", info_bin("Delta Op", ""), 1700000005 });
	}
	return make_archive(dir + "/" + code + ".pff", missions) &&
			make_archive(dir + "/" + code + "L.pff", texts) &&
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
		register_product_jobs(registry, store, temp.file("scratch"));
	}
	// Every job due, run on this thread.
	size_t run_jobs() { return jobs::run_due(pool, registry); }
	std::string job_state(int64_t id) {
		const auto rows = db.query("SELECT state FROM jobs WHERE id = $1;", { BindValue(id) });
		return rows.empty() ? std::string("<gone>") : rows.front().as_text(0).value_or("");
	}
	int64_t seeded(const char *display_name) {
		const auto rows = db.query(
				"SELECT id FROM products WHERE display_name = $1;", { BindValue(display_name) });
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
	// A ready tree of `product_id` holding the files at `dir`, as release.apply leaves one: its
	// id.
	int64_t ready_tree(int64_t product_id, const std::string &dir) {
		const int64_t id = create_tree(db, product_id, std::nullopt, "worker");
		if (!put_dir(tree_prefix(id), dir) || !finish_tree(db, id, TreeReady{}))
			return 0;
		return id;
	}
	// The synthetic retail games the package's command files name (their packages put where a
	// build hands them on): the base and its expansion.
	bool make_synthetic_products(int64_t &base, int64_t &expansion) {
		NewProduct b;
		b.display_name = "Synthetic";
		b.kind = ProductKind::Game;
		b.title = "jo";
		b.lobby = "jop_2_consumer";
		b.updater_login = "SYNus";
		b.command_file = "SYNPTR.CMD";
		b.retail = true;
		b.key = "SYN";
		b.visibility = "private";
		b.created_by = "admin";
		if (create_product(db, b, base) != ProductOutcome::Ok)
			return false;
		NewProduct e;
		e.display_name = "Synthetic Expansion";
		e.kind = ProductKind::Expansion;
		e.parent_id = base;
		e.expansion_code = "jox01";
		e.updater_login = "SYNEus";
		e.command_file = "SYNEPTR.CMD";
		e.retail = true;
		e.key = "SYNX";
		e.visibility = "private";
		e.created_by = "admin";
		return create_product(db, e, expansion) == ProductOutcome::Ok;
	}
	// The package opennova-update-tool builds from `root`'s from and to trees (`full`: every file
	// of `to` a row, as every release now is): false when it builds none.
	bool build_package(const std::string &root, std::vector<update_tool::ProductSpec> products,
			bool full, update::SfsPackage &package) {
		update_tool::BuildSpec spec;
		spec.full = full;
		spec.from = io::os_path(root + "/from");
		spec.to = io::os_path(root + "/to");
		spec.package_product = "Synthetic Product";
		if (products.empty()) {
			spec.products.push_back({ "", "", "SYNPTR.CMD", "Synthetic Product", "1.0.0.10", "" });
			spec.products.push_back({ "expansion/jox01", "jox01", "SYNEPTR.CMD", "Synthetic Expansion", "1.0.0.11", "" });
		} else {
			spec.products = std::move(products);
		}
		std::vector<update_tool::ProductReport> report;
		std::string error;
		if (!update_tool::build_update_package(spec, package, report, error)) {
			std::fprintf(stderr, "  build: %s\n", error.c_str());
			return false;
		}
		return true;
	}
	// The package built from `root`'s trees, uploaded as a new release and validated by the job:
	// its id; `package` the package as built.
	int64_t upload_release(const std::string &root, const char *name, update::SfsPackage &package,
			std::vector<update_tool::ProductSpec> products = {}, bool full = false) {
		if (!build_package(root, std::move(products), full, package))
			return 0;
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
	// The whole package built from `root`'s trees as release.build hands one on: a release of the
	// game `product_id` built from its project's revision 1, the package where an upload lands,
	// validated by the job: its id.
	int64_t built_release(const std::string &root, const char *name, int64_t product_id,
			std::vector<update_tool::ProductSpec> products, update::SfsPackage &package) {
		if (!build_package(root, std::move(products), true, package))
			return 0;
		NewBuiltRelease made;
		made.name = name;
		made.created_by = "admin";
		made.product_id = product_id;
		made.revision = 1;
		made.version = "1.0";
		const int64_t id = create_built_release(db, made);
		if (!put(release_upload_key(id), update::sfs_write(package)) ||
				!set_release_built(db, id, "built") || enqueue_release_validate(db, id) == 0)
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
int expect_manifest_matches_store(Rig &rig, const ProductTree &tree) {
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

// The tree's archive `name` fetched from the store: its entries.
std::map<std::string, Entry> stored_archive(Rig &rig, int64_t tree, const std::string &name) {
	const std::string fetched = rig.temp.file(("fetched-" + std::to_string(tree) + "-" + name).c_str());
	std::string error;
	if (!rig.store.get_file(tree_prefix(tree) + name, fetched, error))
		return {};
	return archive_entries(fetched);
}

// --- the cases ---

// The package validated: a draft with its sections and versions, the object under its digest;
// then a package naming no product and the same bytes again, invalid.
int test_validate() {
	Rig rig("product_jobs_validate");
	TEST_EXPECT(make_trees(rig.temp.root()));
	int64_t base = 0, expansion = 0;
	TEST_EXPECT(rig.make_synthetic_products(base, expansion));
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
			release->package_product == "Synthetic Product" && release->error.empty() &&
			release->product_id == base);
	const std::vector<ReleaseSection> sections = list_sections(rig.db, id);
	TEST_EXPECT(sections.size() == 2);
	if (sections.size() != 2)
		return 1;
	// The base: version.txt, _NSTMOUT.TXT, readme.txt and loose.bms written, score.ini removed;
	// the three archives each a TARGET with their rows (charlie.bin; charlie.bms; wpn.def,
	// add.def, drop.def).
	TEST_EXPECT(sections[0].product_id == base && sections[0].expansion.empty() &&
			sections[0].command_file == "SYNPTR.CMD" && sections[0].version == "1.0.0.10" &&
			sections[0].row_count == 13 && sections[0].blob_bytes > 0 && !sections[0].tree_id);
	// The expansion: version.txt; delta.bms and delta.bin under their archives.
	TEST_EXPECT(sections[1].product_id == expansion && sections[1].expansion == "jox01" &&
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

	// A command file no product takes.
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

// Our own game's first release, built from its project's revision (a game with a project): its
// tree and its expansion's built from nothing, each with its gate-tag override (gt.ssc, ADR 0055
// d5: a game's own data, loose beside its archives).
int test_native_first_release() {
	Rig rig("product_jobs_native_first");
	TEST_EXPECT(make_base_tree(rig.temp.file("to"), true));
	TEST_EXPECT(make_expansion_tree(rig.temp.file("to/expansion/nifty"), true, "nifty"));
	TEST_EXPECT(write_text(rig.temp.file("to") + "/gt.ssc", "the game's gate tag") &&
			write_text(rig.temp.file("to/expansion/nifty") + "/gt.ssc", "the expansion's gate tag"));
	const int64_t native = rig.seeded("OP: Acid Gambit");
	const int64_t expansion = rig.seeded("OP: Nifty Package");
	update::SfsPackage package;
	const int64_t id = rig.built_release(rig.temp.root(), "native first", native,
			{ { "", "", "ACIDGAMBIT.CMD", "OpenNova", "1.0", "opennova.exe" },
					{ "expansion/nifty", "nifty", "NIFTY.CMD", "OpenNova JO", "1.0", "" } },
			package);
	TEST_EXPECT(id != 0 && find_release(rig.db, id)->status == ReleaseStatus::Draft &&
			find_release(rig.db, id)->product_id == native);
	TEST_EXPECT(publish_release(rig.db, id));
	TEST_EXPECT(enqueue_release_apply(rig.db, id, native) > 0 &&
			enqueue_release_apply(rig.db, id, expansion) > 0 && rig.run_jobs() >= 2);
	const auto current = find_product(rig.db, native);
	TEST_EXPECT(current && current->current_tree_id);
	if (!current || !current->current_tree_id)
		return 1;
	const auto tree = find_tree(rig.db, *current->current_tree_id);
	TEST_EXPECT(tree && tree->state == TreeState::Ready && tree->release_id == id);
	for (const char *name : { "resource.pff", "localres.pff", "language.pff" }) {
		const std::string installed = rig.store.path_of(tree_prefix(tree->id) + name);
		TEST_EXPECT(archive_entries(installed) == archive_entries(rig.temp.file("to") + "/" + name));
	}
	TEST_EXPECT(file_named(tree->files, "gt.ssc") != nullptr &&
			rig.get(tree_prefix(tree->id) + "gt.ssc") == read_bytes(rig.temp.file("to") + "/gt.ssc"));
	const auto installed_expansion = find_product(rig.db, expansion);
	TEST_EXPECT(installed_expansion && installed_expansion->current_tree_id);
	if (!installed_expansion || !installed_expansion->current_tree_id)
		return 1;
	for (const char *name : { "nifty.pff", "niftyL.pff" }) {
		const std::string canonical = canonical_tree_file(ProductKind::Expansion, "nifty", name);
		const std::string installed =
				rig.store.path_of(tree_prefix(*installed_expansion->current_tree_id) + canonical);
		TEST_EXPECT(archive_entries(installed) ==
				archive_entries(rig.temp.file("to/expansion/nifty") + "/" + name));
	}
	const auto expansion_tree = find_tree(rig.db, *installed_expansion->current_tree_id);
	TEST_EXPECT(expansion_tree && file_named(expansion_tree->files, "gt.ssc") != nullptr &&
			rig.get(tree_prefix(expansion_tree->id) + "gt.ssc") ==
					read_bytes(rig.temp.file("to/expansion/nifty") + "/gt.ssc"));
	return 0;
}

// The runtime, a game with no project: its package uploaded (naming no game, so it becomes its
// section's), published, served and built into no tree; a second of the same version (a staging
// build of an unchanged version) is a draft too.
int test_runtime_release() {
	Rig rig("product_jobs_runtime");
	TEST_EXPECT(write_text(rig.temp.file("to") + "/opennova.exe", "synthetic runtime"));
	update::SfsPackage package;
	const int64_t id = rig.upload_release(rig.temp.root(), "runtime", package,
			{ { "", "", "ONRUNTIME.CMD", "OpenNova runtime", "1.0", "opennova.exe" } }, true);
	TEST_EXPECT(id != 0);
	const auto release = find_release(rig.db, id);
	const int64_t runtime = rig.seeded("OpenNova runtime");
	TEST_EXPECT(release && release->status == ReleaseStatus::Draft && release->product_id == runtime);
	const auto sections = list_sections(rig.db, id);
	TEST_EXPECT(sections.size() == 1 && sections[0].product_id == runtime &&
			sections[0].command_file == "ONRUNTIME.CMD" && sections[0].version == "1.0");
	TEST_EXPECT(publish_release(rig.db, id));
	TEST_EXPECT(enqueue_release_apply(rig.db, id, runtime) == 0);
	TEST_EXPECT(list_trees(rig.db, runtime).empty());
	const auto served = published_sections(rig.db);
	TEST_EXPECT(served.size() == 1 && served[0].updater_login == "ONRUNTIME");
	TEST_EXPECT(write_text(rig.temp.file("to") + "/opennova.exe", "synthetic runtime, rebuilt"));
	update::SfsPackage again;
	const int64_t second = rig.upload_release(rig.temp.root(), "runtime again", again,
			{ { "", "", "ONRUNTIME.CMD", "OpenNova runtime", "1.0", "opennova.exe" } }, true);
	TEST_EXPECT(second != 0 && find_release(rig.db, second)->status == ReleaseStatus::Draft &&
			list_sections(rig.db, second).size() == 1 &&
			list_sections(rig.db, second)[0].version == "1.0");
	TEST_EXPECT(publish_release(rig.db, second) &&
			find_release(rig.db, id)->status == ReleaseStatus::Retired);
	return 0;
}

// A whole retail-shaped release published (every file of the updated tree a row, with the
// removals since the stock install): each section's tree built from nothing, every archive made
// anew equal to the updated tree's entry for entry; a run again on a building tree continues on
// its row with its prefix emptied first.
int test_apply() {
	Rig rig("product_jobs_apply");
	const std::string root = rig.temp.root();
	TEST_EXPECT(make_trees(root));
	int64_t base = 0, expansion = 0;
	TEST_EXPECT(rig.make_synthetic_products(base, expansion));
	update::SfsPackage package;
	const int64_t release = rig.upload_release(root, "1.0.0.10", package, {}, true);
	TEST_EXPECT(release != 0 && find_release(rig.db, release)->status == ReleaseStatus::Draft);
	if (release == 0)
		return 1;
	TEST_EXPECT(publish_release(rig.db, release));
	// A worker's earlier run of the base's apply was cut short after making its tree and
	// writing one object: the run again continues on that row and starts its prefix afresh.
	const int64_t building = create_tree(rig.db, base, release, "worker");
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
	TEST_EXPECT(tree->release_id == release);
	const char *names[] = { "_NSTMOUT.TXT", "language.pff", "localres.pff", "resource.pff",
		"version.txt" };
	TEST_EXPECT(tree->files.size() == 5);
	for (size_t i = 0; i < 5 && i < tree->files.size(); ++i)
		TEST_EXPECT(tree->files[i].name == names[i]);
	TEST_EXPECT(file_named(tree->files, "score.ini") == nullptr &&
			file_named(tree->files, "readme.txt") == nullptr &&
			file_named(tree->files, "loose.bms") == nullptr);
	// The loose files byte for byte, the archives entry for entry, as the updated tree holds
	// them: made from nothing, so the entries the release left as they were (keep.def) are there
	// beside the changed and added ones; the stray object gone.
	const std::string to = root + "/to";
	TEST_EXPECT(rig.get(tree_prefix(building) + "version.txt") == read_bytes(to + "/version.txt"));
	TEST_EXPECT(
			rig.get(tree_prefix(building) + "_NSTMOUT.TXT") == read_bytes(to + "/_NSTMOUT.TXT"));
	for (const char *archive : { "resource.pff", "localres.pff", "language.pff" }) {
		const std::map<std::string, Entry> got = stored_archive(rig, building, archive),
										   want = archive_entries(to + "/" + archive);
		TEST_EXPECT(!want.empty() && got == want);
	}
	const std::map<std::string, Entry> resource = stored_archive(rig, building, "resource.pff");
	TEST_EXPECT(resource.count(pff::normalized_logical_name("keep.def")) == 1 &&
			resource.count(pff::normalized_logical_name("drop.def")) == 0 && resource.size() == 3);
	TEST_EXPECT(!rig.has(tree_prefix(building) + "stray.bin") &&
			!rig.has(tree_prefix(building) + "score.ini") &&
			!rig.has(tree_prefix(building) + "readme.txt"));
	if (int rc = expect_manifest_matches_store(rig, *tree))
		return rc;
	// The release's version, its command file's (no version file read for a release, ADR 0067).
	TEST_EXPECT(tree->version_text == "1.0.0.10");
	// The scratch folder's loose.bms lists in an install's catalog, never in the tree's.
	TEST_EXPECT(tree->missions.size() == 3);
	TEST_EXPECT(tree->missions.size() == 3 && tree->missions[0].file == "ALPHA.BMS" &&
			tree->missions[1].file == "BRAVO.BMS" && tree->missions[2].file == "CHARLIE.BMS" &&
			tree->missions[2].title == "Charlie Flag");
	// The product's current tree is the release's.
	TEST_EXPECT(find_product(rig.db, base)->current_tree_id == building);

	// The expansion's, its own beside its base game's.
	tree = find_tree(rig.db, *sections[1].tree_id);
	TEST_EXPECT(tree && tree->state == TreeState::Ready && tree->release_id == release);
	if (!tree || tree->state != TreeState::Ready) {
		if (tree)
			std::fprintf(stderr, "  tree: %s\n", tree->error.c_str());
		return 1;
	}
	TEST_EXPECT(tree->files.size() == 3 && tree->version_text == "1.0.0.11");
	TEST_EXPECT(tree->missions.size() == 2 && tree->missions[0].file == "DELTA.BMS" &&
			tree->missions[0].title == "Delta Op" && tree->missions[1].file == "EXP.BMS");
	for (const char *archive : { "jox01.pff", "jox01L.pff" })
		TEST_EXPECT(stored_archive(rig, tree->id, archive) ==
				archive_entries(to + "/expansion/jox01/" + archive));
	if (int rc = expect_manifest_matches_store(rig, *tree))
		return rc;
	TEST_EXPECT(find_product(rig.db, expansion)->current_tree_id == tree->id);
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

// A whole release whose tree would lack a required archive (language.pff): the tree fails with
// the reason, nothing of it stored, the release still published, the product's current tree
// unchanged.
int test_apply_incomplete() {
	Rig rig("product_jobs_incomplete");
	const std::string root = rig.temp.root();
	TEST_EXPECT(make_base_tree(root + "/to", true));
	std::error_code ec;
	TEST_EXPECT(fs::remove(io::os_path(root + "/to/language.pff"), ec));
	int64_t base = 0, expansion = 0;
	TEST_EXPECT(rig.make_synthetic_products(base, expansion));
	update::SfsPackage package;
	const int64_t release = rig.upload_release(root, "1.0.0.10", package,
			{ { "", "", "SYNPTR.CMD", "Synthetic Product", "1.0.0.10", "" } }, true);
	TEST_EXPECT(release != 0 && publish_release(rig.db, release));
	TEST_EXPECT(enqueue_release_apply(rig.db, release, base) > 0 && rig.run_jobs() >= 1);
	const std::vector<ReleaseSection> sections = list_sections(rig.db, release);
	TEST_EXPECT(sections.size() == 1 && sections[0].tree_id.has_value());
	if (sections.size() != 1 || !sections[0].tree_id)
		return 1;
	const auto tree = find_tree(rig.db, *sections[0].tree_id);
	TEST_EXPECT(tree && tree->state == TreeState::Failed &&
			tree->error.find("language.pff") != std::string::npos);
	TEST_EXPECT(rig.names_under(tree_prefix(*sections[0].tree_id)).empty());
	TEST_EXPECT(find_release(rig.db, release)->status == ReleaseStatus::Published);
	TEST_EXPECT(!find_product(rig.db, base)->current_tree_id);
	return 0;
}

// The prune: an orphaned ready tree's objects removed and its row marked, the current tree and a
// fresh failed one kept; a second run finds nothing.
int test_prune() {
	Rig rig("product_jobs_prune");
	const std::string from = rig.temp.file("from");
	TEST_EXPECT(make_base_tree(from, false));
	const int64_t jo = rig.seeded("Joint Operations");
	const int64_t first = rig.ready_tree(jo, from);
	const int64_t second = rig.ready_tree(jo, from);
	TEST_EXPECT(first != 0 && second != 0);
	if (first == 0 || second == 0)
		return 1;
	// The first is not current now, and old enough.
	rig.db.exec("UPDATE product_trees SET created_at = now() - interval '2 hours' WHERE id = $1;",
			{ BindValue(first) });
	// A fresh failed tree is not old enough.
	const int64_t fresh = create_tree(rig.db, jo, std::nullopt, "worker");
	TEST_EXPECT(set_tree_state(rig.db, fresh, TreeState::Building, TreeState::Failed, "x"));
	TEST_EXPECT(rig.put(tree_prefix(fresh) + "version.txt", bytes("fresh")));

	const int64_t job = enqueue_tree_prune(rig.db);
	TEST_EXPECT(job > 0 && enqueue_tree_prune(rig.db) == 0);
	TEST_EXPECT(rig.run_jobs() == 1 && rig.job_state(job) == "done");
	TEST_EXPECT(rig.names_under(tree_prefix(first)).empty());
	TEST_EXPECT(rig.names_under(tree_prefix(second)).size() == 5 &&
			rig.has(tree_prefix(fresh) + "version.txt"));
	const auto pruned = find_tree(rig.db, first);
	TEST_EXPECT(pruned && pruned->state == TreeState::Pruned && !pruned->pruned_at.empty());
	TEST_EXPECT(find_tree(rig.db, second)->state == TreeState::Ready &&
			find_tree(rig.db, fresh)->state == TreeState::Failed);
	// Again: nothing to prune, nothing changes.
	TEST_EXPECT(enqueue_tree_prune(rig.db) > 0 && rig.run_jobs() == 1);
	TEST_EXPECT(rig.names_under(tree_prefix(second)).size() == 5);
	// The hourly schedule.
	const std::vector<jobs::PeriodicJob> schedule = product_schedule();
	TEST_EXPECT(schedule.size() == 1 && schedule[0].kind == kJobTreePrune &&
			schedule[0].period_ms == 60 * 60 * 1000);
	return 0;
}

// A deleted game's purge (product.purge): the prefixes of the trees it names and its project's files
// and manifests (content/<id>/) removed whole, another game's project and tree left; one queued
// for the game already holds its dedupe key; a run again finds nothing to remove and is done.
int test_purge() {
	namespace store = opennova::novaworld_server::projects;
	Rig rig("product_jobs_purge");
	const int64_t gone = 9001; // its row deleted already: the purge reads none
	const int64_t kept = rig.seeded("Joint Operations");
	TEST_EXPECT(kept != 0);
	const std::string blob(64, 'a'), manifest(64, 'b');
	TEST_EXPECT(rig.put(store::blob_key(gone, blob), bytes("blob")) &&
			rig.put(store::manifest_key(gone, manifest), bytes("manifest")) &&
			rig.put(tree_prefix(41) + "version.txt", bytes("tree")) &&
			rig.put(tree_prefix(42) + "resource.pff", bytes("tree")) &&
			rig.put(store::blob_key(kept, blob), bytes("kept")) &&
			rig.put(tree_prefix(43) + "version.txt", bytes("kept")));
	const int64_t job = enqueue_product_purge(rig.db, gone, { 41, 42 });
	TEST_EXPECT(job > 0 && enqueue_product_purge(rig.db, gone, { 41, 42 }) == 0);
	// It waits kProductPurgeDelaySeconds (past the presigned transfers' hour), then runs.
	TEST_EXPECT(rig.db.query("SELECT 1 FROM jobs WHERE id = $1 AND run_at > now() + "
							 "make_interval(secs => $2);",
							{ BindValue(job), BindValue(kProductPurgeDelaySeconds - 60) })
						.size() == 1);
	TEST_EXPECT(rig.run_jobs() == 0 && rig.job_state(job) == "queued");
	rig.db.exec("UPDATE jobs SET run_at = now() WHERE id = $1;", { BindValue(job) });
	TEST_EXPECT(rig.run_jobs() == 1 && rig.job_state(job) == "done");
	TEST_EXPECT(rig.names_under(store::project_prefix(gone)).empty() &&
			rig.names_under(tree_prefix(41)).empty() && rig.names_under(tree_prefix(42)).empty());
	TEST_EXPECT(rig.has(store::blob_key(kept, blob)) && rig.has(tree_prefix(43) + "version.txt"));
	// Again: nothing left to remove.
	const int64_t again = enqueue_product_purge(rig.db, gone, { 41, 42 });
	rig.db.exec("UPDATE jobs SET run_at = now() WHERE id = $1;", { BindValue(again) });
	TEST_EXPECT(again > 0 && rig.run_jobs() == 1 && rig.job_state(again) == "done");
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
		{ "validate", test_validate },
		{ "runtime_release", test_runtime_release },
		{ "native_first_release", test_native_first_release },
		{ "apply", test_apply },
		{ "apply_incomplete", test_apply_incomplete },
		{ "prune", test_prune },
		{ "purge", test_purge },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the products' jobs\n");
	return 0;
}
