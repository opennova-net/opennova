#include <editor/project_build/build_session.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <system_error>

#include <base/io/hash.h>
#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_registry.h>
#include <editor/project/project_files.h>
#include <formats/pff/pff.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

std::string hex64(uint64_t value) {
	char buf[24];
	std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
	return buf;
}

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
// directory: the same VFS the runtime boots with, archive-only, the fixed boot table.
bool verify_staged(const BuildPlan &plan, const std::string &dir, Diagnostic &error) {
	Vfs vfs;
	if (!vfs.mount_game(dir, std::string(), VfsMountMode::Packed, VfsArchiveDiscovery::RetailTable) ||
	    !vfs.has_mounted_archive()) {
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

BuildReport run_build(const BuildPlan &plan, const ProjectDocument &doc, const std::string &output_root,
                      const std::vector<std::string> &protected_dirs, BuildProgress *progress) {
	BuildReport report;
	if (!plan.ok) {
		report.diagnostics = plan.diagnostics;
		report.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "build.blocked",
		                                             "The project has problems that would stop the game; fix them first."));
		return report;
	}
	// The archive flavour the target game ships: every JO-family title reads PFF3
	// [orig: PFF_Open @ 0x7682e0]; the profile keys the SCR policy only, so the
	// format rides on the target game code here until a profile row carries it.
	const pff::PffFormat format = pff::PFF_FORMAT_PFF3;
	(void)doc;

	// Hash every archive's content and the loose files; the build id covers all of it.
	std::map<std::string, std::string> hashes;
	uint64_t build_hash = io::kFnv1a64Offset;
	for (const BuildArchive &archive : plan.archives) {
		uint64_t hash = 0;
		Diagnostic error;
		if (!hash_entries(archive.entries, hash, error)) {
			report.diagnostics.push_back(error);
			return report;
		}
		hashes[archive.file_name] = hex64(hash);
		build_hash = io::fnv1a64_value(build_hash, hash);
	}
	{
		uint64_t loose_hash = 0;
		Diagnostic error;
		if (!hash_entries(plan.loose, loose_hash, error)) {
			report.diagnostics.push_back(error);
			return report;
		}
		build_hash = io::fnv1a64_value(build_hash, loose_hash);
	}
	build_hash = io::fnv1a64_value(build_hash, static_cast<int>(format));
	report.build_id = hex64(build_hash);

	std::string io_error;
	if (!ensure_directory(output_root, io_error)) {
		report.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "build.write", io_error));
		return report;
	}
	const fs::path final_dir = fs::path(output_root) / report.build_id;
	std::error_code ec;
	if (fs::is_directory(final_dir, ec)) {
		// Same content, same build: prove it still mounts and hand it back.
		Diagnostic error;
		if (verify_staged(plan, final_dir.generic_string(), error)) {
			report.ok = true;
			report.reused_existing = true;
			report.build_dir = final_dir.generic_string();
			return report;
		}
		if (!is_prunable_build_dir(final_dir)) {
			report.diagnostics.push_back(make_diagnostic(
			        DiagnosticSeverity::Error, "build.write",
			        "cannot publish the build: " + final_dir.generic_string() +
			                " exists and is not a build directory"));
			return report;
		}
		fs::remove_all(final_dir, ec); // a damaged build is rebuilt
	}

	LastGood last;
	const bool have_last = read_last_good(output_root, last);
	const fs::path last_dir = have_last ? fs::path(output_root) / last.build_id : fs::path();

	const fs::path tmp_dir = fs::path(output_root) / (report.build_id + kBuildStagingSuffix);
	if (fs::exists(tmp_dir, ec) && !is_prunable_build_dir(tmp_dir)) {
		report.diagnostics.push_back(make_diagnostic(
		        DiagnosticSeverity::Error, "build.write",
		        "cannot stage the build: " + tmp_dir.generic_string() + " exists and is not a build directory"));
		return report;
	}
	fs::remove_all(tmp_dir, ec);
	if (!ensure_directory(tmp_dir.generic_string(), io_error) ||
	    !write_file_atomic((tmp_dir / kBuildStagingMarkerFileName).generic_string(), report.build_id, io_error)) {
		report.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "build.write", io_error));
		return report;
	}

	const size_t total_steps = plan.archives.size() + plan.loose.size();
	size_t step = 0;
	for (const BuildArchive &archive : plan.archives) {
		const fs::path target = tmp_dir / archive.file_name;
		const auto previous = last.archive_hashes.find(archive.file_name);
		const bool reusable = have_last && previous != last.archive_hashes.end() &&
		                      previous->second == hashes[archive.file_name] &&
		                      fs::is_regular_file(last_dir / archive.file_name, ec);
		Diagnostic error;
		if (reusable) {
			if (!copy_file(last_dir / archive.file_name, target, error, archive.file_name)) {
				report.diagnostics.push_back(error);
				fs::remove_all(tmp_dir, ec);
				return report;
			}
			report.archives_reused.push_back(archive.file_name);
		} else {
			if (!write_archive(archive, target.generic_string(), format, error)) {
				report.diagnostics.push_back(error);
				fs::remove_all(tmp_dir, ec);
				return report;
			}
			report.archives_written.push_back(archive.file_name);
		}
		if (progress) progress->on_step(archive.file_name, ++step, total_steps);
	}
	std::vector<std::string> loose_names;
	for (const BuildEntry &entry : plan.loose) {
		Diagnostic error;
		if (!copy_file(fs::path(entry.source_path), tmp_dir / entry.logical_name, error, entry.logical_name)) {
			report.diagnostics.push_back(error);
			fs::remove_all(tmp_dir, ec);
			return report;
		}
		report.loose_written.push_back(entry.logical_name);
		loose_names.push_back(entry.logical_name);
		if (progress) progress->on_step(entry.logical_name, ++step, total_steps);
	}

	Diagnostic verify_error;
	if (!verify_staged(plan, tmp_dir.generic_string(), verify_error)) {
		report.diagnostics.push_back(verify_error);
		fs::remove_all(tmp_dir, ec);
		return report;
	}
	const io::JsonValue record = build_record(report.build_id, hashes, loose_names);
	if (!write_file_atomic((tmp_dir / kBuildRecordFileName).generic_string(), io::json_write(record), io_error)) {
		report.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "build.write", io_error));
		fs::remove_all(tmp_dir, ec);
		return report;
	}
	fs::rename(tmp_dir, final_dir, ec);
	if (ec) {
		report.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "build.write",
		                                             "cannot publish the build: " + ec.message()));
		fs::remove_all(tmp_dir, ec);
		return report;
	}
	fs::remove(final_dir / kBuildStagingMarkerFileName, ec); // published: the record is its proof now
	if (!write_file_atomic((fs::path(output_root) / kLastGoodBuildFileName).generic_string(),
	                       io::json_write(record), io_error)) {
		report.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "build.write", io_error));
		return report;
	}
	prune_old_builds(output_root, report.build_id, protected_dirs);
	report.ok = true;
	report.build_dir = final_dir.generic_string();
	return report;
}

} // namespace opennova::editor
