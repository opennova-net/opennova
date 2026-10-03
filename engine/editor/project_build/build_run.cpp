#include <editor/project_build/build_run.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>
#include <utility>

#include <base/io/file_time.h>
#include <base/io/hash.h>
#include <base/io/json.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <formats/pff/pff.h>
#include <formats/pff/pff_stream_writer.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// The archive flavour the target game ships: every JO-family title reads PFF3
// [orig: PFF_Open @ 0x7682e0]; the profile keys the SCR policy only, so the format
// rides here until a profile row carries it.
constexpr pff::PffFormat kBuildArchiveFormat = pff::PFF_FORMAT_PFF3;

// The largest single read, write or copy inside a step: a budget spans several.
constexpr uint64_t kChunkBytes = uint64_t(1) << 20;
// What opening a file costs a step (or asking its size and last write, where the hash cache
// vouches for its content), so one over many small files stays short.
constexpr uint64_t kOpenCost = 4096;
// run_build's step: a whole build in few steps, its archives reported as they land.
constexpr uint64_t kRunBuildStepBytes = uint64_t(4) << 20;
// The staging directories a build tries, `<id>.tmp` then `<id>.1.tmp` and on, while the one
// before cannot be emptied.
constexpr int kStagingAttempts = 8;
// How long the publish keeps trying a rename refused while its refusal may pass (a scanner or an
// indexer holding a staged file), a step at a time.
constexpr std::chrono::seconds kPublishPatience{3};

// A file's last write, as a number two reads of the same file compare equal by (0 when it cannot
// be read).
int64_t last_write_of(const std::string &path) {
	return io::file_modified_ticks(system_path(path));
}

// The staging directory's name for a build's `attempt`-th try: <id>.tmp, <id>.1.tmp, ...
std::string staging_name(const std::string &build_id, int attempt) {
	return attempt == 0 ? build_id + kBuildStagingSuffix
	                    : build_id + "." + std::to_string(attempt) + kBuildStagingSuffix;
}

// The build id a staging directory's name stages ("" for a name that is none).
std::string staged_build_id(const std::string &name) {
	const std::string suffix = kBuildStagingSuffix;
	if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
		return std::string();
	std::string stem = name.substr(0, name.size() - suffix.size());
	if (const size_t dot = stem.find('.'); dot != std::string::npos) {
		const std::string attempt = stem.substr(dot + 1);
		if (attempt.empty() || attempt.size() > 3 ||
		    !std::all_of(attempt.begin(), attempt.end(), [](char c) { return c >= '0' && c <= '9'; }))
			return std::string();
		stem.resize(dot);
	}
	return is_build_id(stem) ? stem : std::string();
}

// The archive's size on disk (0 when it cannot be read).
uint64_t size_of(const std::string &path) {
	std::error_code ec;
	const uintmax_t size = fs::file_size(system_path(path), ec);
	return ec ? 0 : uint64_t(size);
}

// True when `path` still has the size and the last write a read of it started from: a file
// rewritten in place under a read of several steps, even to the same size, is not.
bool still_as_read(const std::string &path, uint64_t size, int64_t written) {
	std::error_code ec;
	return fs::file_size(system_path(path), ec) == size && !ec && last_write_of(path) == written;
}

Diagnostic changed_while_packing(const std::string &name) {
	return make_finding(CoreFinding::BuildChanged, DiagnosticSeverity::Error,
	                    "The file " + name + " changed while the build packed it: build again.", name);
}

std::string archive_write_error(int rc) {
	switch (rc) {
	case pff::PFF_WRITE_ERR_NAME_LEN: return "a name is too long for an archive";
	case pff::PFF_WRITE_ERR_NAME_EMPTY: return "a name is blank";
	case pff::PFF_WRITE_ERR_DUP_NAME: return "two files share a name";
	case pff::PFF_WRITE_ERR_TOO_LARGE: return "the archive would exceed 4 GB";
	case pff::PFF_WRITE_ERR_IO:
	default: return "the archive could not be written";
	}
}

struct LastGood {
	std::string build_id;
	std::map<std::string, std::string> archive_hashes; // file name -> hex hash
	std::map<std::string, uint64_t> archive_sizes;     // file name -> bytes
};

bool read_last_good(const std::string &output_root, LastGood &out) {
	std::string text;
	std::string io_error;
	if (!read_file_text(join_path(output_root, kLastGoodBuildFileName), text, io_error))
		return false;
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(text, json, parse_error) || !json.is_object()) return false;
	if (json.get_int("schema_version", -1) != kBuildRecordSchemaVersion) return false;
	out.build_id = json.get_string("build_id", "");
	if (const io::JsonValue *archives = json.get("archives"); archives && archives->is_object()) {
		for (const io::JsonMember &m : archives->object) {
			if (!m.value.is_object()) continue;
			const std::string hash = m.value.get_string("hash", "");
			const double size = m.value.get_number("size", -1.0);
			if (hash.empty() || size < 0.0) continue;
			out.archive_hashes[m.key] = hash;
			out.archive_sizes[m.key] = uint64_t(size);
		}
	}
	return !out.build_id.empty();
}

