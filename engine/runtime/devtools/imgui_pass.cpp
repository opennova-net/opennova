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

const char *menu_group_label(MenuGroup group) {
	switch (group) {
		case MenuGroup::Workspace:
			return "Workspace";
		case MenuGroup::World:
			return "World";
		case MenuGroup::Sim:
			return "Simulation";
		case MenuGroup::Render:
			return "Render";
		case MenuGroup::Net:
			return "Network";
		case MenuGroup::Tools:
			return "Tools";
		case MenuGroup::Help:
			return "Help";
	}
	return "";
}

void ImGuiPass::post_status(std::string text, StatusLevel level) {
	status_.push_front(StatusLine{std::move(text), level, -1.0});
	while (static_cast<int>(status_.size()) > kStatusHistory) {
		status_.pop_back();
	}
}

const char *ImGuiPass::status_text() const {
	return status_.empty() ? "" : status_.front().text.c_str();
}

StatusLevel ImGuiPass::status_level() const {
	return status_.empty() ? StatusLevel::Info : status_.front().level;
}

const char *ImGuiPass::status_history_text(int index) const {
	if (index < 0 || index >= status_history_count()) return "";
	return status_[static_cast<size_t>(index)].text.c_str();
}

ImGuiAbi imgui_abi() {
	return ImGuiAbi{IMGUI_VERSION, static_cast<int>(sizeof(ImGuiIO)),
			static_cast<int>(sizeof(ImDrawVert)), static_cast<int>(sizeof(ImDrawIdx)),
			static_cast<int>(sizeof(ImWchar))};
}

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

	draw_menu_bar();
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
			if (window.initial_dock_placement() == InitialDockPlacement::None) {
				// The dockspace rebuild re-homes every docked window; an
				// undocked window comes home to the work-area cascade at its
				// preferred size (dragged off to a dead monitor it would
				// otherwise survive the reset out of reach).
				const ImGuiViewport *main = ImGui::GetMainViewport();
				const float step = 32.0f * static_cast<float>(i);
				const WindowSizeHint hint = window.preferred_size();
				const float width = hint.width > 0.0f ? hint.width : 640.0f;
				const float height = hint.height > 0.0f ? hint.height : 720.0f;
				ImGui::SetNextWindowViewport(main->ID);
				ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
				ImGui::SetNextWindowPos(
						ImVec2(main->WorkPos.x + 24.0f + step, main->WorkPos.y + 24.0f + step),
						ImGuiCond_Always);
				ImGui::SetNextWindowSize(
						ImVec2(std::min(main->WorkSize.x - 48.0f - step, width),
								std::min(main->WorkSize.y - 48.0f - step, height)),
						ImGuiCond_Always);
			}
		}
		ImGuiWindowFlags flags = ImGuiWindowFlags_None;
		if (!window.is_collapsible()) {
			flags |= ImGuiWindowFlags_NoCollapse;
		}
		if (!window.is_scrollable()) {
			flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		}
		const WindowSizeHint hint = window.preferred_size();
		if (!reset_layout && hint.width > 0.0f && hint.height > 0.0f) {
			ImGui::SetNextWindowSize(ImVec2(hint.width, hint.height), ImGuiCond_FirstUseEver);
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

	sync_visibility();
	if (close_requested_) {
		close_requested_ = false;
		set_open(false);
	}
	return true;
}

void ImGuiPass::draw_menu_bar() {
	if (!ImGui::BeginMainMenuBar()) {
		return;
	}
	const auto menu_item = [](Window &window) {
		if (window.is_closeable()) {
			ImGui::MenuItem(window.title(), nullptr, &window.open);
		} else {
			bool selected = true;
			ImGui::BeginDisabled();
			ImGui::MenuItem(window.title(), nullptr, &selected);
			ImGui::EndDisabled();
		}
	};
	if (ImGui::BeginMenu("Windows")) {
		// One section per group, in MenuGroup order and registration order
		// within it; Help windows have their own menu.
		for (int g = 0; g < kMenuGroupCount; ++g) {
			const MenuGroup group = static_cast<MenuGroup>(g);
			if (group == MenuGroup::Help) continue;
			bool header = false;
			for (auto &window : windows_) {
				if (window->menu_group() != group) continue;
				if (!header && group != MenuGroup::Workspace) {
					ImGui::SeparatorText(menu_group_label(group));
				}
				header = true;
				menu_item(*window);
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Reset layout")) {
			layout_reset_pending_ = true;
		}
		if (ImGui::MenuItem("Close dev tools", "F3")) {
			close_requested_ = true;
		}
		ImGui::EndMenu();
	}
	bool has_help = false;
	for (const auto &window : windows_) {
		has_help = has_help || window->menu_group() == MenuGroup::Help;
	}
	if (has_help && ImGui::BeginMenu("Help")) {
		for (auto &window : windows_) {
			if (window->menu_group() == MenuGroup::Help) menu_item(*window);
		}
		ImGui::EndMenu();
	}
	draw_status();
	ImGui::EndMainMenuBar();
}

void ImGuiPass::draw_status() {
	if (status_.empty()) {
		return;
	}
	StatusLine &line = status_.front();
	const double now = ImGui::GetTime();
	if (line.shown_at < 0.0) {
		line.shown_at = now;
	}
	const double age = now - line.shown_at;
	// Full strength for most of its life, then a one-second fade; a faded
	// line keeps a dim marker so the history stays one hover away.
	float alpha = 1.0f;
	if (age > kStatusSeconds) {
		alpha = 0.25f;
	} else if (age > kStatusSeconds - 1.0) {
		alpha = std::max(0.25f, static_cast<float>(kStatusSeconds - age));
	}
	const ImVec4 color = line.level == StatusLevel::Error
			? ImVec4(1.0f, 0.45f, 0.35f, alpha)
			: ImVec4(0.65f, 0.9f, 0.65f, alpha);
	const float width = ImGui::CalcTextSize(line.text.c_str()).x;
	const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	const float x = std::max(ImGui::GetCursorPosX() + 16.0f, right - width - 8.0f);
	ImGui::SetCursorPosX(x);
	ImGui::TextColored(color, "%s", line.text.c_str());
	if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
		for (const StatusLine &entry : status_) {
			if (entry.level == StatusLevel::Error) {
				ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", entry.text.c_str());
			} else {
				ImGui::TextUnformatted(entry.text.c_str());
			}
		}
		ImGui::EndTooltip();
	}
}

}  // namespace opennova::devtools
