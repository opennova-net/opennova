#include <editor/project/project_refresh.h>

#include <utility>

namespace opennova::editor {

ProjectRefresh::ProjectRefresh(const ProjectPaths &paths, const ProjectDocument &doc, bool force_import,
		const std::string &only) :
		doc_(doc), pass_(paths, doc, force_import, only), walk_(paths, doc) {}

bool ProjectRefresh::step(uint64_t budget) {
	if (done_) return true;
	// The import pass first: the scan lists what the importers made.
	if (!pass_.done()) {
		if (!pass_.step(budget)) return false;
		imports_ = pass_.take();
		return false;
	}
	if (!walk_.step(budget)) return false;
	scan_ = walk_.take();
	scan_.set_import_findings(imports_.diagnostics);
	requirements_ = evaluate_requirements(doc_, scan_);
	done_ = true;
	return true;
}

uint64_t ProjectRefresh::files_done() const {
	return pass_.sources_done() + walk_.files_visited();
}

uint64_t ProjectRefresh::files_total() const {
	return pass_.sources_listed() + walk_.files_listed();
}

std::string ProjectRefresh::label() const {
	if (done_) return "Checking the required files";
	if (!pass_.done())
		return pass_.listing() ? "Listing the import sources" : "Importing " + pass_.current();
	if (walk_.listing()) return "Listing the project's files";
	return "Scanning " + walk_.current();
}

} // namespace opennova::editor