// Each archive by its content hash and its size, which a later build checks the archive against
// before it links or copies it (one whose size moved is packed again, never reused).
io::JsonValue build_record(const std::string &build_id, const std::map<std::string, std::string> &hashes,
                           const std::map<std::string, uint64_t> &sizes, const std::vector<std::string> &loose) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kBuildRecordSchemaVersion));
	json.set("build_id", io::JsonValue::make_string(build_id));
	io::JsonValue archives = io::JsonValue::make_object();
	for (const auto &[name, hash] : hashes) {
		io::JsonValue archive = io::JsonValue::make_object();
		archive.set("hash", io::JsonValue::make_string(hash));
		const auto size = sizes.find(name);
		archive.set("size", io::JsonValue::make_number(double(size == sizes.end() ? 0 : size->second)));
		archives.set(name, std::move(archive));
	}
	json.set("archives", std::move(archives));
	io::JsonValue loose_names = io::JsonValue::make_array();
	for (const std::string &name : loose) loose_names.push(io::JsonValue::make_string(name));
	json.set("loose", std::move(loose_names));
	return json;
}

// Every planned name must resolve through the engine's own mount of the staged
// directory: the one a stock launch boots with (mount_install: the fixed boot table,
// archive-only), handed the directory's UTF-8 path as the game is handed its own, which it
// opens past MAX_PATH itself (base/io/os_path.h).
bool verify_staged(const BuildPlan &plan, const std::string &dir, Diagnostic &error) {
	Vfs vfs;
	if (!mount_install(vfs, dir, LaunchFlags())) {
		error = make_finding(CoreFinding::BuildVerify, DiagnosticSeverity::Error,
		                     "The built archives do not mount: " + vfs.last_error());
		return false;
	}
	for (const BuildArchive &archive : plan.archives) {
		for (const BuildEntry &entry : archive.entries) {
			if (!vfs.has_file(entry.logical_name)) {
				error = make_finding(CoreFinding::BuildVerify, DiagnosticSeverity::Error,
				                     entry.logical_name + " is missing from the built " + archive.file_name,
				                     entry.logical_name);
				return false;
			}
		}
	}
	std::error_code ec;
	for (const BuildEntry &entry : plan.loose) {
		if (!fs::is_regular_file(system_path(join_path(dir, entry.logical_name)), ec)) {
			error = make_finding(CoreFinding::BuildVerify, DiagnosticSeverity::Error,
			                     entry.logical_name + " is missing from the build directory", entry.logical_name);
			return false;
		}
	}
	return true;
}

// A directory is ours to delete only when it proves it: a published build is named by
// its id and carries a build record naming the same id; an abandoned staging directory
// is `<id>.tmp` (or `<id>.<n>.tmp`) and carries the staging marker. `--out` may point anywhere,
// so anything else under the output root (a user's own folders) is never touched. `dir` is the
// directory's system path.
bool is_prunable_build_dir(const fs::path &dir) {
	const std::string name = utf8_of(dir.filename());
	std::error_code ec;
	const std::string tmp_suffix = kBuildStagingSuffix;
	if (name.size() > tmp_suffix.size() && name.compare(name.size() - tmp_suffix.size(), tmp_suffix.size(), tmp_suffix) == 0)
		return !staged_build_id(name).empty() && fs::is_regular_file(dir / kBuildStagingMarkerFileName, ec);
	if (!is_build_id(name)) return false;
	std::string text;
	std::string io_error;
	if (!read_file_text(utf8_of(dir / kBuildRecordFileName), text, io_error)) return false;
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(text, json, parse_error) || !json.is_object()) return false;
	return json.get_string("build_id", "") == name;
}

// Removes a build or staging directory (`dir`, its system path), the file that proves it one last
// (a staging directory's marker, a build's record): one a file held open keeps (an indexer, a
// scanner, a game running on the last good build, which holds the archives a cancelled build linked
// into its staging) still proves itself, so a later build prunes it instead of refusing its name as
// "not a build directory". False when anything stayed.
bool remove_build_dir(const fs::path &dir) {
	const fs::path proof = dir / (staged_build_id(utf8_of(dir.filename())).empty() ? kBuildRecordFileName
	                                                                             : kBuildStagingMarkerFileName);
	std::error_code ec;
	bool kept = false;
	for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec)) {
		if (entry.path().filename() == proof.filename()) continue;
		std::error_code removed;
		fs::remove_all(entry.path(), removed);
		kept = kept || removed || fs::exists(entry.path(), removed);
	}
	if (kept) return false;
	std::error_code removed;
	fs::remove(proof, removed);
	fs::remove(dir, removed);
	return !fs::exists(dir, removed);
}

void remove_staging(const std::string &dir) {
	remove_build_dir(system_path(dir));
}

