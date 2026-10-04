#include <editor/import/import_run.h>

#include <filesystem>

#include <base/io/file_time.h>
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
	const fs::path dir = path_of(paths.imported_dir).lexically_relative(path_of(paths.root)) / io::hex64(hash);
	return utf8_of(dir);
}

ImportRunResult run_imports(const ProjectPaths &paths, const ProjectDocument &project, bool force, const std::string &only) {
	ImportPass pass(paths, project, force, only);
	while (!pass.step(kWholeWalkStep)) {
	}
	return pass.take();
}

std::vector<std::string> changed_import_sources(const ProjectPaths &paths, const AssetScan &scan) {
	std::vector<std::string> out;
	for (const AssetEntry &entry : scan.entries) {
		if (entry.kind != AssetKind::ImportSource) continue;
		const fs::path file = system_path(join_path(paths.root, entry.relative_path));
		std::error_code ec;
		const uint64_t size = uint64_t(fs::file_size(file, ec));
		if (ec) continue;
		if (size != entry.size_bytes || io::file_modified_ticks(file) != entry.modified_ticks) out.push_back(entry.relative_path);
	}
	return out;
}

} // namespace opennova::editor
