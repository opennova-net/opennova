// The ImGui pass (ADR 0039): the one per-frame Dear ImGui layout pass a
// product composes its tool windows onto. The engine owns everything about
// the windows — registry, open/closed state, the dockspace and menu bar,
// what each window shows — while the embedding shell supplies only the ImGui
// context (created by its ImGui bridge) and the frame bracket (call
// draw_frame between the bridge's NewFrame and Render).
//
// Two products compose it today: the game's dev tools (game_dev_tools.h, the
// Stats window behind F3; compiled with OPENNOVA_DEVTOOLS) and ONED's run
// surface (oned_ui.h; every flavour). Not an editor (ADR 0037).
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
	// A window that issues its own ImGui::Begin/End (ImGui's demo, a
	// full-viewport surface) is drawn without the pass's wrapping Begin/End.
	virtual bool owns_frame() const { return false; }

	bool open = false;

private:
	friend class ImGuiPass;
	bool visible_ = false;
};

// ImGui's allocator hooks (ImGui::SetAllocatorFunctions), typed without an
// ImGui header so the shell can pass them through as plain pointers.
using ImGuiAllocFn = void *(*)(size_t size, void *user_data);
using ImGuiFreeFn = void (*)(void *ptr, void *user_data);

struct ImGuiPassOptions {
	// A passthru dockspace over the main viewport: windows dock to the
	// viewport's edges and to each other without stealing the mouse in the
	// empty area. Off for a single full-viewport surface.
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

private:
	void sync_visibility();

	ImGuiPassOptions options_;
	std::vector<std::unique_ptr<Window>> windows_;
	bool attached_ = false;
	bool open_ = false;
};

}  // namespace opennova::devtools
