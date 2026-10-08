// The menu's click and its mouse capture, the game's rule (engine/runtime/menu/menu_click.h,
// docs/mnu/menu-re.md "The click and the capture", D-MNU-30) [orig: CWnd_ProcessMouseEvent @ 0x647a00,
// the click @ 0x647b14; CButtonWnd_HandleNamedEvent @ 0x658340, the capture @ 0x65839c / 0x6583ed].
//
// Pinned, on the latch alone: a release clicks the claim the pump held down the sample before; the hold
// follows the claim while the button is down; a capture holds the claim to the captured window; a window
// the pump does not reach keeps its hold; a window that is not live is never held; the open dropdown's
// press holds the capture to its release.
//
// Pinned, through the compiler's claim and the runtime (the headless frame the editor's Try mode runs,
// the same rule the game's frame runs): a press on the backdrop, a STATIC or an EDIT slid onto a button
// clicks the button; a press on a button let go over another clicks neither and the other never lights;
// a press on a button let go off every button clicks nothing, and back over it clicks it; a LIST press
// captures (a button it is let go over is not clicked, the list is when let go over it); a CHECKBOX
// press let go over a button toggles nothing; a double click clicks twice; a spin arrow is a button of
// its own (pressed and let go over it, it steps; let go off it, nothing; a press on the list's body
// never clicks the arrow); a slider's shuttle drags over a button and the button is not clicked; a
// DISABLED window is never held, so a press there slid onto a button clicks the button.
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_click.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_sound.h>
#include <runtime/menu/menu_state_frame.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::menu;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// --- the latch alone -------------------------------------------------------------------------------

MenuClickLatch::Claim at(int index, bool captures = false, int part = 0, bool live = true) {
	MenuClickLatch::Claim claim;
	claim.window = MenuPumpWindow{ index, part };
	claim.live = live;
	claim.captures = captures;
	return claim;
}

const auto kAll = [](const MenuPumpWindow &) { return true; };

// One sample as an embedder runs it: the capture let go on a release, then the claim (the capture's
// window where one is held: `over_capture` says whether the mouse is over it), then the latch.
MenuPumpWindow step(MenuClickLatch &latch, MenuClickLatch::Claim claim, bool down, bool over_capture = true) {
	const MenuPumpWindow capture = latch.capture_for(down);
	if (capture.valid()) claim = over_capture ? at(capture.index, true, capture.part) : MenuClickLatch::Claim();
	return latch.sample(claim, down, kAll);
}

