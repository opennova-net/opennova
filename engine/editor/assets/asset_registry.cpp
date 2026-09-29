#include <editor/assets/asset_registry.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <base/io/file_time.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/import/import_run.h>
#include <editor/import/sidecar.h>
#include <editor/project/project_files.h>
#include <formats/pff/pff.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

bool same_path(const fs::path &a, const fs::path &b) {
	std::error_code ec;
	const fs::path ca = fs::weakly_canonical(a, ec);
	if (ec) return false;
	const fs::path cb = fs::weakly_canonical(b, ec);
	if (ec) return false;
	return ca == cb;
}

} // namespace

std::string normalized_logical_name(std::string_view name) {
	char buf[256];
	pff::pff_norm_name(name.data(), name.size(), buf, sizeof(buf));
	return std::string(buf);
}

bool logical_name_fits_archive(std::string_view name) {
	if (name.size() > static_cast<size_t>(pff::PFF_NAME_SIZE)) return false;
	return !normalized_logical_name(name).empty();
}

const AssetEntry *AssetScan::find(std::string_view logical_name) const {
	const std::string key = normalized_logical_name(logical_name);
	for (const AssetEntry &entry : entries) {
		if (normalized_logical_name(entry.logical_name) == key) return &entry;
	}
	return nullptr;
}

