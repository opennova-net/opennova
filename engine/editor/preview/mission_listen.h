#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/json.h>
#include <editor/model/node.h>
#include <editor/preview/dialog_preview.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/mission_script_run.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/preview_clip_sounds.h>
#include <editor/preview/viewport_overlay.h>
#include <runtime/audio/ambient_channel_pool.h>
#include <runtime/audio/ambient_mixer.h>
#include <runtime/audio/sound_selector.h>

namespace opennova::editor {

class MissionDocument;
class MissionScene;
class ProjectAssetSource;
struct AssetScan;
struct SessionView;

// What a source does now, its `status`: "playing" (a layer holds a channel), "outranked" (audible at the listener,
// but no layer among the loudest the channels take), "out_of_range" (its set's layers stop short of the listener),
// "quiet" (its slot for this hour is empty), "no_set" (its set for this hour, or every set it names, is in no bank the
// game searches), "no_wave" (the project lacks the waves of its set for this hour, or of every set it names).
// One ambient source of the mission: an entity whose item the game updates as an env-sound emitter (`envs`, any pool
// [orig: the class table row @ 0x82abd4 -> Entity_UpdateEnvSoundEmitter @ 0x4a8080]), with the four sets its item
// authors for the hours (audio::envs_slot_sets: morning, day, evening, night), each found in the game's bank order
// (where: `banks`, "" none), and what it does at the listener now.
struct MissionSoundSource {
	NodeId row = 0;
	int64_t item = 0;
	const char *pool = "";
	std::array<std::string, 4> slots;
	std::array<std::string, 4> banks;
	// Each slot's set's reach as its bank's layers give it, the waves aside: the widest falloff and the least proximity
	// radius (metres; 0 none).
	std::array<float, 4> reach = { 0.0f, 0.0f, 0.0f, 0.0f };
	std::array<float, 4> near = { 0.0f, 0.0f, 0.0f, 0.0f };
	PreviewVec3 at;
	int32_t stagger = 0; // its cohort and clock stagger (audio::envs_stagger_slot); -1 not registered (no wave)
	int marker = -1;     // its marker in the mixer (-1: none)
	// Now: the region its eval reads (0 morning .. 3 night) and the crossfade's blend, the set that region's slot names
	// ("" none) and its bank, that set's reach (falloff, min), how far the listener is, the loudest of its layers' mixed
	// volumes (0..255), the channel one of them holds (-1 none).
	int region = 0;
	float blend = 1.0f;
	std::string set;
	std::string bank;
	float falloff = 0.0f, min = 0.0f;
	float distance = 0.0f;
	int32_t volume = 0;
	int channel = -1;
	const char *status = "";
};

// One of the channels the loudest-first mix binds (at most audio::kAmbientMixChannels [orig:
// SoundEmitter_UpdateAndMixTop8 @ 0x5284a0, the channel table @ 0x24D6688]): what it plays (-1: free), and from what.
// `started` moves each time a candidate takes it, as a channel opened again starts its wave from its beginning
// (D-SND-6's transient channels): the device restarts its voice then.
struct MissionSoundChannel {
	int channel = 0;
	int32_t candidate = -1;
	uint64_t started = 0;
	const char *source = ""; // "marker" (an ambient source), "rain" (the rain beside the listener)
	NodeId row = 0;
	std::string set, bank, wave;
	std::string path; // the project's file of the wave ("" none: refused by the pool)
	int layer = 0;
	int32_t volume = 0;          // the mix's, 0..255
	uint32_t pitch_q16 = 0x10000; // the member's times the registration's
	PreviewVec3 at;
	float distance = 0.0f;
};

// What a mission sounds like where its camera stands, as the game plays it (ADR 0046 DI-36): the mission's ambient
// sources (its env-sound entities), the rain beside the listener while the script's weather rains, the thunder of
// the weather's lightning and the script's own sounds, through the runtime's own audio rules over the project's
// files:
// - The sources' registration and the mix are the runtime's AmbientMixer: each source registers its hour's set every
//   8th tick in its cohort, crossfading at the hours' edges (audio::time_of_day_region, the same-set suppress), its
//   layers' slots kept alive in ticks [orig: Entity_UpdateEnvSoundEmitter @ 0x4a8080; Entity_UpdateAllEntities
//   @ 0x4c225a]; the mix culls each live slot past its layer's falloff, computes its volume through the two-radius
//   curve on member 0 and ranks the audible loudest first [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0]; the
//   runtime's AmbientChannelPool binds the loudest eight to channels (incumbents keep theirs, a dropout's stops, an
//   entrant starts its wave anew). Each source's layers as audio::emitter_layers reads them; the sources in the
//   order resolve_envs_markers walks them (markers, items, buildings, people), each resolved one staggered by its
//   place among them (audio::envs_stagger_slot), as MissionAudio registers them in the game.
// - The weather, the rain loops and the script's sounds are MissionScriptRun's: the mission's start and its script
//   run by the engine's own kernel on the clock. While it rains, the rain's two loops register beside the listener as
//   the local player's body registers them (world::rain_ambient_emitters: LPNV_RAIN_L and LPNV_RAIN_R two metres
//   either side at the rain's volume, lifetime 20) into the same mixer, so they rank against the sources.
// - The thunder and the script's sounds are one-shots (fire_sounds): the set found in the game's bank order and
//   planned at its distance (the thunder, a script's `sound`) or at its point (a script's sound at an entity),
//   through the session's sound selector, played by the Shell's clip voices (DI-02's player).
// - The mission's dialogs (DI-32): each Play dialog its events fire registers its dialog on the one dialog channel,
//   which runs the game's own queue once a tick after the tick's events (preview/dialog_preview's DialogChannel over
//   audio::DialogQueue [orig: Dialog_Register @ 0x44d980; Dialog_UpdatePlayback @ 0x44e470]): a dialog's lines, each
//   the wave of its name in the mission's dialog bank's sounds at its dialog volume (audio::resolve_dialog_lines),
//   load one after another, each once the one before has held about twice its wave's length, the channel is free and
//   its own delay has run, the dialogs interleaving in slot order; each line handed to the Shell's clip voices on the
//   tick it loads, its subtitle in its words. A script's voice wave (SSNwave, SSNradio) plays at the scripted voice
//   channel's volume at the listener (world::kScriptVoiceVolume through the distance curve, as
//   world::ScriptVoiceChannel::frame plays it [orig: Wac_PlayScriptedVoiceWave @ 0x4ed688; Audio_UpdateAmbientStream
//   @ 0x4edb22]).
// The listener is the camera's eye, standing in for the player's ears (the rain's body has the eye's height, no
// building over it); the hour the picture's (the options' time, else the mission's start time). Not heard: the
// occlusion the game inflates a distance by (no world stands between them in the picture: DI-04's rule), the music
// (the game context's lead-in and its one GAMINT sting, which the shell's music director plays: the body names the
// pair and what it plays, docs/audio/mus-sbf-re.md), what only a running world raises (a vehicle's engine, a person's
// footsteps, a placed genx item's hum: D-SND-32).
class MissionListen {
public:
	MissionListen();
	~MissionListen();
	MissionListen(const MissionListen &) = delete;
	MissionListen &operator=(const MissionListen &) = delete;