void test_latch() {
	{
		// Pressed and let go over one window: its click [orig: @ 0x647b14 — verdict 3, the button up].
		MenuClickLatch latch;
		CHECK(!step(latch, at(1), false).valid());
		CHECK(!step(latch, at(1), true).valid());
		CHECK(latch.held(MenuPumpWindow{ 1, 0 }));
		CHECK(step(latch, at(1), false) == (MenuPumpWindow{ 1, 0 }));
		CHECK(!latch.held(MenuPumpWindow{ 1, 0 }));
		// The sample after finds it done.
		CHECK(!step(latch, at(1), false).valid());
	}
	{
		// A press on a window that does not capture; the hold follows the claim; let go over another:
		// the other's click, wherever the press began.
		MenuClickLatch latch;
		step(latch, at(0), true);
		CHECK(!latch.captured().valid() && latch.held(MenuPumpWindow{ 0, 0 }));
		step(latch, at(2), true);
		CHECK(!latch.held(MenuPumpWindow{ 0, 0 }) && latch.held(MenuPumpWindow{ 2, 0 }));
		CHECK(step(latch, at(2), false) == (MenuPumpWindow{ 2, 0 }));
	}
	{
		// A press on a capturing window: the capture holds the claim to it; off it nothing is held, and
		// let go over another window nothing is clicked [orig: @ 0x647a88..0x647b02].
		MenuClickLatch latch;
		step(latch, at(1, true), true);
		CHECK(latch.captured() == (MenuPumpWindow{ 1, 0 }));
		step(latch, at(2), true, false);
		CHECK(!latch.held(MenuPumpWindow{ 1, 0 }) && !latch.held(MenuPumpWindow{ 2, 0 }));
		CHECK(!step(latch, at(2), false).valid());
		CHECK(!latch.captured().valid());
		// Back over it before the release: its click.
		step(latch, at(1, true), true);
		step(latch, at(2), true, false);
		step(latch, at(2), true, true);
		CHECK(step(latch, at(1, true), false) == (MenuPumpWindow{ 1, 0 }));
	}
	{
		// A spin arrow is a window of its own: its hold is not its list's.
		MenuClickLatch latch;
		step(latch, at(4, true, 1), true);
		CHECK(latch.captured() == (MenuPumpWindow{ 4, 1 }));
		CHECK(!step(latch, at(4, true, 0), false).valid());
	}
	{
		// A window the pump does not reach keeps its hold [orig: @ 0x647a21]; one it reaches lets go.
		MenuClickLatch latch;
		step(latch, at(5), true);
		latch.sample(MenuClickLatch::Claim(), true, [](const MenuPumpWindow &w) { return w.index != 5; });
		CHECK(latch.held(MenuPumpWindow{ 5, 0 }));
		latch.sample(MenuClickLatch::Claim(), false, [](const MenuPumpWindow &w) { return w.index != 5; });
		CHECK(latch.held(MenuPumpWindow{ 5, 0 }));
		CHECK(latch.sample(at(5), false, kAll) == (MenuPumpWindow{ 5, 0 }));
	}
	{
		// A window that is not live never takes the claim: never held, never captures [orig: @ 0x647a27].
		MenuClickLatch latch;
		step(latch, at(6, true, 0, false), true);
		CHECK(!latch.held(MenuPumpWindow{ 6, 0 }) && !latch.captured().valid());
		step(latch, at(2), true);
		CHECK(step(latch, at(2), false) == (MenuPumpWindow{ 2, 0 }));
	}
	{
		// The open dropdown takes the press: the capture is its list's until the release, so the window
		// under the mouse when it closes is never held [orig: list_wnd_on_command @ 0x643f19].
		MenuClickLatch latch;
		latch.dropdown_sample(7, false);
		latch.dropdown_sample(7, true);
		CHECK(latch.captured() == (MenuPumpWindow{ 7, kMenuPumpPartDropdown }));
		step(latch, at(2), true, false);
		CHECK(!latch.held(MenuPumpWindow{ 2, 0 }));
		CHECK(!step(latch, at(2), false).valid());
		CHECK(!latch.captured().valid());
	}
	{
		// Another screen: the holds and the capture go, the button as it was (no press edge after it).
		MenuClickLatch latch;
		step(latch, at(1, true), true);
		latch.reset();
		CHECK(!latch.captured().valid() && !latch.held(MenuPumpWindow{ 1, 0 }));
		step(latch, at(2, true), true);
		CHECK(!latch.captured().valid() && latch.held(MenuPumpWindow{ 2, 0 }));
	}
	CHECK(menu_window_captures(mnu::WindowType::Button) && menu_window_captures(mnu::WindowType::CheckBox) &&
	      menu_window_captures(mnu::WindowType::Radio) && menu_window_captures(mnu::WindowType::List) &&
	      menu_window_captures(mnu::WindowType::LanList) && menu_window_captures(mnu::WindowType::Table) &&
	      menu_window_captures(mnu::WindowType::SpinList));
	CHECK(!menu_window_captures(mnu::WindowType::Window) && !menu_window_captures(mnu::WindowType::Static) &&
	      !menu_window_captures(mnu::WindowType::Edit) && !menu_window_captures(mnu::WindowType::MultilineEdit) &&
	      !menu_window_captures(mnu::WindowType::Combo) && !menu_window_captures(mnu::WindowType::Scroll) &&
	      !menu_window_captures(mnu::WindowType::Marquee) && !menu_window_captures(mnu::WindowType::GlbTable) &&
	      !menu_window_captures(mnu::WindowType::RadioEdit) && !menu_window_captures(mnu::WindowType::Gopher));
}

// --- through the compiler and the runtime ----------------------------------------------------------

