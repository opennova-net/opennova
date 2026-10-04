// The ImGui pass (ADR 0039): the one per-frame Dear ImGui layout pass a
// game composes its tool windows onto. The engine owns everything about
// the windows — registry, open/closed state, the dockspace and menu bar,
// what each window shows — while the embedding shell supplies only the ImGui
// context (created by its ImGui bridge) and the frame bracket (call
// draw_frame between the bridge's NewFrame and Render).
//
// The game's F3 workspace (game_dev_tools.h) owns the pass; it is compiled
// only with OPENNOVA_DEVTOOLS. Escape follows GameWindow's Play/Interact policy.
//
// Ownership: a pass is a plain object its composer owns; there is no
// process-wide instance. Windows are registered once and live as long as the
// pass.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace opennova::devtools {

class ImGuiPass;
class OverlayLayer;
class OverlayCanvas;

enum class InitialDockPlacement {
	None,
	Center,
	Right,
	RightBottom, // the lower split of the right column
	Left,        // a column split off the centre only when a window asks for it
	Bottom,      // a strip under the centre (or the whole width: DockLayout), likewise
	CenterRight, // the right part of the centre, likewise
};

// How the pass builds its default docked layout, the first time its dockspace has none
// and on a Reset layout: the host's say (the editor's), the game's F3 workspace keeping
// the defaults. ImGui's ini keeps a layout by the dockspace's id string, so a new string
// starts a layout of its own. Each ratio is of the node it splits, in this order: the
// right column off the whole (its lower part, 0.45 of it, when a window asks for
// RightBottom), then the bottom strip and the left column off the centre, in the order
// the windows asking for them registered; with `bottom_full_width` the bottom strip comes
// off the whole first, then the left and the right column off what is above it. Last, the
// centre's right part when a window asks for CenterRight. A strip or column no window
// asks for is not split off (the right column always is). Once built, the windows `focus`
// names come forward as they dock, each selected in its dock node, the first focused.
struct DockLayout {
	std::string dockspace = "OpenNovaWorkspaceDockspace";
	bool bottom_full_width = false;
	float right = 0.30f;
	float left = 0.25f;
	float bottom = 0.30f;
	float center_right = 0.50f;
	std::vector<std::string> focus;
};

// A product's own menus on the pass's main menu bar, drawn before the pass's
// "Windows" menu (the editor's File / Edit / Build; the game has none), and
// what it shows at the bar's right end, drawn after the pass's own menus (the
// editor's unsaved files, problem counts and build state; none by default).
class ImGuiPass;
class MenuBarContributor {
public:
	virtual ~MenuBarContributor() = default;
	virtual void draw_menu_bar(ImGuiPass &pass) = 0;
	virtual void draw_menu_bar_trailing(ImGuiPass &pass) { (void)pass; }
};

// The section of the "Windows" menu a window lists under, in menu order.
// Workspace is the mandatory surface (the Game view); Help windows (ImGui's
// own demo and metrics) list in a separate "Help" menu.
enum class MenuGroup : uint8_t {
	Workspace,
	World,
	Sim,
	Render,
	Net,
	Tools,
	Help,
};
inline constexpr int kMenuGroupCount = 7;
const char *menu_group_label(MenuGroup group);

// The status line's severity: an Error reads in the warning color.
enum class StatusLevel : uint8_t {
	Info,
	Error,
};

