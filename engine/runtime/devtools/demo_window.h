// Dear ImGui's own demo window as a dev-tools window: the docking and
// multi-viewport smoke test, and the reference for every widget the real
// windows may use. Off by default.
#pragma once

#include <runtime/devtools/imgui_pass.h>

namespace opennova::devtools {

class DemoWindow : public Window {
public:
	const char *title() const override { return "ImGui demo"; }
	bool owns_frame() const override { return true; }
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
};

}  // namespace opennova::devtools
