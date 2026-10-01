#include <editor/assets/project_scan.h>

#include <algorithm>
#include <iterator>
#include <system_error>

#include <base/io/file_time.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/import/import_run.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>

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
		if (is_dot_directory(dir) || same_path(dir, export_dir)) return false;
	}
	return true;
}

// A file of the project at `path`, listed by `key`: its entry, or the outputs its import record
// lists, and what the walk says of it; `read`, the bytes it read.
void visit_file(const ProjectPaths &paths, const fs::path &root, const fs::path &path, const std::string &key,
		AssetScan::Visit &out, uint64_t &read) {
	const std::string sidecar_suffix = kImportSidecarSuffix;
	const std::string filename = path.filename().string();
	std::error_code ec;
	if (strutil::ends_with_icase(filename, sidecar_suffix)) {
		// An import record: its outputs are project files that live under the cache.
		const std::string source_relative = key.substr(0, key.size() - sidecar_suffix.size());
		// A record whose source is gone lists nothing: its outputs would otherwise
		// outlive the source and still pack.
		if (!fs::is_regular_file(root / source_relative, ec)) {
			out.findings.push_back(make_finding(CoreFinding::ImportOrphanRecord, DiagnosticSeverity::Warning, "The import record names " + source_relative + ", which is not in the project: delete the record.",
					key));
			return;
		}
		const auto record_size = fs::file_size(path, ec);
		if (!ec) read += static_cast<uint64_t>(record_size);
		ImportSidecar sidecar;
		Diagnostic error;
		if (!load_import_sidecar(path.generic_string(), sidecar, error)) {
			if (!error.code().empty()) {
				error.asset = key;
				out.findings.push_back(error);
			}
			return;
		}
		const std::string output_dir = import_output_dir(paths, source_relative);
		for (const std::string &output : sidecar.outputs) {
			// Under the cache, deeper than its source: asked and read through the system path, so a
			// project whose own files fit MAX_PATH lists outputs that pass it (S13 A8).
			const fs::path output_path = system_path((root / output_dir / output).generic_string());
			if (!fs::is_regular_file(output_path, ec)) {
				// Only the import pass makes outputs: one missing after it ran means the
				// last import did not finish (its finding says why).
				out.findings.push_back(make_finding(CoreFinding::ImportOutputMissing, DiagnosticSeverity::Warning, output + " (imported from " + source_relative + ") has not been made: the game will not see it.",
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
			const bool peek = asset_classification_needs_bytes(output) &&
					read_file_bytes(output_path.string(), bytes, io_error);
			read += bytes.size();
			produced.kind = peek ? classify_asset(output, &bytes) : classify_asset(output, nullptr);
			out.entries.push_back(std::move(produced));
		}
		return;
	}

	AssetEntry asset;
	asset.logical_name = filename;
	asset.relative_path = key;
	const auto size = fs::file_size(path, ec);
	if (!ec) asset.size_bytes = static_cast<uint64_t>(size);
	asset.modified_ticks = io::file_modified_ticks(path);
	if (asset_classification_needs_bytes(filename)) {
		std::vector<uint8_t> bytes;
		std::string io_error;
		if (read_file_bytes(path.string(), bytes, io_error)) {
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
		if (asset.kind == AssetKind::Unknown && is_material_chunk_file(path.string(), read))
			asset.kind = AssetKind::MaterialChunk;
	}
	// A file an importer converts is an import source while its `.import` record is there,
	// whatever its name makes it otherwise (S13 A8): the build packs its outputs, never it. One
	// without is the kind its name gives (a PNG a texture the game loads as it is).
	if (importer_for(filename) && fs::is_regular_file(fs::path(path.generic_string() + sidecar_suffix), ec))
		asset.kind = AssetKind::ImportSource;
	out.entries.push_back(std::move(asset));
}

// The path the walk lists a file by: project-relative, '/'-separated, as the file system spells it.
std::string listed_key(const fs::path &root, const fs::path &path) {
	std::error_code ec;
	std::string key = fs::relative(path, root, ec).generic_string();
	return ec ? path.filename().string() : key;
}

} // namespace

bool scan_project_file(const ProjectPaths &paths, const ProjectDocument &doc, const std::string &relative,
		std::string &key, AssetScan::Visit &out, uint64_t *bytes_read) {
	out = AssetScan::Visit();
	const fs::path root(paths.root);
	const fs::path asked = root / fs::path(relative);
	std::error_code ec;
	if (!fs::is_regular_file(asked, ec) || !walk_reaches(root, fs::path(paths.export_dir(doc)), asked)) return false;
	// The file as the walk meets it: its path and its name as the file system spells them (a path
	// asked for in another case names the same file on a file system that ignores case).
	key = listed_key(root, asked);
	const fs::path path = root / fs::path(key);
	uint64_t read = 0;
	visit_file(paths, root, path, key, out, read);
	if (bytes_read) *bytes_read = read;
	return true;
}

ProjectScan::ProjectScan(const ProjectPaths &paths, const ProjectDocument &doc) :
		paths_(paths), root_(paths.root), export_dir_(paths.export_dir(doc)) {}

bool ProjectScan::step(uint64_t budget) {
	uint64_t spent = 0;
	do {
		switch (phase_) {
		case Phase::Start: {
			std::error_code ec;
			walk_ = fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, ec);
			if (ec) {
				AssetScan::Visit root;
				root.findings.push_back(make_finding(CoreFinding::ProjectRootUnreadable, DiagnosticSeverity::Error, "Cannot read the project directory " + paths_.root + ": " + ec.message()));
				visits_[std::string()] = std::move(root);
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
				if (is_dot_directory(path) || same_path(path, export_dir_)) walk_.disable_recursion_pending();
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
