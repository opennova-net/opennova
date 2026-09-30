#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/session/session_revisions.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// Find in project (ADR 0046 S12, Edit > Find in project..., Ctrl+Shift+F): a modal the workspace
// draws every frame. What is typed finds the project's files and the symbols its files define
// whose names hold it (AssetGraph::search), each with where it is and how many uses it has; a
// result opens to its uses (a file's usages, a symbol's uses that reach exactly it), each a
// click away (usage_target, made once while the results stand, drawn only in sight), and its own
// Go to leads to the file or the defining record. Going anywhere, or Escape, closes the modal,
// which keeps what was typed.
class ProjectFind {
public:
	// Asks on the next draw, the keyboard in the text.
	void open();
	void draw(Workspace &workspace);

private:
	// A use of a result as its line shows it, and where it leads.
	struct Usage {
		std::string line;
		std::string tip;
		ReferenceTarget target;
	};
	// The uses of result `hit`, made on its first opening while the hits stand.
	const std::vector<Usage> &usages(const SessionView &view, size_t hit);

	bool ask_ = false;
	char text_[128]{};
	// The hits, kept while what they read (the graph) and the text stand.
	const SessionView *view_ = nullptr;
	RevisionKey key_;
	std::string searched_;
	std::vector<GraphSearchHit> hits_;
	std::map<size_t, std::vector<Usage>> usages_; // by the result's index, kept with the hits
};

} // namespace opennova::editor
