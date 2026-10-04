#include <editor/project_build/export_build.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <system_error>

#include <base/io/file_time.h>
#include <base/io/json.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// The largest single read or write inside a step (a budget spans several).
constexpr size_t kCopyChunk = size_t(1) << 20;
// What opening a file, or swapping the folder, counts for in a step's budget.
constexpr uint64_t kOpenCost = 64 * 1024;

Diagnostic export_error(CoreFinding code, const std::string &message) {
	return make_finding(code, DiagnosticSeverity::Error, message);
}

// The record of the export in `dir` (a system path): read when it names the project `project_id`.
bool export_record_of(const fs::path &dir, const std::string &project_id, io::JsonValue &record) {
	std::string text, error;
	io::JsonValue json;
	if (!read_file_text(utf8_of(dir / kExportRecordFileName), text, error) || !io::json_parse(text, json, error) ||
	    !json.is_object() || project_id.empty() || json.get_string("project_id", "") != project_id)
		return false;
	record = std::move(json);
	return true;
}

// Whether the folder at `dir` (a system path) may be replaced by an export of `project_id`: there is
// none, it is empty, or it is an export of the project.
bool replaceable(const fs::path &dir, const std::string &project_id) {
	std::error_code ec;
	if (!fs::exists(dir, ec)) return true;
	if (!fs::is_directory(dir, ec)) return false;
	if (fs::directory_iterator(dir, ec) == fs::directory_iterator() && !ec) return true;
	io::JsonValue record;
	return export_record_of(dir, project_id, record);
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

// The files under `root` (a system path), '/'-separated and relative, each with its system path and size.
struct Found {
	std::string relative;
	fs::path path;
	uint64_t size = 0;
};
bool files_under(const fs::path &root, std::vector<Found> &out, std::string &error) {
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
	     it.increment(ec)) {
		std::error_code kind;
		if (!it->is_regular_file(kind)) continue;
		Found found;
		found.relative = utf8_of(it->path().lexically_relative(root));
		std::replace(found.relative.begin(), found.relative.end(), '\\', '/');
		found.path = it->path();
		found.size = uint64_t(fs::file_size(it->path(), kind));
		out.push_back(std::move(found));
	}
	if (ec) error = "cannot read " + utf8_of(root) + ": " + ec.message();
	return !ec;
}

std::string lower(std::string text) {
	for (char &c : text)
		if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
	return text;
}

// A few names, for a finding's words.
std::string some_of(const std::vector<std::string> &names) {
	std::string out;
	for (size_t i = 0; i < names.size() && i < 8; ++i) out += (i ? ", " : "") + names[i];
	if (names.size() > 8) out += " and " + std::to_string(names.size() - 8) + " more";
	return out;
}

} // namespace

struct ExportRun::Streams {
	std::ifstream in;
	std::ofstream out;
	std::vector<char> buffer = std::vector<char>(kCopyChunk);
};

bool remove_tree(const std::string &dir, std::string &reason) {
	std::error_code ec, left;
	fs::remove_all(system_path(dir), ec);
	if (!ec && !fs::exists(system_path(dir), left)) return true;
	reason = ec ? ec.message() : std::string("a file of it is in use");
	return false;
}

ExportRun::ExportRun(ExportRequest request, RemoveTree remove_previous) :
		request_(std::move(request)), remove_previous_(std::move(remove_previous)), streams_(std::make_unique<Streams>()) {
	request_.export_dir = without_trailing_separator(request_.export_dir);
	report_.export_dir = request_.export_dir;
	staging_ = request_.export_dir + kExportStagingSuffix;
	previous_ = request_.export_dir + kExportPreviousSuffix;
	label_ = "Exporting to " + request_.export_dir;
}

ExportRun::~ExportRun() {
	if (!done()) cancel();
}

bool ExportRun::step(uint64_t budget) {
	switch (phase_) {
	case Phase::Check: check(); break;
	case Phase::Copy: copy(budget); break;
	case Phase::Swap: swap(); break;
	case Phase::Done: break;
	}
	return done();
}

void ExportRun::close_streams() {
	streams_->in.close();
	streams_->in.clear();
	streams_->out.close();
	streams_->out.clear();
}

void ExportRun::fail(CoreFinding code, const std::string &message) {
	close_streams();
	report_.diagnostics.push_back(export_error(code, message));
	report_.files.clear();
	report_.ok = false;
	remove_staging();
	phase_ = Phase::Done;
}

// The staging folder removed, once it is this export's own (a folder of the name the check refused is
// the person's, never touched).
void ExportRun::remove_staging() {
	if (!staged_) return;
	std::error_code removed;
	fs::remove_all(system_path(staging_), removed);
}

