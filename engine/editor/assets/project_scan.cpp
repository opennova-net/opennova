#include <editor/assets/project_scan.h>

#include <algorithm>
#include <iterator>
#include <system_error>

#include <base/io/file_time.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_kind.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/import/import_context.h>
#include <editor/import/import_run.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// `relative` ('/'-separated) under `root`, a system path (system_path): an extended-length path is
// taken as spelled, its separators backslashes alone.
fs::path under(const fs::path &root, const std::string &relative) {
	return root / path_of(relative).make_preferred();
}

bool same_path(const fs::path &a, const fs::path &b) {
	std::error_code ec;
	const fs::path ca = fs::weakly_canonical(a, ec);
	if (ec) return false;
	const fs::path cb = fs::weakly_canonical(b, ec);
	if (ec) return false;
	return ca == cb;
}

// The export folder, or the staging or set-aside folder an export keeps beside it (ADR 0046 S16): no
// file of the project's, whatever an export cut short or a removal refused left in it.
bool export_folder(const fs::path &dir, const fs::path &export_dir) {
	if (same_path(dir, export_dir)) return true;
	const fs::path name = export_dir.filename();
	for (const char *suffix : {kExportStagingSuffix, kExportPreviousSuffix}) {
		fs::path beside = export_dir;
		beside.replace_filename(fs::path(name).concat(suffix));
		if (same_path(dir, beside)) return true;
	}
	return false;
}

// Whether the walk from `root` reaches the file at `path`: it descends into no dot-directory and
// not into the export output directory, and passes the project file by.
bool walk_reaches(const fs::path &root, const fs::path &export_dir, const fs::path &path) {
	if (path.parent_path() == root && path.filename() == kProjectFileName) return false;
	std::error_code ec;
	const fs::path relative = fs::relative(path, root, ec);
	if (ec || relative.empty()) return false;
	fs::path dir = root;
	for (auto it = relative.begin(); it != relative.end(); ++it) {
		if (std::next(it) == relative.end()) break; // the file itself
		if (*it == "..") return false;
		dir /= *it;
		if (is_dot_directory(dir) || export_folder(dir, export_dir)) return false;
	}
	return true;
}

// Whether the walk from `root` descends into the folder `dir` (the project's own folder included): no
// dot-directory and no export folder on the way.
bool folder_reached(const fs::path &root, const fs::path &export_dir, const fs::path &dir) {
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) return false;
	const fs::path relative = fs::relative(dir, root, ec);
	if (ec || relative.empty()) return false;
	if (relative == ".") return true;
	fs::path at = root;
	for (const fs::path &part : relative) {
		if (part == "..") return false;
		at /= part;
		if (is_dot_directory(at) || export_folder(at, export_dir)) return false;
	}
	return true;
}

