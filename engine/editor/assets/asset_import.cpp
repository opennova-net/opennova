#include <editor/assets/asset_import.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <set>
#include <system_error>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/graph/asset_graph.h>
#include <editor/import/converter.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
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

bool mount_retail(Vfs &game, const std::string &retail_root, const ProjectDocument &document) {
	LaunchFlags stock; // no /d, no expansion
	stock.game = document.target_game;
	return mount_install(game, retail_root, stock);
}

std::vector<ImportSource> list_retail_import_sources(const std::string &retail_root, const ProjectDocument &document,
                                                    std::vector<Diagnostic> &diagnostics) {
	std::vector<ImportSource> sources;
	if (retail_root.empty()) {
		diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.retail",
		                                     "Choose the game install folder in File > Project settings... first."));
		return sources;
	}
	Vfs game;
	if (!mount_retail(game, retail_root, document)) {
		diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.retail",
		                                     "No game archives found under " + retail_root + "."));
		return sources;
	}
	for (const VfsFileLocation &file : game.list_files()) {
		if (strutil::ends_with_icase(file.logical_name, ".pff")) continue; // the archives themselves
		ImportSource source;
		source.path = retail_root;
		source.entry = file.logical_name;
		source.retail = true;
		sources.push_back(std::move(source));
	}
	return sources;
}

std::vector<std::string> list_retail_file_names(const std::string &retail_root, const ProjectDocument &document) {
	std::vector<std::string> names;
	Vfs game;
	if (retail_root.empty() || !mount_retail(game, retail_root, document)) return names;
	for (const VfsFileLocation &file : game.list_files())
		if (!strutil::ends_with_icase(file.logical_name, ".pff")) names.push_back(file.logical_name);
	std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
		return normalized_logical_name(a) < normalized_logical_name(b);
	});
	return names;
}

std::string import_destination(const AssetScan &existing, const std::string &name, AssetKind kind) {
	if (const AssetEntry *prior = existing.find(name)) return prior->relative_path;
	return (fs::path(blank_placement_dir(kind)) / name).generic_string();
}

namespace {

// One file the import writes: its logical name, what it is to the engine once copied (the
// game's own PNG a texture), where it goes (project-relative), its bytes, the source it is
// made from when a converter made it, and the importer whose import record goes beside it
// (an author's loose file an importer converts, with no record there yet).
struct Output {
	std::string name;
	AssetKind kind = AssetKind::Unknown;
	std::string relative;
	std::vector<uint8_t> bytes;
	std::string made_from;
	const Importer *record = nullptr;
};

// The folders from `dir` up that do not exist yet, the outermost first: what making `dir`
// creates.
std::vector<fs::path> missing_folders(fs::path dir) {
	std::vector<fs::path> missing;
	std::error_code ec;
	while (!dir.empty() && !fs::exists(dir, ec)) {
		missing.push_back(dir);
		const fs::path parent = dir.parent_path();
		if (parent == dir) break;
		dir = parent;
	}
	std::reverse(missing.begin(), missing.end());
	return missing;
}

// The import's staging folder with what is left in it (the staging folder too, when no other
// import stages there), and the destinations' folders the import made while they are empty.
void remove_stage(const fs::path &stage, const std::vector<fs::path> &folders) {
	std::error_code ec;
	fs::remove_all(stage, ec);
	fs::remove(stage.parent_path(), ec);
	for (auto it = folders.rbegin(); it != folders.rend(); ++it) fs::remove(*it, ec);
}

// The textures a converter's model names that the import does not bring and the project
// does not have, looked up by the loader's own rule (reference_file_candidates): the files an
// .o3d's materials name come with it only through its plan (the import dialog's "Include
// the files these need", the command line's --with-dependencies), so each is a warning
// saying so. A row whose loader opens no file wants none.
void report_textures_left(const std::vector<Output> &outputs, const AssetScan &existing, const ProjectDocument &document,
                          std::vector<Diagnostic> &out) {
	for (const Output &output : outputs) {
		if (output.made_from.empty() || output.kind != AssetKind::Model) continue;
		Extracted content;
		Diagnostic error;
		if (!extract_from_bytes(output.name, output.kind, output.bytes, document.target_game, content, error)) continue;
		std::set<std::string> told;
		for (const GraphEdge &edge : content.edges) {
			if (edge.kind != ReferenceKind::Texture) continue;
			const auto exists = [&](const std::string &name) {
				const AssetEntry *held = existing.find(name);
				if (held && file_serves_reference(held->kind, edge.kind, edge.material_type)) return true;
				for (const Output &other : outputs)
					if (normalized_logical_name(other.name) == normalized_logical_name(name) &&
					    file_serves_reference(other.kind, edge.kind, edge.material_type))
						return true;
				return false;
			};
			const std::vector<std::string> candidates = reference_file_candidates(edge.kind, edge.value, edge.material_type, exists);
			if (candidates.empty() || std::any_of(candidates.begin(), candidates.end(), exists)) continue;
			if (!told.insert(normalized_logical_name(edge.value)).second) continue;
			Diagnostic d = make_diagnostic(
			        DiagnosticSeverity::Warning, "import.texture_not_imported",
			        output.name + " names the texture " + edge.value +
			                ", which the import does not bring and the project does not have: import " + output.made_from +
			                " with the files it needs (the import dialog's \"Include the files these need\", or "
			                "--with-dependencies on the command line) to bring the textures found beside it.",
			        output.name);
			// What it is about, as the graph's missing reference says it: its fixes read it.
			d.reference = edge.kind;
			d.target = edge.value;
			d.material_type = edge.material_type;
			out.push_back(std::move(d));
		}
	}
}

} // namespace

