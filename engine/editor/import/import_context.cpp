#include <editor/import/import_context.h>

#include <filesystem>
#include <iterator>
#include <system_error>
#include <utility>

#include <base/io/file_time.h>
#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

ImportContext::ImportContext(std::string source_name, const std::vector<uint8_t> &source,
                             const ImportOptions &options, std::string folder, std::string root) :
		source_name_(std::move(source_name)),
		source_(source),
		options_(options),
		folder_(std::move(folder)),
		root_(std::move(root)) {}

bool ImportContext::resolve(const std::string &folder, const std::string &root, const std::string &path,
                            std::string &file, std::string &relative) {
	const fs::path asked = path_of(path);
	if (path.empty() || asked.has_root_name() || asked.has_root_directory()) return false;
	const fs::path full = (path_of(folder) / asked).lexically_normal();
	const fs::path within = full.lexically_relative(path_of(root).lexically_normal());
	if (within.empty() || within.filename().empty()) return false;
	// Inside the project, and in no dot-folder on the way (the cache, a .git): the walks never
	// enter one, so nothing there is a project file.
	for (auto part = within.begin(); part != within.end(); ++part) {
		const std::string name = utf8_of(*part);
		if (name == ".." || name == ".") return false;
		if (std::next(part) != within.end() && !name.empty() && name[0] == '.') return false;
	}
	file = utf8_of(full);
	relative = utf8_of(within);
	return true;
}

bool ImportContext::read(const std::string &path, std::vector<uint8_t> &out) {
	out.clear();
	std::string file, relative;
	const auto refuse = [&](const std::string &why) {
		findings_.push_back(make_finding(CoreFinding::ImportInput, DiagnosticSeverity::Error,
		                                 source_name_ + " reads " + path + ", " + why, source_name_));
		return false;
	};
	if (!resolve(folder_, root_, path, file, relative))
		return refuse("which is no file of the project beside it (an import reads files of its source's folder "
		              "and the folders under the project, never one outside it or under a dot-folder).");
	const fs::path on_disk = system_path(file);
	std::error_code ec;
	if (!fs::is_regular_file(on_disk, ec)) return refuse("which is not there.");
	// The stamp first: a change made while the bytes are read reads as a change the next time.
	ImportInputStamp stamp;
	stamp.relative = relative;
	stamp.size = static_cast<uint64_t>(fs::file_size(on_disk, ec));
	stamp.modified = io::file_modified_ticks(on_disk);
	std::string error;
	if (!io::read_file_bytes(file, out, error)) return refuse("which cannot be read: " + error + ".");
	bytes_read_ += out.size();
	const std::string key = strutil::to_lower(relative);
	for (const ImportInputStamp &known : stamps_)
		if (strutil::to_lower(known.relative) == key) return true; // an input is listed once
	inputs_.push_back({utf8_of(path_of(path).lexically_normal()),
	                   io::fnv1a64_bytes(io::kFnv1a64Offset, out.data(), out.size())});
	stamps_.push_back(std::move(stamp));
	return true;
}

} // namespace opennova::editor
