#include <editor/graph/project_validation.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

#include <editor/assets/asset_kinds.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/use_checks.h>

namespace opennova::editor {

namespace {

// The document type a file's own checks are a validator of (its row's), 0 for none: a type whose files no
// validator reads (S18: a texture's, with no finding codes yet) has none.
size_t validated_type(const AssetEntry &asset) {
	const size_t type = static_cast<size_t>(asset_kind_row(asset.kind).document);
	const DocumentType *registered = document_type_for(asset.kind);
	return type != 0 && type <= kDocumentTypeCount && registered && validates_files(*registered) ? type : 0;
}

// The validation's files of a scan as indices into its entries, kept by the scan's serial (a pass and the rows
// composed after it ask the same list).
std::mutex g_files_mutex;
UseCheckMemo<uint64_t, std::vector<uint32_t>> g_files; // g_files_mutex held

std::vector<uint32_t> validation_indices(const AssetScan &scan) {
	// Each type's files in the scan's order, then the types in the registry's (DocumentTypeId's).
	std::array<std::vector<uint32_t>, kDocumentTypeCount + 1> by_type;
	for (size_t i = 0; i < scan.entries.size(); ++i)
		if (const size_t type = validated_type(scan.entries[i])) by_type[type].push_back(uint32_t(i));
	std::vector<uint32_t> out;
	for (const std::vector<uint32_t> &files : by_type)
		out.insert(out.end(), files.begin(), files.end());
	return out;
}

} // namespace

std::vector<const AssetEntry *> validation_files(const AssetScan &scan) {
	std::vector<uint32_t> made;
	const std::vector<uint32_t> *indices = nullptr;
	std::unique_lock<std::mutex> lock(g_files_mutex);
	if (scan.serial() != 0) indices = g_files.find(scan.serial());
	if (!indices) {
		lock.unlock();
		made = validation_indices(scan);
		lock.lock();
		indices = scan.serial() != 0 ? &g_files.keep(scan.serial(), std::move(made)) : &made;
	}
	std::vector<const AssetEntry *> out;
	out.reserve(indices->size());
	for (const uint32_t i : *indices) out.push_back(&scan.entries[i]);
	return out;
}

std::vector<Diagnostic> validate_project(
		const ValidationInput &input, AssetGraph &graph, ValidationCache &cache) {
	refresh_project(input, graph, cache);
	return project_rows(input, graph, cache);
}

bool refresh_project(const ValidationInput &input, AssetGraph &graph, ValidationCache &cache) {
	GraphReadings readings;
	ProjectValidation validation(graph, cache, readings);
	while (!validation.step(input, UINT64_MAX)) {
	}
	return validation.moved();
}

namespace {

// Whether a reading made ahead is of the file as the scan lists it now (AssetGraph::update's rule).
bool reads_as_listed(const GraphReading &reading, const AssetEntry &asset) {
	return reading.logical_name == asset.logical_name && reading.kind == asset.kind &&
	       reading.size == asset.size_bytes && reading.modified == asset.modified_ticks;
}

} // namespace

ProjectValidation::ProjectValidation(AssetGraph &graph, ValidationCache &cache, GraphReadings &readings) :
		graph_(graph), cache_(cache), readings_(readings), generation_(graph.generation()) {}

bool ProjectValidation::graph_moved() const {
	return graph_read() && graph_.generation() != generation_;
}

bool ProjectValidation::step(const ValidationInput &input, uint64_t budget) {
	uint64_t spent = 0;
	do {
		switch (phase_) {
		case Phase::Start: {
			generation_ = graph_.generation();
			// The readings a validation before this one made of files still as the scan lists them are
			// taken as they are (a validation started again does not read them again); the others go.
			GraphReadings kept;
			for (const AssetEntry *asset : graph_.files_to_read(input.scan, input.open)) {
				const auto found = readings_.find(asset->relative_path);
				if (found != readings_.end() && reads_as_listed(found->second, *asset)) {
					kept.emplace(found->first, std::move(found->second));
					++kept_;
				} else {
					to_read_.push_back(asset);
				}
			}
			readings_ = std::move(kept);
			phase_ = Phase::Read;
			break;
		}
		case Phase::Read: {
			// The graph first (the use checks read what the files define and who uses it): the files
			// its update would read, read ahead a file at a time.
			if (read_next_ == to_read_.size()) {
				phase_ = Phase::Graph;
				break;
			}
			const AssetEntry &asset = *to_read_[read_next_++];
			current_ = asset.relative_path;
			readings_[asset.relative_path] = AssetGraph::read_file(input.paths, input.project, asset);
			spent += kValidationFileCost + asset.size_bytes;
			break;
		}
		case Phase::Graph:
			// The update, a step of its own: it takes the readings and resolves what they reach.
			graph_.update(input.paths, input.project, input.scan, input.open, &readings_);
			readings_.clear();
			phase_ = Phase::Uses;
			return false;
		case Phase::Uses: {
			// The headers the texture check reads of the files its uses' loaders open (none where it keeps its
			// walk of this graph and scan), listed once over the updated graph, then one a step: the check
			// composes from them, reading none (a project's thousands of textures were read in one composition).
			if (!uses_listed_) {
				uses_ = texture_use_reads(graph_, input);
				uses_listed_ = true;
				break;
			}
			if (uses_next_ == uses_.size()) {
				begin_files(input);
				phase_ = Phase::Files;
				break;
			}
			const TextureRead &read = uses_[uses_next_++];
			current_ = read.entry->relative_path;
			spent += kValidationFileCost + read_texture_use(input, read);
			break;
		}
		case Phase::Files: {
			if (next_ == files_.size()) {
				cache_.end();
				const ValidationStats &stats = cache_.stats();
				moved_ = stats.files_validated > 0 || stats.files_dropped > 0 || graph_.generation() != generation_;
				phase_ = Phase::Done;
				return true;
			}
			const AssetEntry &asset = *files_[next_++];
			current_ = asset.relative_path;
			const size_t loaded = cache_.stats().files_loaded;
			cache_.file_findings(input, asset);
			spent += kValidationFileCost + (cache_.stats().files_loaded != loaded ? asset.size_bytes : 0);
			break;
		}
		case Phase::Done: return true;
		}
	} while (spent < budget);
	return false;
}

void ProjectValidation::begin_files(const ValidationInput &input) {
	const uint64_t serial = input.scan.serial();
	const std::string &game = input.project.target_game;
	if (!cache_.keeps(serial, game)) {
		cache_.begin(serial, game);
		files_ = validation_files(input.scan);
		return;
	}
	// The scan the last validation that ended read, for the same game: every closed file's findings stand as
	// they were made, so only the files an open document stands in for, now or then (one closed since), are
	// asked; the cache keeps the rest.
	std::set<std::string> paths;
	for (const std::string &path : cache_.open_files()) paths.insert(path);
	for (const auto &document : input.open)
		if (document) paths.insert(document->path());
	std::vector<std::string> asked;
	for (const std::string &path : paths)
		if (const AssetEntry *asset = input.scan.at_path(path); asset && validated_type(*asset)) {
			files_.push_back(asset);
			asked.push_back(path);
		}
	cache_.begin_kept(serial, game, asked);
}

std::vector<Diagnostic> project_rows(
		const ValidationInput &input, const AssetGraph &graph, const ValidationCache &cache) {
	std::vector<Diagnostic> out;
	for (const AssetEntry *asset : validation_files(input.scan))
		if (const std::vector<Diagnostic> *own = cache.kept_findings(asset->relative_path))
			out.insert(out.end(), own->begin(), own->end());
	run_use_checks(graph, cache, input, out);
	const std::vector<Diagnostic> &references = graph.diagnostics();
	out.insert(out.end(), references.begin(), references.end());
	return out;
}

} // namespace opennova::editor
