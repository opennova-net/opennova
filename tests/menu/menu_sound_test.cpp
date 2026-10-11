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
#include <formats/lwf/wav_pcm.h>
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

// The menu's bank collection: one entry per name without case, opened once; a name no bank
// answers holds none for good (its trigger plays nothing, no other bank stands in); the keys
// count the banks that opened [orig: SoundBank_CollectionAddOrRef @ 0x652b40].
void test_bank_collection() {
	int opens = 0;
	const MenuBankCollection::Open open = [&opens](const std::string &name, lwf::File &out) {
		++opens;
		if (name == "missing.lwf") return false;
		out = lwf::File();
		lwf::Multi set;
		set.name = name;
		out.multis.push_back(set);
		return true;
	};
	MenuBankCollection banks;
	int32_t key = -1;
	CHECK(banks.bank("", open, &key) == nullptr && key == 0 && opens == 0);
	const lwf::File *menu = banks.bank("Menu.lwf", open, &key);
	CHECK(menu != nullptr && key == 1 && opens == 1 && menu->multis[0].name == "Menu.lwf");
	CHECK(banks.bank("MENU.LWF", open, &key) == menu && key == 1 && opens == 1);
	CHECK(banks.bank("missing.lwf", open, &key) == nullptr && key == 0 && opens == 2);
	CHECK(banks.bank("MISSING.lwf", open, &key) == nullptr && key == 0 && opens == 2);
	CHECK(banks.bank("other.lwf", open, &key) != nullptr && key == 2 && opens == 3);
	banks.clear();
	CHECK(banks.bank("menu.lwf", open, &key) != nullptr && key == 1 && opens == 4);
}

// A SOUND row's play from its own bank: the first set of the trigger's name without case, each
// layer with a member picking one by its flags (a sequential layer steps on from play to play),
// at the menu's channel volume and the member's pitch times the set's; nothing for a set the bank
// lacks; a member naming no single is a voice with no file.
void test_plan_menu_sound() {
	lwf::File bank;
	for (const char *wave : { "one", "two" }) {
		lwf::Single single;
		single.name = wave;
		single.path = std::string("SFX\\MENU\\") + wave + ".wav";
		bank.singles.push_back(single);
	}
	for (uint32_t i = 0; i < 3; ++i) {
		lwf::Sndparm member;
		member.single_index = i; // 2 names no single
		member.pitch_scaled = lwf::kPitchUnityQ16 / 2;
		member.volume = 100;
		member.clamp_volume = 255;
		bank.sndparms.push_back(member);
	}
	lwf::Playlist step;
	step.flags = lwf::kFlagSequential; // no view bits: the menu's play reads none
	step.sndparm_indices = { 0, 1 };
	lwf::Playlist far;
	far.falloff_radius = 10;
	far.sndparm_indices = { 2 };
	lwf::Playlist empty;
	bank.playlists = { step, far, empty };
	lwf::Multi set;
	set.name = "CLICK";
	set.pitch_base = 2 * lwf::kPitchUnityQ16;
	set.playlist_indices = { 0, 1, 2 };
	bank.multis.push_back(set);

	audio::SoundSelector selector;
	const std::vector<MenuSoundVoice> first = plan_menu_sound(bank, 1, "click", kMenuMasterVolumeDefault, selector);
	CHECK(first.size() == 2);
	if (first.size() == 2) {
		// No falloff: the master volume; the pitch 0.5 x 2.
		CHECK(first[0].path == "SFX\\MENU\\one.wav" && first[0].volume == 255 && first[0].pitch == 1.0);
		// A falloff layer scales the member's volume: (100 * 256) >> 8.
		CHECK(first[1].path.empty() && first[1].volume == 100);
	}
	const std::vector<MenuSoundVoice> second = plan_menu_sound(bank, 1, "CLICK", 128, selector);
	CHECK(second.size() == 2 && second[0].path == "SFX\\MENU\\two.wav" && second[0].volume == 128 &&
	      second[1].volume == 50);
	CHECK(plan_menu_sound(bank, 1, "NONE", 255, selector).empty());
	CHECK(plan_menu_sound(bank, 1, "", 255, selector).empty());
	// A product of 0, or under 0.01, is the voice's play factor as it is, never 1: the mixer plays
	// a factor whose step is 0 at its least step (lwf::wave_pitch_scale, D-SND-53).
	bank.sndparms[0].pitch_scaled = 0;
	bank.multis[0].pitch_base = 0x8000;
	const std::vector<MenuSoundVoice> zero = plan_menu_sound(bank, 1, "CLICK", 255, selector);
	CHECK(zero.size() == 2 && zero[0].pitch == 0.0);
	bank.sndparms[1].pitch_scaled = 1000;
	const std::vector<MenuSoundVoice> small = plan_menu_sound(bank, 1, "CLICK", 255, selector);
	CHECK(small.size() == 2 && small[0].pitch == 1000.0 / 65536.0 * 0.5);
	// The product is one truncated Q16 word [orig: @ 0x75cef3..0x75cefd]: 127 x 0x8000 >> 16 is 63,
	// not 63.5, so a 44.1 kHz wave's step is 0 and it plays at the least step, as retail's does; the
	// fraction kept would round to 64, a step of 1, and play at its own 42.7 Hz.
	bank.sndparms[0].pitch_scaled = 127;
	const std::vector<MenuSoundVoice> edge = plan_menu_sound(bank, 1, "CLICK", 255, selector);
	CHECK(edge.size() == 2 && edge[0].pitch == 63.0 / 65536.0);
	CHECK(lwf::wave_pitch_scale(lwf::kPitchUnityQ16, 44100, 44100, edge[0].pitch) == (44100.0 / 512.0) / 44100.0);
}

int main() {
	test_bank_collection();
	test_plan_menu_sound();
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
