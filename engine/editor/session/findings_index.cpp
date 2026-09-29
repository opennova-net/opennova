#include <editor/session/findings_index.h>

namespace opennova::editor {

void FindingsIndex::follow(const SessionView &view) {
	if (made_ && view_ == &view && key_ == cache_key(view)) return;
	made_ = true;
	view_ = &view;
	key_ = cache_key(view);
	files_.clear();
	rows_.clear();
	for (size_t i = 0; i < view.diagnostics.size(); ++i) {
		const Diagnostic &d = view.diagnostics[i];
		if (d.asset.empty()) continue;
		files_[d.asset].push_back(i);
		if (d.row_id) rows_[{d.asset, d.row_id}].push_back(i);
	}
}

const std::vector<size_t> &FindingsIndex::of_file(const std::string &file) const {
	static const std::vector<size_t> none;
	const auto found = files_.find(file);
	return found != files_.end() ? found->second : none;
}

std::vector<size_t> FindingsIndex::of_record(const std::string &file, NodeId row,
                                             NodeId child) const {
	const auto found = rows_.find({file, row});
	if (found == rows_.end()) return {};
	if (!child || !view_) return found->second;
	std::vector<size_t> out;
	for (const size_t i : found->second)
		if (i < view_->diagnostics.size() && view_->diagnostics[i].child_id == child)
			out.push_back(i);
	return out;
}

} // namespace opennova::editor
