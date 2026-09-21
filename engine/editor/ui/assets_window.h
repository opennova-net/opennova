#pragma once

#include <editor/ui/editor_host.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

// The project's files as the engine sees them: one row per file with the name the
// game will use and what kind of file it is; Refresh re-reads the folder, Show in
// folder opens it in the OS file manager.
class AssetsWindow : public devtools::Window {
public:
	explicit AssetsWindow(EditorHost &host) : host_(host) { open = true; }

	const char *title() const override { return "Project files"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Left;
	}
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

private:
	EditorHost &host_;
};

} // namespace opennova::editor
