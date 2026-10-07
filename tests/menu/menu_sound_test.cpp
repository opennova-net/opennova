// The menu's sound edges (menu/menu_sound.h): which SOUND row a window plays for a state, and
// when the mouse pump plays it, per window, as the game's pump does: MOUSEIN under the mouse
// with the button up unless the window holds it already, MOUSEOUT where it was under the mouse
// with the button up the sample before, SELECTED on its click, which lets the state go so the
// next sample plays MOUSEIN again; a window left or entered with the button held plays nothing;
// a disabled window under the mouse plays nothing; a window the pump does not reach keeps its
// state. The game's runtime (MenuRuntime) and the editor's menu preview (DI-34) play through
// this alone.
// [orig: CWnd_ProcessMouseEvent @ 0x647a00; CUIElement_ParseXMLDefinition @ 0x648120, the
//  SOUND arm @ 0x648a68..0x648ac0; CWnd_IsVisibleInHierarchy @ 0x646290]
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_sound.h>

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

constexpr uint64_t kA = 7, kB = 9;

bool reach_all(uint64_t) { return true; }

// One sample's edges spelled "A:MOUSEIN B:MOUSEOUT" (the keys as letters).
std::string spell(const std::vector<MenuSoundPump::Edge> &edges) {
	std::string out;
	for (const MenuSoundPump::Edge &edge : edges) {
		if (!out.empty()) out += ' ';
		out += edge.key == kA ? "A" : edge.key == kB ? "B" : "?";
		out += ':';
		out += menu_sound_state_token(edge.state);
	}
	return out;
}

// A sample over `claim` (none for 0) with the button `down`, every window reached.
std::string at(MenuSoundPump &pump, uint64_t claim, bool down, bool live = true) {
	std::vector<MenuSoundPump::Edge> edges;
	pump.sample(claim != 0, claim, live, down, reach_all, edges);
	return spell(edges);
}

void test_window_sound() {
	mnu::Window w;
	CHECK(menu_window_sound(w, kSoundMouseIn) == nullptr);
	w.sounds = { mnu::Sound{ "mousein", "FIRST", "menu.lwf" }, mnu::Sound{ "SELECTED", "CLICK", "menu.lwf" },
		mnu::Sound{ "MouseIn", "SECOND", "menu.lwf" }, mnu::Sound{ "mousein", "", "menu.lwf" },
		mnu::Sound{ "hover", "NOT_A_STATE", "menu.lwf" } };
	// The last row of a state with a TRIGGER fills its slot; one with none, and an unknown state, fill none.
	const mnu::Sound *in = menu_window_sound(w, kSoundMouseIn);
	CHECK(in != nullptr && in->trigger == "SECOND");
	const mnu::Sound *selected = menu_window_sound(w, kSoundSelected);
	CHECK(selected != nullptr && selected->trigger == "CLICK");
	CHECK(menu_window_sound(w, kSoundMouseOut) == nullptr);
	CHECK(menu_window_sound(w, kSoundNone) == nullptr);
	CHECK(menu_sound_state_of("mouseout") == kSoundMouseOut && menu_sound_state_of("hover") == kSoundNone);
	CHECK(std::string(menu_sound_state_token(kSoundSelected)) == "SELECTED");
}

void test_hover() {
	MenuSoundPump pump;
	CHECK(at(pump, kA, false) == "A:MOUSEIN");
	CHECK(pump.sound_state(kA) == kSoundMouseIn && pump.verdict(kA) == 2);
	CHECK(at(pump, kA, false).empty()); // held: nothing more while it stays
	// Moving across: the window left first.
	CHECK(at(pump, kB, false) == "A:MOUSEOUT B:MOUSEIN");
	CHECK(pump.sound_state(kA) == kSoundNone);
	CHECK(at(pump, 0, false) == "B:MOUSEOUT");
	CHECK(at(pump, 0, false).empty());
	// A disabled window under the mouse takes no hover.
	CHECK(at(pump, kA, false, false).empty());
	CHECK(at(pump, kA, false) == "A:MOUSEIN");
	CHECK(at(pump, kA, false, false) == "A:MOUSEOUT");
}

void test_click() {
	MenuSoundPump pump;
	CHECK(at(pump, kA, false) == "A:MOUSEIN");
	CHECK(at(pump, kA, true).empty()); // held down: verdict 3, no sound
	CHECK(pump.verdict(kA) == 3);
	// Released over it: the click plays SELECTED, the same sample nothing more...
	CHECK(spell({ pump.click(kA) }) == "A:SELECTED");
	CHECK(at(pump, kA, false).empty());
	// ...and the next with the mouse still there MOUSEIN again [orig: @ 0x647c6e lets SELECTED go].
	CHECK(at(pump, kA, false) == "A:MOUSEIN");
	// A hotkey's click of a window not under the mouse: SELECTED, then MOUSEOUT the sample after
	// (its verdict "under the mouse" from the click).
	CHECK(spell({ pump.click(kB) }) == "B:SELECTED");
	CHECK(at(pump, kA, false).empty());
	CHECK(at(pump, kA, false) == "B:MOUSEOUT");
}

void test_held_button() {
	MenuSoundPump pump;
	// Entered with the button held: nothing, and nothing as it is let go over it but its MOUSEIN.
	CHECK(at(pump, kA, true).empty());
	CHECK(at(pump, kA, false) == "A:MOUSEIN");
	// Left with the button held: no MOUSEOUT, and back with it up, no MOUSEIN (it still holds it).
	CHECK(at(pump, kA, true).empty());
	CHECK(at(pump, 0, true).empty());
	CHECK(at(pump, kA, false).empty());
	CHECK(pump.sound_state(kA) == kSoundMouseIn);
	CHECK(at(pump, 0, false) == "A:MOUSEOUT");
}

void test_unreached() {
	MenuSoundPump pump;
	CHECK(at(pump, kA, false) == "A:MOUSEIN");
	// Its screen hidden: the pump does not reach it, and it keeps its state.
	std::vector<MenuSoundPump::Edge> edges;
	pump.sample(false, 0, false, false, [](uint64_t key) { return key != kA; }, edges);
	CHECK(edges.empty() && pump.sound_state(kA) == kSoundMouseIn && pump.verdict(kA) == 2);
	CHECK(at(pump, 0, false) == "A:MOUSEOUT");
	pump.reset();
	CHECK(pump.sound_state(kA) == kSoundNone && pump.verdict(kA) == 0);
}

} // namespace

int main() {
	test_window_sound();
	test_hover();
	test_click();
	test_held_button();
	test_unreached();
	if (failures != 0) {
		std::printf("menu_sound: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_sound: ok\n");
	return 0;
}
