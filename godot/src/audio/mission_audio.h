#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <formats/lwf/lwf.h>
#include <runtime/audio/ambient_emitters.h>
#include <runtime/audio/dialog_queue.h>
#include <runtime/audio/mixer.h>
#include <runtime/audio/script_voice_player.h>
#include <runtime/audio/sound_listener.h>
#include <runtime/world/script_voice.h>
#include <runtime/world/script_sounds.h>
#include <runtime/world/sound_emitter_mailbox.h> // SoundEmitterEvent
#include <runtime/world/weather_state.h> // WeatherSoundEvent

#include "audio/mission_audio_records.h"
#include "audio/sound_bank.h"

namespace godot {

class DbfData;
class ItemDatabase;
class MissionData;
class ResourceRoot;
class Simulation;

// The mission audio device (the former mission_audio.gd, ADR 0043 d9; ADR
// 0077's backend) -- and the mission's audio root: setup() parents this node
// under the container; its sounds play on the engine mixer's channels (runtime/audio/mixer.h,
// godot/src/audio/audio_out.h). Loads the global banks
// into a SoundBank and hands the mission's envs markers to the engine's ambient
// emitters (runtime/audio/ambient_emitters.h), which describe each marker's
// time-of-day slot sets BY NAME (items.def soundloop_1..4 =
// morning/day/evening/night, runtime/audio/envs_markers.h), replay the
// entity-attached lanes, run the witnessed emitter mix (two-radius distance
// volumes, region crossfades) and plan the eight physical channels; tick()
// plays that plan on the mixer (AmbientEmitters::play: pans, volumes, doppler).
// Also exposes the PlayWavList action seam and the music/reverb bed. The dialog slots and their line
// timers and the dialog-id resolution are the engine's
// runtime/audio/dialog_queue, the one table the world owns (this node is its
// device: it runs the table's playback tick, plays the wave a line loads and
// says how long it is and whether its voice still plays); the one-shot fires
// ride the sound bank's oneshot_play plan.
//
// The bank chain is the original's: Game_StartMission walks six global name
// slots in order [<exp>L.lwf, <exp>.lwf, gamelocl.lwf, game.lwf, game3.lwf,
// game2.lwf] (name table @ 0x82A5B0, walk @ 0x525443; expansion slots filled
// by Expansion_LoadAssets @ 0x4a495e), and a set name is searched for in
// those slots alone (SoundBank_FindSetByNameAnyBank @ 0x5274f0). The mission
// co-named .lwf is the DIALOG bank's sounds (DialogManager_LoadFromFile
// @ 0x44e7d4, only when the .dbf exists, with a .pwf fallback), read into
// dialog_sounds_ and never searched for a set. The slot table lives in
// engine/runtime/audio (audio/bank_chain.h).
class MissionAudio : public Node3D {
	GDCLASS(MissionAudio, Node3D)

public:
	enum {
		// The ambient emitter mix budget (mission_audio_records.h carries the
		// witness).
		MIX_CHANNELS = kMissionAudioMixChannels,
		// The recent positional one-shots the diagnostics ring keeps.
		RECENT_FIRES = 16,
	};

	MissionAudio();

	// The constructor seam: `resource_root` mounts the banks/waves, `item_db`
	// (nullable) resolves the envs markers. The caller owns the node until
	// setup() parents it.
	static MissionAudio *create(const Ref<ResourceRoot> &p_resource_root,
			const Ref<ItemDatabase> &p_item_db);