// A window's preferred first-open size in pixels, applied with
// ImGuiCond_FirstUseEver so the user's own sizing (and ImGui's ini) always
// wins afterwards. Zero means no preference. Two floats rather than an ImVec2:
// this header deliberately carries no ImGui include.
struct WindowSizeHint {
	float width = 0.0f;
	float height = 0.0f;
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
	virtual bool is_collapsible() const { return true; }
	virtual bool is_scrollable() const { return true; }
	virtual InitialDockPlacement initial_dock_placement() const {
		return InitialDockPlacement::None;
	}
	// Only consulted for an undocked window on its first appearance; a wide
	// surface (a timeline) asks for more than the cascade default.
	virtual WindowSizeHint preferred_size() const { return WindowSizeHint{}; }
	// A window that issues its own ImGui::Begin/End (ImGui's demo, a
	// full-viewport surface) is drawn without the pass's wrapping Begin/End.
	virtual bool owns_frame() const { return false; }
	// The "Windows" menu section the window lists under.
	virtual MenuGroup menu_group() const { return MenuGroup::Tools; }
	// An open window its product steps aside for now (the editor's Preview while the active
	// document's own picture fills the Document tab): not drawn and not visible, as a closed one,
	// `open` still the user's. ImGui keeps the dock node it leaves (the window's dock id kept),
	// which hides while empty so its neighbour takes the room, and the window docks back into it
	// the frame it draws again; the user's docking stands. Its "Windows" menu item reads unticked
	// meanwhile, and ticking it calls show_anyway(): the author's ask to see it, which the window
	// honours as its own rule says (the Preview's: for the document active then).
	virtual bool stands_aside() const { return false; }
	virtual void show_anyway() {}
	// Whether the window takes the focus as it shows (ImGui focuses a window, a docked one too, the frame
	// it appears): a window that comes and goes beside the one in use (the editor's Preview, back from
	// standing aside) never takes the keyboard from it.
	virtual bool focus_on_appearing() const { return true; }
	// The debug-control rows this window reads through the control board
	// (control_board.h) while it shows: the embedder pushes their live
	// states on the board's cadence. The ids are debug_control_ids.h
	// constants.
	virtual void wanted_controls(std::vector<const char *> &out) const { (void)out; }

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

class ImGuiPass {
public:
	ImGuiPass() = default;
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

	// Whether the context keeps the user's window layout: the file the embedder's ImGui reads at
	// its first frame and writes as the docking changes (imgui-godot's user://imgui.ini, one per
	// product name, so every run of it shares the one file). A run that keeps none (the shell's
	// say: a test runner's, an agent's over MCP) has the default layout every time, the file
	// neither read nor written, so it never depends on whoever docked last and never changes the
	// user's. Applied at attach_imgui: a layout the context already read is dropped, and the file
	// is let go for the rest of the run. Default true (the shipped editor's and game's).
	void set_user_layout(bool keep) { user_layout_ = keep; }
	bool user_layout() const { return user_layout_; }

	// Where the mouse is, as the OS has it: whether one of the shell's windows has the focus,
	// and whether the cursor is over one of them with nothing covering it. The shell's say each
	// frame, before draw_frame (until it says, both true). ImGui's bridge feeds it the global
	// cursor whatever covers the window, so a window behind another, or not the one in use,
	// would otherwise take hovers through the window in front: highlights, tooltips.
	//
	// The position always stays the bridge's (a press is placed where the cursor is, the first
	// one into a window not yet focused included). What draw_frame takes of the mouse, by the
	// frame's events:
	// - a press: the pass's when the cursor is over its window (the click that focuses the
	//   window lands); one with the cursor elsewhere is dropped;
	// - a press it took, held or let go: the pass's whatever the place (a drag carried out of
	//   the window keeps its mouse);
	// - the wheel: the pass's when the cursor is over its window (the OS scrolls the window
	//   under the cursor, focused or not), dropped otherwise (lists and canvases alike);
	// - else hover: the pass's only with the focus and the cursor over its window.
	// What is not the pass's is taken back before anything draws: nothing hovered, no
	// scroll, no press.
	void set_mouse_place(bool focused, bool over) {
		mouse_focused_ = focused;
		mouse_over_ = over;
	}
	bool mouse_focused() const { return mouse_focused_; }
	bool mouse_over() const { return mouse_over_; }

	// The whole surface: closed = nothing drawn, no capture, no input.
	void set_open(bool open);
	bool is_open() const { return open_; }
	void toggle() { set_open(!open_); }

