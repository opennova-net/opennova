#include "data_set_jobs.h"

#include "data_set_files.h"
#include "data_set_repository.h"
#include "loggable.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <formats/pff/pff.h>
#include <runtime/mission/mission_catalog.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

namespace fs = std::filesystem;

constexpr const char *kTag = "[data]";
constexpr int kCapFinalize = 2;
constexpr int kCapPrune = 1;
constexpr int kAttemptsFinalize = 3;
constexpr int kAttemptsPrune = 3;
constexpr int64_t kPrunePeriodMs = 60 * 60 * 1000;

int64_t member_int(const io::JsonValue &payload, const char *key) {
	const io::JsonValue *v = payload.get(key);
	return v != nullptr && v->is_number() ? static_cast<int64_t>(v->number) : 0;
}

std::string id_text(int64_t id) {
	return std::to_string(static_cast<long long>(id));
}

std::string join(const std::vector<std::string> &names) {
	std::string out;
	for (const std::string &name : names)
		out += (out.empty() ? "" : ", ") + name;
	return out;
}

// What a step of a job came to: done, refused for good (the row fails with the reason), or
// a store or disk failure the job runs again for.
enum class Outcome : uint8_t { Ok, Refused, Transient };

// A job's own directory under the worker's scratch directory, removed with it.
struct ScratchDir {
	std::string path;

	explicit ScratchDir(std::string path) : path(std::move(path)) {}
	~ScratchDir() {
		std::error_code ec;
		fs::remove_all(io::os_path(path), ec);
	}
	ScratchDir(const ScratchDir &) = delete;
	ScratchDir &operator=(const ScratchDir &) = delete;

	// Made empty (a run cut short may have left it).
	bool make(std::string &error) {
		std::error_code ec;
		fs::remove_all(io::os_path(path), ec);
		return io::ensure_directory(path, error);
	}
	std::string file(const std::string &name) const { return io::utf8_join(path, name); }
};

// The file's size: what the manifest records and hostd checks a fetch by.
bool measure_file(const std::string &path, uint64_t &size, std::string &error) {
	std::error_code ec;
	const uintmax_t bytes = fs::file_size(io::os_path(path), ec);
	if (ec) {
		error = "cannot read " + path + ": " + ec.message();
		return false;
	}
	size = static_cast<uint64_t>(bytes);
	return true;
}

// The first line of version.txt, trimmed; "" when the file is absent.
std::string version_text_of(const std::string &path) {
	std::string text, ignored;
	if (!io::read_file_text(path, text, ignored))
		return std::string();
	return strutil::trim(text.substr(0, text.find_first_of("\r\n")));
}

// Whether every .pff among `files` opens as an archive (its directory read), else why not.
bool archives_open(const std::map<std::string, std::string> &files, std::string &why) {
	for (const auto &[name, path] : files) {
		if (!strutil::ends_with_icase(name, ".pff"))
			continue;
		pff::PffArchive archive{};
		if (pff::pff_open(&archive, path.c_str()) != 0) {
			why = name + " is not a PFF archive";
			return false;
		}
		pff::pff_close(&archive);
	}
	return true;
}