void prune_old_builds(const std::string &output_root, const std::string &keep_id,
                      const std::vector<std::string> &protected_dirs) {
	std::error_code ec;
	for (const fs::directory_entry &entry :
			fs::directory_iterator(system_path(output_root), fs::directory_options::skip_permission_denied, ec)) {
		if (ec) break;
		if (!entry.is_directory(ec)) continue;
		const std::string name = utf8_of(entry.path().filename());
		if (name == keep_id || !is_prunable_build_dir(entry.path())) continue;
		bool keep = false;
		for (const std::string &p : protected_dirs) {
			std::error_code cmp;
			if (fs::equivalent(entry.path(), system_path(p), cmp)) keep = true;
		}
		if (keep) continue;
		remove_build_dir(entry.path());
	}
}

} // namespace

// The files a step has open: the one being hashed, packed or copied, a copy's target (made new:
// create_new_file), and the archive writer, whose reads of the entries come back through
// read_chunk.
struct BuildRun::Streams {
	BuildRun *run = nullptr;
	std::ifstream in;
	std::FILE *out = nullptr;
	uint64_t in_size = 0;
	uint64_t in_done = 0;
	pff::PffStreamWriter writer;
	size_t archive = 0;        // the archive the writer writes
	std::string failed_entry;  // the entry whose file changed while packing ("" for none)
	std::vector<char> buffer;

	~Streams() { drop_out(); }

	// A copy's target made at `path`, which no file may hold yet (false when one does).
	bool open_out(const std::string &path) {
		drop_out();
		out = create_new_file(path);
		return out != nullptr;
	}
	// `size` bytes of the buffer written to the copy's target.
	bool write_out(uint64_t size) {
		return std::fwrite(buffer.data(), 1, static_cast<size_t>(size), out) == size;
	}
	// The copy's target closed, all of it written.
	bool close_out() {
		if (!out) return false;
		const bool written = std::fflush(out) == 0 && !std::ferror(out);
		const bool closed = std::fclose(out) == 0;
		out = nullptr;
		return written && closed;
	}
	void drop_out() {
		if (out) std::fclose(out);
		out = nullptr;
	}

	// Opens `path` for reading when it holds what the hash read (its size and last write).
	bool open_unchanged(const std::string &path, const Stamp &stamp) {
		in.close();
		in.clear();
		const fs::path file = system_path(path);
		std::error_code ec;
		const uint64_t size = fs::file_size(file, ec);
		if (ec || size != stamp.size || last_write_of(path) != stamp.written) return false;
		in.open(file, std::ios::binary);
		in_size = size;
		in_done = 0;
		return static_cast<bool>(in);
	}

	// Reads `size` bytes of the open file into the buffer; false when it has fewer.
	bool read(uint64_t size) {
		if (buffer.size() < size) buffer.resize(static_cast<size_t>(size));
		in.read(buffer.data(), static_cast<std::streamsize>(size));
		if (static_cast<uint64_t>(in.gcount()) != size) return false;
		in_done += size;
		return true;
	}

	// The open file read to its end: false when more bytes follow (it grew).
	bool at_end() {
		const bool end = in.peek() == std::ifstream::traits_type::eof();
		in.close();
		in.clear();
		return end;
	}

	// The PFF writer's source: entry `index` of the archive under way, a chunk at a time, from
	// its file, which must still hold what the hash read.
	static int read_chunk(void *ctx, uint32_t index, uint32_t offset, uint8_t *out, uint32_t size) {
		Streams &self = *static_cast<Streams *>(ctx);
		const BuildRun &run = *self.run;
		const BuildEntry &entry = run.plan_.archives[self.archive].entries[index];
		const Stamp &stamp = run.stamps_[run.stamp_index(self.archive, index)];
		if ((offset == 0 && !self.open_unchanged(entry.source_path, stamp)) || !self.read(size) ||
		    (self.in_done == stamp.size &&
		     (!self.at_end() || !still_as_read(entry.source_path, stamp.size, stamp.written)))) {
			self.failed_entry = entry.logical_name;
			return 1;
		}
		std::copy_n(self.buffer.data(), size, reinterpret_cast<char *>(out));
		return 0;
	}
};

ProtectedDirs protect_dirs(std::vector<std::string> dirs) {
	return [dirs = std::move(dirs)] { return dirs; };
}

bool is_build_id(const std::string &name) {
	if (name.size() != 16) return false;
	for (const char c : name) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	}
	return true;
}

std::string last_good_build_dir(const std::string &output_root) {
	LastGood last;
	if (!read_last_good(output_root, last)) return std::string();
	const std::string dir = join_path(output_root, last.build_id);
	std::error_code ec;
	return fs::is_directory(system_path(dir), ec) ? dir : std::string();
}

BuildRun::BuildRun(BuildPlan plan, std::string output_root, ProtectedDirs protected_dirs) :
		plan_(std::move(plan)), output_root_(std::move(output_root)), protected_dirs_(std::move(protected_dirs)),
		streams_(std::make_unique<Streams>()) {
	streams_->run = this;
	size_t base = 0;
	uint64_t bytes = 0;
	for (const BuildArchive &archive : plan_.archives) {
		group_base_.push_back(base);
		base += archive.entries.size();
		for (const BuildEntry &entry : archive.entries) bytes += entry.size_bytes;
	}
	group_base_.push_back(base);
	base += plan_.loose.size();
	for (const BuildEntry &entry : plan_.loose) bytes += entry.size_bytes;
	stamps_.resize(base);
	// Every byte hashed once (or vouched for by the hash cache), then written or copied once.
	bytes_total_ = plan_.ok ? bytes * 2 : 0;
	group_hash_ = archive_hash_seed();
	build_hash_ = io::kFnv1a64Offset;
}

