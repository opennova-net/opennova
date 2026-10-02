#include <editor/session/view/findings_index.h>

namespace opennova::editor {

void FindingsIndex::follow(const SessionView &view) {
	const std::vector<Diagnostic> &findings = view.findings.diagnostics;
	if (made_ && view_ == &view && key_ == cache_key(view) && count_ == findings.size() &&
	    data_ == findings.data())
		return;
	made_ = true;
	view_ = &view;
	key_ = cache_key(view);
	count_ = findings.size();
	data_ = findings.data();
	files_.clear();
	for (size_t i = 0; i < findings.size(); ++i) {
		const Diagnostic &d = findings[i];
		if (d.asset.empty()) continue;
		File &file = files_[d.asset];
		file.findings.push_back(i);
		if (d.row_id) file.rows[d.row_id].push_back(i);
	}
}

std::vector<size_t> FindingsIndex::of_file(const std::string &file) const {
	std::vector<size_t> out;
	const auto found = files_.find(file);
	if (found == files_.end() || !view_) return out;
	for (const size_t i : found->second.findings)
		if (i < view_->findings.diagnostics.size()) out.push_back(i);
	return out;
}

std::vector<size_t> FindingsIndex::of_record(const std::string &file, NodeId row,
                                             NodeId child) const {
	std::vector<size_t> out;
	const auto found = files_.find(file);
	if (found == files_.end() || !view_) return out;
	const auto on_row = found->second.rows.find(row);
	if (on_row == found->second.rows.end()) return out;
	for (const size_t i : on_row->second)
		if (i < view_->findings.diagnostics.size() &&
		    (!child || view_->findings.diagnostics[i].child_id == child))
			out.push_back(i);
	return out;
}

} // namespace opennova::editor
