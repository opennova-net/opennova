#include <editor/project_build/build_session.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
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

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// The content hash of an archive: every entry's normalized name and bytes, in
// directory order. Reading the bytes (not just size and time) is what makes an
// edit inside the same second still a change.
bool hash_entries(const std::vector<BuildEntry> &entries, uint64_t &hash, Diagnostic &error) {
	hash = io::kFnv1a64Offset;
	for (const BuildEntry &entry : entries) {
		const std::string key = normalized_logical_name(entry.logical_name);
		hash = io::fnv1a64_bytes(hash, key.data(), key.size());
		hash = io::fnv1a64_byte(hash, 0);
		std::vector<uint8_t> bytes;
		std::string io_error;
		if (!read_file_bytes(entry.source_path, bytes, io_error)) {
			error = make_diagnostic(DiagnosticSeverity::Error, "build.read", io_error, entry.logical_name);
			return false;
		}
		const uint64_t size = bytes.size();
		hash = io::fnv1a64_value(hash, size);
		hash = io::fnv1a64_bytes(hash, bytes.data(), bytes.size());
	}
	return true;
}

struct StreamContext {
	const std::vector<BuildEntry> *entries;
	std::string failed_entry;
};

int read_entry_for_writer(void *ctx_ptr, uint32_t index, uint8_t *out, uint32_t size) {
	StreamContext *ctx = static_cast<StreamContext *>(ctx_ptr);
	const BuildEntry &entry = (*ctx->entries)[index];
	std::vector<uint8_t> bytes;
	std::string io_error;
	if (!read_file_bytes(entry.source_path, bytes, io_error) || bytes.size() != size) {
		ctx->failed_entry = entry.logical_name;
		return 1;
	}
	if (size != 0) std::memcpy(out, bytes.data(), size);
	return 0;
}

bool write_archive(const BuildArchive &archive, const std::string &path, pff::PffFormat format,
                   Diagnostic &error) {
	std::vector<pff::PffWriteStreamEntry> entries;
	entries.reserve(archive.entries.size());
	for (const BuildEntry &entry : archive.entries) {
		pff::PffWriteStreamEntry e{};
		e.name = entry.logical_name.c_str();
		e.size = static_cast<uint32_t>(entry.size_bytes);
		e.flags = 0;
		e.timestamp = 0; // ADR 0008: zero for new entries; the engine reads neither field
		e.checksum = 0;
		entries.push_back(e);
	}
	StreamContext ctx{&archive.entries, std::string()};
	const int rc = pff::pff_write_archive_streamed(path.c_str(), format, entries.data(),
	                                               static_cast<uint32_t>(entries.size()),
	                                               read_entry_for_writer, &ctx);
	if (rc == pff::PFF_WRITE_OK) return true;
	std::string why;
	switch (rc) {
	case pff::PFF_WRITE_ERR_IO:
		why = ctx.failed_entry.empty() ? "the archive could not be written"
		                               : "the file " + ctx.failed_entry + " changed while packing";
		break;
	case pff::PFF_WRITE_ERR_NAME_LEN: why = "a name is too long for an archive"; break;
	case pff::PFF_WRITE_ERR_NAME_EMPTY: why = "a name is blank"; break;
	case pff::PFF_WRITE_ERR_DUP_NAME: why = "two files share a name"; break;
	case pff::PFF_WRITE_ERR_TOO_LARGE: why = "the archive would exceed 4 GB"; break;
	default: why = "the archive writer failed"; break;
	}
	error = make_diagnostic(DiagnosticSeverity::Error, "build.archive", archive.file_name + ": " + why);
	return false;
}