void ExportRun::cancel() {
	if (done()) return;
	close_streams();
	remove_staging();
	report_.files.clear();
	report_.ok = false;
	report_.diagnostics.push_back(make_finding(CoreFinding::ExportCancelled, DiagnosticSeverity::Info,
	                                           "The export was cancelled: " + request_.export_dir + " is as it was."));
	phase_ = Phase::Done;
}

void ExportRun::check() {
	const fs::path target = system_path(request_.export_dir);
	const fs::path staging = system_path(staging_);
	const fs::path previous = system_path(previous_);
	std::error_code ec;
	if (request_.build_dir.empty() || !fs::is_directory(system_path(request_.build_dir), ec))
		return fail(CoreFinding::ExportWrite, "There is no build to export: build first.");
	if (within(path_of(request_.export_dir), path_of(request_.build_dir)) ||
	    within(path_of(request_.build_dir), path_of(request_.export_dir)))
		return fail(CoreFinding::ExportFolder,
		            "An export cannot land in " + request_.export_dir + ", which holds the build or lies in it.");
	// The person's folders are never written over: the folder, and a staging folder beside it, only
	// when missing, empty or an export of this project.
	for (const fs::path &dir : {target, staging, previous}) {
		if (replaceable(dir, request_.project_id)) continue;
		return fail(CoreFinding::ExportFolder, utf8_of(dir) + " holds files that are no export of this project: choose an "
		                                                     "empty folder, or move them out first.");
	}
	if (!request_.runtime_dir.empty() && !fs::is_directory(system_path(request_.runtime_dir), ec))
		return fail(CoreFinding::ExportRuntime,
		            "The runtime to ship beside the game was not found in " + request_.runtime_dir +
		                    ": set the runtime in File > Project settings..., or export without it.");
	// What it writes: the build's files but its record, the runtime's under runtime/.
	std::string error;
	std::vector<Found> found;
	if (!files_under(system_path(request_.build_dir), found, error)) return fail(CoreFinding::ExportWrite, error);
	std::set<std::string> writes;
	for (const Found &file : found) {
		if (file.relative == kBuildRecordFileName) continue;
		files_.push_back({ utf8_of(file.path), file.relative, file.size, true });
		writes.insert(lower(file.relative));
	}
	if (!request_.runtime_dir.empty()) {
		found.clear();
		if (!files_under(system_path(request_.runtime_dir), found, error)) return fail(CoreFinding::ExportWrite, error);
		for (const Found &file : found) {
			const std::string to = std::string(kExportRuntimeFolder) + "/" + file.relative;
			files_.push_back({ utf8_of(file.path), to, file.size, true });
			writes.insert(lower(to));
		}
	}
	// Over an earlier export of the project: what the person did there.
	io::JsonValue record;
	if (export_record_of(target, request_.project_id, record)) {
		std::set<std::string> written;
		if (const io::JsonValue *names = record.get("files"))
			for (const io::JsonValue &name : names->array)
				if (name.is_string()) written.insert(lower(name.string));
		const int64_t recorded = io::file_modified_ticks(target / kExportRecordFileName);
		found.clear();
		if (!files_under(target, found, error)) return fail(CoreFinding::ExportWrite, error);
		std::set<std::string> present;
		for (const Found &file : found) {
			if (file.relative == kExportRecordFileName) continue;
			const std::string key = lower(file.relative);
			present.insert(key);
			if (!written.count(key)) {
				// Theirs: kept, unless the new export writes a file of the path.
				if (writes.count(key)) {
					report_.replaced.push_back(file.relative);
				} else {
					files_.push_back({ utf8_of(file.path), file.relative, file.size, false });
					report_.kept.push_back(file.relative);
				}
			} else if (io::file_modified_ticks(file.path) > recorded) {
				// Ours, changed since the export was written: the new export's copy (or none) takes its place.
				if (writes.count(key)) report_.replaced.push_back(file.relative);
				else report_.removed.push_back(file.relative);
			} else if (!writes.count(key)) {
				report_.removed.push_back(file.relative);
			}
		}
	}
	bytes_total_ = 0;
	for (const File &file : files_) bytes_total_ += file.size;
	// Staged afresh, its record first, so a staging folder an export cut short leaves proves itself the
	// project's.
	staged_ = true;
	fs::remove_all(staging, ec);
	if (ec || !ensure_directory(staging_, error))
		return fail(CoreFinding::ExportWrite,
		            "cannot stage the export in " + staging_ + ": " + (ec ? ec.message() : error));
	if (!write_file_atomic(join_path(staging_, kExportRecordFileName), io::json_write(export_record(request_, {})), error))
		return fail(CoreFinding::ExportWrite, error);
	phase_ = Phase::Copy;
}

