#include <editor/assets/asset_import.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/assets/player_files.h>
#include <editor/assets/project_scan.h>
#include <editor/graph/asset_graph.h>
#include <editor/import/converter.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

std::vector<ImportChoice> list_import_choices(const std::vector<std::string> &paths,
                                             std::vector<Diagnostic> &diagnostics) {
	std::vector<ImportChoice> sources;
	for (const std::string &path : paths) {
		std::error_code ec;
		if (!fs::is_regular_file(system_path(path), ec)) {
			diagnostics.push_back(make_finding(CoreFinding::ImportNotFound, DiagnosticSeverity::Error,
			                                  "File not found: " + path));
		} else if (strutil::ends_with_icase(path, ".pff")) {
			Vfs archive;
			if (!archive.set_primary_archive(path)) {
				diagnostics.push_back(make_finding(CoreFinding::ImportArchive, DiagnosticSeverity::Error,
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

bool read_served(const Vfs &game, const std::string &name, std::vector<uint8_t> &out) {
	if (asset_kind_row(classify_asset(name, nullptr)).scr == ScrForm::Shader) return game.read_file_raw(name, out);
	return game.read_file(name, out);
}

bool install_loose_kind(AssetKind kind) {
	switch (kind) {
	case AssetKind::MusicBank:
	case AssetKind::Video:
	case AssetKind::CountryCode:
	case AssetKind::StringTableCoo:
	case AssetKind::NovaWorldScreen: return true;
	default: return false;
	}
}

std::vector<std::string> list_install_loose_files(const std::string &retail_root) {
	std::vector<std::string> names;
	std::error_code ec;
	fs::directory_iterator it(system_path(retail_root), ec);
	for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
		std::error_code status;
		if (!it->is_regular_file(status)) continue;
		// Its name as UTF-8, whatever the code page (project_files.h, utf8_of).
		const std::string name = utf8_of(it->path().filename());
		// The kinds the game ships loose, never a player's own file beside them.
		if (install_loose_kind(classify_asset(name, nullptr)) && !is_player_file(name)) names.push_back(name);
	}
	std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
		return normalized_logical_name(a) < normalized_logical_name(b);
	});
	return names;
}

bool read_install_file(const Vfs &game, const std::string &retail_root, const std::string &name,
                       std::vector<uint8_t> &out) {
	if (read_served(game, name, out)) return true;
	if (!install_loose_kind(classify_asset(name, nullptr))) return false;
	const std::string wanted = normalized_logical_name(name);
	for (const std::string &loose : list_install_loose_files(retail_root)) {
		if (normalized_logical_name(loose) != wanted) continue;
		std::string error;
		return read_file_bytes(join_path(retail_root, loose), out, error);
	}
	return false;
}

namespace {

// The game install's files by their logical names: the archives' (as a stock launch mounts them,
// the archives themselves left out), then the root's loose files of the kinds the game ships loose
// where no archive has the name. False when the folder holds none of the game's archives.
bool list_install_names(const std::string &retail_root, const ProjectDocument &document, std::vector<std::string> &names) {
	Vfs game;
	if (retail_root.empty() || !mount_retail(game, retail_root, document)) return false;
	std::set<std::string> known;
	for (const VfsFileLocation &file : game.list_files()) {
		if (strutil::ends_with_icase(file.logical_name, ".pff")) continue; // the archives themselves
		if (is_player_file(file.logical_name)) continue;                   // never the game's
		names.push_back(file.logical_name);
		known.insert(normalized_logical_name(file.logical_name));
	}
	for (const std::string &loose : list_install_loose_files(retail_root))
		if (known.insert(normalized_logical_name(loose)).second) names.push_back(loose);
	return true;
}

} // namespace

std::vector<ImportChoice> list_retail_import_choices(const std::string &retail_root, const ProjectDocument &document,
                                                    std::vector<Diagnostic> &diagnostics) {
	std::vector<ImportChoice> sources;
	if (retail_root.empty()) {
		diagnostics.push_back(make_finding(CoreFinding::ImportInstall, DiagnosticSeverity::Error,
		                                  "Choose the game install folder in File > Project settings... first."));
		return sources;
	}
	std::vector<std::string> names;
	if (!list_install_names(retail_root, document, names)) {
		diagnostics.push_back(make_finding(CoreFinding::ImportInstall, DiagnosticSeverity::Error,
		                                  "No game archives found under " + retail_root + "."));
		return sources;
	}
	for (const std::string &name : names) {
		ImportChoice source;
		source.path = retail_root;
		source.entry = name;
		source.install = true;
		sources.push_back(std::move(source));
	}
	return sources;
}

std::vector<std::string> list_retail_file_names(const std::string &retail_root, const ProjectDocument &document) {
	std::vector<std::string> names;
	list_install_names(retail_root, document, names);
	std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
		return normalized_logical_name(a) < normalized_logical_name(b);
	});
	return names;
}

std::string import_destination(const AssetScan &existing, const std::string &name, AssetKind kind) {
	if (const AssetEntry *prior = existing.find(name)) return prior->relative_path;
	return join_path(asset_kind_row(kind).folder, name);
}

namespace {

// One file the import writes: its logical name, what it is to the engine once copied (the
// game's own PNG a texture), where it goes (project-relative), where it is staged and its
// staged import record ("" for none), the source it is made from when a converter made it, and
// the texture rows a converter's model names (report_textures_left). Its bytes are not kept: a
// file is staged as it is checked.
struct Output {
	std::string name;
	AssetKind kind = AssetKind::Unknown;
	std::string relative;
	std::string staged;
	std::string staged_record;
	std::string made_from;
	std::vector<GraphEdge> textures;
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
	if (!stage.empty()) {
		fs::remove_all(stage, ec);
		fs::remove(stage.parent_path(), ec);
	}
	for (auto it = folders.rbegin(); it != folders.rend(); ++it) fs::remove(*it, ec);
}

// The textures a converter's model names that the import does not bring and the project
// does not have, looked up by the loader's own rule (reference_file_candidates): the files an
// .o3d's materials name come with it only through its plan (the import dialog's "Include
// the files these need", the command line's --with-dependencies), so each is a warning
// saying so. A row whose loader opens no file wants none.
void report_textures_left(const std::vector<Output> &outputs, const AssetScan &existing, std::vector<Diagnostic> &out) {
	// The import's own files by name: what it brings.
	std::map<std::string, AssetKind> brought;
	for (const Output &output : outputs) brought.emplace(normalized_logical_name(output.name), output.kind);
	for (const Output &output : outputs) {
		std::set<std::string> told;
		for (const GraphEdge &edge : output.textures) {
			const auto exists = [&](const std::string &name) {
				const AssetEntry *held = existing.find(name);
				if (held && file_serves_reference(held->kind, edge.kind, edge.loader_arg)) return true;
				const auto other = brought.find(normalized_logical_name(name));
				return other != brought.end() && file_serves_reference(other->second, edge.kind, edge.loader_arg);
			};
			const std::vector<std::string> candidates = reference_file_candidates(edge.kind, edge.value, edge.loader_arg, exists);
			if (candidates.empty() || std::any_of(candidates.begin(), candidates.end(), exists)) continue;
			if (!told.insert(normalized_logical_name(edge.value)).second) continue;
			Diagnostic d = make_finding(
			        CoreFinding::ImportTextureNotImported, DiagnosticSeverity::Warning,
			        output.name + " names the texture " + edge.value +
			                ", which the import does not bring and the project does not have: import " + output.made_from +
			                " with the files it needs (the import dialog's \"Include the files these need\", or "
			                "--with-dependencies on the command line) to bring the textures found beside it.",
			        output.name);
			// What it is about, as the graph's missing reference says it: its fixes read it.
			d.subject = ReferenceSubject{ edge.kind, edge.value, std::string(), edge.loader_arg };
			out.push_back(std::move(d));
		}
	}
}

// What one source or file costs a step besides its bytes: its checks resolve its place on the
// disk (check_project_file_name), a publish renames it.
constexpr uint64_t kItemCost = 4096;

} // namespace

class AssetImport::Run {
public:
	Run(std::vector<ImportChoice> sources, const ProjectPaths &paths, const ProjectDocument &document, bool replace_existing)
	    : sources_(std::move(sources)), paths_(paths), document_(document), replace_existing_(replace_existing),
	      walk_(paths_, document_) {
		archive_.set_scr_policy(gameprofile::gameprofile_scr_policy_for_code(document_.target_game.c_str()));
	}

	bool step(uint64_t bytes) {
		cost_ = 0;
		for (size_t items = 0; phase_ != Phase::Done && (items == 0 || cost_ < bytes); ++items) {
			cost_ += kItemCost;
			switch (phase_) {
			case Phase::Scan: {
				// A staging folder a crash left behind (under the cache: never scanned, never packed)
				// goes before this import stages its own.
				if (!cleaned_) {
					std::error_code cleaned;
					fs::remove_all(system_path(paths_.staging_dir), cleaned);
					cleaned_ = true;
				}
				// The project's files as they are now, the step's bytes its own.
				if (!walk_.step(bytes)) return false;
				existing_ = walk_.take();
				phase_ = Phase::Check;
				return false;
			}
			case Phase::Check:
				if (next_source_ < sources_.size() && !failed_) {
					check(sources_[next_source_++]);
					break;
				}
				if (failed_) {
					phase_ = Phase::Done;
					break;
				}
				// A refusal writes nothing: what was staged before it goes, the folders made too.
				if (refused_) {
					remove_stage(stage_, folders_);
					phase_ = Phase::Done;
					break;
				}
				report_textures_left(outputs_, existing_, result_.diagnostics);
				phase_ = Phase::Publish;
				break;
			case Phase::Publish:
				if (next_output_ < outputs_.size()) {
					publish(next_output_);
					break;
				}
				remove_stage(stage_, folders_); // empty now; the folders made hold their files
				phase_ = Phase::Done;
				break;
			case Phase::Done: break;
			}
		}
		return phase_ == Phase::Done;
	}

	bool done() const { return phase_ == Phase::Done; }
	bool publishing() const { return published_any_; }
	void abandon() {
		if (published_any_ || phase_ == Phase::Done) return;
		remove_stage(stage_, folders_);
		phase_ = Phase::Done;
	}
	size_t files_done() const { return next_source_ + next_output_; }
	size_t files_total() const { return sources_.size() + outputs_.size(); }
	ImportResult take() { return std::move(result_); }

private:
	enum class Phase : uint8_t { Scan, Check, Publish, Done };

	void refuse(CoreFinding code, const std::string &message, const std::string &name) {
		result_.diagnostics.push_back(make_finding(code, DiagnosticSeverity::Error, message, name));
		refused_ = true;
	}
	// A name the project cannot take, as the import says it.
	static CoreFinding name_refused(FileNameProblem problem) {
		switch (problem) {
		case FileNameProblem::Kind: return CoreFinding::ImportKind;
		case FileNameProblem::Path: return CoreFinding::ImportPath;
		default: return CoreFinding::ImportName;
		}
	}

	// A source's bytes as the import reads them: the install's or an archive's file as its game
	// loader is served it, a loose file from the disk. False, refused, when it cannot be read.
	bool read(const ImportChoice &source, const std::string &name, std::vector<uint8_t> &bytes) {
		std::string io_error;
		if (source.install) {
			if (retail_root_ != source.path) {
				retail_root_.clear();
				if (!mount_retail(retail_, source.path, document_)) {
					refuse(CoreFinding::ImportInstall, "No game archives found under " + source.path + ".", name);
					return false;
				}
				retail_root_ = source.path;
			}
			if (!read_install_file(retail_, source.path, source.entry, bytes)) {
				refuse(CoreFinding::ImportRead, "The game data has no file named " + name + ".", name);
				return false;
			}
		} else if (source.entry.empty()) {
			if (!read_file_bytes(source.path, bytes, io_error)) {
				refuse(CoreFinding::ImportRead, io_error, name);
				return false;
			}
		} else {
			if (archive_path_ != source.path) {
				archive_.clear();
				archive_path_.clear();
				if (!archive_.set_primary_archive(source.path)) {
					refuse(CoreFinding::ImportArchive, "Could not open archive: " + source.path, name);
					return false;
				}
				archive_path_ = source.path;
			}
			if (!read_served(archive_, source.entry, bytes)) {
				refuse(CoreFinding::ImportRead, "Could not read " + name + " from " + source.path, name);
				return false;
			}
		}
		cost_ += bytes.size();
		return true;
	}

	// One source read and every file it makes checked (its name, its kind, a clash with another
	// selected file, a project file of the name, the place of its import record), then, while no
	// source was refused, staged.
	void check(const ImportChoice &source) {
		const std::string name = source.name();
		FileNameProblem problem = FileNameProblem::None;
		std::string message, io_error;
		if (!check_project_file_name(paths_.root, std::string(), name, AssetKind::Unknown, problem, message))
			return refuse(name_refused(problem), message, name);
		// The player's or this machine's own file is never the project's (assets/player_files.h).
		if (is_player_file(name))
			return refuse(CoreFinding::ImportPlayerFile,
			              name + " is " + player_file_words(name) + ": an import never takes the player's own files.", name);
		if (!selected_names_.insert(normalized_logical_name(name)).second)
			return refuse(CoreFinding::ImportDuplicate, "More than one selected file has the name " + name + ".", name);
		// A file of a name the project holds is decided once the source is read: the same bytes are
		// left as they are, other bytes refused unless the import replaces (below, for each file the
		// source makes; review F2).
		std::vector<uint8_t> bytes;
		if (!read(source, name, bytes)) return;
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
				result_.diagnostics.push_back(std::move(d));
			}
			if (broken) {
				refused_ = true;
				return;
			}
			made = std::move(product.outputs);
		} else {
			made.push_back({name, std::move(bytes)});
		}
		// A loose file from the disk is an author's source; a file of the game install or an
		// archive, or one a source marks native, is the game's own, copied as the game reads it.
		const bool authored = !source.install && source.entry.empty() && !source.native;
		for (ImportOutput &output : made) {
			if (output.name != name) {
				if (!check_project_file_name(paths_.root, std::string(), output.name, AssetKind::Unknown, problem, message)) {
					refuse(name_refused(problem), message, output.name);
					continue;
				}
				if (!selected_names_.insert(normalized_logical_name(output.name)).second) {
					refuse(CoreFinding::ImportDuplicate, "More than one selected file makes " + output.name + ".", output.name);
					continue;
				}
			}
			// Importing an author's file an importer converts makes it an import source (its record is
			// written with it, below), whatever its name makes it otherwise; any other file is the kind
			// its name and bytes give (the game's own PNG the texture it loads as it is).
			const Importer *importer = authored ? importer_for(output.name) : nullptr;
			const AssetKind kind = importer ? AssetKind::ImportSource : classify_asset(output.name, &output.bytes);
			if (kind == AssetKind::Unknown || kind == AssetKind::Archive) {
				refuse(CoreFinding::ImportKind, "Unsupported asset type: " + output.name, output.name);
				continue;
			}
			// A file the project holds with the same bytes is left as it is unless the
			// import replaces.
			const AssetEntry *prior = existing_.find(output.name);
			if (prior && !replace_existing_) {
				std::vector<uint8_t> held;
				if (read_file_bytes(join_path(paths_.root, prior->relative_path), held, io_error) &&
				    held == output.bytes)
					continue;
				refuse(CoreFinding::ImportExists, output.name + " already exists; select Replace existing files to replace it.",
				       output.name);
				continue;
			}
			const std::string relative = import_destination(existing_, output.name, kind);
			if (!check_project_file_name(paths_.root, utf8_of(path_of(relative).parent_path()), output.name, kind,
			                             problem, message)) {
				refuse(name_refused(problem), message, output.name);
				continue;
			}
			Output planned;
			planned.name = output.name;
			planned.kind = kind;
			planned.relative = relative;
			planned.made_from = converter ? name : std::string();
			// An import source's record, from the importer's defaults, is written with it (a record
			// there stays). Where the record goes must take a file.
			const Importer *record = nullptr;
			if (importer) {
				const fs::path at = system_path(join_path(paths_.root, relative + kImportSidecarSuffix));
				std::error_code ec;
				if (fs::exists(at, ec) && !fs::is_regular_file(at, ec)) {
					refuse(CoreFinding::ImportRecord,
					       relative + kImportSidecarSuffix + " is not a file: the import record of " + output.name +
					               " cannot be written there.",
					       output.name);
					continue;
				}
				if (!fs::exists(at, ec)) record = importer;
			}
			// A converter's model: the textures it names, kept for what the import leaves out.
			if (converter && kind == AssetKind::Model) {
				Extracted content;
				Diagnostic error;
				if (extract_from_bytes(output.name, kind, output.bytes, document_.target_game, content, error))
					for (GraphEdge &edge : content.edges)
						if (edge.kind == ReferenceKind::Texture) planned.textures.push_back(std::move(edge));
			}
			// Staged now, its bytes dropped with this call: nothing once a source was refused (the
			// import writes nothing then), and nothing after a write failed.
			if (!refused_ && !failed_ && !stage(planned, output.bytes, record)) return;
			outputs_.push_back(std::move(planned));
		}
	}

	// A write that failed while staging: every staged file and made folder goes; the import ends.
	bool stage_failed(const std::string &what, const std::string &error, const std::string &name) {
		remove_stage(stage_, folders_);
		result_.diagnostics.push_back(make_finding(CoreFinding::ImportWrite, DiagnosticSeverity::Error,
		                                          "Could not write " + what + ": " + error + ". Nothing was imported.", name));
		failed_ = true;
		return false;
	}

	// One checked file staged under the cache (an import's own folder, on the project's volume, so
	// a publish is a rename), its import record with it; its destination's folder made.
	bool stage(Output &output, const std::vector<uint8_t> &bytes, const Importer *record) {
		std::string io_error;
		if (stage_.empty()) {
			if (!ensure_project_cache_dir(paths_, io_error)) {
				result_.diagnostics.push_back(make_finding(CoreFinding::ImportWrite, DiagnosticSeverity::Error,
				                                          io_error + ". Nothing was imported."));
				failed_ = true;
				return false;
			}
			stage_ = path_of(paths_.staging_dir) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
			if (!ensure_directory(utf8_of(stage_), io_error))
				return stage_failed(utf8_of(stage_), io_error, std::string());
		}
		const fs::path destination = path_of(paths_.root) / path_of(output.relative);
		for (fs::path &folder : missing_folders(destination.parent_path())) folders_.push_back(std::move(folder));
		const std::string file = utf8_of(stage_ / std::to_string(outputs_.size()));
		if (!ensure_directory(utf8_of(destination.parent_path()), io_error) ||
		    !write_file_atomic(file, bytes.data(), bytes.size(), io_error))
			return stage_failed(output.relative, io_error, output.name);
		cost_ += bytes.size();
		output.staged = file;
		if (record) {
			ImportSidecar sidecar;
			sidecar.importer = record->id;
			sidecar.version = record->version;
			sidecar.options = record->default_options;
			const std::string staged_record = file + kImportSidecarSuffix;
			Diagnostic error;
			if (!save_import_sidecar(staged_record, sidecar, error))
				return stage_failed(output.relative + kImportSidecarSuffix, error.message, output.name);
			output.staged_record = staged_record;
		}
		return true;
	}

	// One file published, after its import record, by replace_file (a rename tried again while a
	// scanner holds the file, the last write moved past the one it replaces, so the caches that know
	// a file by its size and last write never take it for the file it replaced: S13 A8). A failure
	// stops the import: the record of a file that did not publish goes with it (it was written for
	// it), the rest is not published, each said.
	void publish(size_t i) {
		const Output &output = outputs_[i];
		const fs::path destination = path_of(paths_.root) / path_of(output.relative);
		const fs::path record = path_of(utf8_of(destination) + kImportSidecarSuffix);
		std::string failed, why;
		if (!output.staged_record.empty() && !replace_file(output.staged_record, utf8_of(record), why))
			failed = output.relative + kImportSidecarSuffix;
		if (failed.empty() && !replace_file(output.staged, utf8_of(destination), why)) {
			failed = output.relative;
			std::error_code ignored;
			if (!output.staged_record.empty()) fs::remove(record, ignored);
		}
		if (failed.empty()) {
			result_.imported.push_back(output.relative);
			published_any_ = true;
			++next_output_;
			return;
		}
		remove_stage(stage_, folders_);
		result_.diagnostics.push_back(make_finding(
		        CoreFinding::ImportPublish, DiagnosticSeverity::Error,
		        "Could not write " + failed + ": " + why + ". The import stopped there: " + std::to_string(i) + " of " +
		                std::to_string(outputs_.size()) + " files were imported.",
		        output.name));
		for (size_t rest = i; rest < outputs_.size(); ++rest) {
			result_.not_imported.push_back(outputs_[rest].relative);
			if (rest > i)
				result_.diagnostics.push_back(make_finding(CoreFinding::ImportNotPublished, DiagnosticSeverity::Warning,
				                                          outputs_[rest].relative + " was not imported: the import stopped at " +
				                                                  output.relative + ".",
				                                          outputs_[rest].name));
		}
		phase_ = Phase::Done;
	}

	const std::vector<ImportChoice> sources_;
	const ProjectPaths paths_;
	const ProjectDocument document_;
	const bool replace_existing_;
	ProjectScan walk_;
	AssetScan existing_;
	Phase phase_ = Phase::Scan;
	bool cleaned_ = false;
	uint64_t cost_ = 0;      // the bytes this step read and wrote
	size_t next_source_ = 0; // the next source to check
	size_t next_output_ = 0; // the next file to publish
	bool refused_ = false;   // a source was refused: nothing more is staged, nothing is published
	bool failed_ = false;    // a write failed while staging: the import ended there
	bool published_any_ = false;
	std::set<std::string> selected_names_;
	Vfs archive_;
	std::string archive_path_;
	Vfs retail_;
	std::string retail_root_;
	std::vector<Output> outputs_;
	fs::path stage_;                // the import's staging folder, made with its first staged file
	std::vector<fs::path> folders_; // the destinations' folders the import made
	ImportResult result_;
};

AssetImport::AssetImport(std::vector<ImportChoice> sources, const ProjectPaths &paths, const ProjectDocument &document,
                         bool replace_existing)
    : run_(std::make_unique<Run>(std::move(sources), paths, document, replace_existing)) {}

AssetImport::~AssetImport() = default;

bool AssetImport::step(uint64_t bytes) { return run_->step(bytes); }
bool AssetImport::done() const { return run_->done(); }
bool AssetImport::publishing() const { return run_->publishing(); }
void AssetImport::abandon() { run_->abandon(); }
size_t AssetImport::files_done() const { return run_->files_done(); }
size_t AssetImport::files_total() const { return run_->files_total(); }
ImportResult AssetImport::take() { return run_->take(); }

ImportResult import_assets(const std::vector<ImportChoice> &sources, const ProjectPaths &paths,
                           const ProjectDocument &document, bool replace_existing) {
	AssetImport run(sources, paths, document, replace_existing);
	while (!run.step(UINT64_MAX)) {
	}
	return run.take();
}

} // namespace opennova::editor