// The multiplayer missions of a tree's files at `dir` (`files`: the canonical name of each
// tree file to the path it lies at): a base tree mounted as opennova-serve mounts an install
// (the retail boot table, mission_catalog::build), an expansion tree its pair alone
// (mission_catalog::archive_rows over the archives, then the catalog's order).
bool scan_missions(const DataSetRow &data_set, const std::string &dir,
		const std::map<std::string, std::string> &files, std::vector<TreeMission> &out,
		std::string &why) {
	std::vector<mission_catalog::Row> rows;
	if (data_set.kind == DataSetKind::Base) {
		ResourceIndex index;
		if (!index.scan(
					dir, std::string(), VfsMountMode::Packed, VfsArchiveDiscovery::RetailTable) ||
				!index.has_mounted_archive()) {
			why = "the tree does not mount as an install: " + index.last_error();
			return false;
		}
		rows = mission_catalog::build(index);
		index.clear();
	} else {
		const MissionPair pair = mission_pair_of(data_set.kind, data_set.expansion_code);
		const auto missions = files.find(pair.missions);
		const auto texts = files.find(pair.texts);
		if (missions == files.end() || texts == files.end()) {
			why = "the tree lacks its mission pair";
			return false;
		}
		pff::PffArchive missions_archive{}, texts_archive{};
		if (pff::pff_open(&missions_archive, missions->second.c_str()) != 0) {
			why = pair.missions + " is not a PFF archive";
			return false;
		}
		if (pff::pff_open(&texts_archive, texts->second.c_str()) != 0) {
			pff::pff_close(&missions_archive);
			why = pair.texts + " is not a PFF archive";
			return false;
		}
		rows = mission_catalog::archive_rows(missions_archive, &texts_archive);
		pff::pff_close(&missions_archive);
		pff::pff_close(&texts_archive);
		mission_catalog::sort_rows(rows);
	}
	out.clear();
	for (const mission_catalog::Row &row : rows) {
		if (row.game_mode == 0)
			continue; // a single-player mission: no server rotates it
		out.push_back({ row.file, row.title });
	}
	return true;
}

std::string manifest_json(int64_t tree_id, const TreeReady &ready) {
	io::JsonValue list = io::JsonValue::make_array();
	for (const TreeFile &f : ready.files) {
		io::JsonValue item = io::JsonValue::make_object();
		item.set("name", io::json_string(f.name));
		item.set("size", io::json_number(static_cast<double>(f.size)));
		list.push(std::move(item));
	}
	io::JsonValue root = io::JsonValue::make_object();
	root.set("tree", io::json_number(static_cast<double>(tree_id)));
	root.set("version", io::json_string(ready.version_text));
	root.set("files", std::move(list));
	return io::json_write(root);
}

// The manifest written beside the tree's files in the store (the names and sizes hostd checks
// a fetch by).
bool put_manifest(ObjectStore &store, const ScratchDir &scratch, int64_t tree_id,
		const TreeReady &ready, std::string &error) {
	const std::string path = scratch.file(kTreeManifestName);
	if (!io::write_file_atomic(path, manifest_json(tree_id, ready), error))
		return false;
	return store.put_file(tree_prefix(tree_id) + kTreeManifestName, path, error);
}

void sort_files(std::vector<TreeFile> &files) {
	std::sort(files.begin(), files.end(),
			[](const TreeFile &a, const TreeFile &b) { return a.name < b.name; });
}

int64_t total_bytes(const std::vector<TreeFile> &files) {
	int64_t bytes = 0;
	for (const TreeFile &f : files)
		bytes += static_cast<int64_t>(f.size);
	return bytes;
}

// The tree's row settled from what its job came to, with `from` its state while the job ran:
// ready (and a prune queued for whatever that frees), failed with the reason, or the job run
// again for a store failure, the tree failed once no attempt is left.
jobs::JobResult settle_tree(jobs::JobContext &ctx, int64_t tree_id, TreeState from, Outcome outcome,
		const TreeReady &ready, const std::string &why) {
	auto lease = ctx.pool.acquire();
	db::Database &db = *lease;
	switch (outcome) {
		case Outcome::Ok:
			if (!finish_tree(db, tree_id, from, ready)) {
				std::printf("%s tree %s left %s meanwhile; nothing written\n", kTag,
						id_text(tree_id).c_str(), tree_state_name(from));
				return jobs::JobResult::done();
			}
			std::printf("%s tree %s ready: %zu file%s, %s, %zu mission%s, version \"%s\"\n", kTag,
					id_text(tree_id).c_str(), ready.files.size(),
					ready.files.size() == 1 ? "" : "s",
					strutil::byte_size_text(static_cast<uint64_t>(ready.bytes)).c_str(),
					ready.missions.size(), ready.missions.size() == 1 ? "" : "s",
					loggable(ready.version_text).c_str());
			enqueue_tree_prune(db);
			return jobs::JobResult::done();
		case Outcome::Refused:
			set_tree_state(db, tree_id, from, TreeState::Failed, why);
			std::fprintf(stderr, "%s tree %s failed: %s\n", kTag, id_text(tree_id).c_str(),
					loggable(why, 200).c_str());
			return jobs::JobResult::done();
		case Outcome::Transient:
			break;
	}
	if (ctx.job.attempts >= ctx.job.max_attempts) {
		set_tree_state(db, tree_id, from, TreeState::Failed, "the store: " + why);
		std::fprintf(stderr, "%s tree %s failed after %d attempts: %s\n", kTag,
				id_text(tree_id).c_str(), ctx.job.attempts, loggable(why, 200).c_str());
		return jobs::JobResult::fail(why);
	}
	std::fprintf(stderr, "%s WARN tree %s: %s; the job runs again\n", kTag,
			id_text(tree_id).c_str(), loggable(why, 200).c_str());
	return jobs::JobResult::retry(why);
}

