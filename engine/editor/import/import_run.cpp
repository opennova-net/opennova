#include <editor/import/import_run.h>

#include <filesystem>
#include <set>

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

ExternalChanges external_changes(const ProjectPaths &paths, const AssetScan &scan,
                                 const std::vector<ImportedSource> &imports, int64_t now) {
	ExternalChanges out;
	// Whether the scan's entry moved on disk; `gone` when the file is not there.
	enum class Moved { No, Settled, Unsettled, Gone };
	const auto moved = [&](const AssetEntry &entry) {
		const fs::path file = system_path(join_path(paths.root, entry.relative_path));
		std::error_code ec;
		const uint64_t size = uint64_t(fs::file_size(file, ec));
		if (ec) return Moved::Gone;
		const int64_t modified = io::file_modified_ticks(file);
		if (size == entry.size_bytes && modified == entry.modified_ticks) return Moved::No;
		return io::file_stamp_settled(modified, now) ? Moved::Settled : Moved::Unsettled;
	};
	std::set<std::string> files, sources, waiting;
	const auto take = [&](const AssetEntry &entry, bool gone_counts) {
		switch (moved(entry)) {
		case Moved::No: return false;
		case Moved::Unsettled: waiting.insert(entry.relative_path); return false;
		case Moved::Gone: if (!gone_counts) return false; break;
		case Moved::Settled: break;
		}
		files.insert(entry.relative_path);
		return true;
	};
	for (const AssetEntry &entry : scan.entries) {
		// A source a program saved (one gone is none: the next scan says so).
		if (entry.kind == AssetKind::ImportSource) {
			if (take(entry, false)) sources.insert(entry.relative_path);
			continue;
		}
		// A PNG the game reads as it is, edited in place.
		if (entry.kind == AssetKind::Texture && entry.imported_from.empty() &&
		    strutil::ends_with_icase(entry.relative_path, ".png"))
			take(entry, true);
	}
	// A file an import read (an input): its source imported again (one gone too: the import says so).
	for (const ImportedSource &source : imports)
		for (const std::string &input : source.inputs)
			if (const AssetEntry *entry = scan.at_path(input))
				if (files.count(input) || (entry->kind != AssetKind::ImportSource && take(*entry, true))) sources.insert(source.source);
	out.sources.assign(sources.begin(), sources.end());
	out.files.assign(files.begin(), files.end());
	out.unsettled = waiting.size();
	return out;
}

} // namespace opennova::editor