ImportResult import_assets(const std::vector<ImportSource> &sources, const ProjectPaths &paths,
                           const ProjectDocument &document, bool replace_existing) {
	ImportResult result;
	// A staging folder a crash left behind (under the cache: never scanned, never packed) goes
	// before this import stages its own.
	std::error_code cleaned;
	fs::remove_all(paths.staging_dir, cleaned);
	const AssetScan existing = scan_project_assets(paths, document);
	std::set<std::string> selected_names;
	Vfs archive;
	archive.set_scr_policy(gameprofile::gameprofile_scr_policy_for_code(document.target_game.c_str()));
	std::string archive_path;
	Vfs retail;
	std::string retail_root;
	std::vector<Output> outputs;
	bool refused = false;
	const auto refuse = [&](const std::string &code, const std::string &message, const std::string &name) {
		result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, code, message, name));
		refused = true;
	};
	// Every source read, and every file it makes checked, before anything is written.
	for (const ImportSource &source : sources) {
		const std::string name = source.name();
		std::string problem, message;
		if (!check_project_file_name(paths.root, std::string(), name, AssetKind::Unknown, problem, message)) {
			refuse("import." + problem, message, name);
			continue;
		}
		const std::string key = normalized_logical_name(name);
		if (!selected_names.insert(key).second) {
			refuse("import.duplicate", "More than one selected file has the name " + name + ".", name);
			continue;
		}
		const AssetEntry *old = existing.find(name);
		if (old && !replace_existing) {
			refuse("import.exists", name + " already exists; select Replace existing files to replace it.", name);
			continue;
		}
		std::vector<uint8_t> bytes;
		std::string io_error;
		if (source.retail) {
			if (retail_root != source.path) {
				retail_root.clear();
				if (!mount_retail(retail, source.path, document)) {
					refuse("import.retail", "No game archives found under " + source.path + ".", name);
					continue;
				}
				retail_root = source.path;
			}
			if (!retail.read_file(source.entry, bytes)) {
				refuse("import.read", "The game data has no file named " + name + ".", name);
				continue;
			}
		} else if (source.entry.empty()) {
			if (!read_file_bytes(source.path, bytes, io_error)) {
				refuse("import.read", io_error, name);
				continue;
			}
		} else {
			if (archive_path != source.path) {
				archive.clear();
				archive_path.clear();
				if (!archive.set_primary_archive(source.path)) {
					refuse("import.archive", "Could not open archive: " + source.path, name);
					continue;
				}
				archive_path = source.path;
			}
			if (!archive.read_file(source.entry, bytes)) {
				refuse("import.read", "Could not read " + name + " from " + source.path, name);
				continue;
			}
		}
		// What the source becomes: a converter's outputs (the source is not kept), or
		// the file itself.
		std::vector<ImportOutput> made;
		const Converter *converter = converter_for(name);
		if (converter) {
			ImportProduct product;
			converter->run(name, bytes, product);
			bool broken = false;
			for (Diagnostic &d : product.diagnostics) {
				broken = broken || d.severity == DiagnosticSeverity::Error;
				result.diagnostics.push_back(std::move(d));
			}
			if (broken) {
				refused = true;
				continue;
			}
			made = std::move(product.outputs);
		} else {
			made.push_back({name, std::move(bytes)});
		}
		// A loose file from the disk is an author's source; a file of the game install or an
		// archive, or one a source marks native, is the game's own, copied as the game reads it.
		const bool authored = !source.retail && source.entry.empty() && !source.native;
		for (ImportOutput &output : made) {
			if (output.name != name) {
				if (!check_project_file_name(paths.root, std::string(), output.name, AssetKind::Unknown, problem, message)) {
					refuse("import." + problem, message, output.name);
					continue;
				}
				if (!selected_names.insert(normalized_logical_name(output.name)).second) {
					refuse("import.duplicate", "More than one selected file makes " + output.name + ".", output.name);
					continue;
				}
			}
			const AssetKind kind = classify_asset(output.name, &output.bytes);
			if (kind == AssetKind::Unknown || kind == AssetKind::Archive) {
				refuse("import.kind", "Unsupported asset type: " + output.name, output.name);
				continue;
			}
			// A file the project holds with the same bytes is left as it is unless the
			// import replaces.
			const AssetEntry *prior = existing.find(output.name);
			if (prior && !replace_existing) {
				std::vector<uint8_t> held;
				if (read_file_bytes((fs::path(paths.root) / prior->relative_path).generic_string(), held, io_error) &&
				    held == output.bytes)
					continue;
				refuse("import.exists", output.name + " already exists; select Replace existing files to replace it.",
				       output.name);
				continue;
			}
			const std::string relative = import_destination(existing, output.name, kind);
			if (!check_project_file_name(paths.root, fs::path(relative).parent_path().generic_string(), output.name, kind,
			                             problem, message)) {
				refuse("import." + problem, message, output.name);
				continue;
			}
			Output planned;
			planned.name = output.name;
			// The game's own PNG gets no record: it is the texture the game loads as it is.
			planned.kind = kind == AssetKind::ImageSource && !authored ? AssetKind::Texture : kind;
			planned.relative = relative;
			planned.bytes = std::move(output.bytes);
			planned.made_from = converter ? name : std::string();
			// Importing an author's file an importer converts makes it an import source: its
			// record, from the importer's defaults, is written with it (a record there stays).
			// Where the record goes must take a file.
			if (const Importer *importer = authored ? importer_for(output.name) : nullptr) {
				const fs::path record = fs::path(paths.root) / (relative + kImportSidecarSuffix);
				std::error_code ec;
				if (fs::exists(record, ec) && !fs::is_regular_file(record, ec)) {
					refuse("import.record",
					       relative + kImportSidecarSuffix + " is not a file: the import record of " + output.name +
					               " cannot be written there.",
					       output.name);
					continue;
				}
				if (!fs::exists(record, ec)) planned.record = importer;
			}
			outputs.push_back(std::move(planned));
		}
	}
	if (refused) return result;
	report_textures_left(outputs, existing, document, result.diagnostics);

	// Staged under the cache (an import's own folder, on the project's volume, so a publish is
	// a rename), each import record with its file; the destinations' folders made. A failure
	// takes every staged file and made folder back.
	std::string io_error;
	if (!ensure_project_cache_dir(paths, io_error)) {
		result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.write",
		                                             io_error + ". Nothing was imported."));
		return result;
	}
	const fs::path stage = fs::path(paths.staging_dir) /
	                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	std::vector<std::string> staged;  // the staged files, in the order they publish
	std::vector<fs::path> folders;    // the destinations' folders the import made
	std::vector<std::string> records; // each output's staged record ("" for none)
	const auto stage_failed = [&](const std::string &what, const std::string &error, const std::string &name) {
		remove_stage(stage, folders);
		result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.write",
		                                             "Could not write " + what + ": " + error + ". Nothing was imported.",
		                                             name));
	};
	if (!ensure_directory(stage.generic_string(), io_error)) {
		stage_failed(stage.generic_string(), io_error, std::string());
		return result;
	}
	for (size_t i = 0; i < outputs.size(); ++i) {
		const Output &output = outputs[i];
		const fs::path destination = fs::path(paths.root) / output.relative;
		for (fs::path &folder : missing_folders(destination.parent_path())) folders.push_back(std::move(folder));
		const std::string file = (stage / std::to_string(i)).generic_string();
		if (!ensure_directory(destination.parent_path().generic_string(), io_error) ||
		    !write_file_atomic(file, output.bytes.data(), output.bytes.size(), io_error)) {
			stage_failed(output.relative, io_error, output.name);
			return result;
		}
		staged.push_back(file);
		records.emplace_back();
		if (output.record) {
			ImportSidecar sidecar;
			sidecar.importer = output.record->id;
			sidecar.version = output.record->version;
			sidecar.options = output.record->default_options;
			const std::string record = file + kImportSidecarSuffix;
			Diagnostic error;
			if (!save_import_sidecar(record, sidecar, error)) {
				stage_failed(output.relative + kImportSidecarSuffix, error.message, output.name);
				return result;
			}
			records.back() = record;
		}
	}

	// Published in order, each file after its import record, by renames. A failure stops it:
	// the record of a file that did not publish goes with it (it was written for it), the rest
	// is not published, each said.
	for (size_t i = 0; i < outputs.size(); ++i) {
		const Output &output = outputs[i];
		const fs::path destination = fs::path(paths.root) / output.relative;
		const fs::path record = fs::path(destination.generic_string() + kImportSidecarSuffix);
		std::error_code ec;
		std::string failed;
		if (!records[i].empty()) {
			fs::rename(records[i], record, ec);
			if (ec) failed = output.relative + kImportSidecarSuffix;
		}
		if (failed.empty()) {
			fs::rename(staged[i], destination, ec);
			if (ec) {
				failed = output.relative;
				std::error_code ignored;
				if (!records[i].empty()) fs::remove(record, ignored);
			}
		}
		if (failed.empty()) {
			result.imported.push_back(output.relative);
			continue;
		}
		remove_stage(stage, folders);
		result.diagnostics.push_back(make_diagnostic(
		        DiagnosticSeverity::Error, "import.publish",
		        "Could not write " + failed + ": " + ec.message() + ". The import stopped there: " + std::to_string(i) +
		                " of " + std::to_string(outputs.size()) + " files were imported.",
		        output.name));
		for (size_t rest = i; rest < outputs.size(); ++rest) {
			result.not_imported.push_back(outputs[rest].relative);
			if (rest > i)
				result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Warning, "import.not_published",
				                                             outputs[rest].relative + " was not imported: the import stopped at " +
				                                                     output.relative + ".",
				                                             outputs[rest].name));
		}
		return result;
	}
	remove_stage(stage, folders); // empty now; the folders made hold their files
	return result;
}

} // namespace opennova::editor
