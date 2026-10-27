#include "product_jobs.h"

#include "core/loggable.h"
#include "jobs/job_queue.h"
#include "product_repository.h"
#include "project_repository.h"
#include "tree_files.h"

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

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

namespace fs = std::filesystem;

constexpr const char *kTag = "[data]";
constexpr int kCapValidate = 1;
constexpr int kCapApply = 1;
constexpr int kCapPrune = 1;
constexpr int kCapPurge = 1;
constexpr int kAttemptsValidate = 2;
constexpr int kAttemptsApply = 3;
constexpr int kAttemptsPrune = 3;
constexpr int kAttemptsPurge = 10;
constexpr int64_t kPrunePeriodMs = 60 * 60 * 1000;

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

// The multiplayer missions of a tree's files at `dir` (`files`: the canonical name of each
// tree file to the path it lies at): a base tree mounted as opennova-serve mounts an install
// (the retail boot table, mission_catalog::build), an expansion tree its pair alone
// (mission_catalog::archive_rows over the archives, then the catalog's order).
bool scan_missions(const ProductRow &product, const std::string &dir,
		const std::map<std::string, std::string> &files, std::vector<TreeMission> &out,
		std::string &why) {
	std::vector<mission_catalog::Row> rows;
	if (product.kind == ProductKind::Game) {
		ResourceIndex index;
		if (!index.scan(
					dir, std::string(), VfsMountMode::Packed, VfsArchiveDiscovery::RetailTable) ||
				!index.has_mounted_archive()) {
			why = "the tree does not mount as an install: " + index.last_error();
			return false;
		}
		// The service lists a tree's own tables: no expansion override table is installed.
		rows = mission_catalog::build(index, nullptr);
		index.clear();
	} else {
		const MissionPair pair = mission_pair_of(product.kind, product.expansion_code);
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
		rows = mission_catalog::archive_rows(missions_archive, &texts_archive, nullptr);
		pff::pff_close(&missions_archive);
		pff::pff_close(&texts_archive);
		mission_catalog::sort_rows(rows);
	}
	out.clear();
	for (const mission_catalog::Row &row : rows) {
		if (row.game_mode == 0)
			continue; // a single-player mission: no server rotates it
		if (row.loose)
			continue; // a stray file in the scratch folder: the tree keeps no loose mission
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

int64_t total_bytes(const std::vector<TreeFile> &files) {
	int64_t bytes = 0;
	for (const TreeFile &f : files)
		bytes += static_cast<int64_t>(f.size);
	return bytes;
}

// The building tree's row settled from what its job came to: ready (and a prune queued for
// whatever that frees), failed with the reason, or the job run again for a store failure, the
// tree failed once no attempt is left.
jobs::JobResult settle_tree(jobs::JobContext &ctx, int64_t tree_id, Outcome outcome,
		const TreeReady &ready, const std::string &why) {
	auto lease = ctx.pool.acquire();
	db::Database &db = *lease;
	switch (outcome) {
		case Outcome::Ok:
			if (!finish_tree(db, tree_id, ready)) {
				std::printf("%s tree %s left building meanwhile; nothing written\n", kTag,
						std::to_string(tree_id).c_str());
				return jobs::JobResult::done();
			}
			std::printf("%s tree %s ready: %zu file%s, %s, %zu mission%s, version \"%s\"\n", kTag,
					std::to_string(tree_id).c_str(), ready.files.size(),
					ready.files.size() == 1 ? "" : "s",
					strutil::byte_size_text(static_cast<uint64_t>(ready.bytes)).c_str(),
					ready.missions.size(), ready.missions.size() == 1 ? "" : "s",
					loggable(ready.version_text).c_str());
			enqueue_tree_prune(db);
			return jobs::JobResult::done();
		case Outcome::Refused:
			set_tree_state(db, tree_id, TreeState::Building, TreeState::Failed, why);
			std::fprintf(stderr, "%s tree %s failed: %s\n", kTag, std::to_string(tree_id).c_str(),
					loggable(why, 200).c_str());
			return jobs::JobResult::done();
		case Outcome::Transient:
			break;
	}
	if (ctx.job.attempts >= ctx.job.max_attempts) {
		set_tree_state(db, tree_id, TreeState::Building, TreeState::Failed, "the store: " + why);
		std::fprintf(stderr, "%s tree %s failed after %d attempts: %s\n", kTag,
				std::to_string(tree_id).c_str(), ctx.job.attempts, loggable(why, 200).c_str());
		return jobs::JobResult::fail(why);
	}
	std::fprintf(stderr, "%s WARN tree %s: %s; the job runs again\n", kTag,
			std::to_string(tree_id).c_str(), loggable(why, 200).c_str());
	return jobs::JobResult::retry(why);
}

// --- release.validate ---

// The section's entry named `name`, as the apply library finds it (without case: the blobs land
// in one flat update directory).
const update::SfsEntrySpan *find_entry(
		const update::SfsSectionSpans &section, std::string_view name) {
	for (const update::SfsEntrySpan &entry : section.entries)
		if (strutil::iequals(entry.name, name))
			return &entry;
	return nullptr;
}

// A package on disk, its entries located (formats/update/sfs.h sfs_locate) and each read only
// when asked: a whole game's release, hundreds of megabytes, is never held in memory.
class PackageFile {
public:
	PackageFile() = default;
	PackageFile(const PackageFile &) = delete;
	PackageFile &operator=(const PackageFile &) = delete;
	~PackageFile() {
		if (file_ != nullptr)
			std::fclose(file_);
	}

	// False with `why` when the file does not open or holds no package.
	bool open(const std::string &path, std::string &why) {
		file_ = io::fopen_utf8(path.c_str(), "rb");
		if (file_ == nullptr) {
			why = "cannot open " + path;
			return false;
		}
		return update::sfs_locate(file_, layout_, why);
	}
	const update::SfsLayout &layout() const { return layout_; }

	bool read(const update::SfsEntrySpan &entry, std::vector<uint8_t> &out, std::string &why) {
		out.resize(entry.size);
		if (io::fseek64(file_, static_cast<int64_t>(entry.offset), SEEK_SET) != 0 ||
				(entry.size != 0 && std::fread(out.data(), 1, entry.size, file_) != entry.size)) {
			why = "cannot read " + entry.name + " from the package";
			return false;
		}
		return true;
	}

	// The section's blobs by name, without case, as the apply library reads them.
	update::SectionBlobs blobs(const update::SfsSectionSpans &section) {
		auto by_name = std::make_shared<std::map<std::string, const update::SfsEntrySpan *>>();
		for (const update::SfsEntrySpan &entry : section.entries)
			by_name->emplace(strutil::to_lower(entry.name), &entry);
		return [this, by_name](std::string_view name, std::vector<uint8_t> &packed,
					   std::string &why) {
			const auto it = by_name->find(strutil::to_lower(std::string(name)));
			if (it == by_name->end()) {
				why = "the package has no file for the row " + std::string(name);
				return false;
			}
			return read(*it->second, packed, why);
		};
	}

private:
	std::FILE *file_ = nullptr;
	update::SfsLayout layout_;
};

// The section's command file read and every row checked as the apply library will check it,
// so a package that would fail to apply is refused at its upload: `out` gets the version, the
// row count and the blobs' bytes. Each blob is read and inflated alone.
bool check_section(PackageFile &package, const update::SfsSectionSpans &section,
		const update::SfsEntrySpan &command, NewReleaseSection &out, std::string &why) {
	std::vector<uint8_t> command_bytes;
	if (!package.read(command, command_bytes, why))
		return false;
	const std::optional<update::CommandFile> cmd = update::read_command_file(
			std::string(command_bytes.begin(), command_bytes.end()));
	if (!cmd) {
		why = "the command file " + command.name + " does not read";
		return false;
	}
	const update::SectionBlobs blobs = package.blobs(section);
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
				std::vector<uint8_t> packed, content;
				if (!blobs(row.name, packed, why))
					return false;
				if (update::nlz_decode(packed.data(), packed.size(), content) ==
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
	for (const update::SfsEntrySpan &entry : section.entries)
		if (&entry != &command)
			out.blob_bytes += static_cast<int64_t>(entry.size);
	return true;
}

// Each section of the package mapped to the one product whose command file it carries: a base
// section (no PACKAGEBREAK) to a game or an app, an expansion section
// to an expansion of its code and, when the package carries a base section, of that base.
// None or two refuse.
bool map_sections(PackageFile &package, const std::vector<ProductRow> &products,
		std::vector<NewReleaseSection> &out, std::string &why) {
	out.clear();
	std::optional<int64_t> base_id;
	std::set<int64_t> taken;
	for (const update::SfsSectionSpans &section : package.layout().sections) {
		const bool expansion = !section.expansion.empty();
		const std::string label =
				expansion ? "the section " + section.expansion : std::string("the base section");
		const ProductRow *match = nullptr;
		const update::SfsEntrySpan *command = nullptr;
		for (const ProductRow &ds : products) {
			if (ds.command_file.empty() || (ds.kind == ProductKind::Expansion) != expansion)
				continue;
			if (expansion && !strutil::iequals(ds.expansion_code, section.expansion))
				continue;
			if (expansion && base_id && ds.parent_id != base_id)
				continue;
			const update::SfsEntrySpan *entry = find_entry(section, ds.command_file);
			if (entry == nullptr)
				continue;
			if (match != nullptr) {
				why = label + " carries the command files of two products (" +
						match->display_name + ", " + ds.display_name + ")";
				return false;
			}
			match = &ds;
			command = entry;
		}
		if (match == nullptr) {
			why = label + " carries no product's command file";
			return false;
		}
		if (!taken.insert(match->id).second) {
			why = "two sections are " + match->display_name + "'s";
			return false;
		}
		if (!expansion)
			base_id = match->id;
		NewReleaseSection s;
		s.product_id = match->id;
		s.expansion = section.expansion;
		s.command_file = match->command_file;
		if (!check_section(package, section, *command, s, why)) {
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
	std::fprintf(stderr, "%s release %s invalid: %s\n", kTag, std::to_string(release_id).c_str(),
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
				std::to_string(release_id).c_str(), ctx.job.attempts, loggable(why, 200).c_str());
		return jobs::JobResult::fail(why);
	}
	std::fprintf(stderr, "%s WARN release %s: %s; the job runs again\n", kTag,
			std::to_string(release_id).c_str(), loggable(why, 200).c_str());
	return jobs::JobResult::retry(why);
}

jobs::JobResult validate(
		jobs::JobContext &ctx, ObjectStore &store, const std::string &scratch_root) {
	const int64_t release_id = jobs::member_int(ctx.job.payload, "release_id");
	std::vector<ProductRow> products;
	{
		auto lease = ctx.pool.acquire();
		const std::optional<ReleaseRow> release = find_release(*lease, release_id);
		if (!release || release->status != ReleaseStatus::Validating)
			return jobs::JobResult::done();
		products = list_products(*lease);
	}
	std::printf("%s release %s: validating\n", kTag, std::to_string(release_id).c_str());
	const std::string upload_key = release_upload_key(release_id);
	std::string why;
	ScratchDir scratch(io::utf8_join(scratch_root, "release-" + std::to_string(release_id)));
	if (!scratch.make(why))
		return release_transient(ctx, release_id, why);
	const std::string path = scratch.file("upload.sfs");
	if (!store.get_file(upload_key, path, why))
		return release_transient(ctx, release_id, why);
	uint64_t size = 0;
	std::string sha256;
	if (!digest_file(path, size, sha256, why))
		return release_transient(ctx, release_id, why);
	std::optional<int64_t> other;
	{
		auto lease = ctx.pool.acquire();
		other = find_release_by_sha(*lease, sha256);
	}
	if (other && *other != release_id)
		return invalid_release(
				ctx, release_id, "already uploaded as release " + std::to_string(*other));
	PackageFile package;
	if (!package.open(path, why))
		return invalid_release(ctx, release_id, "not a 1SFS package: " + why);
	std::vector<NewReleaseSection> sections;
	if (!map_sections(package, products, sections, why))
		return invalid_release(ctx, release_id, why);
	// The package under its own digest first, then the row: a run cut short between them
	// finds the upload still there and copies again; the upload goes once the row is draft.
	if (!store.copy(upload_key, release_package_key(sha256), why))
		return release_transient(ctx, release_id, why);
	{
		auto lease = ctx.pool.acquire();
		if (!set_release_validated(*lease, release_id, sha256, static_cast<int64_t>(size),
					package.layout().product, sections)) {
			std::printf("%s release %s left validating meanwhile\n", kTag,
					std::to_string(release_id).c_str());
			return jobs::JobResult::done();
		}
	}
	std::string ignored;
	if (!store.remove(upload_key, ignored))
		std::fprintf(stderr, "%s WARN release %s: the upload %s stays: %s\n", kTag,
				std::to_string(release_id).c_str(), upload_key.c_str(),
				loggable(ignored, 200).c_str());
	std::printf("%s release %s draft: \"%s\" %s, %zu section%s\n", kTag,
			std::to_string(release_id).c_str(), loggable(package.layout().product).c_str(),
			strutil::byte_size_text(size).c_str(), sections.size(),
			sections.size() == 1 ? "" : "s");
	return jobs::JobResult::done();
}

// --- release.apply ---

// The section's rows applied to an empty tree: the tree's objects under its own prefix (a run
// again's leftovers removed first), the package brought to the scratch directory, the section
// applied there with formats/update/apply.h (every release is whole, so its rows make every
// archive from nothing, ADR 0008), the files the tree takes (tree_files.h) checked, read for
// the missions, sized and uploaded, and the manifest written.
Outcome build_release_tree(ObjectStore &store, const std::string &scratch_root,
		const ProductRow &product, const ReleaseSection &section, const std::string &sha256,
		int64_t tree_id, TreeReady &ready, std::string &why) {
	const std::string to = tree_prefix(tree_id);
	if (!store.remove_prefix(to, why))
		return Outcome::Transient;
	ScratchDir scratch(io::utf8_join(scratch_root, "apply-" + std::to_string(tree_id)));
	if (!scratch.make(why))
		return Outcome::Transient;
	const std::string dir = scratch.file("tree");
	if (!io::ensure_directory(dir, why))
		return Outcome::Transient;

	// The package and its section for this product, its entries read as the rows need them.
	const std::string package_path = scratch.file("package.sfs");
	if (!store.get_file(release_package_key(sha256), package_path, why))
		return Outcome::Transient;
	PackageFile package;
	if (!package.open(package_path, why)) {
		why = "the package does not read: " + why;
		return Outcome::Refused;
	}
	const update::SfsSectionSpans *packed = nullptr;
	for (const update::SfsSectionSpans &s : package.layout().sections)
		if (strutil::iequals(s.expansion, section.expansion))
			packed = &s;
	if (packed == nullptr) {
		why = "the package has no section for " + product.display_name;
		return Outcome::Refused;
	}
	const update::SfsEntrySpan *command = find_entry(*packed, section.command_file);
	if (command == nullptr) {
		why = "the package has no command file " + section.command_file;
		return Outcome::Refused;
	}
	std::vector<uint8_t> command_bytes;
	if (!package.read(*command, command_bytes, why))
		return Outcome::Transient;
	update::ApplyReport report;
	if (!update::apply_commands(command_bytes, section.command_file, package.blobs(*packed),
				io::os_path(dir), report, why))
		return Outcome::Refused;

	// What the rows left that the tree takes, under its canonical name; the rest (a loose file no
	// server reads, Jointops.exe) stays here, and a name a later row removed is gone.
	std::vector<std::string> written = report.files_written;
	written.insert(
			written.end(), report.archives_rewritten.begin(), report.archives_rewritten.end());
	std::map<std::string, std::string> local; // canonical name -> its path
	size_t left_out = 0;
	for (const std::string &name : written) {
		const std::string canonical =
				canonical_tree_file(product.kind, product.expansion_code, name);
		const std::string path = io::utf8_join(dir, name);
		std::error_code ec;
		if (canonical.empty())
			++left_out;
		else if (fs::is_regular_file(io::os_path(path), ec))
			local[canonical] = path;
	}
	std::vector<std::string> names;
	for (const auto &[name, path] : local)
		names.push_back(name);
	const std::vector<std::string> missing =
			missing_tree_files(product.kind, product.expansion_code, names);
	if (!missing.empty()) {
		why = "the release leaves the tree without " + join(missing);
		return Outcome::Refused;
	}
	ready = TreeReady{};
	// The release's version, its command file's VERSION (ADR 0067: no version file is read for a
	// release).
	ready.version_text = section.version;
	if (!scan_missions(product, dir, local, ready.missions, why))
		return Outcome::Refused;
	for (const auto &[name, path] : local) {
		TreeFile file;
		file.name = name;
		if (!measure_file(path, file.size, why) || !store.put_file(to + name, path, why))
			return Outcome::Transient;
		ready.files.push_back(std::move(file));
	}
	ready.bytes = total_bytes(ready.files);
	if (!put_manifest(store, scratch, tree_id, ready, why))
		return Outcome::Transient;
	std::printf("%s tree %s: %zu row%s applied, %zu file%s of the tree, %zu left out\n", kTag,
			std::to_string(tree_id).c_str(), report.rows_applied,
			report.rows_applied == 1 ? "" : "s", ready.files.size(),
			ready.files.size() == 1 ? "" : "s", left_out);
	return Outcome::Ok;
}

jobs::JobResult apply(jobs::JobContext &ctx, ObjectStore &store, const std::string &scratch_root) {
	const int64_t release_id = jobs::member_int(ctx.job.payload, "release_id");
	const int64_t product_id = jobs::member_int(ctx.job.payload, "product_id");
	std::optional<ReleaseSection> section;
	std::optional<ProductRow> product;
	std::optional<ProductTree> building; // a run again's tree
	std::string sha256;
	{
		auto lease = ctx.pool.acquire();
		db::Database &db = *lease;
		const std::optional<ReleaseRow> release = find_release(db, release_id);
		// Retired, or superseded by a newer release meanwhile: nothing to build.
		if (!release || release->status != ReleaseStatus::Published)
			return jobs::JobResult::done();
		sha256 = release->sha256;
		product = find_product(db, product_id);
		if (!product || product->kind == ProductKind::App ||
				product->published_release_id != release_id)
			return jobs::JobResult::done();
		for (const ReleaseSection &s : list_sections(db, release_id))
			if (s.product_id == product_id)
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
	std::printf("%s release %s -> %s: applying%s\n", kTag, std::to_string(release_id).c_str(),
			loggable(product->display_name).c_str(), building ? " again" : "");
	int64_t tree_id = 0;
	if (building) {
		tree_id = building->id;
	} else {
		// The section's tree, made with its row in one transaction.
		auto lease = ctx.pool.acquire();
		db::Transaction tx(*lease);
		tree_id = create_tree(*lease, product_id, release_id, "worker");
		set_section_tree(*lease, release_id, product_id, tree_id);
		tx.commit();
	}
	TreeReady ready;
	std::string why;
	const Outcome outcome = build_release_tree(
			store, scratch_root, *product, *section, sha256, tree_id, ready, why);
	return settle_tree(ctx, tree_id, outcome, ready, why);
}

// --- tree.prune ---

jobs::JobResult prune(jobs::JobContext &ctx, ObjectStore &store) {
	std::vector<int64_t> ids;
	{
		auto lease = ctx.pool.acquire();
		ids = prunable_trees(*lease, kPruneMinAgeS);
	}
	if (ids.empty())
		return jobs::JobResult::done();
	std::printf("%s pruning %zu tree%s\n", kTag, ids.size(), ids.size() == 1 ? "" : "s");
	size_t pruned = 0;
	std::string first_error;
	for (const int64_t id : ids) {
		const std::string prefix = tree_prefix(id);
		std::string error;
		if (!store.remove_prefix(prefix, error)) {
			std::fprintf(stderr, "%s WARN tree %s: not pruned: %s\n", kTag,
					std::to_string(id).c_str(), loggable(error, 200).c_str());
			if (first_error.empty())
				first_error = "tree " + std::to_string(id) + ": " + error;
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

// --- product.purge ---

// A deleted game's objects: its trees' prefixes and its project's (content/<id>/), each removed
// whole. Its row went with the delete, its trees' rows with it, so the payload names them; a
// prefix already gone removes nothing, so a run again after a store failure takes the rest.
jobs::JobResult purge(jobs::JobContext &ctx, ObjectStore &store) {
	const int64_t product_id = jobs::member_int(ctx.job.payload, "product_id");
	std::vector<std::string> prefixes;
	if (const io::JsonValue *trees = ctx.job.payload.get("trees"); trees && trees->is_array())
		for (const io::JsonValue &tree : trees->array)
			if (tree.is_number() && tree.number > 0)
				prefixes.push_back(tree_prefix(static_cast<int64_t>(tree.number)));
	if (product_id > 0)
		prefixes.push_back(projects::project_prefix(product_id));
	for (const std::string &prefix : prefixes) {
		std::string error;
		if (!store.remove_prefix(prefix, error)) {
			std::fprintf(stderr, "%s WARN game %s: %s not removed: %s\n", kTag,
					std::to_string(product_id).c_str(), prefix.c_str(),
					loggable(error, 200).c_str());
			return jobs::JobResult::retry(prefix + ": " + error);
		}
	}
	std::printf("%s game %s purged: %zu prefix%s removed\n", kTag,
			std::to_string(product_id).c_str(), prefixes.size(), prefixes.size() == 1 ? "" : "es");
	return jobs::JobResult::done();
}

} // namespace

std::string tree_prefix(int64_t tree_id) {
	return "trees/" + std::to_string(tree_id) + "/";
}

std::string release_package_key(const std::string &sha256) {
	return "updates/" + sha256 + ".sfs";
}

std::string release_upload_key(int64_t release_id) {
	return "updates/upload-" + std::to_string(release_id) + ".sfs";
}

std::string release_source_key(int64_t release_id) {
	return "updates/source-" + std::to_string(release_id) + ".zip";
}

std::string release_download_key(const std::string &sha256) {
	return "updates/" + sha256 + ".zip";
}

// Streamed (a cumulative package is hundreds of megabytes).
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

int64_t enqueue_release_validate(db::Database &db, int64_t release_id) {
	jobs::NewJob job;
	job.kind = kJobReleaseValidate;
	job.payload.set("release_id", io::json_number(static_cast<double>(release_id)));
	job.dedupe_key = std::string(kJobReleaseValidate) + ":" + std::to_string(release_id);
	job.max_attempts = kAttemptsValidate;
	return jobs::enqueue(db, job);
}

int64_t enqueue_release_apply(db::Database &db, int64_t release_id, int64_t product_id) {
	const auto product = find_product(db, product_id);
	if (!product || product->kind == ProductKind::App)
		return 0;
	jobs::NewJob job;
	job.kind = kJobReleaseApply;
	job.payload.set("release_id", io::json_number(static_cast<double>(release_id)));
	job.payload.set("product_id", io::json_number(static_cast<double>(product_id)));
	job.dedupe_key = std::string(kJobReleaseApply) + ":" + std::to_string(release_id) + ":" +
			std::to_string(product_id);
	job.max_attempts = kAttemptsApply;
	return jobs::enqueue(db, job);
}

int64_t enqueue_product_purge(
		db::Database &db, int64_t product_id, const std::vector<int64_t> &tree_ids) {
	jobs::NewJob job;
	job.kind = kJobProductPurge;
	job.payload.set("product_id", io::json_number(static_cast<double>(product_id)));
	io::JsonValue trees = io::JsonValue::make_array();
	for (const int64_t id : tree_ids)
		trees.push(io::json_number(static_cast<double>(id)));
	job.payload.set("trees", std::move(trees));
	job.dedupe_key = std::string(kJobProductPurge) + ":" + std::to_string(product_id);
	job.delay_ms = kProductPurgeDelaySeconds * 1000;
	job.max_attempts = kAttemptsPurge;
	return jobs::enqueue(db, job);
}

int64_t enqueue_tree_prune(db::Database &db) {
	jobs::NewJob job;
	job.kind = kJobTreePrune;
	job.dedupe_key = kJobTreePrune;
	job.max_attempts = kAttemptsPrune;
	return jobs::enqueue(db, job);
}

void register_product_jobs(
		jobs::JobRegistry &registry, ObjectStore &store, std::string scratch_dir) {
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
	jobs::JobKind purge_kind{ kJobProductPurge,
		[&store](jobs::JobContext &ctx) { return purge(ctx, store); }, kCapPurge };
	purge_kind.retry_base_ms = 10 * 1000;
	purge_kind.retry_max_ms = 10 * 60 * 1000;
	registry.add(std::move(purge_kind));
}

std::vector<jobs::PeriodicJob> product_schedule() {
	jobs::PeriodicJob p;
	p.kind = kJobTreePrune;
	p.period_ms = kPrunePeriodMs;
	p.max_attempts = kAttemptsPrune;
	return { std::move(p) };
}

} // namespace opennova::novaworld_server::hosted
