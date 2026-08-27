#include <devtools/demo_window.h>

#include <imgui.h>

namespace opennova::devtools {

void DemoWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	// ShowDemoWindow owns its Begin/End; ImGuiPass::draw_frame skips the
	// wrapping Begin/End for a window that reports owns_frame().
	ImGui::ShowDemoWindow(&open);
}

}  // namespace opennova::devtools
