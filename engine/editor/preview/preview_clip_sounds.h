#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_preview_rig.h>
#include <editor/preview/sound_preview.h>
#include <runtime/anim/clip_timeline.h>
#include <runtime/audio/sound_profile.h>
#include <runtime/audio/sound_selector.h>

namespace opennova::editor {

class ProjectAssetSource;

// A clip's events heard in the model preview (DI-04; ADR 0046, "A clip's events heard"): as the preview
// clock runs a clip, each tick the clock passes through fires what the game's body fires on that tick,
// through the engine's own rules. The body's channel reads the frame's event word unlerped
// (anim::clip_trigger_at) and its sound block consumes it on its own tick half
// (world::anim_sound_tick: an NPC body on odd ticks, a player body on even); the word plays its foley
// slots at the body's origin and its footsteps at foot level through the slot the surface under the
// feet picks (world::anim_event_sounds over audio::footstep_slot); each slot is the bound profile's
// (audio::item_sound_profile: the item's sound_profile, its sound_profileFemale for a female player,
// "default" where it names none), its set found in the game's bank order and played as the game plays a
// body's slot sound, a 3D one-shot heard at the preview camera (PreviewHearing). The editor's own
// choices beside the game's: the Surface under the feet (the game reads it from the world), the body
// (the paired item's move_function picks it, as the game's class table does), a female player, a
// profile picked where no item pairs the clip, Mute.

// The body whose sound block reads the clip's events: Auto the one the paired item's move_function runs
// (org2 the player body, any other an NPC's), else the one chosen.
enum class ClipSoundBody : uint8_t { Auto, Npc, Player };
const char *clip_sound_body_token(ClipSoundBody body); // "auto", "npc", "player"

// The model viewport's sound options (its options' `sound`).
struct ClipSoundOptions {
	bool mute = false; // the events fire and say what they play, and nothing is heard
	FootSurface surface = FootSurface::Ground; // what is under the feet
	ClipSoundBody body = ClipSoundBody::Auto;
	bool female = false; // a female avatar: a player body plays its item's sound_profileFemale
	std::string profile; // a SndProf.def profile picked ("" the paired item's)
	bool operator==(const ClipSoundOptions &other) const {
		return mute == other.mute && surface == other.surface && body == other.body && female == other.female &&
		       profile == other.profile;
	}
	bool operator!=(const ClipSoundOptions &other) const { return !(*this == other); }
};
// On the wire: {mute, surface (ground, snow, object, water), body (auto, npc, player), female, profile}.
io::JsonValue clip_sound_options_to_json(const ClipSoundOptions &options);
// `json`'s members over `held`, each optional; false, nothing changed, with why, for another member or
// a value of another type or out of its set.
bool read_clip_sound_options(const io::JsonValue &json, ClipSoundOptions &held, std::string &error);

// What a clip's sounds read of the record pairing its animation with its model (PreviewRig's record):
// an item's sound profile names and the move function its body runs, as the game's item parser reads
// them (def::def_parse_items_memory: an item that authors no sound_profileFemale has its sound_profile
// there too).
struct ClipSoundItem {
	bool found = false;
	std::string name;
	std::string sound_profile;
	std::string sound_profile_female;
	std::string move_function;
};

// The project's files a clip's sounds play from, read as the game reads them by name (the project's
// files as the game looks them up, an open document standing in for its file): SndProf.def through the
// game's walk (audio::SoundProfileTable::parse) and the banks the game searches for a set, the global
// chain over the project's expansion (audio::global_bank_chain) [orig: Game_StartMission @ 0x525443 over
// the slot table @ 0x82A5B0]; and the item pairing the clip, from its catalog. Each read again only when
// its stamp moves.
class ClipSoundSources {
public:
	// True when anything it holds moved.
	bool refresh(const ProjectAssetSource &files, const std::string &expansion, const PreviewRig &rig);
	const std::vector<audio::SoundProfile> &profiles() const { return profiles_; }
	const std::vector<PreviewBank> &banks() const { return banks_; }
	const std::string &expansion() const { return expansion_; }
	bool has_profile_file() const { return profile_file_; }
	const ClipSoundItem &item() const { return item_; }

private:
	struct Stamp {
		std::string name;
		uint64_t stamp = 0;
	};
	std::vector<Stamp> stamps_; // SndProf.def's, then each chain bank's, as last read
	std::string expansion_;
	bool profile_file_ = false;
	std::vector<audio::SoundProfile> profiles_;
	std::vector<PreviewBank> banks_;
	std::string item_file_;
	std::string item_record_;
	uint64_t item_stamp_ = 0;
	ClipSoundItem item_;
};

// The profile and the body a clip's sounds play through, each with how it came to be in words.
struct ClipSoundBinding {
	std::string profile; // the bound profile's name ("" none: SndProf.def holds no profile)
	std::string profile_words;
	bool player = false; // the player body (org2), else an NPC's (org1)
	std::string body_words;
};
// `options` over what pairs the clip: a profile picked, else the item's sound_profile (its
// sound_profileFemale for a female player body [orig: Entity_GetProfileSlotSound @0x528300, the female
// byte @0x52831c, a player's alone: D-SND-12]) bound as the game binds it (audio::item_sound_profile);
// the body the item's move_function runs [orig: the class table g_EntityClassPhysicsTable, its row
// "org1" @0x82ac28 -> Entity_UpdateInfantryAI @0x4b9910, "org2" @0x82ac34 ->
// Entity_UpdateInfantryPlayerBody @0x4b40e0], else the one chosen.
ClipSoundBinding clip_sound_binding(const std::vector<audio::SoundProfile> &profiles, const ClipSoundOptions &options,
                                    const ClipSoundItem &item, const PreviewRig &rig);

// The clip playing, as its channel reads its events: a word and a capsule bottom (metres, the body's
// origin above the ground) per record, its frames and then its end pose.
struct ClipSoundTrack {
	std::vector<uint32_t> triggers;
	std::vector<float> bottoms;
};

// An event word the body's sound block reads as the clock runs: the clock's tick (the game's logic tick
// its parity is), the clip's own tick and frame there, the word, and the frame's capsule bottom.
struct ClipEventDue {
	int32_t tick = 0;
	int32_t clip_tick = 0;
	int frame = 0;
	uint32_t word = 0;
	float bottom = 0.0f;
};
// How far behind a run of the clock fires: clip_events_due reads the last this many ticks of a longer run,
// and a model viewport fires nothing for a run longer than it (it was not the one previewed meanwhile, or
// the Shell was held up), going on from where the clock is.
inline constexpr int32_t kClipSoundCatchUpTicks = 62;
// The words read on each tick in (`from`, `to`] of the clock, in order: each tick the body's sound block
// reads (world::anim_sound_tick), the clip's channel at its own tick there (the clock's tick; a repeated
// one-shot's taken again from 0 every `period` ticks, 0 none, whose start reads nothing, as a channel
// started reads nothing before its first step) reading a word (anim::clip_trigger_at).
std::vector<ClipEventDue> clip_events_due(const anim::ClipTimeline &clock, const ClipSoundTrack &track, int32_t period,
                                          int32_t from, int32_t to, bool player_body);

// One sound an event fired: what the editor's wire and the Shell read of it. `seq` the session's order of
// every clip sound fired (moves with each), the animation document's path, the clock's tick and the
// clip's frame, the event word, the slot and the foot (-1 a foley sound), the profile, the set and the
// bank it was found in, its state (played, muted, empty: the slot names no set, missing: no bank the
// game searches holds the set, out_of_range: past the set's range from the camera, silent: no layer
// sounds there, no_profile: SndProf.def holds none, no_wave: the project lacks its waves), what it
// played in words, and the voices (each wave with the project's file it plays, "" where the project
// lacks it, at the pitch and the volume the game's pick gave it).
struct ClipSoundFired {
	struct Voice {
		std::string wave;
		std::string file;
		std::string path;
		uint32_t pitch_q16 = 0;
		int32_t volume = 0;
	};
	uint64_t seq = 0;
	std::string path;
	int32_t tick = 0;
	int frame = 0;
	uint32_t bits = 0;
	int slot = 0;
	int foot = -1;
	bool pressed = false; // a timeline mark pressed (play_sound {frame}), not the clock running
	std::string profile;
	std::string set;
	std::string bank;
	std::string state;
	std::string words;
	std::vector<Voice> voices;
};
io::JsonValue clip_sound_fired_to_json(const ClipSoundFired &fired);

// The listener's view flags of the preview camera: an outside view [the listener's view flags: 2 first
// person, 4 the external modes; audio::layer_matches_listener_view], the preview orbiting the body.
inline constexpr uint8_t kClipSoundListenerView = 4;

// The sounds `due` fires, each planned as the game plays the bound profile's slot (plan_slot_play heard
// at `listener` from the body's origin, or from its feet `due.bottom` below it, in the preview's space),
// its member picked through `selector` (the session's, so the picks step on as the game's one stream
// does); muted or not, the picks are made.
std::vector<ClipSoundFired> plan_clip_event(const ClipEventDue &due, const ClipSoundOptions &options,
                                            const ClipSoundBinding &binding, const ClipSoundSources &sources,
                                            const PreviewVec3 &listener, audio::SoundSelector &selector);

// Each voice's wave as the project's file the game loads by its name (the scan's first file of the name,
// a wave); a sound that would play none of whose waves the project has is no_wave, its words saying
// which it lacks.
void find_clip_sound_waves(ClipSoundFired &fired, const AssetScan &scan);

// What an event word plays under `options`, a line per sound and none picked (the timeline's hover):
// "SSRFootGND: FSP_DIRT_R (game.lwf)", "SSAudio5 is empty: nothing plays", "SSLFootGND's FSP_DIRT_L: no
// bank the game searches holds it".
std::vector<std::string> clip_event_sound_words(uint32_t word, const ClipSoundOptions &options,
                                                const ClipSoundBinding &binding, const ClipSoundSources &sources);

} // namespace opennova::editor
