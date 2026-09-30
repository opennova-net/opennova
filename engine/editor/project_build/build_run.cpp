#include <editor/project_build/build_run.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>
#include <utility>

#include <base/io/hash.h>
#include <base/io/json.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_registry.h>
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
// What opening a file costs a step, so one over many small files stays short.
constexpr uint64_t kOpenCost = 4096;
// run_build's step: a whole build in few steps, its archives reported as they land.
constexpr uint64_t kRunBuildStepBytes = uint64_t(4) << 20;

// A file's last write, as a number two reads of the same file compare equal by.
int64_t last_write_of(const fs::path &path) {
	std::error_code ec;
	const fs::file_time_type time = fs::last_write_time(path, ec);
	return ec ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
}

Diagnostic changed_while_packing(const std::string &name) {
	return make_diagnostic(DiagnosticSeverity::Error, "build.changed",
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
};

bool read_last_good(const std::string &output_root, LastGood &out) {
	std::string text;
	std::string io_error;
	if (!read_file_text((fs::path(output_root) / kLastGoodBuildFileName).generic_string(), text, io_error))
		return false;
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(text, json, parse_error) || !json.is_object()) return false;
	if (json.get_int("schema_version", -1) != kBuildRecordSchemaVersion) return false;
	out.build_id = json.get_string("build_id", "");
	if (const io::JsonValue *archives = json.get("archives"); archives && archives->is_object()) {
		for (const io::JsonMember &m : archives->object) {
			if (m.value.is_string()) out.archive_hashes[m.key] = m.value.string;
		}
	}
	return !out.build_id.empty();
}

io::JsonValue build_record(const std::string &build_id, const std::map<std::string, std::string> &hashes,
                           const std::vector<std::string> &loose) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kBuildRecordSchemaVersion));
	json.set("build_id", io::JsonValue::make_string(build_id));
	io::JsonValue archives = io::JsonValue::make_object();
	for (const auto &[name, hash] : hashes) archives.set(name, io::JsonValue::make_string(hash));
	json.set("archives", std::move(archives));
	io::JsonValue loose_names = io::JsonValue::make_array();
	for (const std::string &name : loose) loose_names.push(io::JsonValue::make_string(name));
	json.set("loose", std::move(loose_names));
	return json;
}

// Every planned name must resolve through the engine's own mount of the staged
// directory: the one a stock launch boots with (mount_install: the fixed boot table,
// archive-only).
bool verify_staged(const BuildPlan &plan, const std::string &dir, Diagnostic &error) {
	Vfs vfs;
	if (!mount_install(vfs, dir, LaunchFlags())) {
		error = make_diagnostic(DiagnosticSeverity::Error, "build.verify",
		                        "The built archives do not mount: " + vfs.last_error());
		return false;
	}
	for (const BuildArchive &archive : plan.archives) {
		for (const BuildEntry &entry : archive.entries) {
			if (!vfs.has_file(entry.logical_name)) {
				error = make_diagnostic(DiagnosticSeverity::Error, "build.verify",
				                        entry.logical_name + " is missing from the built " + archive.file_name,
				                        entry.logical_name);
				return false;
			}
		}
	}
	std::error_code ec;
	for (const BuildEntry &entry : plan.loose) {
		if (!fs::is_regular_file(fs::path(dir) / entry.logical_name, ec)) {
			error = make_diagnostic(DiagnosticSeverity::Error, "build.verify",
			                        entry.logical_name + " is missing from the build directory", entry.logical_name);
			return false;
		}
	}
	return true;
}

