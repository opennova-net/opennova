#pragma once

#include <cstddef>
#include <cstdint>

#include <editor/ui/editor_host.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

// What the editor did and what the running game says: the build log and the game's own
// log, oldest first, following the newest line unless scrolled up to read, a finding's
// line in its severity's colour ("error: ", "warning: "); Clear empties it (the session's
// ClearOutput), Copy puts every line on the clipboard; empty, it says nothing is there yet.
class OutputWindow : public devtools::Window {
public:
	explicit OutputWindow(EditorHost &host) : host_(host) { open = true; }

	const char *title() const override { return "Output"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Bottom;
	}
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

private:
	EditorHost &host_;
	uint64_t lines_seen_ = 0; // the absolute index after the newest line drawn (OutputLog)
};

} // namespace opennova::editor