void ExportRun::copy(uint64_t budget) {
	uint64_t left = std::max<uint64_t>(budget, 1);
	while (left > 0 && next_ < files_.size()) {
		const File &file = files_[next_];
		const std::string target = join_path(staging_, file.to);
		if (!streams_->in.is_open()) {
			std::string error;
			if (!ensure_directory(utf8_of(path_of(target).parent_path()), error)) return fail(CoreFinding::ExportWrite, error);
			streams_->in.open(system_path(file.from), std::ios::binary);
			streams_->out.open(system_path(target), std::ios::binary | std::ios::trunc);
			if (!streams_->in || !streams_->out)
				return fail(CoreFinding::ExportWrite, "cannot copy " + file.from + " to " + target);
			offset_ = 0;
			label_ = "Exporting " + file.to;
			left -= std::min(left, kOpenCost);
		}
		while (left > 0 && offset_ < file.size) {
			const size_t chunk = size_t(std::min<uint64_t>({ left, file.size - offset_, kCopyChunk }));
			streams_->in.read(streams_->buffer.data(), std::streamsize(chunk));
			if (size_t(streams_->in.gcount()) != chunk) return fail(CoreFinding::ExportWrite, "cannot read " + file.from);
			streams_->out.write(streams_->buffer.data(), std::streamsize(chunk));
			if (!streams_->out) return fail(CoreFinding::ExportWrite, "cannot write " + target);
			offset_ += chunk;
			bytes_done_ += chunk;
			left -= chunk;
		}
		if (offset_ < file.size) return;
		close_streams();
		if (file.listed) {
			report_.files.push_back(file.to);
			report_.bytes += file.size;
		}
		++next_;
	}
	if (next_ == files_.size()) phase_ = Phase::Swap;
}

void ExportRun::swap() {
	std::sort(report_.files.begin(), report_.files.end());
	std::string error;
	if (!write_file_atomic(join_path(staging_, kExportRecordFileName), io::json_write(export_record(request_, report_.files)),
	                       error))
		return fail(CoreFinding::ExportWrite, error);
	// In place: the folder's last export (this project's, or an empty folder) set aside, the new one
	// renamed into its place, then the old removed; the old put back when the new cannot go in.
	const fs::path target = system_path(request_.export_dir);
	const fs::path staging = system_path(staging_);
	const fs::path previous = system_path(previous_);
	std::error_code ec;
	fs::remove_all(previous, ec);
	if (ec) return fail(CoreFinding::ExportWrite, "cannot replace " + request_.export_dir + ": " + ec.message());
	if (!ensure_directory(utf8_of(path_of(request_.export_dir).parent_path()), error))
		return fail(CoreFinding::ExportWrite, error);
	const bool had = fs::exists(target, ec);
	if (had && !rename_with_retry(target, previous, ec))
		return fail(CoreFinding::ExportWrite, "cannot replace " + request_.export_dir + ": " + ec.message());
	if (!rename_with_retry(staging, target, ec)) {
		const std::string reason = ec.message();
		if (had) rename_with_retry(previous, target, ec);
		return fail(CoreFinding::ExportWrite, "cannot put the export in place: " + reason);
	}
	std::string reason;
	if (had && !remove_previous_(previous_, reason))
		report_.diagnostics.push_back(make_finding(
		        CoreFinding::ExportCleanup, DiagnosticSeverity::Warning,
		        "The last export, set aside in " + previous_ + ", could not be removed (" + reason +
		                "): the new export is in place, and the next export removes it first."));
	if (!report_.kept.empty() || !report_.replaced.empty() || !report_.removed.empty()) {
		std::string words = "The export replaced the one in " + request_.export_dir + ".";
		if (!report_.kept.empty()) words += " Kept what you added there: " + some_of(report_.kept) + ".";
		if (!report_.replaced.empty())
			words += " Replaced what you changed or added there under the export's own names: " + some_of(report_.replaced) + ".";
		if (!report_.removed.empty())
			words += " Removed the earlier export's files the build no longer has: " + some_of(report_.removed) + ".";
		report_.diagnostics.push_back(make_finding(CoreFinding::ExportReplaced, DiagnosticSeverity::Info, words));
	}
	report_.ok = true;
	label_ = "Exported to " + request_.export_dir;
	bytes_done_ = bytes_total_;
	phase_ = Phase::Done;
}

ExportReport export_build(const ExportRequest &request, const RemoveTree &remove_previous) {
	ExportRun run(request, remove_previous);
	while (!run.step(UINT64_MAX)) {
	}
	return run.report();
}

} // namespace opennova::editor
