#include <runtime/devtools/imgui_pass.h>

#include <runtime/devtools/imgui_abi.h>

#include <imgui.h>

#include <algorithm>

// The engine's ImGui copy must be the commit the imgui-godot addon bundles
// (third_party/imgui/CMakeLists.txt): a bump of one side without the other
// fails here before it can fail at the runtime hand-off.
static_assert(IMGUI_VERSION_NUM == 19160,
		"Dear ImGui pin drifted: bump third_party/imgui and scripts/bootstrap_imgui_godot.sh together");
static_assert(sizeof(ImDrawIdx) == 2, "imgui-godot expects 16-bit draw indices");
static_assert(sizeof(ImWchar) == 2, "imgui-godot expects 16-bit ImWchar");

namespace opennova::devtools {

ImGuiAbi imgui_abi() {
	return ImGuiAbi{IMGUI_VERSION, static_cast<int>(sizeof(ImGuiIO)),
			static_cast<int>(sizeof(ImDrawVert)), static_cast<int>(sizeof(ImDrawIdx)),
			static_cast<int>(sizeof(ImWchar))};
}

ImGuiPass::ImGuiPass(ImGuiPassOptions options) : options_(options), open_(options.start_open) {}

ImGuiPass::~ImGuiPass() {
	open_ = false;
	sync_visibility();
	detach_imgui();
}

bool ImGuiPass::attach_imgui(void *context, ImGuiAllocFn alloc, ImGuiFreeFn free, void *user_data) {
	if (context == nullptr) {
		return false;
	}
	if (alloc != nullptr && free != nullptr) {
		ImGui::SetAllocatorFunctions(alloc, free, user_data);
	}
	ImGui::SetCurrentContext(static_cast<ImGuiContext *>(context));
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
	attached_ = true;
	return true;
}

void ImGuiPass::detach_imgui() {
	if (!attached_) {
		return;
	}
	attached_ = false;
	ImGui::SetCurrentContext(nullptr);
}

void ImGuiPass::set_open(bool open) {
	if (open == open_) {
		return;
	}
	open_ = open;
	sync_visibility();
}

Window &ImGuiPass::register_window(std::unique_ptr<Window> window) {
	windows_.push_back(std::move(window));
	sync_visibility();
	return *windows_.back();
}

void ImGuiPass::sync_visibility() {
	for (auto &window : windows_) {
		const bool visible = open_ && window->open;
		if (visible != window->visible_) {
			window->visible_ = visible;
			window->on_visibility(visible);
		}
	}
}

bool ImGuiPass::draw_frame(uint64_t frame_index) {
	if (!attached_ || !open_) {
		return false;
	}

	if (options_.dockspace) {
		// The dockspace covers the main viewport: tool windows dock to its
		// edges and to each other, the passthru central node leaves the
		// game's mouse alone.
		ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
	}

	bool close_requested = false;
	if (options_.menu_bar && ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("Windows")) {
			for (auto &window : windows_) {
				ImGui::MenuItem(window->title(), nullptr, &window->open);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Reset layout")) {
				layout_reset_pending_ = true;
			}
			if (options_.escape_closes) {
				if (ImGui::MenuItem("Close dev tools", "Esc")) {
					close_requested = true;
				}
			}
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}
	sync_visibility();

	const bool reset_layout = layout_reset_pending_;
	layout_reset_pending_ = false;
	for (int i = 0; i < static_cast<int>(windows_.size()); ++i) {
		Window &window = *windows_[static_cast<size_t>(i)];
		if (!window.open) {
			continue;
		}
		if (window.owns_frame()) {
			window.draw(*this, frame_index);
			continue;
		}
		if (reset_layout) {
			place_window_home(i);
		}
		if (ImGui::Begin(window.title(), &window.open)) {
			window.draw(*this, frame_index);
		}
		ImGui::End();
	}

	if (options_.escape_closes && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) {
		close_requested = true;
	}
	sync_visibility();
	if (close_requested) {
		set_open(false);
	}
	return true;
}

// The next Begin of window `index` lands it in the main viewport: no dock
// node, expanded, cascaded from the work area's corner at a readable size.
void ImGuiPass::place_window_home(int index) {
	const ImGuiViewport *main = ImGui::GetMainViewport();
	const float step = 32.0f * static_cast<float>(index);
	ImGui::SetNextWindowViewport(main->ID);
	ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
	ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(main->WorkPos.x + 24.0f + step, main->WorkPos.y + 24.0f + step),
			ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(std::min(main->WorkSize.x - 48.0f - step, 640.0f),
									std::min(main->WorkSize.y - 48.0f - step, 720.0f)),
			ImGuiCond_Always);
}

}  // namespace opennova::devtools
