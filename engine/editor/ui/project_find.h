#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/session/view/view_revisions.h>
#include <editor/session/view/workspace_view.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The project's finder, a modal the workspace draws every frame, in four scopes (the workspace's
// project_find {open, text, scope, path, locator}: the MCP gaps lane, ADR 0046 DI-18):
// - Find in project (ADR 0046 S12, Edit > Find in project..., Ctrl+Shift+F): what is typed finds the
//   project's files and the symbols its files define whose names hold it (AssetGraph::search), each with
//   where it is and how many uses it has; a result opens to its uses (a file's usages, a symbol's uses that
//   reach exactly it), each a click away (usage_target, made once while the results stand, drawn only in
//   sight), and its own Go to leads to the file or the defining record.
// - Go to file (Go > Go to file..., Ctrl+P) and Go to name (Go > Go to name..., Ctrl+T): the files alone, or
//   the names the files define alone (an item, a weapon, a string, a screen), ranked as search_project ranks
//   them, a click on one going there.
// - Find usages (Go > Find usages, Shift+F12, a Find usages of a menu): the uses of a file, or of the names a
//   record of it defines (usages_at), each a Go to; the text filters them.
// The Up and Down arrows move through what it lists and Enter goes to the one marked. Going anywhere, or
// Escape, closes the modal, which keeps what was typed (another scope or subject starts afresh).
class ProjectFind {
public:
	using Scope = WorkspaceView::FindScope;
	// The finder opened in `scope` (the keyboard in the text as it appears).
	static void open(Workspace &workspace, Scope scope = Scope::All);
	// Find usages of the project file `path`, or of its record at `locator`.
	static void open_usages(Workspace &workspace, const std::string &path, const std::string &locator = std::string());
	void draw(Workspace &workspace);

private:
	// A use as its line shows it, and where it leads.
	struct Usage {
		std::string line;
		std::string tip;
		ReferenceTarget target;
	};
	// The uses of `edges` as their lines show them.
	static std::vector<Usage> usage_lines(const SessionView &view, const std::vector<const GraphEdge *> &edges);
	// The uses of result `hit`, made on its first opening while the hits stand.
	const std::vector<Usage> &usages(const SessionView &view, size_t hit);
	// What the scope lists: the hits of the text (Find in project, Go to file, Go to name), or Find usages' uses
	// with the text's filter, made again only when what they read moves.
	void refresh(const SessionView &view, const std::string &text);
	// The results of Find in project, Go to file and Go to name; Find usages' uses. `go` is called with where a
	// line leads; `enter` true goes to the marked one.
	void draw_hits(const SessionView &view, const std::function<void(const ReferenceTarget &)> &go, bool enter, bool moved);
	void draw_uses(const std::function<void(const ReferenceTarget &)> &go, bool enter, bool moved);

	ui_kit::HeldPopup popup_;
	ui_kit::HeldText<kWorkspaceText> text_;
	// The hits (or the uses), kept while what they read (the graph) and the text, the scope and the subject stand.
	const SessionView *view_ = nullptr;
	RevisionKey key_;
	std::string searched_;
	Scope scope_ = Scope::All;
	std::string path_, locator_;
	std::vector<GraphSearchHit> hits_;
	std::map<size_t, std::vector<Usage>> usages_; // by the result's index, kept with the hits
	std::vector<Usage> uses_;                     // Find usages': every use of the subject
	std::vector<size_t> shown_uses_;              // ... those the text's filter keeps
	std::string subject_;                         // ... the subject in words
	size_t cursor_ = 0;                           // the line Enter goes to
};

} // namespace opennova::editor