// A directory is ours to delete only when it proves it: a published build is named by
// its id and carries a build record naming the same id; an abandoned staging directory
// is `<id>.tmp` and carries the staging marker. `--out` may point anywhere, so anything
// else under the output root (a user's own folders) is never touched.
bool is_prunable_build_dir(const fs::path &dir) {
	const std::string name = dir.filename().string();
	std::error_code ec;
	const std::string tmp_suffix = kBuildStagingSuffix;
	if (name.size() > tmp_suffix.size() && name.compare(name.size() - tmp_suffix.size(), tmp_suffix.size(), tmp_suffix) == 0) {
		return is_build_id(name.substr(0, name.size() - tmp_suffix.size())) &&
		       fs::is_regular_file(dir / kBuildStagingMarkerFileName, ec);
	}
	if (!is_build_id(name)) return false;
	std::string text;
	std::string io_error;
	if (!read_file_text((dir / kBuildRecordFileName).generic_string(), text, io_error)) return false;
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(text, json, parse_error) || !json.is_object()) return false;
	return json.get_string("build_id", "") == name;
}

// Removes a staging directory, its marker last: one a file held open (an indexer, a scanner)
// keeps is still marked, so the next build of the same content prunes it instead of refusing its
// build id as "not a build directory".
void remove_staging(const std::string &dir) {
	std::error_code ec;
	bool kept = false;
	for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec)) {
		if (entry.path().filename() == kBuildStagingMarkerFileName) continue;
		std::error_code removed;
		fs::remove_all(entry.path(), removed);
		kept = kept || removed;
	}
	if (kept) return;
	std::error_code removed;
	fs::remove(fs::path(dir) / kBuildStagingMarkerFileName, removed);
	fs::remove(dir, removed);
}

void prune_old_builds(const std::string &output_root, const std::string &keep_id,
                      const std::vector<std::string> &protected_dirs) {
	std::error_code ec;
	for (const fs::directory_entry &entry : fs::directory_iterator(output_root, fs::directory_options::skip_permission_denied, ec)) {
		if (ec) break;
		if (!entry.is_directory(ec)) continue;
		const std::string name = entry.path().filename().string();
		if (name == keep_id || !is_prunable_build_dir(entry.path())) continue;
		bool keep = false;
		for (const std::string &p : protected_dirs) {
			std::error_code cmp;
			if (fs::equivalent(entry.path(), fs::path(p), cmp)) keep = true;
		}
		if (keep) continue;
		std::error_code remove_ec;
		fs::remove_all(entry.path(), remove_ec);
	}
}

} // namespace

// The files a step has open: the one being hashed, packed or copied, a copy's target, and the
// archive writer, whose reads of the entries come back through read_chunk.
struct BuildRun::Streams {
	BuildRun *run = nullptr;
	std::ifstream in;
	std::ofstream out;
	uint64_t in_size = 0;
	uint64_t in_done = 0;
	pff::PffStreamWriter writer;
	size_t archive = 0;        // the archive the writer writes
	std::string failed_entry;  // the entry whose file changed while packing ("" for none)
	std::vector<char> buffer;

	// Opens `path` for reading when it holds what the hash read (its size and last write).
	bool open_unchanged(const std::string &path, const Stamp &stamp) {
		in.close();
		in.clear();
		const fs::path file(path);
		std::error_code ec;
		const uint64_t size = fs::file_size(file, ec);
		if (ec || size != stamp.size || last_write_of(file) != stamp.written) return false;
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
		    (self.in_done == stamp.size && !self.at_end())) {
			self.failed_entry = entry.logical_name;
			return 1;
		}
		std::copy_n(self.buffer.data(), size, reinterpret_cast<char *>(out));
		return 0;
	}
};

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
	const fs::path dir = fs::path(output_root) / last.build_id;
	std::error_code ec;
	return fs::is_directory(dir, ec) ? dir.generic_string() : std::string();
}

BuildRun::BuildRun(BuildPlan plan, std::string output_root, std::vector<std::string> protected_dirs) :
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
	// Every byte hashed once, then written or copied once.
	bytes_total_ = plan_.ok ? bytes * 2 : 0;
	group_hash_ = io::kFnv1a64Offset;
	build_hash_ = io::kFnv1a64Offset;
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
	streams_->out.close();
	streams_->out.clear();
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

