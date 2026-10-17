#include "data_set_jobs.h"

#include "data_set_files.h"
#include "data_set_repository.h"
#include "loggable.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <formats/pff/pff.h>
#include <formats/update/apply.h>
#include <formats/update/command_file.h>
#include <formats/update/nlz.h>
#include <formats/update/sfs.h>
#include <formats/update/update_crc.h>
#include <runtime/mission/mission_catalog.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

namespace fs = std::filesystem;

constexpr const char *kTag = "[data]";
constexpr int kCapFinalize = 2;
constexpr int kCapValidate = 1;
constexpr int kCapApply = 1;
constexpr int kCapPrune = 1;
constexpr int kAttemptsFinalize = 3;
constexpr int kAttemptsValidate = 2;
constexpr int kAttemptsApply = 3;
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

// The file's size and SHA-256, streamed (a cumulative package is hundreds of megabytes): what
// names a validated release's package in the store.
bool digest_file(const std::string &path, uint64_t &size, std::string &sha256, std::string &error) {
	std::FILE *f = io::fopen_utf8(path.c_str(), "rb");
	if (f == nullptr) {
		error = "cannot open " + path;
		return false;
	}
	io::Sha256 hash;
	std::vector<uint8_t> buffer(64 * 1024);
	size = 0;
	size_t n = 0;
	while ((n = std::fread(buffer.data(), 1, buffer.size(), f)) > 0) {
		hash.update(buffer.data(), n);
		size += n;
	}
	const bool ok = std::ferror(f) == 0;
	std::fclose(f);
	if (!ok) {
		error = "cannot read " + path;
		return false;
	}
	sha256 = io::sha256_hex(hash.finish());
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
	std::printf("%s tree %s (%s, baseline): finalizing\n", kTag, id_text(tree_id).c_str(),
			loggable(data_set->display_name).c_str());
	TreeReady ready;
	std::string why;
	const Outcome outcome = finalize_tree(store, scratch_root, tree_id, *data_set, ready, why);
	return settle_tree(ctx, tree_id, TreeState::Finalizing, outcome, ready, why);
}

// --- release.validate ---

// The section's entry named `name`, as the apply library finds it (without case: the blobs land
// in one flat update directory).
const update::SfsEntry *find_entry(const update::SfsSection &section, std::string_view name) {
	for (const update::SfsEntry &entry : section.entries)
		if (strutil::iequals(entry.name, name))
			return &entry;
	return nullptr;
}

// The section's command file read and every row checked as the apply library will check it,
// so a package that would fail to apply is refused at its upload: `out` gets the version, the
// row count and the blobs' bytes.
bool check_section(const update::SfsSection &section, const update::SfsEntry &command,
		NewReleaseSection &out, std::string &why) {
	const std::optional<update::CommandFile> cmd =
			update::read_command_file(std::string(command.data.begin(), command.data.end()));
	if (!cmd) {
		why = "the command file " + command.name + " does not read";
		return false;
	}
	out.version = cmd->version;
	out.row_count = 0;
	for (const update::CommandRow &row : cmd->rows) {
		if (!update::is_safe_row_name(row.name)) {
			why = "the row \"" + row.name + "\" does not name a file";
			return false;
		}
		switch (row.type) {
			case update::CommandType::Target:
				if (row.name != "." && !strutil::ends_with_icase(row.name, ".pff")) {
					why = "the TARGET " + row.name + " is neither . nor a .pff";
					return false;
				}
				break;
			case update::CommandType::Remove:
				break;
			case update::CommandType::Loose:
			case update::CommandType::Copy: {
				if (row.name.size() >= update::kSfsNameBytes) {
					why = "the row " + row.name + " is longer than a packed update's 15 characters";
					return false;
				}
				const update::SfsEntry *entry = find_entry(section, row.name);
				if (entry == nullptr) {
					why = "the package has no file for the row " + row.name;
					return false;
				}
				std::vector<uint8_t> content;
				if (update::nlz_decode(entry->data.data(), entry->data.size(), content) ==
						update::NlzDecode::Corrupt) {
					why = "the file for the row " + row.name + " does not inflate";
					return false;
				}
				if (row.size < 0 || content.size() != static_cast<size_t>(row.size)) {
					why = "the file for the row " + row.name + " is " +
							std::to_string(content.size()) + " bytes where its row says " +
							std::to_string(row.size);
					return false;
				}
				if (row.crc && *row.crc != 0 &&
						*row.crc !=
								update::crc_as_signed(
										update::file_crc(content.data(), content.size()))) {
					why = "the file for the row " + row.name + " does not match its row's crc";
					return false;
				}
				break;
			}
		}
		++out.row_count;
	}
	out.blob_bytes = 0;
	for (const update::SfsEntry &entry : section.entries)
		if (&entry != &command)
			out.blob_bytes += static_cast<int64_t>(entry.data.size());
	return true;
}