// Where each archive's hash starts: the format it is written in and the writer's version, so a
// writer that would pack the same entries otherwise never finds the archive the old one packed
// unchanged (and a link to it): a writer change rebuilds every archive once.
uint64_t BuildRun::archive_hash_seed() {
	uint64_t seed = io::fnv1a64_value(io::kFnv1a64Offset, static_cast<int>(kBuildArchiveFormat));
	return io::fnv1a64_value(seed, pff::PFF_WRITER_VERSION);
}

BuildRun::~BuildRun() {
	if (!done()) cancel();
}

size_t BuildRun::stamp_index(size_t group, size_t entry) const {
	return group_base_[group] + entry;
}

const std::string &BuildRun::item_name(size_t index) const {
	if (index < plan_.archives.size()) return plan_.archives[index].file_name;
	return plan_.loose[index - plan_.archives.size()].logical_name;
}

void BuildRun::advance(uint64_t bytes) {
	bytes_done_ = std::min(bytes_total_, bytes_done_ + bytes);
}

void BuildRun::close_streams() {
	streams_->in.close();
	streams_->in.clear();
	streams_->drop_out();
	streams_->writer.abort();
}

void BuildRun::fail(Diagnostic error) {
	close_streams(); // an open file would keep the staging directory from going
	report_.diagnostics.push_back(std::move(error));
	if (!tmp_dir_.empty()) {
		remove_staging(tmp_dir_);
		tmp_dir_.clear();
	}
	phase_ = Phase::Done;
}

void BuildRun::cancel() {
	if (done()) return;
	close_streams();
	if (!tmp_dir_.empty()) {
		remove_staging(tmp_dir_);
		tmp_dir_.clear();
	}
	cancelled_ = true;
	report_.ok = false;
	label_ = "Cancelled";
	phase_ = Phase::Done;
}

bool BuildRun::step(uint64_t budget_bytes) {
	const uint64_t budget = std::max<uint64_t>(budget_bytes, 1);
	switch (phase_) {
	case Phase::Prepare: prepare(); break;
	case Phase::Hash: hash(budget); break;
	case Phase::Settle: settle(); break;
	case Phase::Archives: pack(budget); break;
	case Phase::Loose: copy_loose(budget); break;
	case Phase::Publish: publish(); break;
	case Phase::Done: break;
	}
	return done();
}

// The gate: a plan with a blocking finding never packs. Then the hash cache, read once.
void BuildRun::prepare() {
	if (!plan_.ok) {
		// Refused by the gate: the line names what refuses it (the UX round's problems lane).
		report_.diagnostics = plan_.diagnostics;
		report_.refused = true;
		report_.diagnostics.push_back(make_finding(CoreFinding::BuildBlocked, DiagnosticSeverity::Error,
		                                           refusal_words(build_blockers(plan_))));
		phase_ = Phase::Done;
		return;
	}
	// What the plan itself says of the files and blocks nothing (a player's own file it leaves out,
	// ADR 0046 S14) is the build's to report; the Problems rows it was gated on are not.
	for (const Diagnostic &d : plan_.diagnostics)
		if (d.row() && d.row()->group == FindingGroup::Build) report_.diagnostics.push_back(d);
	pass_began_ = io::file_clock_now_ticks();
	load_cache();
	label_ = "Hashing the project's files";
	phase_ = Phase::Hash;
}

// A cache that is missing, broken or of another schema reads as empty: every file is then hashed,
// which is all a lost cache costs.
void BuildRun::load_cache() {
	if (plan_.hash_cache.empty()) return;
	std::string message;
	std::error_code ec;
	if (!fs::is_regular_file(system_path(plan_.hash_cache), ec) || !read_file_text(plan_.hash_cache, cache_text_, message))
		return;
	io::JsonValue json;
	if (!io::json_parse(cache_text_, json, message) || !json.is_object() ||
	    json.get_int("schema_version", -1) != kBuildCacheSchemaVersion)
		return;
	const io::JsonValue *files = json.get("files");
	if (!files || !files->is_array()) return;
	for (const io::JsonValue &item : files->array) {
		if (!item.is_object()) continue;
		const std::string path = item.get_string("path", "");
		Hashed hashed;
		uint64_t written = 0;
		hashed.size = uint64_t(item.get_number("size", 0));
		if (path.empty() || !io::parse_hex64(item.get_string("modified", ""), written) ||
		    !io::parse_hex64(item.get_string("hash", ""), hashed.hash))
			continue;
		hashed.written = int64_t(written);
		cache_[path] = hashed;
	}
}