AssetScan scan_project_assets(const ProjectPaths &paths, const ProjectDocument &doc) {
	AssetScan scan;
	const fs::path root(paths.root);
	const fs::path export_dir(paths.export_dir(doc));
	const std::string sidecar_suffix = kImportSidecarSuffix;

	std::error_code ec;
	fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
	if (ec) {
		scan.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "project.root.unreadable",
		                                           "Cannot read the project directory " + paths.root +
		                                                   ": " + ec.message()));
		return scan;
	}
	const fs::recursive_directory_iterator end;
	for (; it != end; it.increment(ec)) {
		if (ec) break;
		const fs::directory_entry &entry = *it;
		const fs::path &path = entry.path();
		if (entry.is_directory(ec)) {
			if (is_dot_directory(path) || same_path(path, export_dir)) it.disable_recursion_pending();
			continue;
		}
		if (!entry.is_regular_file(ec)) continue;
		const std::string filename = path.filename().string();
		if (path.parent_path() == root && filename == kProjectFileName) continue;
		if (strutil::ends_with_icase(filename, sidecar_suffix)) {
			// An import record: its outputs are project files that live under the cache.
			std::string sidecar_relative = fs::relative(path, root, ec).generic_string();
			if (ec) sidecar_relative = filename;
			const std::string source_relative = sidecar_relative.substr(0, sidecar_relative.size() - sidecar_suffix.size());
			// A record whose source is gone lists nothing: its outputs would otherwise
			// outlive the source and still pack.
			if (!fs::is_regular_file(root / source_relative, ec)) {
				scan.diagnostics.push_back(make_diagnostic(
				        DiagnosticSeverity::Warning, "import.orphan_record",
				        "The import record names " + source_relative + ", which is not in the project: delete the record.",
				        sidecar_relative));
				continue;
			}
			ImportSidecar sidecar;
			Diagnostic error;
			if (!load_import_sidecar(path.generic_string(), sidecar, error)) {
				if (!error.code.empty()) {
					error.asset = sidecar_relative;
					scan.diagnostics.push_back(error);
				}
				continue;
			}
			const std::string output_dir = import_output_dir(paths, source_relative);
			for (const std::string &output : sidecar.outputs) {
				const fs::path output_path = root / output_dir / output;
				if (!fs::is_regular_file(output_path, ec)) {
					// Only the import pass makes outputs: one missing after it ran means the
					// last import did not finish (its finding says why).
					scan.diagnostics.push_back(make_diagnostic(
					        DiagnosticSeverity::Warning, "import.output_missing",
					        output + " (imported from " + source_relative + ") has not been made: the game will not see it.",
					        source_relative));
					continue;
				}
				AssetEntry produced;
				produced.logical_name = output;
				produced.relative_path = (fs::path(output_dir) / output).generic_string();
				produced.imported_from = source_relative;
				const auto produced_size = fs::file_size(output_path, ec);
				if (!ec) produced.size_bytes = static_cast<uint64_t>(produced_size);
				produced.modified_ticks = io::file_modified_ticks(output_path);
				std::vector<uint8_t> bytes;
				std::string io_error;
				produced.kind = asset_classification_needs_bytes(output) && read_file_bytes(output_path.string(), bytes, io_error)
				                        ? classify_asset(output, &bytes)
				                        : classify_asset(output, nullptr);
				scan.entries.push_back(std::move(produced));
			}
			continue;
		}

		AssetEntry asset;
		asset.logical_name = filename;
		asset.relative_path = fs::relative(path, root, ec).generic_string();
		if (ec) asset.relative_path = filename;
		const auto size = fs::file_size(path, ec);
		if (!ec) asset.size_bytes = static_cast<uint64_t>(size);
		asset.modified_ticks = io::file_modified_ticks(path);
		if (asset_classification_needs_bytes(filename)) {
			std::vector<uint8_t> bytes;
			std::string io_error;
			if (read_file_bytes(path.string(), bytes, io_error)) {
				asset.kind = classify_asset(filename, &bytes);
			} else {
				asset.kind = classify_asset(filename, nullptr);
				scan.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Warning,
				                                           "asset.unreadable", io_error,
				                                           asset.relative_path));
			}
		} else {
			asset.kind = classify_asset(filename, nullptr);
		}
		// A PNG is an import source only when its `.import` record says so; one
		// without is a texture the game loads as it is (retail's menu loader
		// decodes .png [orig: CTextureManager_LoadOrFindTexture @ 0x654980 ->
		// load_png_from_file @ 0x6654d0]).
		if (asset.kind == AssetKind::ImageSource &&
		    !fs::is_regular_file(fs::path(path.generic_string() + sidecar_suffix), ec))
			asset.kind = AssetKind::Texture;
		scan.entries.push_back(std::move(asset));
	}

	std::sort(scan.entries.begin(), scan.entries.end(), [](const AssetEntry &a, const AssetEntry &b) {
		const std::string ka = normalized_logical_name(a.logical_name);
		const std::string kb = normalized_logical_name(b.logical_name);
		if (ka != kb) return ka < kb;
		return a.relative_path < b.relative_path;
	});

	for (size_t i = 0; i < scan.entries.size(); ++i) {
		const AssetEntry &asset = scan.entries[i];
		if (asset.logical_name.size() > static_cast<size_t>(pff::PFF_NAME_SIZE)) {
			scan.diagnostics.push_back(make_diagnostic(
			        DiagnosticSeverity::Error, "asset.name.too_long",
			        "The file name " + asset.logical_name + " is longer than " +
			                std::to_string(pff::PFF_NAME_SIZE) +
			                " characters; the game cannot store it in an archive.",
			        asset.relative_path));
		} else if (normalized_logical_name(asset.logical_name).empty()) {
			scan.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "asset.name.empty",
			                                           "The file name is blank once normalized.",
			                                           asset.relative_path));
		}
		if (i > 0 && normalized_logical_name(scan.entries[i - 1].logical_name) ==
		                     normalized_logical_name(asset.logical_name)) {
			scan.diagnostics.push_back(make_diagnostic(
			        DiagnosticSeverity::Error, "asset.name.duplicate",
			        "Two files share the name " + asset.logical_name + " (" +
			                scan.entries[i - 1].relative_path + " and " + asset.relative_path +
			                "); the game resolves names without folders, so only one can exist.",
			        asset.relative_path));
		}
		if (asset.kind == AssetKind::Unknown) {
			scan.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Warning, "asset.kind.unknown",
			                                           "The game does not use files of this type.",
			                                           asset.relative_path));
		}
	}
	return scan;
}

} // namespace opennova::editor