// The gate: a plan with a blocking finding never packs.
void BuildRun::prepare() {
	if (!plan_.ok) {
		report_.diagnostics = plan_.diagnostics;
		report_.diagnostics.push_back(make_diagnostic(
		        DiagnosticSeverity::Error, "build.blocked",
		        "The project has problems that would stop the game; fix them first."));
		phase_ = Phase::Done;
		return;
	}
	label_ = "Hashing the project's files";
	phase_ = Phase::Hash;
}

// Every archive's content and the loose files hashed, the build id covering all of it: each
// entry's normalized name, a 0, its size and its bytes, in the archive's order. Reading the
// bytes (not only size and time) is what makes an edit inside the same second still a change;
// each file's size and last write are kept, and the pass that packs it reads it only while it
// still holds them.
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
				group_hash_ = io::kFnv1a64Offset;
				continue;
			}
			build_hash_ = io::fnv1a64_value(build_hash_, static_cast<int>(kBuildArchiveFormat));
			report_.build_id = io::hex64(build_hash_);
			phase_ = Phase::Settle;
			return;
		}
		const BuildEntry &entry = group[hash_entry_];
		Stamp &stamp = stamps_[stamp_index(hash_group_, hash_entry_)];
		if (!s.in.is_open()) {
			const fs::path path(entry.source_path);
			std::error_code ec;
			const uint64_t size = fs::file_size(path, ec);
			if (ec) {
				return fail(make_diagnostic(DiagnosticSeverity::Error, "build.read",
				                            "cannot read " + entry.logical_name + ": " + ec.message(),
				                            entry.logical_name));
			}
			if (size != entry.size_bytes) return fail(changed_while_packing(entry.logical_name));
			stamp = {size, last_write_of(path)};
			s.in.open(path, std::ios::binary);
			if (!s.in) {
				return fail(make_diagnostic(DiagnosticSeverity::Error, "build.read",
				                            "cannot open " + entry.logical_name, entry.logical_name));
			}
			s.in_size = size;
			s.in_done = 0;
			const std::string key = normalized_logical_name(entry.logical_name);
			group_hash_ = io::fnv1a64_bytes(group_hash_, key.data(), key.size());
			group_hash_ = io::fnv1a64_byte(group_hash_, 0);
			group_hash_ = io::fnv1a64_value(group_hash_, size);
			label_ = "Hashing " + entry.logical_name;
			left -= std::min(left, kOpenCost);
		}
		const uint64_t want = std::min({left, s.in_size - s.in_done, kChunkBytes});
		if (want > 0) {
			if (!s.read(want)) return fail(changed_while_packing(entry.logical_name));
			group_hash_ = io::fnv1a64_bytes(group_hash_, s.buffer.data(), static_cast<size_t>(want));
			left -= want;
			advance(want);
		}
		if (s.in_done == s.in_size) {
			if (!s.at_end()) return fail(changed_while_packing(entry.logical_name));
			++hash_entry_;
		}
	}
}