const char *kMenu = R"(<SCREEN>
	<NAME>CLICKS</NAME>
	<WINDOW type="window" name="MAIN">
		<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
		<WINDOW type="button" name="A">
			<SOUND state="selected" trigger="CLICK_A">menu.lwf</SOUND>
			<POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>200</RIGHT><BOTTOM>140</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="button" name="B">
			<SOUND state="mousein" trigger="OVER_B">menu.lwf</SOUND>
			<SOUND state="selected" trigger="CLICK_B">menu.lwf</SOUND>
			<POSITION><LEFT>300</LEFT><TOP>100</TOP><RIGHT>400</RIGHT><BOTTOM>140</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="static" name="LABEL">
			<POSITION><LEFT>100</LEFT><TOP>200</TOP><RIGHT>200</RIGHT><BOTTOM>240</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="edit" name="NAME">
			<POSITION><LEFT>300</LEFT><TOP>200</TOP><RIGHT>400</RIGHT><BOTTOM>240</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="list" name="LIST">
			<POSITION><LEFT>100</LEFT><TOP>300</TOP><RIGHT>200</RIGHT><BOTTOM>360</BOTTOM></POSITION>
			<MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
			<ITEMS>
				<ITEM value="0">ZERO</ITEM>
				<ITEM value="1">ONE</ITEM>
				<ITEM value="2">TWO</ITEM>
			</ITEMS>
		</WINDOW>
		<WINDOW type="checkbox" name="CHECK">
			<POSITION><LEFT>300</LEFT><TOP>300</TOP><RIGHT>400</RIGHT><BOTTOM>340</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="spinlist" name="SPIN">
			<POSITION><LEFT>500</LEFT><TOP>100</TOP><RIGHT>600</RIGHT><BOTTOM>120</BOTTOM></POSITION>
			<ITEMS>
				<ITEM value="0">LOW</ITEM>
				<ITEM value="1">MID</ITEM>
				<ITEM value="2">HIGH</ITEM>
			</ITEMS>
			<SPINUP><POSITION><LEFT>110</LEFT><TOP>0</TOP><RIGHT>125</RIGHT><BOTTOM>20</BOTTOM></POSITION></SPINUP>
			<SPINDOWN><POSITION><LEFT>-25</LEFT><TOP>0</TOP><RIGHT>-10</RIGHT><BOTTOM>20</BOTTOM></POSITION></SPINDOWN>
		</WINDOW>
		<WINDOW type="scroll" name="VOLUME">
			<POSITION><LEFT>500</LEFT><TOP>200</TOP><RIGHT>700</RIGHT><BOTTOM>220</BOTTOM></POSITION>
			<ORIENTATION>HORIZONTAL</ORIENTATION>
			<APPEARANCE type="color" state="default">303030</APPEARANCE>
			<SHUTTLE type="color" state="default">80FF0000</SHUTTLE>
			<SCROLLUP type="color" state="default">505050</SCROLLUP>
			<SCROLLDOWN type="color" state="default">505050</SCROLLDOWN>
		</WINDOW>
		<WINDOW type="button" name="OFF" DISABLE>
			<POSITION><LEFT>100</LEFT><TOP>500</TOP><RIGHT>200</RIGHT><BOTTOM>540</BOTTOM></POSITION>
		</WINDOW>
	</WINDOW>
</SCREEN>
)";

struct Run {
	mnu::Document doc;
	MenuStateFrame frame;
	MenuRuntime rt;
	std::vector<MenuEvent> events;

	bool open() {
		std::string error;
		if (!mnu::parse(kMenu, doc, error)) return false;
		frame.set_configure([this](const std::string &screen, MenuFrameCompiler &compiler) {
			const mnu::Screen *found = screen.empty() ? doc.first_screen() : doc.find_screen(screen);
			compiler.configure(found);
			return found != nullptr;
		});
		frame.set_clicked([this](int index, int part) { rt.on_widget_clicked(index, part); });
		frame.set_scrolled([this](int index, int value) { rt.on_frame_scroll_value(index, value); });
		rt.set_frame(&frame);
		rt.set_sink([this](const MenuEvent &e) { events.push_back(e); });
		return rt.open_document(&doc, "clicks.mnu", std::string());
	}
	void move(float x, float y, bool down) { rt.process_mouse(x, y, down, 1000); }
	// The activations of a window by NAME since the events were last cleared.
	int activated(const char *name) const {
		int n = 0;
		for (const MenuEvent &e : events)
			if (e.kind == MenuEvent::Kind::WidgetActivated && e.text == name) ++n;
		return n;
	}
	int sounds(const char *trigger) const {
		int n = 0;
		for (const MenuEvent &e : events)
			if (e.kind == MenuEvent::Kind::Sound && e.text2 == trigger) ++n;
		return n;
	}
	const MenuWidgetState *row(const char *name) const {
		return find_frame_widget(frame.state(), rt.frame_index(rt.widget_id(name)));
	}
	bool pressed(const char *name) const {
		const MenuWidgetState *r = row(name);
		return r != nullptr && r->pressed;
	}
	bool hovered(const char *name) const {
		const MenuWidgetState *r = row(name);
		return r != nullptr && r->hovered;
	}
};

