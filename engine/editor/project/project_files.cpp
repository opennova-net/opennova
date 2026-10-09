#include <editor/project/project_files.h>

#include <filesystem>
#include <system_error>

#include <base/io/os_path.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>

namespace fs = std::filesystem;

namespace opennova::editor {

fs::path path_of(const std::string &utf8) {
#ifdef _WIN32
	return fs::path(io::widen_utf8(utf8));
#else
	return fs::path(utf8);
#endif
}

std::string utf8_of(const fs::path &path) {
	return io::utf8_generic_path(path);
}

std::string join_path(const std::string &dir, const std::string &name) {
	return utf8_of(path_of(dir) / path_of(name));
}

fs::path system_path(const std::string &path) {
#ifdef _WIN32
	fs::path given = path_of(path);
	const std::wstring raw = given.wstring();
	// Already in a form the system takes as it is, extended-length or a device's, its separators
	// made backslashes (a generic spelling of one has slashes).
	for (const wchar_t *prefix : {L"\\\\?\\", L"\\\\.\\", L"//?/", L"//./"})
		if (raw.rfind(prefix, 0) == 0) return given.make_preferred();
	std::error_code ec;
	const fs::path absolute = fs::absolute(given, ec);
	if (ec || path.empty()) return given;
	// An extended-length path is taken as it is spelled: its separators backslashes, no "." or
	// ".." left in it.
	const std::wstring normal = absolute.lexically_normal().make_preferred().wstring();
	if (normal.rfind(L"\\\\", 0) == 0) return fs::path(L"\\\\?\\UNC\\" + normal.substr(2));
	if (normal.size() >= 3 && normal[1] == L':' && normal[2] == L'\\') return fs::path(L"\\\\?\\" + normal);
	return absolute;
#else
	return path_of(path);
#endif
}

bool is_dot_directory(const fs::path &path) {
	const std::string name = utf8_of(path.filename());
	return !name.empty() && name[0] == '.';
}

std::string shown_path(const std::string &path, const std::string &root) {
	if (root.empty()) return path;
	const fs::path relative = path_of(path).lexically_relative(path_of(root));
	if (relative.empty() || *relative.begin() == "..") return path;
	return utf8_of(relative);
}

std::string basename_of(const std::string &path) {
	return utf8_of(path_of(path).filename());
}

bool check_file_name(const std::string &name, AssetKind kind, FileNameProblem &problem, std::string &message) {
	if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) {
		problem = FileNameProblem::Name;
		message = "'" + name + "' is not a plain file name: give a name with no folders.";
		return false;
	}
	// The archive's name limit binds only a file the build packs into an archive and an import
	// source, whose outputs take its name: a loose kind (a video, a music bank, a config) is copied
	// beside the archives under any name, and Unknown (a kind not decided yet, or one the build
	// leaves out) binds nothing.
	if (archive_name_limit_binds(kind) && !logical_name_fits_archive(name)) {
		problem = FileNameProblem::Name;
		message = "'" + name + "' does not fit the game's archives: names are up to 16 characters.";
		return false;
	}
	if (kind != AssetKind::Unknown) {
		if (!asset_name_fits_kind(name, kind)) {
			problem = FileNameProblem::Kind;
			message = "The game reads '" + name + "' as " + asset_kind_label(classify_asset(name, nullptr)) + ", not " +
			          asset_kind_label(kind) + ": a file's name decides its kind.";
			return false;
		}
	}
	return true;
}

bool check_project_file_name(const std::string &root, const std::string &dir, const std::string &name, AssetKind kind,
                             FileNameProblem &problem, std::string &message) {
	if (!check_file_name(name, kind, problem, message)) return false;
	// Where it lands, symbolic links resolved, must stay under the project.
	if (root.empty() || !io::path_within(path_of(root) / path_of(dir) / path_of(name), path_of(root))) {
		problem = FileNameProblem::Path;
		message = "'" + join_path(dir, name) + "' would land outside the project.";
		return false;
	}
	return true;
}

} // namespace opennova::editor