// Settle whether this content is built already, and open the staging directory.
void BuildRun::settle() {
	std::string io_error;
	if (!ensure_directory(output_root_, io_error)) {
		return fail(make_diagnostic(DiagnosticSeverity::Error, "build.write", io_error));
	}
	const fs::path final_dir = fs::path(output_root_) / report_.build_id;
	final_dir_ = final_dir.generic_string();
	std::error_code ec;
	if (fs::is_directory(final_dir, ec)) {
		// Same content, same build: prove it still mounts and hand it back.
		Diagnostic error;
		if (verify_staged(plan_, final_dir_, error)) {
			report_.ok = true;
			report_.reused_existing = true;
			report_.build_dir = final_dir_;
			bytes_done_ = bytes_total_;
			label_ = "Build unchanged";
			phase_ = Phase::Done;
			return;
		}
		if (!is_prunable_build_dir(final_dir)) {
			return fail(make_diagnostic(DiagnosticSeverity::Error, "build.write",
			                            "cannot publish the build: " + final_dir_ +
			                                    " exists and is not a build directory"));
		}
		fs::remove_all(final_dir, ec); // a damaged build is rebuilt
	}

	LastGood last;
	if (read_last_good(output_root_, last)) {
		last_dir_ = (fs::path(output_root_) / last.build_id).generic_string();
		last_hashes_ = last.archive_hashes;
	}

	const fs::path tmp_dir = fs::path(output_root_) / (report_.build_id + kBuildStagingSuffix);
	if (fs::exists(tmp_dir, ec) && !is_prunable_build_dir(tmp_dir)) {
		return fail(make_diagnostic(DiagnosticSeverity::Error, "build.write",
		                            "cannot stage the build: " + tmp_dir.generic_string() +
		                                    " exists and is not a build directory"));
	}
	fs::remove_all(tmp_dir, ec);
	if (!ensure_directory(tmp_dir.generic_string(), io_error)) {
		return fail(make_diagnostic(DiagnosticSeverity::Error, "build.write", io_error));
	}
	tmp_dir_ = tmp_dir.generic_string(); // from here a failure or a cancel removes it
	if (!write_file_atomic((tmp_dir / kBuildStagingMarkerFileName).generic_string(), report_.build_id, io_error)) {
		return fail(make_diagnostic(DiagnosticSeverity::Error, "build.write", io_error));
	}
	phase_ = Phase::Archives;
}

