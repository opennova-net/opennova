#pragma once

// The witnessed menu UI sounds: when a window plays its <SOUND> rows (the pump's
// sound edges, MenuSoundPump), which row a state plays (menu_window_sound), and
// the per-play channel volume and pitch decisions of the LWF set player, pure
// over ints/doubles so the embedder only routes the selected members into its
// audio device [orig: the <SOUND> element play path CWnd_ProcessMouseEvent
// @ 0x647a00 -> collection play @ 0x652de0 -> SoundBank_FindTriggerByName
// @ 0x75be90 -> SoundBank_PlayTriggerEntries @ 0x75ccd0]. Member selection per
// layer rides audio/sound_selector.h; the per-play jitter draws stay an
// accepted divergence (docs/audio/lwf-dbf-sound-re.md). The game's runtime
// (MenuRuntime) and the editor's menu preview (DI-34) play through these alone.
//
// A row plays from the bank its element names and no other: the parse opens
// that bank into the menu's collection, and an open that fails (a missing
// bank, or an element with no name) frees the entry and writes the row no
// index [orig: CUIElement_ParseXMLDefinition @ 0x648ada -> SoundBank_CollectionAddOrRef
// @ 0x652b40, @ 0x652c95..0x652caf], so the one play site finds no bank and
// plays nothing [orig: CWnd_ProcessMouseEvent @ 0x647c59 -> Sound_CollectionPlayTrigger
// @ 0x652de0 -> SoundBank_FindTriggerAndPlay @ 0x75d010]; the embedder plays
// a row's trigger from that bank alone (MenuBankCollection, plan_menu_sound;
// godot/src/mnu/menu_audio is the device).

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <formats/lwf/lwf.h>
#include <runtime/audio/sound_selector.h>

namespace opennova::mnu {
struct Sound;
struct Window;
} // namespace opennova::mnu