// A file of the project at `path`, listed by `key`: its entry, or the outputs its import record
// lists, and what the walk says of it; `read`, the bytes it read.
void visit_file(const ProjectPaths &paths, const fs::path &root, const fs::path &path, const std::string &key,
		AssetScan::Visit &out, uint64_t &read) {
	const std::string sidecar_suffix = kImportSidecarSuffix;
	const std::string filename = utf8_of(path.filename());
	std::error_code ec;
	// The file's own stamp, whatever it makes (an import record's too): what a look for a change made
	// outside the editor compares (assets/disk_changes.h).
	const auto own_size = fs::file_size(path, ec);
	if (!ec) out.size_bytes = static_cast<uint64_t>(own_size);
	out.modified_ticks = io::file_modified_ticks(path);
	if (strutil::ends_with_icase(filename, sidecar_suffix)) {
		// An import record: its outputs are project files that live under the cache.
		const std::string source_relative = key.substr(0, key.size() - sidecar_suffix.size());
		// A record whose source is gone lists nothing: its outputs would otherwise
		// outlive the source and still pack.
		if (!fs::is_regular_file(under(root, source_relative), ec)) {
			out.findings.push_back(make_finding(CoreFinding::ImportOrphanRecord, DiagnosticSeverity::Warning, "The import record names " + source_relative + ", which is not in the project: delete the record.",
					key));
			return;
		}
		read += out.size_bytes;
		ImportSidecar sidecar;
		Diagnostic error;
		if (!load_import_sidecar(utf8_of(path), sidecar, error)) {
			if (!error.code().empty()) {
				error.asset = key;
				out.findings.push_back(error);
			}
			return;
		}
		// Its inputs, from the source's folder to the project's (the files the scan types ImportInput).
		const std::string source_folder = utf8_of(path_of(source_relative).parent_path());
		for (const std::string &input : sidecar.inputs) {
			std::string file, relative;
			if (ImportContext::resolve(join_path(paths.root, source_folder), paths.root, input, file, relative))
				out.inputs.push_back(relative);
		}
		const std::string output_dir = import_output_dir(paths, source_relative);
		for (const std::string &output : sidecar.outputs) {
			// Under the cache, deeper than its source: asked and read through the system path, so a
			// project whose own files fit MAX_PATH lists outputs that pass it (S13 A8).
			const fs::path output_path = under(root, join_path(output_dir, output));
			if (!fs::is_regular_file(output_path, ec)) {
				// Only the import pass makes outputs: one missing after it ran means the
				// last import did not finish (its finding says why).
				out.findings.push_back(make_finding(CoreFinding::ImportOutputMissing, DiagnosticSeverity::Warning, output + " (imported from " + source_relative + ") has not been made: the game will not see it.",
						source_relative));
				continue;
			}
			AssetEntry produced;
			produced.logical_name = output;
			produced.relative_path = join_path(output_dir, output);
			produced.imported_from = source_relative;
			const auto produced_size = fs::file_size(output_path, ec);
			if (!ec) produced.size_bytes = static_cast<uint64_t>(produced_size);
			produced.modified_ticks = io::file_modified_ticks(output_path);
			std::vector<uint8_t> bytes;
			std::string io_error;
			const bool peek = asset_classification_needs_bytes(output) &&
					read_file_bytes(utf8_of(output_path), bytes, io_error);
			read += bytes.size();
			produced.kind = peek ? classify_asset(output, &bytes) : classify_asset(output, nullptr);
			out.entries.push_back(std::move(produced));
		}
		return;
	}

	AssetEntry asset;
	asset.logical_name = filename;
	asset.relative_path = key;
	asset.size_bytes = out.size_bytes;
	asset.modified_ticks = out.modified_ticks;
	if (asset_classification_needs_bytes(filename)) {
		std::vector<uint8_t> bytes;
		std::string io_error;
		if (read_file_bytes(utf8_of(path), bytes, io_error)) {
			read += bytes.size();
			asset.kind = classify_asset(filename, &bytes);
		} else {
			asset.kind = classify_asset(filename, nullptr);
			out.findings.push_back(
					make_finding(CoreFinding::AssetUnreadable, DiagnosticSeverity::Warning, io_error, asset.relative_path));
		}
	} else {
		asset.kind = classify_asset(filename, nullptr);
		// A name no rule types is a material chunk when the file holds one (a model's chunk row
		// reads a file of any name as one), asked of its chunk headers alone: the build packs a
		// material chunk and leaves a file of no kind the game knows out (S13 A8).
		if (asset.kind == AssetKind::Unknown && is_material_chunk_file(utf8_of(path), read))
			asset.kind = AssetKind::MaterialChunk;
		// A name with no extension that holds text is a note (a LICENSE), asked of its head alone: the
		// build leaves it out without a word, where a file of no kind is said (classify_asset).
		if (asset.kind == AssetKind::Unknown && resource_extension_for_name(filename).empty() &&
		    is_text_file(utf8_of(path), read))
			asset.kind = AssetKind::Notes;
	}
	// A file an importer converts is an import source while its `.import` record is there,
	// whatever its name makes it otherwise (S13 A8): the build packs its outputs, never it. One
	// without is the kind its name gives (a PNG a texture the game loads as it is).
	fs::path record = path;
	record += sidecar_suffix;
	if (importer_for(filename) && fs::is_regular_file(record, ec)) asset.kind = AssetKind::ImportSource;
	out.entries.push_back(std::move(asset));
}

// The path the walk lists a file by: project-relative, '/'-separated, as the file system spells it.
std::string listed_key(const fs::path &root, const fs::path &path) {
	std::error_code ec;
	std::string key = utf8_of(fs::relative(path, root, ec));
	return ec ? utf8_of(path.filename()) : key;
}

} // namespace

bool scan_project_file(const ProjectPaths &paths, const ProjectDocument &doc, const std::string &relative,
		std::string &key, AssetScan::Visit &out, uint64_t *bytes_read) {
	out = AssetScan::Visit();
	// Through the system's paths (project_files.h): a project file past MAX_PATH is read as well.
	const fs::path root = system_path(paths.root);
	const fs::path asked = under(root, relative);
	std::error_code ec;
	if (!fs::is_regular_file(asked, ec) || !walk_reaches(root, system_path(paths.export_dir(doc)), asked)) return false;
	// The file as the walk meets it: its path and its name as the file system spells them (a path
	// asked for in another case names the same file on a file system that ignores case).
	key = listed_key(root, asked);
	const fs::path path = under(root, key);
	uint64_t read = 0;
	visit_file(paths, root, path, key, out, read);
	if (bytes_read) *bytes_read = read;
	return true;
}