// Written when the hash pass ends, and only when it changed: the plan's files alone, each by what
// this build read or took from the cache, so a file gone from the project leaves it; and of those
// only a file whose last write lies far enough before the pass (io::file_stamp_settled), so a
// rewrite of the same size inside the same timestamp tick is never taken for the bytes read
// (a file written just before a build is read by the next one too).
void BuildRun::save_cache() const {
	if (plan_.hash_cache.empty()) return;
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kBuildCacheSchemaVersion));
	io::JsonValue files = io::JsonValue::make_array();
	for (const auto &[path, hashed] : seen_) {
		if (!io::file_stamp_settled(hashed.written, pass_began_)) continue;
		io::JsonValue item = io::JsonValue::make_object();
		item.set("path", io::JsonValue::make_string(path));
		item.set("size", io::JsonValue::make_number(double(hashed.size)));
		item.set("modified", io::JsonValue::make_string(io::hex64(uint64_t(hashed.written))));
		item.set("hash", io::JsonValue::make_string(io::hex64(hashed.hash)));
		files.push(std::move(item));
	}
	json.set("files", std::move(files));
	const std::string text = io::json_write(json);
	if (text == cache_text_) return;
	std::string message;
	write_file_atomic(plan_.hash_cache, text, message); // a cache that cannot be written is only slower
}

void BuildRun::fold(const BuildEntry &entry, uint64_t size, uint64_t content) {
	const std::string key = normalized_logical_name(entry.logical_name);
	group_hash_ = io::fnv1a64_bytes(group_hash_, key.data(), key.size());
	group_hash_ = io::fnv1a64_byte(group_hash_, 0);
	group_hash_ = io::fnv1a64_value(group_hash_, size);
	group_hash_ = io::fnv1a64_value(group_hash_, content);
}

// Every archive's content and the loose files hashed, the build id covering all of it: each
// entry's normalized name, a 0, its size and its content hash, in the archive's order. Reading the
// bytes (not only size and time) is what makes an edit inside the same second still a change, so a
// file's content hash comes from the cache only while its size and last write are the ones it was
// read at (S13 A8, the import cache's rule); each file's size and last write are kept, and the pass
// that packs it reads it only while it still holds them.
void BuildRun::hash(uint64_t budget) {
	Streams &s = *streams_;
	uint64_t left = budget;
	while (left > 0) {
		const bool loose = hash_group_ == plan_.archives.size();
		const std::vector<BuildEntry> &group = loose ? plan_.loose : plan_.archives[hash_group_].entries;
		if (hash_entry_ == group.size()) {
			build_hash_ = io::fnv1a64_value(build_hash_, group_hash_);
			if (!loose) {
				hashes_[plan_.archives[hash_group_].file_name] = io::hex64(group_hash_);
				++hash_group_;
				hash_entry_ = 0;
				// The next archive's from its seed; the loose files, copied as they are, from none.
				group_hash_ = hash_group_ < plan_.archives.size() ? archive_hash_seed() : io::kFnv1a64Offset;
				continue;
			}
			build_hash_ = io::fnv1a64_value(build_hash_, static_cast<int>(kBuildArchiveFormat));
			report_.build_id = io::hex64(build_hash_);
			save_cache();
			phase_ = Phase::Settle;
			return;
		}
		const BuildEntry &entry = group[hash_entry_];
		Stamp &stamp = stamps_[stamp_index(hash_group_, hash_entry_)];
		if (!s.in.is_open()) {
			const fs::path path = system_path(entry.source_path);
			std::error_code ec;
			const uint64_t size = fs::file_size(path, ec);
			if (ec) {
				return fail(make_finding(CoreFinding::BuildRead, DiagnosticSeverity::Error,
				                         "cannot read " + entry.logical_name + ": " + ec.message(),
				                         entry.logical_name));
			}
			if (size != entry.size_bytes) return fail(changed_while_packing(entry.logical_name));
			stamp = {size, last_write_of(entry.source_path)};
			label_ = "Hashing " + entry.logical_name;
			left -= std::min(left, kOpenCost);
			const auto cached = plan_.rehash ? cache_.end() : cache_.find(entry.source_path);
			if (stamp.written != 0 && cached != cache_.end() && cached->second.size == size &&
			    cached->second.written == stamp.written) {
				fold(entry, size, cached->second.hash);
				seen_[entry.source_path] = cached->second;
				advance(size);
				++hash_entry_;
				continue;
			}
			s.in.open(path, std::ios::binary);
			if (!s.in) {
				return fail(make_finding(CoreFinding::BuildRead, DiagnosticSeverity::Error,
				                         "cannot open " + entry.logical_name, entry.logical_name));
			}
			s.in_size = size;
			s.in_done = 0;
			file_hash_ = io::kFnv1a64Offset;
			++report_.files_hashed;
		}
		const uint64_t want = std::min({left, s.in_size - s.in_done, kChunkBytes});
		if (want > 0) {
			if (!s.read(want)) return fail(changed_while_packing(entry.logical_name));
			file_hash_ = io::fnv1a64_bytes(file_hash_, s.buffer.data(), static_cast<size_t>(want));
			left -= want;
			advance(want);
			report_.bytes_hashed += want;
		}
		if (s.in_done == s.in_size) {
			if (!s.at_end() || !still_as_read(entry.source_path, stamp.size, stamp.written))
				return fail(changed_while_packing(entry.logical_name));
			fold(entry, stamp.size, file_hash_);
			if (stamp.written != 0) seen_[entry.source_path] = {stamp.size, stamp.written, file_hash_};
			++hash_entry_;
		}
	}
}