// Each archive written through the stream writer, or copied from the last good build when its
// content hashes the same, a budget of bytes at a time.
void BuildRun::pack(uint64_t budget) {
	Streams &s = *streams_;
	uint64_t left = budget;
	while (left > 0) {
		if (archive_index_ == plan_.archives.size()) {
			phase_ = Phase::Loose;
			return;
		}
		const BuildArchive &archive = plan_.archives[archive_index_];
		const fs::path target = fs::path(tmp_dir_) / archive.file_name;
		if (!item_started_) {
			item_started_ = true;
			item_share_ = 0;
			item_share_done_ = 0;
			for (size_t i = 0; i < archive.entries.size(); ++i) item_share_ += stamps_[stamp_index(archive_index_, i)].size;
			const fs::path previous = fs::path(last_dir_) / archive.file_name;
			const auto found = last_hashes_.find(archive.file_name);
			std::error_code ec;
			item_reused_ = !last_dir_.empty() && found != last_hashes_.end() &&
			               found->second == hashes_[archive.file_name] && fs::is_regular_file(previous, ec);
			if (item_reused_) {
				s.in.close();
				s.in.clear();
				s.in.open(previous, std::ios::binary);
				s.out.open(target, std::ios::binary | std::ios::trunc);
				s.in_size = fs::file_size(previous, ec);
				s.in_done = 0;
				if (!s.in || !s.out || ec) {
					return fail(make_diagnostic(DiagnosticSeverity::Error, "build.copy",
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
				const int rc = s.writer.open(target.generic_string().c_str(), kBuildArchiveFormat, entries.data(),
				                             static_cast<uint32_t>(entries.size()), &Streams::read_chunk, &s);
				if (rc != pff::PFF_WRITE_OK) {
					return fail(make_diagnostic(DiagnosticSeverity::Error, "build.archive",
					                            archive.file_name + ": " + archive_write_error(rc)));
				}
				label_ = "Packing " + archive.file_name;
			}
			left -= std::min(left, kOpenCost);
			continue;
		}
		uint64_t moved = 0;
		bool finished = false;
		if (item_reused_) {
			const uint64_t want = std::min({left, s.in_size - s.in_done, kChunkBytes});
			if (want > 0) {
				if (!s.read(want) || !s.out.write(s.buffer.data(), static_cast<std::streamsize>(want))) {
					return fail(make_diagnostic(DiagnosticSeverity::Error, "build.copy",
					                            "cannot copy " + archive.file_name + " from the last build",
					                            archive.file_name));
				}
				moved = want;
			}
			if (s.in_done == s.in_size) {
				s.in.close();
				s.in.clear();
				s.out.close();
				if (!s.out) {
					return fail(make_diagnostic(DiagnosticSeverity::Error, "build.copy",
					                            "cannot copy " + archive.file_name + " from the last build",
					                            archive.file_name));
				}
				s.out.clear();
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
				return fail(make_diagnostic(DiagnosticSeverity::Error, "build.archive",
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
			s.out.open(fs::path(tmp_dir_) / entry.logical_name, std::ios::binary | std::ios::trunc);
			if (!s.out) {
				return fail(make_diagnostic(DiagnosticSeverity::Error, "build.copy",
				                            "cannot copy " + entry.logical_name, entry.logical_name));
			}
			label_ = "Copying " + entry.logical_name;
			left -= std::min(left, kOpenCost);
		}
		const uint64_t want = std::min({left, s.in_size - s.in_done, kChunkBytes});
		if (want > 0) {
			if (!s.read(want)) return fail(changed_while_packing(entry.logical_name));
			if (!s.out.write(s.buffer.data(), static_cast<std::streamsize>(want))) {
				return fail(make_diagnostic(DiagnosticSeverity::Error, "build.copy",
				                            "cannot copy " + entry.logical_name, entry.logical_name));
			}
			left -= want;
			advance(want);
		}
		if (s.in_done == s.in_size) {
			if (!s.at_end()) return fail(changed_while_packing(entry.logical_name));
			s.out.close();
			if (!s.out) {
				return fail(make_diagnostic(DiagnosticSeverity::Error, "build.copy",
				                            "cannot copy " + entry.logical_name, entry.logical_name));
			}
			s.out.clear();
			report_.loose_written.push_back(entry.logical_name);
			item_started_ = false;
			++loose_index_;
			++items_done_;
		}
	}
}

// Prove the staged directory mounts, record it, rename it into place, prune.
void BuildRun::publish() {
	label_ = "Publishing the build";
	Diagnostic verify_error;
	if (!verify_staged(plan_, tmp_dir_, verify_error)) return fail(std::move(verify_error));
	const io::JsonValue record = build_record(report_.build_id, hashes_, report_.loose_written);
	std::string io_error;
	if (!write_file_atomic((fs::path(tmp_dir_) / kBuildRecordFileName).generic_string(),
	                       io::json_write(record), io_error)) {
		return fail(make_diagnostic(DiagnosticSeverity::Error, "build.write", io_error));
	}
	std::error_code ec;
	fs::rename(tmp_dir_, final_dir_, ec);
	if (ec) {
		return fail(make_diagnostic(DiagnosticSeverity::Error, "build.write",
		                            "cannot publish the build: " + ec.message()));
	}
	tmp_dir_.clear(); // published: nothing left to clean up
	fs::remove(fs::path(final_dir_) / kBuildStagingMarkerFileName, ec); // the record is its proof now
	if (!write_file_atomic((fs::path(output_root_) / kLastGoodBuildFileName).generic_string(),
	                       io::json_write(record), io_error)) {
		return fail(make_diagnostic(DiagnosticSeverity::Error, "build.write", io_error));
	}
	prune_old_builds(output_root_, report_.build_id, protected_dirs_);
	report_.ok = true;
	report_.build_dir = final_dir_;
	bytes_done_ = bytes_total_;
	phase_ = Phase::Done;
}

BuildReport run_build(const BuildPlan &plan, const std::string &output_root,
                      const std::vector<std::string> &protected_dirs, BuildProgress *progress) {
	BuildRun run(plan, output_root, protected_dirs);
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
