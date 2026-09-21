#pragma once

#include <cstddef>

#include <editor/ui/editor_host.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

// What the editor did and what the running game says: the build log and the game's
// own log, oldest first, following the newest line.
class OutputWindow : public devtools::Window {
public:
	explicit OutputWindow(EditorHost &host) : host_(host) { open = true; }

	const char *title() const override { return "Output"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Bottom;
	}
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

private:
	EditorHost &host_;
	size_t lines_seen_ = 0;
};

} // namespace opennova::editor