// Settle whether this content is built already, and open the staging directory.
void BuildRun::settle() {
	std::string io_error;
	if (!ensure_directory(output_root_, io_error)) {
		return fail(make_finding(CoreFinding::BuildWrite, DiagnosticSeverity::Error, io_error));
	}
	final_dir_ = join_path(output_root_, report_.build_id);
	std::error_code ec;
	if (fs::is_directory(system_path(final_dir_), ec)) {
		// Same content, same build: prove it still mounts and hand it back.
		Diagnostic error;
		if (verify_staged(plan_, final_dir_, error)) {
			report_.ok = true;
			report_.reused_existing = true;
			report_.build_dir = final_dir_;
			list_built();
			bytes_done_ = bytes_total_;
			label_ = "Build unchanged";
			phase_ = Phase::Done;
			return;
		}
		if (!is_prunable_build_dir(system_path(final_dir_))) {
			return fail(make_finding(CoreFinding::BuildWrite, DiagnosticSeverity::Error,
			                         "cannot publish the build: " + final_dir_ +
			                                 " exists and is not a build directory"));
		}
		// A damaged build is rebuilt, once it is gone.
		if (!remove_build_dir(system_path(final_dir_))) {
			return fail(make_finding(CoreFinding::BuildWrite, DiagnosticSeverity::Error,
			                         "cannot build again over " + final_dir_ + ", which does not mount: a file in it is in use"));
		}
	}

	LastGood last;
	if (read_last_good(output_root_, last)) {
		last_dir_ = join_path(output_root_, last.build_id);
		last_hashes_ = last.archive_hashes;
		last_sizes_ = last.archive_sizes;
	}

	// The staging directory, empty: one a build of this content left (a cancel, a failure) is
	// emptied first; when a file in it will not go (a game running on the last good build holds the
	// archives a cancelled build linked there), the next name is taken, so nothing is ever staged
	// over a name already there.
	std::string tmp_dir;
	for (int attempt = 0; attempt < kStagingAttempts && tmp_dir.empty(); ++attempt) {
		const std::string candidate = join_path(output_root_, staging_name(report_.build_id, attempt));
		const fs::path staging = system_path(candidate);
		if (fs::exists(staging, ec)) {
			if (!is_prunable_build_dir(staging)) {
				return fail(make_finding(CoreFinding::BuildWrite, DiagnosticSeverity::Error,
				                         "cannot stage the build: " + candidate + " exists and is not a build directory"));
			}
			if (!remove_build_dir(staging)) continue;
		}
		tmp_dir = candidate;
	}
	if (tmp_dir.empty()) {
		return fail(make_finding(CoreFinding::BuildWrite, DiagnosticSeverity::Error,
		                         "cannot stage the build: its staging directories could not be emptied (a file in "
		                         "them is in use)"));
	}
	if (!ensure_directory(tmp_dir, io_error)) {
		return fail(make_finding(CoreFinding::BuildWrite, DiagnosticSeverity::Error, io_error));
	}
	tmp_dir_ = tmp_dir; // from here a failure or a cancel removes it
	if (!write_file_atomic(join_path(tmp_dir, kBuildStagingMarkerFileName), report_.build_id, io_error)) {
		return fail(make_finding(CoreFinding::BuildWrite, DiagnosticSeverity::Error, io_error));
	}
	phase_ = Phase::Archives;
}

