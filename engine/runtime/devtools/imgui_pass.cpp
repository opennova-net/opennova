#include <runtime/devtools/imgui_pass.h>

#include <runtime/devtools/imgui_abi.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

// The engine's ImGui copy must be the commit the imgui-godot addon bundles
// (third_party/imgui/CMakeLists.txt): a bump of one side without the other
// fails here before it can fail at the runtime hand-off.
static_assert(IMGUI_VERSION_NUM == 19160,
		"Dear ImGui pin drifted: bump third_party/imgui and scripts/bootstrap_imgui_godot.sh together");
static_assert(sizeof(ImDrawIdx) == 2, "imgui-godot expects 16-bit draw indices");
static_assert(sizeof(ImWchar) == 2, "imgui-godot expects 16-bit ImWchar");

namespace opennova::devtools {

namespace {

constexpr const char *kWorkspaceDockspace = "OpenNovaWorkspaceDockspace";

void create_default_layout(ImGuiID dockspace_id, const ImGuiViewport &viewport,
		const std::vector<std::unique_ptr<Window>> &windows) {
	ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodePos(dockspace_id, viewport.WorkPos);
	ImGui::DockBuilderSetNodeSize(dockspace_id, viewport.WorkSize);
	ImGuiID center_id = dockspace_id;
	ImGuiID right_id = 0;
	ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.30f, &right_id, &center_id);
	// The right column splits only when a window asks for its lower half, so
	// a product without one keeps the whole column for its right windows.
	ImGuiID right_bottom_id = 0;
	for (const auto &window : windows) {
		if (window->initial_dock_placement() == InitialDockPlacement::RightBottom) {
			ImGui::DockBuilderSplitNode(right_id, ImGuiDir_Down, 0.45f, &right_bottom_id, &right_id);
			break;
		}
	}
	for (const auto &window : windows) {
		const InitialDockPlacement placement = window->initial_dock_placement();
		if (placement == InitialDockPlacement::Center) {
			ImGui::DockBuilderDockWindow(window->title(), center_id);
		} else if (placement == InitialDockPlacement::Right) {
			ImGui::DockBuilderDockWindow(window->title(), right_id);
		} else if (placement == InitialDockPlacement::RightBottom) {
			ImGui::DockBuilderDockWindow(window->title(), right_bottom_id);
		}
	}
	ImGui::DockBuilderFinish(dockspace_id);
}

}  // namespace

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
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	attached_ = true;
	set_platform_windows_enabled(platform_windows_enabled_);
	return true;
}

void ImGuiPass::set_platform_windows_enabled(bool enabled) {
	platform_windows_enabled_ = enabled;
	if (!attached_) {
		return;
	}
	ImGuiIO &io = ImGui::GetIO();
	if (enabled) {
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	} else {
		io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
	}
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

	// A reset requested last frame (the menu item, a probe) rebuilds the
	// default layout below and expands every window this frame.
	const bool reset_layout = layout_reset_pending_;
	layout_reset_pending_ = false;

	if (options_.dockspace) {
		// The workspace owns the main viewport. Install its default when ImGui
		// did not restore this dockspace from persisted settings, and again on
		// a reset: the persisted dockspace is torn down first, so every window
		// (a new one the ini never saw, one dragged out to another monitor)
		// docks back into its declared placement.
		const ImGuiID dockspace_id = ImHashStr(kWorkspaceDockspace);
		const ImGuiViewport *viewport = ImGui::GetMainViewport();
		if (reset_layout) {
			ImGui::DockBuilderRemoveNode(dockspace_id);
		}
		if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
			create_default_layout(dockspace_id, *viewport, windows_);
		}
		ImGui::DockSpaceOverViewport(dockspace_id, viewport, ImGuiDockNodeFlags_None);
	}

	bool close_requested = false;
	if (options_.menu_bar && ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("Windows")) {
			for (auto &window : windows_) {
				if (window->is_closeable()) {
					ImGui::MenuItem(window->title(), nullptr, &window->open);
				} else {
					bool selected = true;
					ImGui::BeginDisabled();
					ImGui::MenuItem(window->title(), nullptr, &selected);
					ImGui::EndDisabled();
				}
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

	for (int i = 0; i < static_cast<int>(windows_.size()); ++i) {
		Window &window = *windows_[static_cast<size_t>(i)];
		if (!window.is_closeable()) {
			window.open = true;
		}
		if (!window.open) {
			// A closed window never carries a focus request forward: the pick
			// that raised it was declined when the window was closed again.
			window.take_focus_request();
			continue;
		}
		if (window.owns_frame()) {
			// No pass-owned Begin to focus; the request would only latch.
			window.take_focus_request();
			window.draw(*this, frame_index);
			continue;
		}
		if (reset_layout) {
			ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
		}
		ImGuiWindowFlags flags = ImGuiWindowFlags_None;
		if (!window.is_collapsible()) {
			flags |= ImGuiWindowFlags_NoCollapse;
		}
		if (!window.is_scrollable()) {
			flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		}
		ImGuiWindowClass window_class;
		if (!window.is_undockable()) {
			window_class.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_NoUndocking;
			ImGui::SetNextWindowClass(&window_class);
		}
		bool *open = window.is_closeable() ? &window.open : nullptr;
		if (window.focus_requested()) {
			// Honoured once the window has had its first Begin (a window
			// opening this very frame is docked by that Begin; the request
			// waits a frame so the tab exists to select). Two windows may ask
			// in the same frame (a pick raises the list and the card): each
			// selects its own tab in its dock node explicitly, since ImGui's
			// tab bar only follows the window that ends the frame focused.
			if (ImGuiWindow *imgui_window = ImGui::FindWindowByName(window.title())) {
				window.take_focus_request();
				if (imgui_window->DockNode != nullptr && imgui_window->DockNode->TabBar != nullptr) {
					imgui_window->DockNode->TabBar->NextSelectedTabId = imgui_window->TabId;
				}
				ImGui::SetNextWindowFocus();
			}
		}
		if (ImGui::Begin(window.title(), open, flags)) {
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

}  // namespace opennova::devtools