// --- tree.finalize ---

// The tree's objects under its prefix checked against the kind's files, downloaded, measured,
// read for the version and the missions, and its manifest written.
Outcome finalize_tree(ObjectStore &store, const std::string &scratch_root, int64_t tree_id,
		const DataSetRow &data_set, TreeReady &ready, std::string &why) {
	const std::string prefix = tree_prefix(tree_id);
	std::vector<StoredObject> objects;
	if (!store.list(prefix, objects, why))
		return Outcome::Transient;
	std::vector<std::string> names;
	for (const StoredObject &object : objects) {
		const std::string name = object.key.substr(prefix.size());
		if (name == kTreeManifestName)
			continue; // this job's own, from a run cut short
		const std::string canonical =
				canonical_tree_file(data_set.kind, data_set.expansion_code, name);
		if (name.find('/') != std::string::npos || canonical.empty()) {
			why = "the tree holds " + name + ", which a " +
					std::string(data_set_kind_name(data_set.kind)) + " data set's tree does not";
			return Outcome::Refused;
		}
		if (canonical != name) {
			why = "the file " + name + " is stored under its own spelling, not " + canonical;
			return Outcome::Refused;
		}
		names.push_back(name);
	}
	const std::vector<std::string> missing =
			missing_tree_files(data_set.kind, data_set.expansion_code, names);
	if (!missing.empty()) {
		why = "the tree lacks " + join(missing);
		return Outcome::Refused;
	}
	ScratchDir scratch(io::utf8_join(scratch_root, "tree-" + id_text(tree_id)));
	if (!scratch.make(why))
		return Outcome::Transient;
	const std::string dir = scratch.file("tree");
	if (!io::ensure_directory(dir, why))
		return Outcome::Transient;
	std::map<std::string, std::string> files; // canonical name -> its path
	ready = TreeReady{};
	for (const std::string &name : names) {
		const std::string path = io::utf8_join(dir, name);
		if (!store.get_file(prefix + name, path, why))
			return Outcome::Transient;
		TreeFile file;
		file.name = name;
		if (!measure_file(path, file.size, why))
			return Outcome::Transient;
		ready.files.push_back(std::move(file));
		files[name] = path;
	}
	sort_files(ready.files);
	ready.bytes = total_bytes(ready.files);
	if (!archives_open(files, why))
		return Outcome::Refused;
	ready.version_text = version_text_of(io::utf8_join(dir, "version.txt"));
	if (!scan_missions(data_set, dir, files, ready.missions, why))
		return Outcome::Refused;
	if (!put_manifest(store, scratch, tree_id, ready, why))
		return Outcome::Transient;
	return Outcome::Ok;
}