// Each archive written through the stream writer, or, when its content hashes the same, the last
// good build's linked (one file under two names: a build never writes an archive again) or copied
// where the file system cannot link it, a budget of bytes at a time.
void BuildRun::pack(uint64_t budget) {
	Streams &s = *streams_;
	uint64_t left = budget;
	while (left > 0) {
		if (archive_index_ == plan_.archives.size()) {
			phase_ = Phase::Loose;
			return;
		}
		const BuildArchive &archive = plan_.archives[archive_index_];
		const std::string target = join_path(tmp_dir_, archive.file_name);
		if (!item_started_) {
			item_started_ = true;
			item_share_ = 0;
			item_share_done_ = 0;
			for (size_t i = 0; i < archive.entries.size(); ++i) item_share_ += stamps_[stamp_index(archive_index_, i)].size;
			const std::string previous = join_path(last_dir_, archive.file_name);
			const auto found = last_hashes_.find(archive.file_name);
			const auto recorded = last_sizes_.find(archive.file_name);
			std::error_code ec;
			// The last good build's archive, its content unchanged and its file still the size its
			// build recorded (one cut short under a game is packed again, never passed on).
			item_reused_ = !last_dir_.empty() && found != last_hashes_.end() &&
			               found->second == hashes_[archive.file_name] && recorded != last_sizes_.end() &&
			               fs::is_regular_file(system_path(previous), ec) && size_of(previous) == recorded->second;
			left -= std::min(left, kOpenCost);
			if (item_reused_) {
				std::string link_error;
				if (link_file(previous, target, link_error)) {
					report_.archives_reused.push_back(archive.file_name);
					report_.archives_linked.push_back(archive.file_name);
					label_ = "Linking " + archive.file_name + " from the last build";
					advance(item_share_);
					item_started_ = false;
					++archive_index_;
					++items_done_;
					continue;
				}
				// Copied where it cannot be linked, into a file made new: a name already in the staging
				// directory is never written through (it may be a link to the very archive copied).
				s.in.close();
				s.in.clear();
				s.in.open(system_path(previous), std::ios::binary);
				s.in_size = fs::file_size(system_path(previous), ec);
				s.in_done = 0;
				if (!s.in || ec || !s.open_out(target)) {
					return fail(make_finding(CoreFinding::BuildCopy, DiagnosticSeverity::Error,
					                         "cannot copy " + archive.file_name + " from the last build",
					                         archive.file_name));
				}
				label_ = "Copying " + archive.file_name + " from the last build";
			} else {
				std::vector<pff::PffWriteStreamEntry> entries;
				entries.reserve(archive.entries.size());
				for (size_t i = 0; i < archive.entries.size(); ++i) {
					pff::PffWriteStreamEntry e{};
					e.name = archive.entries[i].logical_name.c_str();
					e.size = static_cast<uint32_t>(stamps_[stamp_index(archive_index_, i)].size);
					e.flags = 0;
					e.timestamp = 0; // ADR 0008: zero for new entries; the engine reads neither field
					e.checksum = 0;
					entries.push_back(e);
				}
				s.archive = archive_index_;
				s.failed_entry.clear();
				const int rc = s.writer.open(target.c_str(), kBuildArchiveFormat, entries.data(),
				                             static_cast<uint32_t>(entries.size()), &Streams::read_chunk, &s);
				if (rc != pff::PFF_WRITE_OK) {
					return fail(make_finding(CoreFinding::BuildArchive, DiagnosticSeverity::Error,
					                         archive.file_name + ": " + archive_write_error(rc)));
				}
				label_ = "Packing " + archive.file_name;
			}
			continue;
		}
		uint64_t moved = 0;
		bool finished = false;
		if (item_reused_) {
			const uint64_t want = std::min({left, s.in_size - s.in_done, kChunkBytes});
			if (want > 0) {
				if (!s.read(want) || !s.write_out(want)) {
					return fail(make_finding(CoreFinding::BuildCopy, DiagnosticSeverity::Error,
					                         "cannot copy " + archive.file_name + " from the last build",
					                         archive.file_name));
				}
				moved = want;
			}
			if (s.in_done == s.in_size) {
				s.in.close();
				s.in.clear();
				if (!s.close_out()) {
					return fail(make_finding(CoreFinding::BuildCopy, DiagnosticSeverity::Error,
					                         "cannot copy " + archive.file_name + " from the last build",
					                         archive.file_name));
				}
				report_.archives_reused.push_back(archive.file_name);
				finished = true;
			}
		} else {
			const uint64_t before = s.writer.payload_written();
			int rc = s.writer.write(left);
			moved = s.writer.payload_written() - before;
			if (rc == pff::PFF_WRITE_OK && s.writer.payloads_written()) {
				rc = s.writer.finish();
				finished = rc == pff::PFF_WRITE_OK;
			}
			if (rc != pff::PFF_WRITE_OK) {
				if (!s.failed_entry.empty()) return fail(changed_while_packing(s.failed_entry));
				return fail(make_finding(CoreFinding::BuildArchive, DiagnosticSeverity::Error,
				                         archive.file_name + ": " + archive_write_error(rc)));
			}
			if (finished) report_.archives_written.push_back(archive.file_name);
		}
		const uint64_t credit = std::min(moved, item_share_ - item_share_done_);
		item_share_done_ += credit;
		advance(credit);
		left -= std::min(left, moved);
		if (finished) {
			advance(item_share_ - item_share_done_);
			item_started_ = false;
			++archive_index_;
			++items_done_;
		} else if (moved == 0) {
			break; // nothing moved: the next step goes on
		}
	}
}

