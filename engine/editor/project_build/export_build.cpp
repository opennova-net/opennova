#include <editor/project_build/export_build.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <base/io/json.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

Diagnostic export_error(CoreFinding code, const std::string &message) {
	return make_finding(code, DiagnosticSeverity::Error, message);
}

// Whether `dir` (a system path) holds an export of the project `project_id`: its record names it.
bool export_of(const fs::path &dir, const std::string &project_id) {
	std::string text, error;
	io::JsonValue json;
	return read_file_text(utf8_of(dir / kExportRecordFileName), text, error) && io::json_parse(text, json, error) &&
	       json.is_object() && !project_id.empty() && json.get_string("project_id", "") == project_id;
}

// Whether the folder at `dir` (a system path) may be replaced by an export of `project_id`: there is
// none, it is empty, or it is an export of the project.
bool replaceable(const fs::path &dir, const std::string &project_id) {
	std::error_code ec;
	if (!fs::exists(dir, ec)) return true;
	if (!fs::is_directory(dir, ec)) return false;
	if (fs::directory_iterator(dir, ec) == fs::directory_iterator() && !ec) return true;
	return export_of(dir, project_id);
}

// Whether `inner` lies in `outer` (or is it), lexically.
bool within(const fs::path &inner, const fs::path &outer) {
	const fs::path relative = inner.lexically_normal().lexically_relative(outer.lexically_normal());
	return !relative.empty() && *relative.begin() != "..";
}

io::JsonValue export_record(const ExportRequest &request, const std::vector<std::string> &files) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kExportRecordSchemaVersion));
	json.set("project_id", io::JsonValue::make_string(request.project_id));
	json.set("build_id", io::JsonValue::make_string(request.build_id));
	json.set("expansion", io::JsonValue::make_string(request.expansion));
	io::JsonValue names = io::JsonValue::make_array();
	for (const std::string &name : files) names.push(io::JsonValue::make_string(name));
	json.set("files", std::move(names));
	return json;
}

// Every file under `from` copied to the same place under `to` (prefixed `prefix` in the list), but
// the build's record at its top; false with `error`.
bool copy_tree(const std::string &from, const std::string &to, const std::string &prefix, bool skip_record,
               ExportReport &report, std::string &error) {
	std::error_code ec;
	const fs::path root = system_path(from);
	for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
	     it.increment(ec)) {
		std::error_code kind;
		if (!it->is_regular_file(kind)) continue;
		const std::string relative = utf8_of(it->path().lexically_relative(root));
		if (skip_record && relative == kBuildRecordFileName) continue;
		const std::string target = join_path(to, relative);
		const fs::path parent = path_of(target).parent_path();
		if (!ensure_directory(utf8_of(parent), error)) return false;
		std::error_code copied;
		fs::copy_file(it->path(), system_path(target), copied);
		if (copied) {
			error = "cannot copy " + utf8_of(it->path()) + ": " + copied.message();
			return false;
		}
		report.files.push_back(prefix + relative);
		report.bytes += fs::file_size(it->path(), kind);
	}
	if (ec) error = "cannot read " + from + ": " + ec.message();
	return !ec;
}

} // namespace

ExportReport export_build(const ExportRequest &request) {
	ExportReport report;
	report.export_dir = request.export_dir;
	const fs::path target = system_path(request.export_dir);
	const fs::path staging = system_path(request.export_dir + kExportStagingSuffix);
	const fs::path previous = system_path(request.export_dir + kExportPreviousSuffix);
	std::error_code ec;
	if (request.build_dir.empty() || !fs::is_directory(system_path(request.build_dir), ec)) {
		report.diagnostics.push_back(export_error(CoreFinding::ExportWrite, "There is no build to export: build first."));
		return report;
	}
	if (within(path_of(request.export_dir), path_of(request.build_dir)) ||
	    within(path_of(request.build_dir), path_of(request.export_dir))) {
		report.diagnostics.push_back(export_error(
		        CoreFinding::ExportFolder, "An export cannot land in " + request.export_dir + ", which holds the build or lies in it."));
		return report;
	}
	// The person's folders are never written over: the folder, and a staging folder beside it, only
	// when missing, empty or an export of this project.
	for (const fs::path &dir : {target, staging, previous}) {
		if (replaceable(dir, request.project_id)) continue;
		report.diagnostics.push_back(export_error(
		        CoreFinding::ExportFolder, utf8_of(dir) + " holds files that are no export of this project: choose an empty "
		                                                 "folder, or move them out first."));
		return report;
	}
	if (!request.runtime_dir.empty() && !fs::is_directory(system_path(request.runtime_dir), ec)) {
		report.diagnostics.push_back(export_error(
		        CoreFinding::ExportRuntime, "The runtime to ship beside the game was not found in " + request.runtime_dir +
		                                            ": set the runtime in File > Project settings..., or export without it."));
		return report;
	}
	std::string error;
	fs::remove_all(staging, ec);
	if (ec || !ensure_directory(utf8_of(staging), error)) {
		report.diagnostics.push_back(export_error(CoreFinding::ExportWrite,
		                                          "cannot stage the export in " + utf8_of(staging) + ": " +
		                                                  (ec ? ec.message() : error)));
		return report;
	}
	// Its record first, so a staging folder an export cut short leaves proves itself the project's.
	const std::string staged = utf8_of(staging);
	const auto fail = [&](const std::string &message) {
		report.diagnostics.push_back(export_error(CoreFinding::ExportWrite, message));
		report.files.clear();
		std::error_code removed;
		fs::remove_all(staging, removed);
		return report;
	};
	if (!write_file_atomic(join_path(staged, kExportRecordFileName), io::json_write(export_record(request, {})), error) ||
	    !copy_tree(request.build_dir, staged, std::string(), true, report, error))
		return fail(error);
	if (!request.runtime_dir.empty() &&
	    !copy_tree(request.runtime_dir, join_path(staged, kExportRuntimeFolder), std::string(kExportRuntimeFolder) + "/",
	               false, report, error))
		return fail(error);
	std::sort(report.files.begin(), report.files.end());
	if (!write_file_atomic(join_path(staged, kExportRecordFileName), io::json_write(export_record(request, report.files)),
	                       error))
		return fail(error);
	// In place: the folder's last export (this project's, or an empty folder) set aside, the new one
	// renamed into its place, then the old removed; the old put back when the new cannot go in.
	fs::remove_all(previous, ec);
	if (ec) return fail("cannot replace " + request.export_dir + ": " + ec.message());
	if (!ensure_directory(utf8_of(path_of(request.export_dir).parent_path()), error)) return fail(error);
	const bool had = fs::exists(target, ec);
	if (had && !rename_with_retry(target, previous, ec))
		return fail("cannot replace " + request.export_dir + ": " + ec.message());
	if (!rename_with_retry(staging, target, ec)) {
		const std::string reason = ec.message();
		if (had) rename_with_retry(previous, target, ec);
		return fail("cannot put the export in place: " + reason);
	}
	fs::remove_all(previous, ec); // what stays of it is an export of this project, replaced by the next
	report.ok = true;
	return report;
}

} // namespace opennova::editor