constexpr float kBackX = 50, kBackY = 50;   // the backdrop (MAIN)
constexpr float kAX = 150, kAY = 120;       // A
constexpr float kBX = 350, kBY = 120;       // B

// A press on a window that does not capture slid onto a button and let go there clicks the button: the
// backdrop, a STATIC, an EDIT (which keeps the focus its press took) [orig: @ 0x647b14 — the verdict 3
// of the sample before, wherever the press began; their handlers never reach @ 0x65839c].
void test_press_elsewhere_release_on_a_button() {
	Run run;
	CHECK(run.open());
	const float starts[][2] = { { kBackX, kBackY }, { 150, 220 }, { 350, 220 } };
	for (const auto &start : starts) {
		run.move(start[0], start[1], false);
		run.events.clear();
		run.move(start[0], start[1], true);
		run.move(kBX, kBY, true);
		// B is held under the mouse: it draws pressed.
		CHECK(run.pressed("B"));
		run.move(kBX, kBY, false);
		CHECK(run.activated("B") == 1 && run.sounds("CLICK_B") == 1);
		CHECK(run.activated("A") == 0);
	}
	CHECK(run.rt.focused_widget() == run.rt.widget_id("NAME"));
}

// A press on a button let go over another clicks neither, and the other never lights while the first
// holds the capture; off every button nothing; back over the pressed one, its click [orig:
// CWnd_ProcessMouseEvent @ 0x647a88..0x647b02; CButtonWnd_HandleNamedEvent @ 0x65839c / 0x6583ed].
void test_press_on_a_button() {
	Run run;
	CHECK(run.open());
	run.move(kAX, kAY, false);
	run.events.clear();
	run.move(kAX, kAY, true);
	CHECK(run.pressed("A"));
	run.move(kBX, kBY, true);
	CHECK(!run.pressed("A") && !run.hovered("A") && !run.pressed("B") && !run.hovered("B"));
	CHECK(run.sounds("OVER_B") == 0);
	run.move(kBX, kBY, false);
	CHECK(run.activated("A") == 0 && run.activated("B") == 0 && run.sounds("CLICK_B") == 0);
	// Let go, the capture goes: B takes the claim with the button up, its MOUSEIN.
	CHECK(run.hovered("B") && run.sounds("OVER_B") == 1);
	// Off every button: nothing.
	run.move(kAX, kAY, false);
	run.events.clear();
	run.move(kAX, kAY, true);
	run.move(kBackX, kBackY, true);
	run.move(kBackX, kBackY, false);
	CHECK(run.activated("A") == 0 && run.activated("MAIN") == 0);
	// Off and back over it before the release: its click.
	run.move(kAX, kAY, false);
	run.move(kAX, kAY, true);
	run.move(kBackX, kBackY, true);
	run.move(kAX, kAY, true);
	CHECK(run.pressed("A"));
	run.move(kAX, kAY, false);
	CHECK(run.activated("A") == 1 && run.sounds("CLICK_A") == 1);
}

// A LIST's press captures (list_wnd_on_command's tail reaches the button handler @ 0x643f19): its row
// picks on the press; a button it is let go over is not clicked; let go over the list, the list's click.
// A CHECKBOX's press captures too: let go over a button, neither toggles nor clicks.
void test_lists_and_check_boxes_capture() {
	Run run;
	CHECK(run.open());
	const int list = run.rt.widget_id("LIST");
	run.move(150, 330, false);
	run.events.clear();
	run.move(150, 330, true);
	CHECK(run.rt.selected_row(list) == 1 && run.activated("LIST") == 1);
	run.move(kBX, kBY, true);
	run.move(kBX, kBY, false);
	CHECK(run.activated("B") == 0 && run.activated("LIST") == 1);
	run.events.clear();
	run.move(150, 330, true);
	run.move(kBX, kBY, true);
	run.move(150, 350, true);
	run.move(150, 350, false);
	// The press activates and picks, the click (let go over another row) activates again and picks
	// nothing [orig: list_wnd_on_command @ 0x643d0b picks on 0x1000002 / 0x1000004 alone].
	CHECK(run.activated("LIST") == 2 && run.rt.selected_row(list) == 1);
	const int check = run.rt.widget_id("CHECK");
	const bool checked = run.rt.is_widget_checked(check);
	run.events.clear();
	run.move(350, 320, true);
	run.move(kBX, kBY, true);
	run.move(kBX, kBY, false);
	CHECK(run.rt.is_widget_checked(check) == checked && run.activated("B") == 0 && run.activated("CHECK") == 0);
	// And a button's press let go over the check box toggles nothing.
	run.move(kBX, kBY, true);
	run.move(350, 320, true);
	run.move(350, 320, false);
	CHECK(run.rt.is_widget_checked(check) == checked && run.activated("CHECK") == 0);
}

