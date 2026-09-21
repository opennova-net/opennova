#pragma once

#include <editor/ui/editor_host.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

// Every finding about the project (ADR 0046 d9), worst first: what is wrong, in plain
// words, and which file it is about.
class ProblemsWindow : public devtools::Window {
public:
	explicit ProblemsWindow(EditorHost &host) : host_(host) { open = true; }

	const char *title() const override { return "Problems"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Bottom;
	}
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

private:
	EditorHost &host_;
};

} // namespace opennova::editor