// Each section of the package mapped to the one data set whose command file it carries: a base
// section (no PACKAGEBREAK) to a base data set, an expansion section to an expansion of its
// code and, when the package carries a base section, of that base. None or two refuse.
bool map_sections(const update::SfsPackage &package, const std::vector<DataSetRow> &data_sets,
		std::vector<NewReleaseSection> &out, std::string &why) {
	out.clear();
	std::optional<int64_t> base_id;
	std::set<int64_t> taken;
	for (const update::SfsSection &section : package.sections) {
		const bool expansion = !section.expansion.empty();
		const std::string label =
				expansion ? "the section " + section.expansion : std::string("the base section");
		const DataSetRow *match = nullptr;
		const update::SfsEntry *command = nullptr;
		for (const DataSetRow &ds : data_sets) {
			if (ds.command_file.empty() || (ds.kind == DataSetKind::Expansion) != expansion)
				continue;
			if (expansion && !strutil::iequals(ds.expansion_code, section.expansion))
				continue;
			if (expansion && base_id && ds.parent_id != base_id)
				continue;
			const update::SfsEntry *entry = find_entry(section, ds.command_file);
			if (entry == nullptr)
				continue;
			if (match != nullptr) {
				why = label + " carries the command files of two data sets (" +
						match->display_name + ", " + ds.display_name + ")";
				return false;
			}
			match = &ds;
			command = entry;
		}
		if (match == nullptr) {
			why = label + " carries no data set's command file";
			return false;
		}
		if (!taken.insert(match->id).second) {
			why = "two sections are " + match->display_name + "'s";
			return false;
		}
		if (!expansion)
			base_id = match->id;
		NewReleaseSection s;
		s.data_set_id = match->id;
		s.expansion = section.expansion;
		s.command_file = match->command_file;
		if (!check_section(section, *command, s, why)) {
			why = label + ": " + why;
			return false;
		}
		out.push_back(std::move(s));
	}
	if (out.empty()) {
		why = "the package carries no section";
		return false;
	}
	return true;
}

jobs::JobResult invalid_release(jobs::JobContext &ctx, int64_t release_id, const std::string &why) {
	auto lease = ctx.pool.acquire();
	set_release_status(*lease, release_id, ReleaseStatus::Validating, ReleaseStatus::Invalid, why);
	std::fprintf(stderr, "%s release %s invalid: %s\n", kTag, id_text(release_id).c_str(),
			loggable(why, 200).c_str());
	return jobs::JobResult::done();
}

