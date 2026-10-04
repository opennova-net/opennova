#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include <editor/ui/workspace.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

class OutputLog;

// What the editor did and what the running game says: the build log and the game's own
// log, oldest first, following the newest line unless scrolled up to read, a finding's
// line in its severity's colour ("error: ", "warning: "); Clear empties it (the session's
// ClearOutput), Copy puts every line on the clipboard; empty, it says nothing is there yet.
// A line with others folded under it (OutputLog: an import's files under its one line, the game's
// whole log under its one line, what matters of it shown below as it comes) opens with a click,
// its folded lines indented under it.
class OutputWindow : public devtools::Window {
public:
	explicit OutputWindow(Workspace &workspace) : workspace_(workspace) { open = true; }

	const char *title() const override { return "Output"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Bottom;
	}
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

	// The rows a log draws with the lines `open` names opened: each line (its absolute index, -1 as the
	// folded index), each opened line followed by its folded lines (their index under it). The portable
	// half the tests read.
	static std::vector<std::pair<uint64_t, int64_t>> rows(const OutputLog &output, const std::set<uint64_t> &open);
	// How many times the window made its rows again (for the tests: only when the log or the lines opened move).
	size_t rows_made() const { return rows_made_; }

private:
	Workspace &workspace_;
	uint64_t lines_seen_ = 0; // the absolute index after the newest line drawn (OutputLog)
	std::set<uint64_t> open_; // the lines whose folded lines show, by absolute index
	// The rows drawn, made again only when the log or the lines opened move (an opened game log holds
	// thousands): the log's generation and next index they were made at, and whether open_ moved since.
	std::vector<std::pair<uint64_t, int64_t>> rows_;
	uint64_t rows_generation_ = UINT64_MAX, rows_next_ = 0;
	bool open_moved_ = true;
	size_t rows_made_ = 0;
};

} // namespace opennova::editor
