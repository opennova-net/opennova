#include <editor/assets/asset_import.h>

#include <filesystem>
#include <set>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

std::string ImportSource::name() const {
	return entry.empty() ? fs::path(path).filename().string() : entry;
}

std::vector<ImportSource> list_import_sources(const std::vector<std::string> &paths,
                                             std::vector<Diagnostic> &diagnostics) {
	std::vector<ImportSource> sources;
	for (const std::string &path : paths) {
		std::error_code ec;
		if (!fs::is_regular_file(path, ec)) {
			diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.source",
			                                     "File not found: " + path));
		} else if (strutil::ends_with_icase(path, ".pff")) {
			Vfs archive;
			if (!archive.set_primary_archive(path)) {
				diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.archive",
				                                     "Could not open archive: " + path));
				continue;
			}
			for (const auto &file : archive.list_files()) sources.push_back({path, file.logical_name});
		} else {
			sources.push_back({path, {}});
		}
	}
	return sources;
}

ImportResult import_assets(const std::vector<ImportSource> &sources, const ProjectPaths &paths,
                           const ProjectDocument &document, bool replace_existing) {
	ImportResult result;
	const AssetScan existing = scan_project_assets(paths, document);
	std::set<std::string> selected_names;
	Vfs archive;
	archive.set_scr_policy(gameprofile::gameprofile_scr_policy_for_code(document.target_game.c_str()));
	std::string archive_path;
	for (const ImportSource &source : sources) {
		const std::string name = source.name();
		const auto fail = [&](const char *code, const std::string &message) {
			result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, code, message, name));
		};
		if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) {
			fail("import.name", "Import requires a plain file name: " + name);
			continue;
		}
		const std::string key = normalized_logical_name(name);
		if (!selected_names.insert(key).second) {
			fail("import.duplicate", "More than one selected file has the name " + name + ".");
			continue;
		}
		const AssetEntry *old = existing.find(name);
		if (old && !replace_existing) {
			fail("import.exists", name + " already exists; select Replace existing files to replace it.");
			continue;
		}
		std::vector<uint8_t> bytes;
		std::string io_error;
		if (source.entry.empty()) {
			if (!read_file_bytes(source.path, bytes, io_error)) {
				fail("import.read", io_error);
				continue;
			}
		} else {
			if (archive_path != source.path) {
				archive.clear();
				archive_path.clear();
				if (!archive.set_primary_archive(source.path)) {
					fail("import.archive", "Could not open archive: " + source.path);
					continue;
				}
				archive_path = source.path;
			}
			if (!archive.read_file(source.entry, bytes)) {
				fail("import.read", "Could not read " + name + " from " + source.path);
				continue;
			}
		}
		const AssetKind kind = classify_asset(name, &bytes);
		if (kind == AssetKind::Unknown || kind == AssetKind::Archive) {
			fail("import.kind", "Unsupported asset type: " + name);
			continue;
		}
		const std::string relative = old ? old->relative_path :
		        (fs::path(blank_placement_dir(kind)) / name).generic_string();
		const fs::path destination = fs::path(paths.root) / relative;
		std::error_code ec;
		const fs::path root = fs::weakly_canonical(paths.root, ec);
		const fs::path resolved = fs::weakly_canonical(destination, ec);
		const fs::path within = resolved.lexically_relative(root);
		if (ec || within.empty() || within.is_absolute() || *within.begin() == "..") {
			fail("import.path", "The destination must stay inside the project: " + relative);
			continue;
		}
		if (!ensure_directory(destination.parent_path().generic_string(), io_error) ||
		    !write_file_atomic(destination.generic_string(), bytes.data(), bytes.size(), io_error)) {
			fail("import.write", io_error);
			continue;
		}
		result.imported.push_back(relative);
	}
	return result;
}

} // namespace opennova::editor
