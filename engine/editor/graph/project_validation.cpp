#include <editor/graph/project_validation.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include <editor/assets/asset_kinds.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/use_checks.h>

namespace opennova::editor {

std::vector<const AssetEntry *> validation_files(const AssetScan &scan) {
	// Each type's files in the scan's order, then the types in the registry's (DocumentTypeId's).
	std::array<std::vector<const AssetEntry *>, kDocumentTypeCount + 1> by_type;
	for (const AssetEntry &asset : scan.entries) {
		const size_t type = static_cast<size_t>(asset_kind_row(asset.kind).document);
		if (type != 0 && type < by_type.size())
			by_type[type].push_back(&asset);
	}
	std::vector<const AssetEntry *> out;
	for (const std::vector<const AssetEntry *> &files : by_type)
		out.insert(out.end(), files.begin(), files.end());
	return out;
}

std::vector<Diagnostic> validate_project(
		const ValidationInput &input, AssetGraph &graph, ValidationCache &cache) {
	refresh_project(input, graph, cache);
	return project_rows(input, graph, cache);
}

bool refresh_project(const ValidationInput &input, AssetGraph &graph, ValidationCache &cache) {
	ProjectValidation validation(graph, cache);
	while (!validation.step(input, UINT64_MAX)) {
	}
	return validation.moved();
}

ProjectValidation::ProjectValidation(AssetGraph &graph, ValidationCache &cache) :
		graph_(graph), cache_(cache), generation_(graph.generation()) {}

bool ProjectValidation::graph_moved() const {
	return (phase_ == Phase::Files || phase_ == Phase::Done) && graph_.generation() != generation_;
}

bool ProjectValidation::step(const ValidationInput &input, uint64_t budget) {
	uint64_t spent = 0;
	do {
		switch (phase_) {
		case Phase::Start:
			generation_ = graph_.generation();
			to_read_ = graph_.files_to_read(input.scan, input.open);
			phase_ = Phase::Read;
			break;
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
			cache_.begin();
			files_ = validation_files(input.scan);
			phase_ = Phase::Files;
			return false;
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

std::vector<Diagnostic> project_rows(
		const ValidationInput &input, const AssetGraph &graph, const ValidationCache &cache) {
	std::vector<Diagnostic> out;
	for (const AssetEntry *asset : validation_files(input.scan))
		if (const std::vector<Diagnostic> *own = cache.kept_findings(asset->relative_path))
			out.insert(out.end(), own->begin(), own->end());
	run_use_checks(graph, cache, out);
	const std::vector<Diagnostic> &references = graph.diagnostics();
	out.insert(out.end(), references.begin(), references.end());
	return out;
}

} // namespace opennova::editor