// A store failure while validating: the job runs again; the release is invalid once no
// attempt is left.
jobs::JobResult release_transient(
		jobs::JobContext &ctx, int64_t release_id, const std::string &why) {
	if (ctx.job.attempts >= ctx.job.max_attempts) {
		auto lease = ctx.pool.acquire();
		set_release_status(*lease, release_id, ReleaseStatus::Validating, ReleaseStatus::Invalid,
				"the store: " + why);
		std::fprintf(stderr, "%s release %s invalid after %d attempts: %s\n", kTag,
				id_text(release_id).c_str(), ctx.job.attempts, loggable(why, 200).c_str());
		return jobs::JobResult::fail(why);
	}
	std::fprintf(stderr, "%s WARN release %s: %s; the job runs again\n", kTag,
			id_text(release_id).c_str(), loggable(why, 200).c_str());
	return jobs::JobResult::retry(why);
}

jobs::JobResult validate(
		jobs::JobContext &ctx, ObjectStore &store, const std::string &scratch_root) {
	const int64_t release_id = member_int(ctx.job.payload, "release_id");
	std::vector<DataSetRow> data_sets;
	{
		auto lease = ctx.pool.acquire();
		const std::optional<ReleaseRow> release = find_release(*lease, release_id);
		if (!release || release->status != ReleaseStatus::Validating)
			return jobs::JobResult::done();
		data_sets = list_data_sets(*lease);
	}
	std::printf("%s release %s: validating\n", kTag, id_text(release_id).c_str());
	const std::string upload_key = release_upload_key(release_id);
	std::string why;
	ScratchDir scratch(io::utf8_join(scratch_root, "release-" + id_text(release_id)));
	if (!scratch.make(why))
		return release_transient(ctx, release_id, why);
	const std::string path = scratch.file("upload.sfs");
	if (!store.get_file(upload_key, path, why))
		return release_transient(ctx, release_id, why);
	uint64_t size = 0;
	std::string sha256;
	if (!digest_file(path, size, sha256, why))
		return release_transient(ctx, release_id, why);
	{
		auto lease = ctx.pool.acquire();
		const std::optional<int64_t> other = find_release_by_sha(*lease, sha256);
		if (other && *other != release_id)
			return invalid_release(
					ctx, release_id, "already uploaded as release " + id_text(*other));
	}
	std::vector<uint8_t> bytes;
	if (!io::read_file_bytes(path, bytes, why))
		return release_transient(ctx, release_id, why);
	update::SfsPackage package;
	if (!update::sfs_read(bytes.data(), bytes.size(), package, why))
		return invalid_release(ctx, release_id, "not a 1SFS package: " + why);
	std::vector<NewReleaseSection> sections;
	if (!map_sections(package, data_sets, sections, why))
		return invalid_release(ctx, release_id, why);
	// The package under its own digest first, then the row: a run cut short between them
	// finds the upload still there and copies again; the upload goes once the row is draft.
	if (!store.copy(upload_key, release_package_key(sha256), why))
		return release_transient(ctx, release_id, why);
	{
		auto lease = ctx.pool.acquire();
		if (!set_release_validated(*lease, release_id, sha256, static_cast<int64_t>(size),
					package.product, sections)) {
			std::printf(
					"%s release %s left validating meanwhile\n", kTag, id_text(release_id).c_str());
			return jobs::JobResult::done();
		}
	}
	std::string ignored;
	if (!store.remove(upload_key, ignored))
		std::fprintf(stderr, "%s WARN release %s: the upload %s stays: %s\n", kTag,
				id_text(release_id).c_str(), upload_key.c_str(), loggable(ignored, 200).c_str());
	std::printf("%s release %s draft: \"%s\" %s, %zu section%s\n", kTag,
			id_text(release_id).c_str(), loggable(package.product).c_str(),
			strutil::byte_size_text(size).c_str(), sections.size(),
			sections.size() == 1 ? "" : "s");
	return jobs::JobResult::done();
}

// --- release.apply ---

