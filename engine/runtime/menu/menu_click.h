#pragma once

// The menu's click and its mouse capture, the game's rule (docs/mnu/menu-re.md "The click and the
// capture", D-MNU-30). Two pieces of the original's input decide which window a release clicks:
//
// - The pump's verdict [orig: CWnd_ProcessMouseEvent @ 0x647a00]: every sample, each window the pump
//   reaches writes +0xE8: 3 when it took the claim with the left button down (input_mask's MK_LBUTTON,
//   @ 0x647b3c..0x647b53), 2 when it took it with the button up, 1 disabled, 0 otherwise; a window
//   the pump does not reach keeps its own (a hidden one returns at once @ 0x647a21; with a popup open
//   only the popup is pumped, CUIScene_EndFrame @ 0x63e691). A window that takes the claim with the
//   button up and whose verdict the sample before was 3 is clicked: its SELECTED and its 0x3000001
//   (@ 0x647b14..0x647b32, the event @ 0x647cf4), wherever the press began.
// - The capture [orig: g_UIMouseCaptureWnd @ 0x31C16CC]: a press (0x1000002, or 0x1000004 for a
//   double click) reaches the windows under the mouse front to back until one takes the capture
//   (MenuFrameCompiler::press_reach; CWnd_DispatchMouseEventToChildren @ 0x647900), a window of a
//   capturing class taking it (CButtonWnd_HandleNamedEvent @ 0x65839c, menu_window_captures), and
//   its release lets it go
//   (@ 0x6583ed), the release message reaching the captured window wherever the mouse is
//   (CWnd_DispatchMouseEventToChildren @ 0x64793f) ahead of the frame's pump. While a capture is
//   held no window but the captured one takes the pump's claim (@ 0x647a88..0x647b02), so none but
//   it is held, and it is held only while the mouse is over it.
//
// So a press on a button that is let go over another clicks neither; a press on a window that does
// not capture (the backdrop, an edit field, a combo's cell, a scrollbar's track) slid onto a button
// and let go there clicks the button; a press on a button let go off it clicks nothing, and back
// over it clicks it; a scrollbar's arrow steps on its click (D-MNU-32).
// The right button never holds a window (only MK_LBUTTON is read). The latch is the embedder's: the
// game's frame (godot/src/mnu/menu_frame.cpp), the headless frame (MenuStateFrame, the editor's Try
// mode, DI-35) and the editor's menu preview (DI-34) each keep one over the compiler's claim.

#include <formats/mnu/mnu.h>

#include <functional>
#include <vector>