namespace opennova::menu {

// One pooled channel per original mixer slot: a set play opens up to eight
// channels, one per layer [orig: the 8-slot open loop @ 0x75ce06].
inline constexpr int kMenuSoundChannels = 8;

// The menu master volume default [orig: CGameMenu+92 initialises 255].
inline constexpr int kMenuMasterVolumeDefault = 255;

// The channel volume for one selected layer member [orig: @ 0x75cf25..
// 0x75cf6e]: a layer with no falloff radius (every shipped menu layer) plays
// at the menu's master volume and the member volume is not consulted; a
// falloff layer scales the member volume by (master+1)/256 toward its clamp
// (the distance curve @ 0x75ca20 evaluated at the UI emitter's distance 0).
inline int menu_channel_volume(int master_volume, int member_volume,
		int clamp_volume, int falloff_radius) {
	if (falloff_radius <= 0) {
		return master_volume;
	}
	int att = (member_volume * (master_volume + 1)) >> 8;
	if (clamp_volume > 0 && att > clamp_volume) {
		att = clamp_volume;
	}
	return att;
}

// Set-level pitch composes multiplicatively with the member pitch (Q16;
// 0xFFFF ~ 1.0) [orig: (member * set) >> 16 @ 0x75c0be]; a degenerate
// product resets to 1.0.
inline double menu_effective_pitch(double member_pitch, double set_pitch) {
	const double pitch = member_pitch * set_pitch;
	return pitch <= 0.01 ? 1.0 : pitch;
}

// The menu's bank collection: one entry per bank a SOUND row names, the names
// compared without case [orig: SoundBank_CollectionAddOrRef @ 0x652b40,
// stricmp], each opened the first time a row names it [orig:
// CUIElement_ParseXMLDefinition @ 0x648ada -> SoundBank_OpenFile @ 0x75caa0].
// A name with no file, or a file that is not a bank, holds no bank: the open's
// failure frees the entry and writes the row no index, so its trigger plays
// nothing and no other bank stands in [orig: @ 0x652c95..0x652caf]; the answer
// stands, the name is not opened again.
class MenuBankCollection {
public:
	// The embedder's open of the bank a name names: false for none.
	using Open = std::function<bool(const std::string &name, lwf::File &out)>;
	// The bank `name` names, opened through `open` the first time it is named;
	// null for an empty name or a bank that did not open. `key` (optional) takes
	// the bank's selection key (SoundSelector::make_key's bank): 1, 2, ... in the
	// order the banks opened, 0 for none.
	const lwf::File *bank(const std::string &name, const Open &open, int32_t *key = nullptr);
	// Every bank forgotten (a new resource root: its files are others).
	void clear();

private:
	struct Entry {
		std::string name;
		bool opened = false;
		int32_t key = 0;
		lwf::File file;
	};
	std::deque<Entry> entries_;
	int32_t next_key_ = 1;
};

// One voice of a SOUND row's play: the member's wave file as the bank records
// it ("" where the member names no single: nothing plays), its channel volume
// (menu_channel_volume) and its pitch scale (menu_effective_pitch).
struct MenuSoundVoice {
	std::string path;
	int volume = 0;
	double pitch = 1.0;
};

// What a SOUND row's TRIGGER plays from the row's bank (`bank_key` its
// collection key): the bank's first set of the name, without case [orig:
// SoundBank_FindTriggerByName @ 0x75be90]; each of its layers with a member
// picks one through `selector` by the layer's flags under the (bank, set, layer)
// key (audio::pick_layer_member), at menu_channel_volume of `master_volume` and
// the member's volume, clamp and the layer's falloff, its pitch
// menu_effective_pitch of the member's and the set's (no jitter draws: the
// accepted divergence above). None for a set the bank lacks or an empty trigger.
std::vector<MenuSoundVoice> plan_menu_sound(const lwf::File &bank, int32_t bank_key,
		const std::string &trigger, int master_volume, audio::SoundSelector &selector);

// The pump's sound states, a <SOUND> row's STATE by number, and the per-state
// slot the parse stores it in [orig: CUIElement_ParseXMLDefinition @ 0x648120,
// the SOUND arm: MOUSEIN 1, MOUSEOUT 2, SELECTED 3 without case; the slot
// written @ 0x648ac0, the mask bit 1 << state @ 0x648a7d]. 0: none.
enum MenuSoundState : int32_t {
	kSoundNone = 0,
	kSoundMouseIn = 1,
	kSoundMouseOut = 2,
	kSoundSelected = 3,
};
// The state a STATE token names (MOUSEIN, MOUSEOUT, SELECTED, without case);
// kSoundNone for any other, which the parse refuses.
int menu_sound_state_of(const std::string &token);
// "MOUSEIN", "MOUSEOUT", "SELECTED"; "" for another.
const char *menu_sound_state_token(int state);

// The row a window plays for `state`: each SOUND row of a state the parse knows,
// with a TRIGGER, writes that state's one slot, so the last of them is the one
// played [orig: @ 0x648a68..0x648ac0]; null when none fills it.
const mnu::Sound *menu_window_sound(const mnu::Window &window, int state);

// The sound edges of the menu's mouse pump, per window [orig: CWnd_ProcessMouseEvent
// @ 0x647a00]. Each window keeps its sound state (+240, 0 or 1 between samples) and
// the pump's verdict of the sample before (+232: 2 under the mouse with the button
// up, 3 held down). Every sample, each window the pump reaches (shown up its chain,
// the open popup's alone while one is open):
// - under the mouse (it took the claim and is live: shown and enabled up its chain,
//   CWnd_IsVisibleInHierarchy @ 0x646290): clicked this sample (released over it, or
//   a hotkey's mark @ 0x647b20), it plays SELECTED (3); the button down, it is held
//   (verdict 3) and plays nothing; else MOUSEIN (1) unless it already holds it
//   [orig: @ 0x647b14..0x647b51];
// - else, under the mouse with the button up the sample before (verdict 2), it plays
//   MOUSEOUT (2) [orig: @ 0x647b8e..0x647b97];
// - a state it does not hold already plays its row, if it has one (the mask
//   @ 0x647bb4..0x647bd7), and is held; MOUSEOUT and SELECTED let go at once (0)
//   [orig: @ 0x647c5e..0x647c6e].
// So a click plays SELECTED and the next sample with the mouse still there MOUSEIN
// again; a window left with the button held plays no MOUSEOUT, nor MOUSEIN when the
// mouse comes back to it; a disabled window under the mouse plays nothing. A window
// is named by a key of the embedder's (the runtime's widget id, the editor's window
// record): one the pump has not met holds nothing.
class MenuSoundPump {
public:
	struct Edge {
		uint64_t key = 0;
		int state = kSoundNone;
	};

	// The window `key` clicked this sample (released over it as the embedder's click
	// rule has it, or its hotkey): SELECTED, its sound state let go and its verdict
	// "under the mouse, button up", so the sample that follows the click finds it
	// done; the sample of the same mouse sample plays nothing more of it.
	Edge click(uint64_t key);
	// One sample of the mouse: `claimed` whether a window took the claim, `claim` its
	// key, `live` whether it is enabled up its chain, `button_down` the left button;
	// `reached` whether the pump reaches a window it holds a state for this sample
	// (one it does not keeps its state, as a hidden window's pump returns at once
	// @ 0x647a21). The edges played, in order (the windows leaving first), appended
	// to `out`.
	void sample(bool claimed, uint64_t claim, bool live, bool button_down,
			const std::function<bool(uint64_t)> &reached, std::vector<Edge> &out);
	// Every window forgotten (a menu opened anew: its windows are new).
	void reset();
	// The sound state `key` holds (0 or 1), and the pump's verdict of its last sample
	// (0 none, 2 under the mouse, 3 held down).
	int sound_state(uint64_t key) const;
	int verdict(uint64_t key) const;

private:
	struct Latch {
		int32_t sound = kSoundNone;
		int32_t verdict = 0;
		bool clicked = false; // clicked since the last sample
	};
	static Edge step_(uint64_t key, Latch &latch, int to);
	std::map<uint64_t, Latch> latches_;
};

} // namespace opennova::menu
