#pragma once

#include <editor/ui/editor_host.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

// The checklist (ADR 0046 d7): every file the game demands by name, whether the
// project has it, and Create for the ones the editor can make from scratch. Required
// rows first; the optional rows under a fold.
class RequirementsWindow : public devtools::Window {
public:
	explicit RequirementsWindow(EditorHost &host) : host_(host) { open = true; }

	const char *title() const override { return "Files the game needs"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Right;
	}
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

private:
	void draw_rows(bool required);

	EditorHost &host_;
};

} // namespace opennova::editor