	Window &register_window(std::unique_ptr<Window> window);
	// The product's menus (not owned; nullptr = none).
	void set_menu_bar_contributor(MenuBarContributor *contributor) { menu_bar_ = contributor; }
	// The default docked layout (DockLayout); set before the first layout pass, it
	// applies to the next build of the dockspace.
	void set_dock_layout(DockLayout layout) { layout_ = std::move(layout); }
	const DockLayout &dock_layout() const { return layout_; }
	int window_count() const { return static_cast<int>(windows_.size()); }
	Window &window(int index) { return *windows_[static_cast<size_t>(index)]; }
	const Window &window(int index) const { return *windows_[static_cast<size_t>(index)]; }

	// One layout pass, between the bridge's NewFrame and Render: the
	// dockspace, the menu bar, every visible window. frame_index is the
	// render frame (windows key their cadences on it). Returns false when
	// nothing was drawn (closed or not attached). The caller reads is_open()
	// after processing the Game window's requests (including Escape).
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

	// Close the whole surface at the end of the next layout pass (the
	// "Close dev tools" menu item; the embedder reads is_open() afterwards,
	// exactly as for Escape). Safe to call from inside a window's draw.
	void request_close() { close_requested_ = true; }
	// Whether the menu offers "Close dev tools" (the editor's workspace has no
	// closed state, so it turns the item off).
	void set_closeable(bool closeable) { closeable_ = closeable; }

	// World-space overlay layers (overlay_canvas.h): owned by their windows,
	// registered here, toggled from the "Overlays" menu independently of
	// their window, and drawn by the Game window over the game image while
	// the tools are open (lower draw_priority first).
	void register_overlay(OverlayLayer &layer);
	int overlay_count() const { return static_cast<int>(overlays_.size()); }
	OverlayLayer &overlay(int index) { return *overlays_[static_cast<size_t>(index)]; }
	const OverlayLayer &overlay(int index) const { return *overlays_[static_cast<size_t>(index)]; }
	bool any_overlay_enabled() const;
	// Fires the layer's on_enabled edge when the value changes.
	void set_overlay_enabled(OverlayLayer &layer, bool enabled);
	void draw_overlays(OverlayCanvas &canvas);

	// The menu bar's status line: the newest message, drawn right-aligned in
	// the menu bar and faded out kStatusSeconds after it was first drawn;
	// hovering it lists the last kStatusHistory messages, newest first.
	static constexpr double kStatusSeconds = 6.0;
	static constexpr int kStatusHistory = 16;
	void post_status(std::string text, StatusLevel level);
	const char *status_text() const;
	StatusLevel status_level() const;
	int status_history_count() const { return static_cast<int>(status_.size()); }
	const char *status_history_text(int index) const;

private:
	struct StatusLine {
		std::string text;
		StatusLevel level = StatusLevel::Info;
		double shown_at = -1.0; // ImGui time of the first draw; -1 = not drawn yet
	};

	void sync_visibility();
	// What of the frame's mouse is the pass's (set_mouse_place), the rest taken back.
	void gate_mouse();
	void draw_menu_bar();
	void draw_overlays_menu();
	void draw_status();

	std::vector<std::unique_ptr<Window>> windows_;
	std::vector<OverlayLayer *> overlays_; // registration order (the menu's)
	std::deque<StatusLine> status_; // newest first
	MenuBarContributor *menu_bar_ = nullptr;
	DockLayout layout_;
	// The layout's `focus` windows still to bring forward, from the frame after the build
	// (armed): a window docks into the new layout at its Begin, and a tab bar selects its
	// newest tab on the frame the tab appears.
	std::vector<std::string> layout_focus_;
	std::string layout_focus_first_;
	bool layout_focus_armed_ = false;
	bool attached_ = false;
	bool platform_windows_enabled_ = true;
	bool mouse_focused_ = true;
	bool mouse_over_ = true;
	bool press_taken_ = false; // the press held is one the pass took
	bool user_layout_ = true;
	bool open_ = false;
	bool layout_reset_pending_ = false;
	bool close_requested_ = false;
	bool closeable_ = true;
};

}  // namespace opennova::devtools