	// The sources followed over the scene and the project's files (the item catalogs the graph resolves each item to,
	// the game's banks, the waves), and the mission's start and script over the document; true when the sources moved.
	bool refresh(const SessionView &view, const MissionScene &scene, const MissionDocument &document,
			const std::string &basename);
	// The clock's tick `tick` reached at `listener` (the camera's eye, the presentation frame) at the hour `hours`: the
	// script and the weather run to it, the sources registered on their cohorts and the rain beside the listener, the
	// mix and the channels made again.
	void play_to(int32_t tick, const PreviewVec3 &listener, double hours);
	// The one-shots heard since the last call (the thunder, the script's sounds), each planned as the game plays it at
	// `listener` with its member picked through `selector` and its voices' volume scaled by `volume`, numbered from
	// `seq`; kept for the wire (the last 16) and returned for the Shell.
	std::vector<ClipSoundFired> fire_sounds(const AssetScan *scan, audio::SoundSelector &selector, uint64_t &seq,
			float volume);
	// Nothing held (Listen off, no mission).
	void close();
	bool open() const { return opened_; }

	const std::vector<MissionSoundSource> &sources() const { return sources_; }
	const MissionSoundSource *source(NodeId row) const;
	// Every channel the pool holds, free or not, by its index.
	const std::vector<MissionSoundChannel> &channels() const { return channels_; }
	const MissionScriptRun &script() const { return script_; }
	const std::vector<ClipSoundFired> &sounds_fired() const { return fired_; }
	int32_t tick() const { return tick_; }
	double hours() const { return hours_; }
	const PreviewVec3 &listener() const { return listener_; }
	// Moves whenever what plays changes (a channel bound, released, its volume moved): the device's cue.
	uint64_t serial() const { return serial_; }
	// What the last play cost (microseconds).
	int64_t step_us() const { return step_us_; }

	// The dialog channel (the game's queue: its slots, the lines they hold, the voices) and the next tick it runs.
	const DialogChannel &dialog_channel() const { return dialog_; }
	int32_t dialog_tick() const { return dialog_tick_; }
	const std::string &dialog_bank() const { return dialog_bank_; }