bool copy_file(const fs::path &from, const fs::path &to, Diagnostic &error, const std::string &what) {
	std::error_code ec;
	fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
	if (ec) {
		error = make_diagnostic(DiagnosticSeverity::Error, "build.copy",
		                        "cannot copy " + what + ": " + ec.message(), what);
		return false;
	}
	return true;
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

bool is_build_id(const std::string &name) {
	if (name.size() != 16) return false;
	for (const char c : name) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
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

std::string last_good_build_dir(const std::string &output_root) {
	LastGood last;
	if (!read_last_good(output_root, last)) return std::string();
	const fs::path dir = fs::path(output_root) / last.build_id;
	std::error_code ec;
	return fs::is_directory(dir, ec) ? dir.generic_string() : std::string();
}

namespace {

// The archive flavour the target game ships: every JO-family title reads PFF3
// [orig: PFF_Open @ 0x7682e0]; the profile keys the SCR policy only, so the format
// rides here until a profile row carries it.
constexpr pff::PffFormat kBuildArchiveFormat = pff::PFF_FORMAT_PFF3;

} // namespace

BuildRun::BuildRun(BuildPlan plan, std::string output_root, std::vector<std::string> protected_dirs)
		: plan_(std::move(plan)), output_root_(std::move(output_root)), protected_dirs_(std::move(protected_dirs)) {}

void BuildRun::fail(Diagnostic error) {
	report_.diagnostics.push_back(std::move(error));
	if (!tmp_dir_.empty()) {
		std::error_code ec;
		fs::remove_all(tmp_dir_, ec);
	}
	phase_ = Phase::Done;
}

bool BuildRun::step() {
	switch (phase_) {
	case Phase::Prepare: prepare(); break;
	case Phase::Archives: pack_next_archive(); break;
	case Phase::Loose: copy_next_loose(); break;
	case Phase::Publish: publish(); break;
	case Phase::Done: break;
	}
	return done();
}

// Gate, hash, settle whether this content is already built, and open the staging
// directory.
void BuildRun::prepare() {
	if (!plan_.ok) {
		report_.diagnostics = plan_.diagnostics;
		report_.diagnostics.push_back(make_diagnostic(
		        DiagnosticSeverity::Error, "build.blocked",
		        "The project has problems that would stop the game; fix them first."));
		phase_ = Phase::Done;
		return;
	}

	// Hash every archive's content and the loose files; the build id covers all of it.
	uint64_t build_hash = io::kFnv1a64Offset;
	for (const BuildArchive &archive : plan_.archives) {
		uint64_t hash = 0;
		Diagnostic error;
		if (!hash_entries(archive.entries, hash, error)) return fail(std::move(error));
		hashes_[archive.file_name] = io::hex64(hash);
		build_hash = io::fnv1a64_value(build_hash, hash);
	}
	{
		uint64_t loose_hash = 0;
		Diagnostic error;
		if (!hash_entries(plan_.loose, loose_hash, error)) return fail(std::move(error));
		build_hash = io::fnv1a64_value(build_hash, loose_hash);
	}
	build_hash = io::fnv1a64_value(build_hash, static_cast<int>(kBuildArchiveFormat));
	report_.build_id = io::hex64(build_hash);

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
			steps_done_ = steps_total();
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
	if (!ensure_directory(tmp_dir.generic_string(), io_error) ||
	    !write_file_atomic((tmp_dir / kBuildStagingMarkerFileName).generic_string(), report_.build_id, io_error)) {
		return fail(make_diagnostic(DiagnosticSeverity::Error, "build.write", io_error));
	}
	tmp_dir_ = tmp_dir.generic_string(); // from here a failure removes it
	phase_ = Phase::Archives;
}

void BuildRun::pack_next_archive() {
	if (archive_index_ >= plan_.archives.size()) {
		phase_ = Phase::Loose;
		return copy_next_loose();
	}
	const BuildArchive &archive = plan_.archives[archive_index_++];
	const fs::path target = fs::path(tmp_dir_) / archive.file_name;
	const fs::path previous_file = fs::path(last_dir_) / archive.file_name;
	const auto previous = last_hashes_.find(archive.file_name);
	std::error_code ec;
	const bool reusable = !last_dir_.empty() && previous != last_hashes_.end() &&
	                      previous->second == hashes_[archive.file_name] &&
	                      fs::is_regular_file(previous_file, ec);
	Diagnostic error;
	if (reusable) {
		if (!copy_file(previous_file, target, error, archive.file_name)) return fail(std::move(error));
		report_.archives_reused.push_back(archive.file_name);
	} else {
		if (!write_archive(archive, target.generic_string(), kBuildArchiveFormat, error)) {
			return fail(std::move(error));
		}
		report_.archives_written.push_back(archive.file_name);
	}
	last_step_ = archive.file_name;
	++steps_done_;
}

void BuildRun::copy_next_loose() {
	if (loose_index_ >= plan_.loose.size()) {
		phase_ = Phase::Publish;
		return;
	}
	const BuildEntry &entry = plan_.loose[loose_index_++];
	Diagnostic error;
	if (!copy_file(fs::path(entry.source_path), fs::path(tmp_dir_) / entry.logical_name, error,
	               entry.logical_name)) {
		return fail(std::move(error));
	}
	report_.loose_written.push_back(entry.logical_name);
	last_step_ = entry.logical_name;
	++steps_done_;
}

// Prove the staged directory mounts, record it, rename it into place, prune.
void BuildRun::publish() {
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
	phase_ = Phase::Done;
}

BuildReport run_build(const BuildPlan &plan, const std::string &output_root,
                      const std::vector<std::string> &protected_dirs, BuildProgress *progress) {
	BuildRun run(plan, output_root, protected_dirs);
	size_t reported = 0;
	while (!run.step()) {
		if (progress && run.steps_done() > reported) {
			reported = run.steps_done();
			progress->on_step(run.last_step(), reported, run.steps_total());
		}
	}
	if (progress && run.report().ok && !run.report().reused_existing && run.steps_done() > reported) {
		progress->on_step(run.last_step(), run.steps_done(), run.steps_total());
	}
	return run.report();
}

} // namespace opennova::editor