// Each loose file copied beside the archives, a budget of bytes at a time, while it still holds
// what the hash read.
void BuildRun::copy_loose(uint64_t budget) {
	Streams &s = *streams_;
	uint64_t left = budget;
	while (left > 0) {
		if (loose_index_ == plan_.loose.size()) {
			phase_ = Phase::Publish;
			return;
		}
		const BuildEntry &entry = plan_.loose[loose_index_];
		const Stamp &stamp = stamps_[stamp_index(plan_.archives.size(), loose_index_)];
		if (!item_started_) {
			item_started_ = true;
			if (!s.open_unchanged(entry.source_path, stamp)) return fail(changed_while_packing(entry.logical_name));
			if (!s.open_out(join_path(tmp_dir_, entry.logical_name))) {
				return fail(make_finding(CoreFinding::BuildCopy, DiagnosticSeverity::Error,
				                         "cannot copy " + entry.logical_name, entry.logical_name));
			}
			label_ = "Copying " + entry.logical_name;
			left -= std::min(left, kOpenCost);
		}
		const uint64_t want = std::min({left, s.in_size - s.in_done, kChunkBytes});
		if (want > 0) {
			if (!s.read(want)) return fail(changed_while_packing(entry.logical_name));
			if (!s.write_out(want)) {
				return fail(make_finding(CoreFinding::BuildCopy, DiagnosticSeverity::Error,
				                         "cannot copy " + entry.logical_name, entry.logical_name));
			}
			left -= want;
			advance(want);
		}
		if (s.in_done == s.in_size) {
			if (!s.at_end() || !still_as_read(entry.source_path, stamp.size, stamp.written))
				return fail(changed_while_packing(entry.logical_name));
			if (!s.close_out()) {
				return fail(make_finding(CoreFinding::BuildCopy, DiagnosticSeverity::Error,
				                         "cannot copy " + entry.logical_name, entry.logical_name));
			}
			report_.loose_written.push_back(entry.logical_name);
			item_started_ = false;
			++loose_index_;
			++items_done_;
		}
	}
}

// Prove the staged directory mounts, record it, rename it into place, prune. A rename refused while
// its refusal may pass (a scanner or an indexer holding a staged file: rename_refusal_passes) is
// tried again on the next steps, for kPublishPatience at most.
void BuildRun::publish() {
	label_ = "Publishing the build";
	std::string io_error;
	if (!publish_ready_) {
		Diagnostic verify_error;
		if (!verify_staged(plan_, tmp_dir_, verify_error)) return fail(std::move(verify_error));
		std::map<std::string, uint64_t> sizes;
		for (const BuildArchive &archive : plan_.archives)
			sizes[archive.file_name] = size_of(join_path(tmp_dir_, archive.file_name));
		record_ = io::json_write(build_record(report_.build_id, hashes_, sizes, report_.loose_written));
		if (!write_file_atomic(join_path(tmp_dir_, kBuildRecordFileName), record_, io_error)) {
			return fail(make_finding(CoreFinding::BuildWrite, DiagnosticSeverity::Error, io_error));
		}
		publish_ready_ = true;
	}
	std::error_code ec;
	if (!rename_with_retry(system_path(tmp_dir_), system_path(final_dir_), ec)) {
		const auto now = std::chrono::steady_clock::now();
		if (rename_refusal_passes(ec)) {
			if (publish_refused_at_ == std::chrono::steady_clock::time_point{}) publish_refused_at_ = now;
			if (now - publish_refused_at_ < kPublishPatience) return; // the next step tries again
		}
		return fail(make_finding(CoreFinding::BuildWrite, DiagnosticSeverity::Error,
		                         "cannot publish the build: " + ec.message()));
	}
	tmp_dir_.clear(); // published: nothing left to clean up
	fs::remove(system_path(join_path(final_dir_, kBuildStagingMarkerFileName)), ec); // the record is its proof now
	if (!write_file_atomic(join_path(output_root_, kLastGoodBuildFileName), record_, io_error)) {
		return fail(make_finding(CoreFinding::BuildWrite, DiagnosticSeverity::Error, io_error));
	}
	// The directories games run from, asked now: a game started (or found alive) since the build
	// began is as protected as one that ran when it started.
	prune_old_builds(output_root_, report_.build_id,
	                 protected_dirs_ ? protected_dirs_() : std::vector<std::string>());
	report_.ok = true;
	report_.build_dir = final_dir_;
	list_built();
	bytes_done_ = bytes_total_;
	phase_ = Phase::Done;
}

void BuildRun::list_built() {
	report_.built.clear();
	const auto size_of = [this](const std::string &name) {
		std::error_code ec;
		const uintmax_t bytes = fs::file_size(system_path(join_path(final_dir_, name)), ec);
		return ec ? uint64_t(0) : uint64_t(bytes);
	};
	for (const BuildArchive &archive : plan_.archives) {
		BuiltFile file;
		file.name = archive.file_name;
		file.archive = true;
		file.files = archive.entries.size();
		file.bytes = size_of(archive.file_name);
		file.reused = std::find(report_.archives_reused.begin(), report_.archives_reused.end(), archive.file_name) !=
		              report_.archives_reused.end();
		report_.built.push_back(std::move(file));
	}
	for (const BuildEntry &entry : plan_.loose) {
		BuiltFile file;
		file.name = entry.logical_name;
		file.bytes = size_of(entry.logical_name);
		report_.built.push_back(std::move(file));
	}
}

BuildReport run_build(const BuildPlan &plan, const std::string &output_root, ProtectedDirs protected_dirs,
                      BuildProgress *progress) {
	BuildRun run(plan, output_root, std::move(protected_dirs));
	size_t reported = 0;
	for (;;) {
		const bool finished = run.step(kRunBuildStepBytes);
		for (; progress && reported < run.items_done(); ++reported)
			progress->on_step(run.item_name(reported), reported + 1, run.items_total());
		if (finished) break;
	}
	return run.report();
}

} // namespace opennova::editor