	// Load banks, hand the envs markers to the engine's ambient emitters, parent this node under
	// `container`, and apply the reverb bed. `mission_name` is the .bms filename
	// (its basename selects the dialog bank). Returns the setup Stats record.
	Ref<MissionAudioStats> setup(const Ref<MissionData> &p_mission, const String &p_mission_name,
			Node3D *p_container);
	Ref<MissionAudioStats> get_stats() const { return stats_; }
	// The candidate ids the top-eight mixer holds a physical channel for, sorted.
	TypedArray<int64_t> active_ambient_candidate_ids() const;
	// The mixer channel (4..11) playing `candidate_id`, or -1 when the mixer holds
	// none for it.
	int ambient_channel_for_candidate(int p_candidate_id) const;
	// A mixer channel's bytes as the sounds left them (the tests' and probes' read): its volume,
	// pan and play word, and whether it plays a buffer other than the silent one.
	int channel_volume(int p_channel) const;
	int channel_pan(int p_channel) const;
	int64_t channel_play(int p_channel) const;
	bool channel_sounding(int p_channel) const;
	// The wave a candidate's layer plays (its member 0's file as the bank names
	// it), "" for a candidate it does not describe: the ambient_channels probe's
	// read, which the editor's Listen (ADR 0046 DI-36) is compared against.
	String ambient_candidate_wave(int p_candidate_id) const;
	// The "ambience disabled" arm: banks and the mission .DBF still load, no
	// ambient marker resolves (the dialog-vs-ambient probe's silent control).
	void set_ambient_markers_enabled(bool p_enabled);
	// Whether the dialog channel still plays the last loaded line's wave.
	bool is_dialog_voice_playing() const;
	// The dialog channel's handle for the last loaded line (0 none): a new line's is another.
	int64_t dialog_voice_handle() const { return dialog_handle_; }
	// Channel zero's handle for the scripted voice's clip (0 none): a new clip's is another.
	int64_t script_voice_handle() const { return script_voice_.handle(); }
	Ref<MissionAudioPerf> get_perf_counters() const;
	Ref<SoundBank> get_bank() const { return bank_; }
	// The frame's view as the sound layer's listener, before session presentation fires
	// one-shots (engine audio::SoundListener::update): its place, its yaw from its facing, its
	// velocity over the frame's logic ticks.
	void set_listener_view(const Transform3D &p_camera);
	// Apply the portable entity-attached emitter drain. These are keep-alive
	// registrations, not one-shots: the same (source registry lifetime, lane,
	// layer) refreshes in place and competes with placed ambience in retail's one
	// loudest-eight table. Pitch/volume zero is the common keyed clear. The
	// engine's ambient emitters queue them for the clock to replay
	// (engine: runtime/audio/ambient_emitters.h).
	void apply_sound_emitters(const Array &p_events);
	// The native leg the fire pass feeds (the bound form above unwraps the
	// test-authored SoundEmitterRow records into the same rows).
	void apply_sound_emitter_events(const std::vector<opennova::world::SoundEmitterEvent> &p_events);
	// The weather tick's thunder: the THUNDER trigger set played at a distance
	// from the listener along a bearing (a 0..255 turn; 128 = behind the camera)
	// (engine: runtime/world/weather_state.h -- the 24-byte emitter
	// {0x10000, bearing, g_SoundVolumeOption, 0, distance, 0} into
	// SoundBank_PlayTriggerEntries @ 0x75ccd0 on dword_24E0914; sequencer A at
	// 1 m centred @ 0x57ecfb, B at 10 m from behind @ 0x57edc4).
	void play_weather_sounds(const std::vector<opennova::world::WeatherSoundEvent> &p_events,
			const Transform3D &p_camera_xform);
    void play_script_sounds(const std::vector<opennova::world::ScriptSoundEvent> &p_events,
            const Transform3D &p_camera_xform);
	// The audio channel shutdown, as a device: every channel this node owns
	// falls silent (the one-shot pool, the ambient pool with its candidate claims
	// dropped, the dialog and WAC voices), so the ambient mix takes its channels
	// anew on its next pass (engine WorldOutbox::audio_channels_shutdown carries
	// the witness).
	void shutdown_audio_channels();
	// The round end's dialog line stop, as a device: the voice the dialog
	// channel's last loaded line plays on falls silent (engine
	// WorldOutbox::dialog_line_stop carries the witness).
	void stop_dialog_line();
	// The single-player round end's world-sound park: the process's sound-effects option
	// word saved and zeroed, so what opens or updates after falls silent while what sounds
	// plays out (runtime/audio/sound_options.h SoundOptions::park); the release puts the word
	// back, as the mission teardown does (engine EpilogCine::world_sound_parked says when it
	// stands). The frame's audio leg sets it from the world every frame.
	void set_world_sound_parked(bool p_parked);
	bool is_world_sound_parked() const;
	// PlayWavList / event-action seam: fire a one-shot sound set by name at a world
	// position. The .bms action param -> set-name decode is left to the caller (the
	// engine resolves a pre-loaded sound_id handle; the action path plays it at full
	// emitter volume: ActionSlot_PlaySound @0x4010c0 -> Entity_PlaySound3D_FullVolume
	// @0x528e20). One-shot volume snapshots the listener distance at fire time, and
	// the set's cull range gates the fire entirely (engine: runtime/audio/oneshot_play.h).
	// `p_sound_id` is the sim's own-channel reuse key for the source
	// (audio::oneshot_sound_id, carried on ReadyFireSound); 0 = no identity.
	bool fire_soundset(const String &p_name, const Vector3 &p_world_pos, int p_source_bms_id = 0,
			int p_sound_id = 0);
	// A non-positional interface one-shot: the engine's zero-position play used by
	// the weapon-switch/equip deny click -- a 24-byte emitter with header 0x10000,
	// zeroed position, and the interface volume option, routed straight into the
	// trigger-set player [orig: Sound_PlayInterfaceTriggerSet @ 0x527be0 ->
	// SoundBank_PlayTriggerEntries @ 0x75ccd0]. The set NAME comes from the
	// mission-load resolver walking the 36-B {name[32], slot*} table @ 0x82F590
	// across every loaded bank (DialogSystem_Init @ 0x527687/@ 0x5276e6, two
	// passes of SoundBank_FindTriggerByName @ 0x75be90).
	bool ui_soundset(const String &p_name);
	// A body slot sound (footstep/foley/landing/scream) from the sim's per-tick
	// drain: the same full-volume positional one-shot as fire_soundset (engine: runtime/inmatch/client_effects.cpp), with an
	// shares the finite one-shot pool with the other positional triggers.
	bool slot_soundset(const String &p_name, const Vector3 &p_world_pos,
			int p_source_bms_id = 0, int p_sound_id = 0);
	// Play a mission dialog by its PlayWavList id (param1) in the simulation's
	// dialog table, as the BMS action does (Simulation::play_dialog: the dialog
	// bank's dialog registers, each line the wave of its name in the bank's
	// sounds); its lines load on the dialog ticks (advance_dialog_tick). For
	// probes and tools. Returns true if the id resolved to a dialog of the bank
	// and a slot took it; false without a simulation.
	bool play_dialog(int p_wav_id);
	// One dialog playback tick, which the world runs once a logic tick after
	// that tick's PlayWavList dialogs registered: the simulation's dialog
	// table decides which lines load (runtime/audio/dialog_queue
	// DialogQueue::tick), this node plays them and reports each to the co-op
	// broadcast. A frame rendered since the previous dialog tick (tick() marks
	// one) starts a fresh line's hold. Nothing without a simulation.
	void advance_dialog_tick();
	// A co-op dialog line the host sent (world::DialogLineOrder): the
	// engine's resolution (runtime/audio/dialog_queue resolve_dialog_line)
	// picks the clip, which plays at once on a voice of its own, outside the
	// queue. Returns true when a clip spawned.
	bool play_dialog_line(const String &p_dialog_name, int p_line, int p_player_class);
	// Resolve-only (no playback) for tests/diagnostics: the wave file the first
	// line of a dialog id plays that the dialog bank's sounds hold, or "" if none.
	String resolve_dialog_wave(int p_wav_id);
	// Play through the mission's ScriptVoiceChannel. A new line interrupts the
	// previous line independently of the DBF dialog queue. Standalone tooling
	// uses the same interruption rule and tolerates a missing .wav extension.
	// Returns true if the file resolved and played.
	bool play_wac_wave(const String &p_filename);
	// The scripted voice's device follows the channel as it stands (a start's clip at its
	// start gain, a stop), with no anchor reset: after a command, and on every frame.
	void follow_script_voice();
	// The scene frame's voice update: the validation (a released channel completes), the
	// anchor reset, then the stream's distance gain and position. GameWorld's script_voice
	// row runs it on a frame that draws the scene alone; under the status page the busy word
	// holds (engine world::ScriptVoiceChannel::frame carries the witness).
	void update_script_voice();
	// The mixer meter's words, which channel zero's follower stages (audio::Mixer::meter),
	// handed to the world (Simulation::set_audio_out_meter). GameWorld's script_voice row
	// runs it each frame.
	void meter_out();
	// Pump for the mission clock; HHMM like MissionEnvironment.time_of_day.
	void set_time_of_day_hhmm(double p_hhmm);
	// World-driven eval clock: the world tick pushes the sim's logic tick after each
	// session tick batch, and the engine's ambient emitters replay the queued lanes
	// and run the table's staggered cohort walk to it
	// (engine: runtime/audio/ambient_emitters.h AmbientEmitters::advance_to_logic_tick).
	void advance_ticks(int64_t p_logic_tick);
	// Occlusion provider (the Simulation) -- emitter/one-shot distances inflate
	// through the witnessed two-ray LOS so occluded sources sound farther
	// (engine: runtime/audio/ambient_mixer.h). Optional: tests and the menu run
	// without a sim and mix unoccluded.
	void set_simulation(const Ref<Simulation> &p_sim);
	// Test-injection seam: a Callable(listener, source, dist_q16, source_id) -> int
	// occlusion override, consulted by the bank and the mixer only when no
	// Simulation is set. Replaces the deleted duck-typed provider stubs.
	void set_occlusion_override(const Callable &p_override);
	// The per-frame ambient mix pass: the engine's ambient emitters rank the live slots
	// and plan the channels (AmbientEmitters::frame), this node resolves each entrant's
	// wave, the plan plays on the mixer's emitter channels (AmbientEmitters::play), and
	// every channel playing for an entity is re-panned (audio::repan_entity_channels). `delta` free-runs the autonomous 62.5 Hz eval
	// clock only for owners that never push logic ticks (editor idle) -- see
	// docs/audio/lwf-dbf-sound-re.md §driver cadence. The bound form takes a place alone,
	// the listener facing -Z.
	void tick(const Vector3 &p_camera_pos, double p_delta = 0.0);
	void tick_view(const Transform3D &p_camera, double p_delta);
	// Mission unload: undo the reverb, stop and free every voice child, forget
	// the mission state. The owner frees the node itself.
	void teardown();
	// The recent positional one-shots, oldest first (see recent_fires_).
	TypedArray<FiredSoundset> recent_fired_soundsets() const;

protected:
	static void _bind_methods();

private:
	Ref<Simulation> _simulation() const;
	static int _oneshot_sound_id(int p_source_bms_id, int p_sound_id);
	bool _record_fire(const String &p_set_name, const Vector3 &p_world_pos, int p_source_bms_id,
			bool p_slot, bool p_played);
	void _play_listener_relative(const String &p_name, int32_t p_distance_q16, int32_t p_bearing);
	void _attach_under(Node3D *p_container);
	std::vector<opennova::audio::DialogLineRef> _resolve_dialog_lines(int p_wav_id) const;
	// The device half of a line's load: what the wave loader records of it, and with
	// `p_play` its wave on a new voice of the dialog channel (the engine's gate,
	// audio::DialogQueue::LoadLine).
	opennova::audio::DialogClip _load_dialog_line(const opennova::audio::DialogLineRef &p_line,
			bool p_play);
	// A dialog voice of the queue's still plays: its mixer handle validates.
	bool _dialog_voice_playing(uint64_t p_voice) const;
	// A dialog line's wave on the dialog channel (engine audio::dialog_line_params); its handle,
	// 0 for none.
	uint32_t _play_dialog_wave(const opennova::audio::MixWaveRef &p_wave, int p_volume);
	void _install_voice_resolver();
	// The scripted voice let go, its finish reported to the world first when asked.
	void _stop_script_voice(bool p_report_finished);
	opennova::audio::MixWaveRef _resolve_wav(const String &p_filename);
	// An entrant's wave through the bank, the first failure warned.
	opennova::audio::MixWaveRef _resolve_ambient_wave(
			const opennova::audio::AmbientCandidateRef &p_candidate);
	// The SoundBank's hearing of a 3D fire: the listener, and whether it rides the source.
	SoundBank::Hearing _hearing(int p_sound_id) const;
	void _reset_ambient_channels();
	void _reset_mission_playback_state();
	// The engine's ambient counts into the setup Stats record.
	void _publish_ambient_stats();
	void _load_bank(const String &p_lwf_name);
	void _apply_music(int p_music_id);

