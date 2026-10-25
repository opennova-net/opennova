#include <editor/assets/project_walk.h>

#include <system_error>

#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

bool same_path(const fs::path &a, const fs::path &b) {
	std::error_code ec;
	const fs::path ca = fs::weakly_canonical(a, ec);
	if (ec)
		return false;
	const fs::path cb = fs::weakly_canonical(b, ec);
	if (ec)
		return false;
	return ca == cb;
}

} // namespace

bool export_folder(const fs::path &dir, const fs::path &export_dir) {
	if (same_path(dir, export_dir))
		return true;
	const fs::path name = export_dir.filename();
	for (const char *suffix : { kExportStagingSuffix, kExportPreviousSuffix }) {
		fs::path beside = export_dir;
		beside.replace_filename(fs::path(name).concat(suffix));
		if (same_path(dir, beside))
			return true;
	}
	return false;
}

bool walk_skips_folder(const fs::path &dir, const fs::path &export_dir) {
	return is_dot_directory(dir) || export_folder(dir, export_dir);
}

} // namespace opennova::editor
