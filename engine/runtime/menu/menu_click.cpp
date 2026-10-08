#include <runtime/menu/menu_click.h>

#include <algorithm>

namespace opennova::menu {

bool menu_window_captures(mnu::WindowType type) {
	// The classes whose own-name press reaches CButtonWnd_HandleNamedEvent's capture [orig: @ 0x65839c;
	// the handlers' tails in menu_click.h].
	switch (type) {
	case mnu::WindowType::Button:
	case mnu::WindowType::CheckBox:
	case mnu::WindowType::Radio:
	case mnu::WindowType::List:
	case mnu::WindowType::LanList:
	case mnu::WindowType::Table:
	case mnu::WindowType::SpinList:
		return true;
	default:
		return false;
	}
}

bool menu_pump_window_captures(mnu::WindowType type, int part) {
	// [orig: the spin arrows and the scrollbar's buttons are CButtonWnds, CButtonWnd_HandleNamedEvent
	// @ 0x65839c; a scroll window's own press pages and captures nothing, CScrollWnd_HandleEvent
	// @ 0x64d087..0x64d10e]
	switch (part) {
	case 0: return menu_window_captures(type);
	case 1:
	case 2:
	case kMenuPumpPartScrollUp:
	case kMenuPumpPartScrollDown:
	case kMenuPumpPartScrollShuttle: return true;
	default: return false;
	}
}

MenuPumpWindow MenuClickLatch::capture_for(bool button_down) {
	// The release reaches the captured window ahead of the pump and lets the capture go [orig:
	// CButtonWnd_HandleNamedEvent @ 0x6583ed, 0x1000003 -> UI_ClearMouseCaptureWnd].
	if (!button_down) capture_ = MenuPumpWindow();
	return capture_;
}

MenuPumpWindow MenuClickLatch::sample(const Claim &claim, bool button_down,
		const std::function<bool(const MenuPumpWindow &)> &reached) {
	const bool claimed = claim.window.valid() && claim.live;
	// The claim with the button up, held the sample before: the click [orig: CWnd_ProcessMouseEvent
	// @ 0x647b14..0x647b28, verdict +0xE8 == 3 and input_mask's MK_LBUTTON clear].
	MenuPumpWindow clicked;
	if (claimed && !button_down && held(claim.window)) clicked = claim.window;
	// Every window the pump reaches writes its verdict this sample; one it does not reach keeps its own
	// [orig: @ 0x647a21 a hidden window returns before the write @ 0x647c7b].
	held_.erase(std::remove_if(held_.begin(), held_.end(),
						[&](const MenuPumpWindow &w) { return w == claim.window || reached(w); }),
			held_.end());
	// The claim with the button down is held: verdict 3 [orig: @ 0x647b3e -> 0x647b53].
	if (claimed && button_down) held_.push_back(claim.window);
	down_ = button_down;
	return clicked;
}

void MenuClickLatch::press(const MenuPumpWindow &capture) {
	// The window whose press handler reached CButtonWnd_HandleNamedEvent last [orig:
	// UI_SetMouseCaptureWnd @ 0x65839c]; a press that reached none leaves the capture as it was (none:
	// the release before let it go).
	if (capture.valid()) capture_ = capture;
}

void MenuClickLatch::dropdown_sample(int combo, bool button_down) {
	// The dropdown's list (or its scrollbar's buttons) took the press and holds the capture until the
	// release: no window of the menu takes the claim meanwhile [orig: list_wnd_on_command @ 0x643f19
	// reaches CButtonWnd_HandleNamedEvent @ 0x65839c; the pick hides the list,
	// CComboWnd_HandleEvent @ 0x65c312]. The menu's own windows are not pumped while the dropdown has
	// the mouse (the runtime's exclusive pump), so their holds stand.
	if (!button_down) capture_ = MenuPumpWindow();
	else if (!down_) capture_ = MenuPumpWindow{ combo, kMenuPumpPartDropdown };
	down_ = button_down;
}

void MenuClickLatch::reset() {
	held_.clear();
	capture_ = MenuPumpWindow();
}

bool MenuClickLatch::held(const MenuPumpWindow &window) const {
	return std::find(held_.begin(), held_.end(), window) != held_.end();
}

} // namespace opennova::menu