// The section's rows applied to a copy of the baseline: the tree's objects under its own prefix
// (copied server-side, a run again's leftovers removed first), the package and the baseline
// files the rows touch brought to the scratch directory under the rows' own spellings (the
// apply library opens each file by its row's name), the section applied, what changed sized
// and uploaded and what went removed, and the manifest redone from the baseline's.
Outcome build_release_tree(ObjectStore &store, const std::string &scratch_root,
		const DataSetRow &data_set, const ReleaseSection &section, const std::string &sha256,
		const DataSetTree &baseline, int64_t tree_id, TreeReady &ready, std::string &why) {
	const std::string from = tree_prefix(baseline.id), to = tree_prefix(tree_id);
	if (!store.remove_prefix(to, why) || !store.copy_prefix(from, to, why))
		return Outcome::Transient;
	ScratchDir scratch(io::utf8_join(scratch_root, "apply-" + id_text(tree_id)));
	if (!scratch.make(why))
		return Outcome::Transient;
	const std::string dir = scratch.file("tree");
	if (!io::ensure_directory(dir, why))
		return Outcome::Transient;

	// The package and its section for this data set.
	const std::string package_path = scratch.file("package.sfs");
	if (!store.get_file(release_package_key(sha256), package_path, why))
		return Outcome::Transient;
	std::vector<uint8_t> bytes;
	if (!io::read_file_bytes(package_path, bytes, why))
		return Outcome::Transient;
	update::SfsPackage package;
	if (!update::sfs_read(bytes.data(), bytes.size(), package, why)) {
		why = "the package does not read: " + why;
		return Outcome::Refused;
	}
	bytes.clear();
	bytes.shrink_to_fit();
	const update::SfsSection *packed = nullptr;
	for (const update::SfsSection &s : package.sections)
		if (strutil::iequals(s.expansion, section.expansion))
			packed = &s;
	if (packed == nullptr) {
		why = "the package has no section for " + data_set.display_name;
		return Outcome::Refused;
	}
	const update::SfsEntry *command = find_entry(*packed, section.command_file);
	if (command == nullptr) {
		why = "the package has no command file " + section.command_file;
		return Outcome::Refused;
	}
	const std::optional<update::CommandFile> cmd =
			update::read_command_file(std::string(command->data.begin(), command->data.end()));
	if (!cmd) {
		why = "the command file " + section.command_file + " does not read";
		return Outcome::Refused;
	}

	// The baseline files the rows touch, each under the spelling its row opens it by: the
	// loose files written or removed at the root, the archives targeted, and the mission pair
	// the rescan reads. A row's name the tree does not hold (Jointops.exe) brings nothing.
	std::map<std::string, std::string> spelling; // canonical name -> the name on disk
	const auto want = [&](const std::string &name) {
		const std::string canonical =
				canonical_tree_file(data_set.kind, data_set.expansion_code, name);
		if (!canonical.empty())
			spelling.emplace(canonical, name);
	};
	std::string target = ".";
	for (const update::CommandRow &row : cmd->rows) {
		switch (row.type) {
			case update::CommandType::Target:
				target = row.name;
				if (strutil::ends_with_icase(target, ".pff"))
					want(target);
				break;
			case update::CommandType::Loose:
			case update::CommandType::Remove:
				if (target == ".")
					want(row.name);
				break;
			case update::CommandType::Copy:
				break;
		}
	}
	const MissionPair pair = mission_pair_of(data_set.kind, data_set.expansion_code);
	want(pair.missions);
	want(pair.texts);
	std::map<std::string, TreeFile> files; // the new manifest, from the baseline's
	for (const TreeFile &f : baseline.files)
		files[f.name] = f;
	std::map<std::string, std::string> local; // canonical name -> its path, of what is here
	for (const auto &[canonical, name] : spelling) {
		if (files.count(canonical) == 0)
			continue;
		const std::string path = io::utf8_join(dir, name);
		if (!store.get_file(from + canonical, path, why))
			return Outcome::Transient;
		local[canonical] = path;
	}

	update::ApplyReport report;
	if (!update::apply_section(*packed, section.command_file, io::os_path(dir), report, why))
		return Outcome::Refused;

	// What changed, under its canonical name; what the tree does not hold is left here.
	std::vector<std::string> changed = report.files_written;
	changed.insert(
			changed.end(), report.archives_rewritten.begin(), report.archives_rewritten.end());
	for (const std::string &name : changed) {
		const std::string canonical =
				canonical_tree_file(data_set.kind, data_set.expansion_code, name);
		if (canonical.empty()) {
			std::printf("%s tree %s: the release writes %s, which the tree does not hold\n", kTag,
					id_text(tree_id).c_str(), loggable(name).c_str());
			continue;
		}
		const std::string path = io::utf8_join(dir, name);
		TreeFile file;
		file.name = canonical;
		if (!measure_file(path, file.size, why))
			return Outcome::Transient;
		if (!store.put_file(to + canonical, path, why))
			return Outcome::Transient;
		files[canonical] = file;
		local[canonical] = path;
	}
	for (const std::string &name : report.files_deleted) {
		const std::string canonical =
				canonical_tree_file(data_set.kind, data_set.expansion_code, name);
		if (canonical.empty())
			continue;
		if (!store.remove(to + canonical, why))
			return Outcome::Transient;
		files.erase(canonical);
		local.erase(canonical);
	}
	ready = TreeReady{};
	std::vector<std::string> names;
	for (const auto &[name, file] : files) {
		ready.files.push_back(file);
		names.push_back(name);
	}
	const std::vector<std::string> missing =
			missing_tree_files(data_set.kind, data_set.expansion_code, names);
	if (!missing.empty()) {
		why = "the release leaves the tree without " + join(missing);
		return Outcome::Refused;
	}
	ready.bytes = total_bytes(ready.files);
	// The version from the file when it is here (written, or brought for a row), else the
	// baseline's while the tree still holds it.
	const auto version = local.find("version.txt");
	if (version != local.end())
		ready.version_text = version_text_of(version->second);
	else if (files.count("version.txt") != 0)
		ready.version_text = baseline.version_text;
	if (!scan_missions(data_set, dir, local, ready.missions, why))
		return Outcome::Refused;
	if (!put_manifest(store, scratch, tree_id, ready, why))
		return Outcome::Transient;
	std::printf("%s tree %s: %zu row%s applied, %zu file%s written, %zu removed\n", kTag,
			id_text(tree_id).c_str(), report.rows_applied, report.rows_applied == 1 ? "" : "s",
			changed.size(), changed.size() == 1 ? "" : "s", report.files_deleted.size());
	return Outcome::Ok;
}

