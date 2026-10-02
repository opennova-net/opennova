#include <editor/import/import_run.h>

#include <filesystem>

#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <editor/assets/project_scan.h>
#include <editor/import/import_pass.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

bool import_source_named(const std::string &only, const std::string &source_relative_path) {
	if (only.empty()) return true;
	const std::string wanted = strutil::to_lower(only);
	return wanted == strutil::to_lower(source_relative_path) ||
	       wanted == strutil::to_lower(basename_of(source_relative_path));
}

std::string import_output_dir(const ProjectPaths &paths, const std::string &source_relative_path) {
	const std::string key = strutil::to_lower(source_relative_path);
	const uint64_t hash = io::fnv1a64_bytes(io::kFnv1a64Offset, key.data(), key.size());
	const fs::path dir = fs::relative(fs::path(paths.imported_dir), fs::path(paths.root)) / io::hex64(hash);
	return dir.generic_string();
}

ImportRunResult run_imports(const ProjectPaths &paths, const ProjectDocument &project, bool force, const std::string &only) {
	ImportPass pass(paths, project, force, only);
	while (!pass.step(kWholeWalkStep)) {
	}
	return pass.take();
}

} // namespace opennova::editor