bool list_project_folder(const ProjectPaths &paths, const ProjectDocument &doc, const std::string &folder,
		std::vector<std::string> &files, std::vector<std::string> &folders) {
	files.clear();
	folders.clear();
	const fs::path root = system_path(paths.root);
	const fs::path export_dir = system_path(paths.export_dir(doc));
	const fs::path dir = folder.empty() ? root : under(root, folder);
	if (!folder_reached(root, export_dir, dir)) return false;
	std::error_code ec;
	fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
	if (ec) return false;
	for (const fs::directory_iterator end; it != end; it.increment(ec)) {
		if (ec) break; // listed as far as it could go
		const fs::path path = it->path();
		std::error_code kind;
		if (it->is_directory(kind)) {
			if (!is_dot_directory(path) && !export_folder(path, export_dir)) folders.push_back(listed_key(root, path));
		} else if (it->is_regular_file(kind) && !(dir == root && path.filename() == kProjectFileName)) {
			files.push_back(listed_key(root, path));
		}
	}
	std::sort(files.begin(), files.end());
	std::sort(folders.begin(), folders.end());
	return true;
}

// The walk goes through the system's paths (project_files.h, system_path): a project whose own files
// lie past MAX_PATH is walked, listed and read whole.
ProjectScan::ProjectScan(const ProjectPaths &paths, const ProjectDocument &doc) :
		paths_(paths), root_(system_path(paths.root)), export_dir_(system_path(paths.export_dir(doc))) {}

bool ProjectScan::step(uint64_t budget) {
	uint64_t spent = 0;
	do {
		switch (phase_) {
		case Phase::Start: {
			std::error_code ec;
			// Each folder's last write taken before what it holds is listed: a file made in it after the
			// listing moves it past the stamp kept.
			folders_[std::string()] = io::file_modified_ticks(root_);
			walk_ = fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, ec);
			if (ec) {
				AssetScan::Visit root;
				root.findings.push_back(make_finding(CoreFinding::ProjectRootUnreadable, DiagnosticSeverity::Error, "Cannot read the project directory " + paths_.root + ": " + ec.message()));
				visits_[std::string()] = std::move(root);
				folders_.clear();
				scan_.set_visits(std::move(visits_));
				phase_ = Phase::Done;
				return true;
			}
			phase_ = Phase::Listing;
			break;
		}
		case Phase::Listing: {
			if (walk_ == fs::recursive_directory_iterator()) {
				std::sort(listed_.begin(), listed_.end(),
						[](const auto &a, const auto &b) { return a.first < b.first; });
				phase_ = Phase::Visiting;
				break;
			}
			spent += kWalkEntryCost;
			std::error_code ec;
			const fs::directory_entry &entry = *walk_;
			const fs::path path = entry.path();
			if (entry.is_directory(ec)) {
				if (is_dot_directory(path) || export_folder(path, export_dir_)) walk_.disable_recursion_pending();
				else folders_[listed_key(root_, path)] = io::file_modified_ticks(path); // before the walk lists it
			} else if (entry.is_regular_file(ec) &&
					!(path.parent_path() == root_ && path.filename() == kProjectFileName)) {
				listed_.emplace_back(listed_key(root_, path), path);
			}
			walk_.increment(ec);
			if (ec) walk_ = fs::recursive_directory_iterator(); // the walk stops where it could not go on
			break;
		}
		case Phase::Visiting: {
			if (next_ == listed_.size()) {
				scan_.set_visits(std::move(visits_));
				scan_.set_folders(std::move(folders_));
				phase_ = Phase::Done;
				return true;
			}
			const auto &[key, path] = listed_[next_++];
			current_ = key;
			std::error_code ec;
			spent += kWalkEntryCost;
			// A file gone since it was listed is not in the scan.
			if (!fs::is_regular_file(path, ec)) break;
			AssetScan::Visit visit;
			uint64_t read = 0;
			visit_file(paths_, root_, path, key, visit, read);
			spent += read;
			visits_[key] = std::move(visit);
			break;
		}
		case Phase::Done: return true;
		}
	} while (spent < budget);
	return done();
}

AssetScan ProjectScan::take() {
	return done() ? std::move(scan_) : AssetScan();
}

} // namespace opennova::editor
