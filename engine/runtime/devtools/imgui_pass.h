// The ImGui pass (ADR 0039): the one per-frame Dear ImGui layout pass a
// product composes its tool windows onto. The engine owns everything about
// the windows — registry, open/closed state, the dockspace and menu bar,
// what each window shows — while the embedding shell supplies only the ImGui
// context (created by its ImGui bridge) and the frame bracket (call
// draw_frame between the bridge's NewFrame and Render).
//
// Two products compose it today: the game's F3 workspace
// (game_dev_tools.h; compiled with OPENNOVA_DEVTOOLS) and ONED's run surface
// (oned_ui.h; every flavour). Not an editor (ADR 0037).
//
// Ownership: a pass is a plain object its composer owns; there is no
// process-wide instance. Windows are registered once and live as long as the
// pass.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace opennova::devtools {

class ImGuiPass;

enum class InitialDockPlacement {
	None,
	Center,
	Right,
	RightBottom, // the lower split of the right column
};

// One tool window. draw() runs inside ImGui::Begin/End for the window each
// frame the window is visible; on_visibility() fires on the edges of
// (pass open && window open) so a window can arm and disarm its data
// capture.
class Window {
public:
	virtual ~Window() = default;
	virtual const char *title() const = 0;
	virtual void draw(ImGuiPass &pass, uint64_t frame_index) = 0;
	virtual void on_visibility(bool visible) { (void)visible; }
	// Window policy is explicit so the pass never special-cases a title.
	virtual bool is_closeable() const { return true; }
	virtual bool is_undockable() const { return true; }
	virtual bool is_collapsible() const { return true; }
	virtual bool is_scrollable() const { return true; }
	virtual InitialDockPlacement initial_dock_placement() const {
		return InitialDockPlacement::None;
	}
	// A window that issues its own ImGui::Begin/End (ImGui's demo, a
	// full-viewport surface) is drawn without the pass's wrapping Begin/End.
	virtual bool owns_frame() const { return false; }

	// Focus this window (and select its tab in its dock node) on the pass's
	// next layout in which the window exists: a window sharing a dock node
	// with another (Entities beside Stats) is otherwise an inactive tab when
	// something opens it from outside the menu. One-shot; a request made the
	// frame the window first opens lands on the frame after.
	void request_focus() { focus_requested_ = true; }
	bool focus_requested() const { return focus_requested_; }

	bool open = false;

private:
	friend class ImGuiPass;
	bool take_focus_request() {
		const bool requested = focus_requested_;
		focus_requested_ = false;
		return requested;
	}
	bool visible_ = false;
	bool focus_requested_ = false;
};

// ImGui's allocator hooks (ImGui::SetAllocatorFunctions), typed without an
// ImGui header so the shell can pass them through as plain pointers.
using ImGuiAllocFn = void *(*)(size_t size, void *user_data);
using ImGuiFreeFn = void (*)(void *ptr, void *user_data);

struct ImGuiPassOptions {
	// An opaque application dockspace over the main viewport. Off for a single
	// full-viewport surface.
	bool dockspace = true;
	// The main menu bar with the "Windows" menu of open toggles.
	bool menu_bar = true;
	// Escape (outside a text field) closes the pass.
	bool escape_closes = true;
	// The pass starts open (a product surface) or closed (a toggled overlay).
	bool start_open = false;
};

class ImGuiPass {
public:
	explicit ImGuiPass(ImGuiPassOptions options = ImGuiPassOptions{});
	~ImGuiPass();
	ImGuiPass(const ImGuiPass &) = delete;
	ImGuiPass &operator=(const ImGuiPass &) = delete;

	// Adopt the bridge's ImGui context: the engine's ImGui copy binds to it
	// and the docking + multi-viewport config flags are set. Re-entrant (a
	// reloadable extension attaches again). Returns false for a null context.
	bool attach_imgui(void *context, ImGuiAllocFn alloc, ImGuiFreeFn free, void *user_data);
	// Drop the context binding (the bridge owns and destroys the context).
	void detach_imgui();
	bool is_attached() const { return attached_; }

	// Multi-viewport (an undocked tool window becomes an OS window) is part
	// of the attach policy, and the shell can suspend it: with
	// ImGuiConfigFlags_ViewportsEnable set, a main window in either
	// fullscreen mode presents black through imgui-godot 6.3.2 on Godot
	// 4.6.1 / D3D12 (witnessed 2026-08-28, the window_fullscreen probe), so
	// the shell drops the flag while the window is fullscreen and restores
	// it when windowed. Takes effect at ImGui's next NewFrame, so it is safe
	// to call between the bridge's NewFrame and Render.
	void set_platform_windows_enabled(bool enabled);
	bool platform_windows_enabled() const { return platform_windows_enabled_; }

	// The whole surface: closed = nothing drawn, no capture, no input.
	void set_open(bool open);
	bool is_open() const { return open_; }
	void toggle() { set_open(!open_); }

	Window &register_window(std::unique_ptr<Window> window);
	int window_count() const { return static_cast<int>(windows_.size()); }
	Window &window(int index) { return *windows_[static_cast<size_t>(index)]; }
	const Window &window(int index) const { return *windows_[static_cast<size_t>(index)]; }

	// One layout pass, between the bridge's NewFrame and Render: the
	// dockspace, the menu bar, every visible window. frame_index is the
	// render frame (windows key their cadences on it). Returns false when
	// nothing was drawn (closed or not attached). The caller reads is_open()
	// after the call (Escape and the menu close from inside).
	bool draw_frame(uint64_t frame_index);

	// Restore the default docked layout on the next layout pass: the
	// persisted dockspace is rebuilt from every window's declared placement
	// and every window is expanded. ImGui persists window placement in its
	// ini (a window dragged out to a second monitor stays in its own OS
	// viewport across runs; a window that did not exist when the ini was
	// written floats), so a probe or a reader on a different desktop asks
	// for this before reading. Also the "Reset layout" menu item.
	void request_layout_reset() { layout_reset_pending_ = true; }
	bool is_layout_reset_pending() const { return layout_reset_pending_; }

private:
	void sync_visibility();

	ImGuiPassOptions options_;
	std::vector<std::unique_ptr<Window>> windows_;
	bool attached_ = false;
	bool platform_windows_enabled_ = true;
	bool open_ = false;
	bool layout_reset_pending_ = false;
};

}  // namespace opennova::devtools