	// What a source does in words, a line each (the hover's): what it is, its sets for the hours, what plays now.
	std::vector<std::string> source_words(NodeId row) const;
	// The body's `listen` (its options' with it): {on, volume, hours, tick, budget, sources {count, playing, ...},
	// channels, rain, overcast, script, music, sounds_fired, step_us}.
	io::JsonValue to_json(const MissionListenOptions &options) const;
	// An entity's `sound` in the items: {slots, banks, region, set, bank, falloff, min, distance, volume, channel,
	// status}; null for an entity that is no source.
	io::JsonValue source_json(NodeId row) const;

private:
	// One layer that may take a channel: a source's set's layer, or a rain loop's.
	struct Candidate {
		int source = -1; // index into sources_ (-1: the rain)
		const char *kind = "marker";
		std::string set, bank, wave, path;
		int layer = 0;
		uint32_t pitch_q16 = 0x10000;
	};
	struct Item {
		bool defined = false;
		bool envs = false;
		std::array<std::string, 4> slots;
	};
	struct Catalog {
		uint64_t stamp = 0;
		std::unordered_map<int64_t, Item> items;
	};
	const Catalog &catalog_(const SessionView &view, const std::string &file);
	// The set `name` in the game's bank order: its bank (null none) and index.
	const PreviewBank *find_set_(const std::string &name, int32_t &index) const;
	// The layers of a set as candidates, ids from next_candidate_ on (none for a set no bank holds).
	std::vector<audio::AmbientMixer::LayerDesc> describe_(const std::string &set, int source, const char *kind,
			const SessionView &view);
	// The mixer made again over the sources (each resolved one a marker), its clock at the start.
	void remix_();
	void apply_plan_(const audio::AmbientChannelPlan &plan);
	// The dialog `number` (dlg%03i) a Play dialog registers: the dialog of the name in the mission's dialog bank, its
	// lines with their waves (audio::resolve_dialog_lines), onto the channel (DI-32) [orig: Dialog_PlayByIndex
	// @ 0x527ae0 -> Dialog_PlayByName @ 0x44d9f0 -> Dialog_Register @ 0x44d980].
	void register_dialog_(int32_t number);
	// The channel run on through the ticks before `end`, the lines it loads appended to `out`.
	void run_dialog_to_(int32_t end, std::vector<DialogChannel::Loaded> &out);
	// The mission's dialog bank, its sounds and its text, read again where a stamp moved.
	const DialogSources &dialog_sources_now_();
	DialogWave dialog_wave_(const std::string &file);

	bool opened_ = false;
	// The music pair the game opens at a mission's start, and whether the project holds each half.
	std::string music_bank_, music_script_;
	bool music_bank_found_ = false, music_script_found_ = false;
	ClipSoundSources banks_;
	MissionScriptRun script_;
	uint64_t graph_generation_ = 0;
	bool graph_read_ = false;
	std::unordered_map<int64_t, std::string> resolved_; // item id -> its catalog's file ("" none)
	std::map<std::string, Catalog> catalogs_;
	uint64_t followed_scene_ = 0, followed_files_ = 0, followed_graph_ = 0;
	bool followed_ = false;
	std::vector<MissionSoundSource> sources_;
	std::unordered_map<NodeId, size_t> source_index_;
	std::unordered_map<int32_t, Candidate> candidates_;
	std::vector<std::vector<std::vector<audio::AmbientMixer::LayerDesc>>> marker_sets_; // per source, its sets' layers
	std::vector<std::array<int32_t, 4>> marker_keys_; // per source, its slots' sets by index (-1 none)
	std::vector<audio::AmbientMixer::LayerDesc> rain_layers_[2];
	int32_t next_candidate_ = 1;
	std::unique_ptr<audio::AmbientMixer> mixer_;
	audio::AmbientChannelPool pool_;
	std::vector<MissionSoundChannel> channels_;
	std::vector<ClipSoundFired> fired_;
	PreviewVec3 listener_;
	double hours_ = 12.0;
	int32_t tick_ = -1;
	int32_t mixed_tick_ = -1;
	uint64_t serial_ = 0;
	uint64_t started_ = 0;
	int64_t step_us_ = 0;
	// The dialog channel (DI-32): the project's files as the game reads them, the mission's dialog bank and its text,
	// what was read of them and at which stamps, the waves as the device loads them, the channel and its next tick.
	std::shared_ptr<const ProjectAssetSource> files_;
	std::string dialog_bank_, dialog_text_;
	DialogSources dialog_sources_;
	std::string dialog_stamps_;
	std::map<std::string, std::pair<uint64_t, DialogWave>> dialog_waves_;
	DialogChannel dialog_;
	int32_t dialog_tick_ = 0;
};

// The Listen's marks over the picture (the overlay, while Listen is on): each source's set ringed on the ground at its
// falloff radius (and its proximity radius where it has one) and dotted at its place, in the colour of its status
// (playing, outranked, out of range or quiet, no set or no wave), a heard one's over a dark ring; the hovered source's
// ring thicker. The 128 nearest sources at most; those farther than `range` metres from the eye past their ring are
// left out (0: none left out).
void mission_listen_shapes(const MissionListen &listen, const OrbitCamera &camera, int width, int height, NodeId hovered,
		float range, OverlayList &out);
inline constexpr uint32_t kListenPlayingRgb = 0x5AE6A0;
inline constexpr uint32_t kListenOutrankedRgb = 0xFFC83C;
inline constexpr uint32_t kListenSilentRgb = 0x9A9A9A;
inline constexpr uint32_t kListenMissingRgb = 0xFF6A5A;

} // namespace opennova::editor