jobs::JobResult finalize(
		jobs::JobContext &ctx, ObjectStore &store, const std::string &scratch_root) {
	const int64_t tree_id = member_int(ctx.job.payload, "tree_id");
	std::optional<DataSetTree> tree;
	std::optional<DataSetRow> data_set;
	{
		auto lease = ctx.pool.acquire();
		tree = find_tree(*lease, tree_id);
		if (tree)
			data_set = find_data_set(*lease, tree->data_set_id);
	}
	// Deleted, or finished by another run: nothing to do.
	if (!tree || tree->state != TreeState::Finalizing || !data_set)
		return jobs::JobResult::done();
	std::printf("%s tree %s (%s): finalizing\n", kTag, id_text(tree_id).c_str(),
			loggable(data_set->display_name).c_str());
	TreeReady ready;
	std::string why;
	const Outcome outcome = finalize_tree(store, scratch_root, tree_id, *data_set, ready, why);
	return settle_tree(ctx, tree_id, TreeState::Finalizing, outcome, ready, why);
}

// --- tree.prune ---

jobs::JobResult prune(jobs::JobContext &ctx, ObjectStore &store) {
	std::vector<int64_t> ids;
	{
		auto lease = ctx.pool.acquire();
		ids = prunable_trees(*lease, kPruneMinAgeS, kPruneAbandonedUploadS);
	}
	if (ids.empty())
		return jobs::JobResult::done();
	std::printf("%s pruning %zu tree%s\n", kTag, ids.size(), ids.size() == 1 ? "" : "s");
	size_t pruned = 0;
	std::string first_error;
	for (const int64_t id : ids) {
		const std::string prefix = tree_prefix(id);
		std::string error;
		if (!store.remove_prefix(prefix, error) || !store.abort_uploads_under(prefix, error)) {
			std::fprintf(stderr, "%s WARN tree %s: not pruned: %s\n", kTag, id_text(id).c_str(),
					loggable(error, 200).c_str());
			if (first_error.empty())
				first_error = "tree " + id_text(id) + ": " + error;
			continue;
		}
		auto lease = ctx.pool.acquire();
		mark_tree_pruned(*lease, id);
		++pruned;
	}
	std::printf("%s pruned %zu tree%s\n", kTag, pruned, pruned == 1 ? "" : "s");
	if (!first_error.empty())
		return jobs::JobResult::retry(first_error);
	return jobs::JobResult::done();
}

} // namespace

std::string tree_prefix(int64_t tree_id) {
	return "trees/" + id_text(tree_id) + "/";
}

int64_t enqueue_tree_finalize(db::Database &db, int64_t tree_id) {
	jobs::NewJob job;
	job.kind = kJobTreeFinalize;
	job.payload.set("tree_id", io::json_number(static_cast<double>(tree_id)));
	job.dedupe_key = std::string(kJobTreeFinalize) + ":" + id_text(tree_id);
	job.max_attempts = kAttemptsFinalize;
	return jobs::enqueue(db, job);
}

int64_t enqueue_tree_prune(db::Database &db) {
	jobs::NewJob job;
	job.kind = kJobTreePrune;
	job.dedupe_key = kJobTreePrune;
	job.max_attempts = kAttemptsPrune;
	return jobs::enqueue(db, job);
}

void register_data_set_jobs(
		jobs::JobRegistry &registry, ObjectStore &store, std::string scratch_dir) {
	jobs::JobKind finalize_kind{ kJobTreeFinalize,
		[&store, scratch_dir](jobs::JobContext &ctx) { return finalize(ctx, store, scratch_dir); },
		kCapFinalize };
	finalize_kind.retry_base_ms = 5 * 1000;
	finalize_kind.retry_max_ms = 60 * 1000;
	registry.add(std::move(finalize_kind));
	registry.add({ kJobTreePrune, [&store](jobs::JobContext &ctx) { return prune(ctx, store); },
			kCapPrune });
}

std::vector<jobs::PeriodicJob> data_set_schedule() {
	jobs::PeriodicJob p;
	p.kind = kJobTreePrune;
	p.period_ms = kPrunePeriodMs;
	p.max_attempts = kAttemptsPrune;
	return { std::move(p) };
}

} // namespace opennova::novaworld_server::hosted
