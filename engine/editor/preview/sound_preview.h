#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/lwf/lwf.h>
#include <runtime/audio/footstep_slot.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/audio/sound_profile.h>
#include <runtime/audio/sound_selector.h>

namespace opennova::editor {

// What the editor plays of a sound set or a sound profile's slot (ADR 0046, the sound lane; DI-02), the
// portable half of the one preview sound player: the set found as the game finds it, each layer's
// member picked as the game picks it, its pitch composed as the game composes it, the volume the
// member's own (a preview is heard at the listener: no distance). The Shell plays the voices; the
// session keeps the selector, so a sequential layer steps on from one play to the next as it does in
// the game. A footstep's slot by the surface under the foot is the seam the animation preview's
// footsteps play through (DI-04: plan_footstep_play).

// A bank the preview can play from: its logical name, its project path and its contents as the
// engine reads them (an open bank document's rows, edits and all, or its file).
struct PreviewBank {
	std::string name;
	std::string path;
	lwf::File file;
};

// The banks of `banks` the game searches for a set by name, in its order: the global chain
// (audio::global_bank_chain over the project's expansion: <exp>L.lwf, <exp>.lwf, gamelocl.lwf,
// game.lwf, game3.lwf, game2.lwf) [orig: Game_StartMission @ 0x525443 over the slot table @ 0x82A5B0;
// SoundBank_FindSetByNameAnyBank @ 0x5274f0, the first bank's first set of the name]. A bank of
// another name (menu.lwf, a mission's own) is no part of the search.
std::vector<const PreviewBank *> chain_banks(const std::vector<PreviewBank> &banks, const std::string &expansion);

// One sound the preview starts: the layer it is for, the bank's wave (its name and the file it names),
// the pitch (Q16, 0x10000 as recorded) and the volume (0..255).
struct PreviewVoice {
	int layer = 0;
	std::string wave;
	std::string file;
	uint32_t pitch_q16 = lwf::kPitchUnityQ16;
	int32_t volume = 255;
};

// What a play comes to: the set and the bank it was found in (none: `found` false, `words` why), the
// voices, and what it plays in words ("FSP_DIRT_L in game.lwf: fs_dirt2.wav at pitch 1.02, volume
// 230"). A play heard at a distance (audio::SetHearing) past the set's range is found but out of range:
// no voice.
struct PreviewPlay {
	bool found = false;
	bool in_range = true;
	std::string set;
	std::string bank;
	std::string bank_path;
	std::vector<PreviewVoice> voices;
	std::string words;
};


// The set named `set`, in the bank named `only` when it is given (a menu's SOUND plays from its own
// bank [orig: Sound_CollectionPlayTrigger @ 0x652de0]), else in the chain's order; each layer the
// listener's view admits picks its member through `selector` and composes the set's and the member's
// pitch [orig: SoundBank_PlayTriggerEntries @ 0x75ccd0; audio::plan_oneshot_at_distance, flat], or at
// the distance `heard` says.
// `view_flags` the listener's view (2 first-person, 4 outside, 6 either: the editor's). A menu's SOUND plays
// through menu::plan_menu_sound instead (editor/preview/menu_sounds).
PreviewPlay plan_set_play(const std::vector<PreviewBank> &banks, const std::string &expansion, const std::string &set,
                          const std::string &only, audio::SoundSelector &selector, uint8_t view_flags = 6,
                          const audio::SetHearing *heard = nullptr);

// A profile's slot played (the profile the game binds `profile` to, audio::find_sound_profile): the set
// the slot names, through plan_set_play's search [orig:
// SoundProfile_ResolveAllTriggers @ 0x528210 resolves each slot's name across the loaded banks]. An
// empty slot plays nothing, said in words ("default's SSRFootGND is empty: the game plays nothing").
PreviewPlay plan_slot_play(const std::vector<audio::SoundProfile> &profiles, const std::string &profile, int slot,
                           const std::vector<PreviewBank> &banks, const std::string &expansion,
                           audio::SoundSelector &selector, uint8_t view_flags = 6,
                           const audio::SetHearing *heard = nullptr);

// The surface a word names ("ground", "snow", "object", "water": audio::FootSurface, the ground a foot
// lands on as the game tests it); false for none.
bool foot_surface_of(const std::string &word, audio::FootSurface &out);
const char *foot_surface_word(audio::FootSurface surface);

// A footstep played as the game plays one (DI-04's seam): the slot audio::footstep_slot_on picks, then
// plan_slot_play.
PreviewPlay plan_footstep_play(const std::vector<audio::SoundProfile> &profiles, const std::string &profile,
                               audio::FootSurface surface, int foot, const std::vector<PreviewBank> &banks,
                               const std::string &expansion, audio::SoundSelector &selector);

} // namespace opennova::editor
