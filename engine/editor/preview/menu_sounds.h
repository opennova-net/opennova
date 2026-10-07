#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/model/value.h>
#include <editor/preview/preview_clip_sounds.h>
#include <editor/preview/sound_preview.h>
#include <runtime/audio/sound_selector.h>

namespace opennova::mnu {
struct Document;
struct Sound;
struct Window;
} // namespace opennova::mnu

namespace opennova::editor {

class MnuDocument;
class ProjectAssetSource;

// A menu's sounds heard in its preview (DI-34; ADR 0046 "Menu sounds in the preview"): the game's mouse over
// the menu's picture (a canvas's, else where a client holds it: the menu viewport's pointer_at and
// pointer_down) runs the game's pump of the windows' sounds (menu::MenuSoundPump over the windows' records):
// MOUSEIN under the mouse, MOUSEOUT where it leaves, SELECTED on a click and MOUSEIN again after it, each the
// window's SOUND row of that state (menu::menu_window_sound). The row's set is found in the bank the row's
// text names alone and played as the menu plays it, each layer at the menu's master volume [orig:
// CWnd_ProcessMouseEvent @ 0x647a00 -> sound_collection_play_trigger @ 0x652de0 -> SoundBank_FindTriggerAndPlay
// @ 0x75d010]. A screen plays nothing of its own: the pump's call is the only one into the menu's bank
// collection (its one caller @ 0x647c59); a screen's MUSICVAR is the music script's.

// The menu viewport's sound options (its options' `sound`).
struct MenuSoundOptions {
	bool mute = false; // the windows' sounds fire and say what they play, and nothing is heard
	bool operator==(const MenuSoundOptions &other) const { return mute == other.mute; }
	bool operator!=(const MenuSoundOptions &other) const { return !(*this == other); }
};
// On the wire: {mute}.
io::JsonValue menu_sound_options_to_json(const MenuSoundOptions &options);
// `json`'s members over `held`, each optional; false, nothing changed, with why, for another member or a
// value of another type.
bool read_menu_sound_options(const io::JsonValue &json, MenuSoundOptions &held, std::string &error);

// The banks a menu's SOUND rows name, each read as the game opens it, by the name the row's text gives as
// written [orig: CUIElement_ParseXMLDefinition @ 0x648ada -> SoundBank_CollectionAddOrRef @ 0x652b40 ->
// SoundBank_OpenFile @ 0x75caa0] (the project's file of the name, an open document standing in for it), again
// only when its stamp moves. A name no file answers, or a file that does not read as a bank, is none: the
// game plays nothing from it.
class MenuSoundBanks {
public:
	// The bank the name names, read now where its stamp moved; null for none.
	const PreviewBank *bank(const ProjectAssetSource &files, const std::string &name);
	// Every bank read, in the order first named (each keeps its place: a sequential layer's pick is keyed by
	// it, as one bank's is in the game).
	const std::vector<PreviewBank> &banks() const { return banks_; }

private:
	struct Held {
		std::string name;
		uint64_t stamp = 0;
		bool read = false;
		size_t index = 0; // its place in banks_
	};
	std::vector<Held> held_;
	std::vector<PreviewBank> banks_;
};

// One window sound the preview fired: the play (`sound`: its seq in the order of every preview sound fired,
// the menu's path, the set the row's TRIGGER names, the bank its text names, its state (played, muted,
// no_bank: the project has no bank of the name or it does not read, missing: the bank has no set of the
// name, silent: no layer has a member, no_wave: the project lacks its waves), what it played in words and the
// voices), and whose it is: the screen, the window's record and NAME, and the pump's state (menu::kSoundMouseIn,
// kSoundMouseOut, kSoundSelected).
struct MenuSoundFired {
	ClipSoundFired sound;
	std::string screen;
	NodeId window = 0;
	std::string name;
	int state = 0;
};
// {seq, sound: MOUSEIN | MOUSEOUT | SELECTED, screen, window, name, set, bank, state, words, voices}.
io::JsonValue menu_sound_fired_to_json(const MenuSoundFired &fired);

// What a window's SOUND row plays, as the menu plays it (plan_set_play over the row's own bank, at the menu's
// master volume, menu::kMenuMasterVolumeDefault), each layer's member picked through `selector` (the
// session's one stream); `mute`, picked and said, nothing heard. Each voice's wave found as the project's file
// the game loads by its name (`scan`, null: none looked for).
MenuSoundFired plan_menu_sound(const mnu::Sound &row, MenuSoundBanks &banks, const ProjectAssetSource &files,
                               const AssetScan *scan, audio::SoundSelector &selector, bool mute);

// What a window plays and when, one line per state it has a row for, in the pump's order (under the mouse,
// clicked, left): the state, the set and the bank (menu::menu_window_sound's row).
struct MenuWindowSound {
	int state = 0;
	std::string set;
	std::string bank;
};
std::vector<MenuWindowSound> menu_window_sounds(const mnu::Window &window);
// "on hover", "on click", "on leaving": when the game plays a state's row.
const char *menu_sound_state_words(int state);
// [{sound: MOUSEIN | SELECTED | MOUSEOUT, when, set, bank}] (the viewport hit's `sounds`).
io::JsonValue menu_window_sounds_to_json(const std::vector<MenuWindowSound> &sounds);

// The window record `window` as the game's reader read the menu were it saved now (the document's saved image,
// held in `image` for as long as the window is read; its screen by the record's row, the window by its
// pre-order index); null for none (not a window, a part, a menu the game would not read).
const mnu::Window *menu_image_window(const MnuDocument &document, const NodeAddress &window,
                                     std::shared_ptr<const mnu::Document> &image);

} // namespace opennova::editor