	Ref<ResourceRoot> resource_root_;
	Ref<ItemDatabase> item_db_;
	// A header-only join defers its envs markers to the streamed world (tick).
	bool world_envs_pending_ = false;
	ObjectID simulation_id_; // occlusion LOS; optional
	// Test-injection seam (Callable(listener, source, dist_q16, source_id) -> int),
	// forwarded to the bank and asked by the ambient mix; production uses the sim.
	Callable occlusion_override_;
	Ref<SoundBank> bank_;
	Ref<DbfData> dbf_; // the mission's dialog bank; null if absent
	// The dialog bank's sounds (<bank>.lwf, else .pwf), loaded with the bank: the
	// waves its lines name. Empty when the mission has no dialog bank.
	opennova::lwf::File dialog_sounds_;
	bool dialog_sounds_loaded_ = false;
	// True while this node has a SceneTree home (setup) -- the audio root
	// exists; teardown clears it.
	bool root_attached_ = false;
	// The engine's ambient emitters: the markers, the lanes, their candidates, the
	// table's clock, the channel plan and its mixer channels (runtime/audio/ambient_emitters.h).
	opennova::audio::AmbientEmitters ambient_{ MIX_CHANNELS };
	bool warned_ambient_decode_failure_ = false;
	// The "ambience disabled" arm (the dialog-vs-ambient probe): banks and the
	// .DBF still load, no marker resolves.
	bool ambient_markers_enabled_ = true;
	Ref<MissionAudioStats> stats_;
	Vector3 last_camera_pos_; // listener at the last tick; INF until first tick
	// The sound layer's listener and the logic tick it was last moved at.
	opennova::audio::SoundListener listener_;
	int64_t logic_tick_ = -1;
	int64_t listener_tick_ = -1;
	// The dialog channel's handle for the last loaded line, and whether a frame has
	// rendered since the last dialog tick (the dialog table is the simulation's
	// world's: runtime/audio/dialog_queue.h).
	uint32_t dialog_handle_ = 0;
	bool dialog_frame_rendered_ = false;
	// Channel zero: the scripted voice (engine audio::ScriptVoicePlayer), or a standalone
	// tool's WAC wave on its own handle.
	opennova::audio::ScriptVoicePlayer script_voice_;
	uint32_t tool_voice_handle_ = 0;
	std::unordered_map<std::string, opennova::audio::MixWaveRef> wac_waves_; // filename(lower) -> wave (or null)
	int64_t perf_tick_us_ = 0;
	int perf_markers_ = 0;
	int perf_voice_writes_ = 0;
	// The last RECENT_FIRES positional one-shots (fire_soundset / slot_soundset),
	// oldest first from recent_fire_head_: a bounded diagnostic ring of plain
	// records the F3 audio rows and the GUT pins read (ADR 0018 read seam,
	// recent_fired_soundsets makes the objects on asking). Teardown empties it.
	struct RecentFire {
		String set_name;
		Vector3 position;
		int source_bms_id = 0;
		bool slot = false;
		bool played = false;
	};
	RecentFire recent_fires_[RECENT_FIRES];
	int recent_fire_head_ = 0;
	int recent_fire_count_ = 0;
};

} // namespace godot