namespace opennova::menu {

// A window of the menu's pump: a widget by its pre-order index, or a window a widget makes of its own
// by its index and the part: a spin list's arrows, child buttons of their own (part 1 SPINLISTWND_UP,
// 2 SPINLISTWND_DOWN) [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0], and a scrollbar's
// windows (kMenuPumpPartScroll..kMenuPumpPartScrollShuttle). Index -1: none.
struct MenuPumpWindow {
	int index = -1;
	int part = 0;
	bool valid() const { return index >= 0; }
	bool operator==(const MenuPumpWindow &o) const { return index == o.index && part == o.part; }
	bool operator!=(const MenuPumpWindow &o) const { return !(*this == o); }
};

// The part an open dropdown's list stands as while a press the dropdown took holds the capture (a
// row pick or its scrollbar: the list and its scrollbar's buttons capture, CListWnd's press reaching
// CButtonWnd_HandleNamedEvent): a window no claim is ever over, the list hidden once it picks.
inline constexpr int kMenuPumpPartDropdown = 3;

// A scrollbar's windows [orig: CScrollWnd_Construct @ 0x64c450 builds three CButtonWnd children,
// which CUIScrollbar_CreateChildWindows @ 0x64d330 names SCROLLWND_SHUTTLE, SCROLLWND_UP and
// SCROLLWND_DOWN in that order]. A SCROLL widget is a scroll window itself (part 0, its track); a
// LIST's, LAN_LIST's or TABLE's scroll window is a child window of its own, its last (LISTWND_SCROLL,
// CListWnd_Init @ 0x644d7d -> CListWnd_CreateScrollChild @ 0x6444c0, after the parse made the
// authored children), whose index here is its owner's: part kMenuPumpPartScroll, its track. The
// three buttons capture a press reaching them; the track never does (CScrollWnd_HandleEvent
// @ 0x64d087 pages on it).
inline constexpr int kMenuPumpPartScroll = 4;
inline constexpr int kMenuPumpPartScrollUp = 5;
inline constexpr int kMenuPumpPartScrollDown = 6;
inline constexpr int kMenuPumpPartScrollShuttle = 7;
// A scrollbar's window: its track (an owner's scroll window) or one of its buttons.
inline bool menu_pump_part_scroll(int part) {
	return part >= kMenuPumpPartScroll && part <= kMenuPumpPartScrollShuttle;
}

// Whether a press reaching a widget of the type takes the capture: the classes whose own-name press
// reaches CButtonWnd_HandleNamedEvent [orig: BUTTON's own handler @ 0x658340; CHECKBOX
// CCheckboxWnd_HandleNamedEvent @ 0x64ac4e; RADIO radio_button_on_click @ 0x656e0a; LIST and
// LAN_LIST list_wnd_on_command @ 0x643f19 (lan_browser_button_click_handler @ 0x65bbba); TABLE
// CTableWnd_HandleNamedEvent @ 0x642734; SPINLIST CSpinListWnd_HandleEvent @ 0x64c43e]. STATIC, the
// generic window, EDIT, MULTILINE_EDIT, COMBOBOX, MARQUEE, GOPHER, RADIOEDIT, GLB_TABLE and SCROLL's
// track do not (their handlers never reach it). A spin list's arrows and a scrollbar's buttons are
// CButtonWnds and capture (menu_pump_window_captures).
bool menu_window_captures(mnu::WindowType type);
// The same for a pump window of a widget of the type: a spin arrow or a scrollbar's button captures,
// a scroll window's track never, a widget by its type.
bool menu_pump_window_captures(mnu::WindowType type, int part);

class MenuClickLatch {
public:
	// The sample's claim as the latch reads it.
	struct Claim {
		MenuPumpWindow window; // none: nothing took it, or a scrollbar part owns the sample
		// Visible in the hierarchy: shown and enabled up its chain (inside the open popup while one is
		// open) [orig: CWnd_IsVisibleInHierarchy @ 0x646290, the pump's gate @ 0x647a27]. A window that
		// is not never takes the claim, so it is never held, clicked or pressed.
		bool live = false;
	};

	// The capture the sample's claim honors (MenuFrameCompiler::pump_mouse, claim_at): the release lets
	// it go first, its message reaching the captured window ahead of the frame's pump. None while no
	// press holds it.
	MenuPumpWindow capture_for(bool button_down);
	// The press, ahead of its sample's pump: the window its message left holding the capture
	// (MenuFrameCompiler::press_capture over its press_reach; none: no window captured) takes it
	// [orig: CButtonWnd_HandleNamedEvent @ 0x65839c].
	void press(const MenuPumpWindow &capture);
	// One sample, after its claim: the window it clicks (none: no click). The claim with the button up
	// is clicked when it was held the sample before; then every window the pump reaches this sample
	// (`reached`) lets its hold go and the claim is held while the button is down.
	MenuPumpWindow sample(const Claim &claim, bool button_down,
			const std::function<bool(const MenuPumpWindow &)> &reached);
	// A sample the open dropdown takes (the runtime's exclusive pump over the combo `combo`): no window
	// of the menu takes the claim, and a press there holds the capture until its release.
	void dropdown_sample(int combo, bool button_down);
	// The menu shows another screen, or none: its windows' holds and the capture go (a screen select
	// clears the capture [orig: CUIScene_SelectNodeByName @ 0x63b7c4]); the button stays as it is.
	void reset();

	MenuPumpWindow captured() const { return capture_; }
	// The left button at the last sample (a press edge is the button down over it up).
	bool button_down() const { return down_; }
	// Whether the window's verdict is 3 (held under the mouse with the button down when the pump last
	// reached it).
	bool held(const MenuPumpWindow &window) const;

private:
	std::vector<MenuPumpWindow> held_;
	MenuPumpWindow capture_;
	bool down_ = false;
};

} // namespace opennova::menu