// A double click is two clicks: the second press (WM_LBUTTONDBLCLK, 0x1000004) captures as a press and
// holds the window (its wParam carries MK_LBUTTON) [orig: CButtonWnd_HandleNamedEvent @ 0x6583a1; the
// verdict @ 0x647b53].
void test_double_click() {
	Run run;
	CHECK(run.open());
	run.move(kBX, kBY, false);
	run.events.clear();
	run.move(kBX, kBY, true);
	run.move(kBX, kBY, false);
	run.move(kBX, kBY, true);
	run.move(kBX, kBY, false);
	CHECK(run.activated("B") == 2 && run.sounds("CLICK_B") == 2);
}

// A spin arrow is a button of its own: pressed and let go over it, it steps; let go off it, nothing; a
// press on the list's body steps once on the press, captures the body, and an arrow it is let go over is
// not clicked [orig: CSpinListWnd_HandleEvent @ 0x64c370 — the body's press SelectNext then the button
// handler's capture @ 0x64c43e; the arrows' 0x3000001 @ 0x64c404 / 0x64c428].
void test_spin_arrows() {
	Run run;
	CHECK(run.open());
	const int spin = run.rt.widget_id("SPIN");
	const int start = run.rt.selected_row(spin);
	// UP (610..625): pressed, slid onto the body and let go: nothing.
	run.move(615, 110, false);
	run.move(615, 110, true);
	run.move(550, 110, true);
	run.move(550, 110, false);
	CHECK(run.rt.selected_row(spin) == start);
	// Pressed and let go over it: one step.
	run.move(615, 110, true);
	run.move(615, 110, false);
	const int up = run.rt.selected_row(spin);
	CHECK(up != start);
	// The body: one step on the press; slid onto DOWN (475..490) and let go: no arrow click.
	run.move(550, 110, true);
	const int pressed = run.rt.selected_row(spin);
	CHECK(pressed != up);
	run.move(480, 110, true);
	run.move(480, 110, false);
	CHECK(run.rt.selected_row(spin) == pressed);
}

// A slider's shuttle drags while it holds the mouse, and a button the drag ends over is not clicked
// (the shuttle is a CButtonWnd child that captures) [orig: CScrollWnd_HandleEvent @ 0x64d231].
void test_slider_drag() {
	Run run;
	CHECK(run.open());
	const int volume = run.rt.widget_id("VOLUME");
	run.rt.set_widget_scroll_range(volume, 0, 100, 10, 0);
	// value 0: the shuttle sits at the travel's start, 520..540.
	run.move(530, 210, false);
	run.events.clear();
	run.move(530, 210, true);
	run.move(650, 210, true);
	MenuScrollRangeState range;
	CHECK(run.rt.get_widget_scroll_range(volume, range) && range.value > 0);
	run.move(kBX, kBY, true);
	run.move(kBX, kBY, false);
	CHECK(run.activated("B") == 0 && run.sounds("CLICK_B") == 0);
}

// A DISABLED window never takes the claim, so it is never held or pressed: a press there slid onto a
// button clicks the button [orig: CWnd_IsVisibleInHierarchy @ 0x646290, the gate @ 0x647a27].
void test_disabled_window_is_never_held() {
	Run run;
	CHECK(run.open());
	run.move(150, 520, false);
	run.events.clear();
	run.move(150, 520, true);
	run.move(150, 520, false);
	CHECK(run.activated("OFF") == 0);
	run.move(150, 520, true);
	run.move(kBX, kBY, true);
	run.move(kBX, kBY, false);
	CHECK(run.activated("B") == 1);
}

} // namespace

int main() {
	test_latch();
	test_press_elsewhere_release_on_a_button();
	test_press_on_a_button();
	test_lists_and_check_boxes_capture();
	test_double_click();
	test_spin_arrows();
	test_slider_drag();
	test_disabled_window_is_never_held();
	if (failures) {
		std::printf("menu_click: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_click OK\n");
	return 0;
}