jobs::JobResult apply(jobs::JobContext &ctx, ObjectStore &store, const std::string &scratch_root) {
	const int64_t release_id = member_int(ctx.job.payload, "release_id");
	const int64_t data_set_id = member_int(ctx.job.payload, "data_set_id");
	std::optional<ReleaseSection> section;
	std::optional<DataSetRow> data_set;
	std::optional<DataSetTree> building; // a run again's tree
	std::string sha256;
	{
		auto lease = ctx.pool.acquire();
		db::Database &db = *lease;
		const std::optional<ReleaseRow> release = find_release(db, release_id);
		// Retired, or superseded by a newer baseline or release meanwhile: nothing to build.
		if (!release || release->status != ReleaseStatus::Published)
			return jobs::JobResult::done();
		sha256 = release->sha256;
		data_set = find_data_set(db, data_set_id);
		if (!data_set || data_set->published_release_id != release_id)
			return jobs::JobResult::done();
		for (const ReleaseSection &s : list_sections(db, release_id))
			if (s.data_set_id == data_set_id)
				section = s;
		if (!section)
			return jobs::JobResult::done();
		if (section->tree_id) {
			building = find_tree(db, *section->tree_id);
			// Built already (ready), or failed for good: the admin's to act on.
			if (!building || building->state != TreeState::Building)
				return jobs::JobResult::done();
		}
	}
	std::printf("%s release %s -> %s: applying%s\n", kTag, id_text(release_id).c_str(),
			loggable(data_set->display_name).c_str(), building ? " again" : "");
	std::optional<DataSetTree> baseline;
	int64_t tree_id = 0;
	{
		auto lease = ctx.pool.acquire();
		db::Database &db = *lease;
		if (data_set->baseline_tree_id)
			baseline = find_tree(db, *data_set->baseline_tree_id);
		if (baseline && baseline->state != TreeState::Ready)
			baseline.reset();
		if (building) {
			tree_id = building->id;
		} else {
			// The section's tree, made with its row in one transaction.
			db::Transaction tx(db);
			tree_id = create_tree(db, data_set_id, TreeSource::Release, TreeState::Building,
					release_id, baseline ? std::optional<int64_t>(baseline->id) : std::nullopt,
					"worker");
			set_section_tree(db, release_id, data_set_id, tree_id);
			tx.commit();
		}
		if (!baseline) {
			set_tree_state(
					db, tree_id, TreeState::Building, TreeState::Failed, "no baseline uploaded");
			std::fprintf(stderr, "%s tree %s failed: %s has no baseline uploaded\n", kTag,
					id_text(tree_id).c_str(), loggable(data_set->display_name).c_str());
			return jobs::JobResult::done();
		}
	}
	TreeReady ready;
	std::string why;
	const Outcome outcome = build_release_tree(
			store, scratch_root, *data_set, *section, sha256, *baseline, tree_id, ready, why);
	return settle_tree(ctx, tree_id, TreeState::Building, outcome, ready, why);
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

std::string release_package_key(const std::string &sha256) {
	return "updates/" + sha256 + ".sfs";
}

std::string release_upload_key(int64_t release_id) {
	return "updates/upload-" + id_text(release_id) + ".sfs";
}

int64_t enqueue_tree_finalize(db::Database &db, int64_t tree_id) {
	jobs::NewJob job;
	job.kind = kJobTreeFinalize;
	job.payload.set("tree_id", io::json_number(static_cast<double>(tree_id)));
	job.dedupe_key = std::string(kJobTreeFinalize) + ":" + id_text(tree_id);
	job.max_attempts = kAttemptsFinalize;
	return jobs::enqueue(db, job);
}

int64_t enqueue_release_validate(db::Database &db, int64_t release_id) {
	jobs::NewJob job;
	job.kind = kJobReleaseValidate;
	job.payload.set("release_id", io::json_number(static_cast<double>(release_id)));
	job.dedupe_key = std::string(kJobReleaseValidate) + ":" + id_text(release_id);
	job.max_attempts = kAttemptsValidate;
	return jobs::enqueue(db, job);
}

int64_t enqueue_release_apply(db::Database &db, int64_t release_id, int64_t data_set_id) {
	jobs::NewJob job;
	job.kind = kJobReleaseApply;
	job.payload.set("release_id", io::json_number(static_cast<double>(release_id)));
	job.payload.set("data_set_id", io::json_number(static_cast<double>(data_set_id)));
	job.dedupe_key =
			std::string(kJobReleaseApply) + ":" + id_text(release_id) + ":" + id_text(data_set_id);
	job.max_attempts = kAttemptsApply;
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
	jobs::JobKind validate_kind{ kJobReleaseValidate,
		[&store, scratch_dir](jobs::JobContext &ctx) { return validate(ctx, store, scratch_dir); },
		kCapValidate };
	validate_kind.retry_base_ms = 5 * 1000;
	validate_kind.retry_max_ms = 60 * 1000;
	registry.add(std::move(validate_kind));
	jobs::JobKind apply_kind{ kJobReleaseApply,
		[&store, scratch_dir](jobs::JobContext &ctx) { return apply(ctx, store, scratch_dir); },
		kCapApply };
	apply_kind.retry_base_ms = 5 * 1000;
	apply_kind.retry_max_ms = 60 * 1000;
	registry.add(std::move(apply_kind));
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
