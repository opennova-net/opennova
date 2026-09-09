#pragma once

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <runtime/audio/dialog_queue.h>
#include <runtime/world/script_voice.h>
#include <runtime/world/script_sounds.h>
#include <runtime/world/sound_emitter_mailbox.h> // SoundEmitterEvent
#include <runtime/world/weather_state.h> // WeatherSoundEvent

#include "audio/ambient_layer.h"
#include "audio/ambient_mixer.h"
#include "audio/mission_audio_records.h"
#include "audio/sound_bank.h"

namespace godot {

class DbfData;
class ItemDatabase;
class MissionData;
class ResourceRoot;
class Simulation;

// Runtime mission audio orchestrator (the former mission_audio.gd, ADR 0043
// d9) -- and the mission's audio root: setup() parents this node under the
// container, and the physical ambient channels, the one-shot voices, the
// dialog voice and the WAC voice are its children. Loads the mission's
// co-named .LWF + the global banks into a SoundBank, resolves each placed
// envs-class entity's time-of-day slot sets BY NAME (items.def soundloop_1..4
// = morning/day/evening/night [orig: Entity_UpdateEnvSoundEmitter @ 0x4a8080];
// the engine is name-keyed -- see docs/audio/lwf-dbf-sound-re.md), and
// retains each layer as lightweight candidate data. tick(camera_pos) runs the
// witnessed ambient emitter mix: per-voice two-radius distance volumes, region
// crossfades, and an eight-player physical channel pool [orig:
// SoundEmitter_UpdateAndMixTop8 @ 0x5284a0]. Also exposes the PlayWavList
// action seam and the music/reverb bed. The serialized dialog queue, the
// dialog-id resolution and the WAC voice channel rule are the engine's
// runtime/audio/dialog_queue; the one-shot fires ride the sound bank's
// oneshot_play plan.
//
// Bank chain vs the original: Game_StartMission walks six global name slots in
// order [<exp>L.lwf, <exp>.lwf, gamelocl.lwf, game.lwf, game3.lwf, game2.lwf]
// (name table @ 0x82A5B0, walk @ 0x525443; expansion slots filled by
// Expansion_LoadAssets @ 0x4a495e), and the mission co-named .lwf is loaded
// separately as the DIALOG bank (DialogManager_LoadFromFile @ 0x44e7d4, only
// when the .dbf exists, with a .pwf fallback). We load one merged chain with
// the co-named bank first (it carries the dialog voices) then the global
// slots in the engine's order -- the slot table lives in engine/runtime/audio
// (audio/bank_chain.h).
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

	// Load banks, describe ambient marker candidates, parent this node under
	// `container`, and apply the reverb bed. `mission_name` is the .bms filename
	// (its basename selects the co-named .LWF). Returns the setup Stats record.
	Ref<MissionAudioStats> setup(const Ref<MissionData> &p_mission, const String &p_mission_name,
			Node3D *p_container);
	Ref<MissionAudioStats> get_stats() const { return stats_; }
	// The candidate ids the top-eight mixer holds a physical channel for, sorted.
	TypedArray<int64_t> active_ambient_candidate_ids() const;
	// The physical channel playing `candidate_id`, or null when the mixer holds
	// none for it.
	AudioStreamPlayer3D *ambient_player_for_candidate(int p_candidate_id) const;
	// Read/drive seams (ADR 0018): tests and diagnostics go through these, never
	// the private fields. set_markers injects fully-described Marker records so
	// the mix tick can be driven without a mission. `container` supplies a
	// SceneTree home for the physical test channels.
	void set_markers(const Array &p_markers, Node3D *p_container = nullptr);
	// The "ambience disabled" arm: banks and the mission .DBF still load, no
	// ambient marker resolves (the dialog-vs-ambient probe's silent control).
	void set_ambient_markers_enabled(bool p_enabled);
	AudioStreamPlayer *dialog_voice() const;
	Ref<MissionAudioPerf> get_perf_counters() const;
	Ref<SoundBank> get_bank() const { return bank_; }
    // Capture the frame's listener before session presentation fires one-shots.
    void set_listener_position(const Vector3 &p_position) { last_camera_pos_ = p_position; }
	// Apply the portable entity-attached emitter drain. These are keep-alive
	// registrations, not one-shots: the same (source registry lifetime, lane,
	// layer) refreshes in place and competes with placed ambience in retail's one
	// loudest-eight table. Pitch/volume zero is the common keyed clear.
	// [orig: SoundEmitter_RegisterSetLayers @0x528340;
	// SoundEmitter_ClearByEntityAndSlot @0x527a50]
	void apply_sound_emitters(const Array &p_events);
	// The native leg the fire pass feeds (the bound form above unwraps the
	// test-authored SoundEmitterRow records into the same rows).
	void apply_sound_emitter_events(const std::vector<opennova::world::SoundEmitterEvent> &p_events);
	// The weather tick's thunder: the THUNDER trigger set played at a distance
	// from the listener along a bearing (a 0..255 turn; 128 = behind the camera)
	// (retail Sound_PlayTriggerSetScaled @ 0x527b90 -- the 24-byte emitter
	// {0x10000, bearing, g_SoundVolumeOption, 0, distance, 0} into
	// SoundBank_PlayTriggerEntries @ 0x75ccd0 on dword_24E0914; sequencer A at
	// 1 m centred @ 0x57ecfb, B at 10 m from behind @ 0x57edc4).
	void play_weather_sounds(const std::vector<opennova::world::WeatherSoundEvent> &p_events,
			const Transform3D &p_camera_xform);
    void play_script_sounds(const std::vector<opennova::world::ScriptSoundEvent> &p_events,
            const Transform3D &p_camera_xform);
    void reset_oneshot_playback();
	// PlayWavList / event-action seam: fire a one-shot sound set by name at a world
	// position. The .bms action param -> set-name decode is left to the caller (the
	// engine resolves a pre-loaded sound_id handle; the action path plays it at full
	// emitter volume: ActionSlot_PlaySound @0x4010c0 -> Entity_PlaySound3D_FullVolume
	// @0x528e20). One-shot volume snapshots the listener distance at fire time, and
	// the set's cull range gates the fire entirely [orig: Sound_Play3DPositional
	// @ 0x527cb0; cull @ 0x527cd1].
	bool fire_soundset(const String &p_name, const Vector3 &p_world_pos, int p_source_bms_id = 0);
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
	// drain: the same full-volume positional one-shot as fire_soundset [orig:
	// Entity_PlaySound3D_FullVolume @ 0x528e20 -- emitter volume 255], with an
	// optional exclusive key for the every-tick refire slots (chute flap/freefall).
	bool slot_soundset(const String &p_name, const Vector3 &p_world_pos,
			const String &p_exclusive_key = String(), int p_source_bms_id = 0);
	// Enqueue a mission dialog by its PlayWavList id (param1): the engine's
	// resolution (runtime/audio/dialog_queue resolve_dialog_sets) then the
	// serialized queue, pumped here by spawning one voice at a time. Returns
	// true if the id resolved to at least one playable set.
	bool play_dialog(int p_wav_id);
	// Resolve-only (no playback) for tests/diagnostics: the first set name a dialog id
	// maps to that the loaded banks actually contain, or "" if none.
	String resolve_dialog_set(int p_wav_id);
	// Play through the mission's ScriptVoiceChannel. A new line interrupts the
	// previous line independently of the DBF dialog queue. Standalone tooling
	// uses the same interruption rule and tolerates a missing .wav extension.
	// Returns true if the file resolved and played.
	bool play_wac_wave(const String &p_filename);
	void sync_script_voice();
	// Pump for the mission clock; HHMM like MissionEnvironment.time_of_day.
	void set_time_of_day_hhmm(double p_hhmm);
	// World-driven eval clock: the world tick pushes the sim's logic tick after each
	// session tick batch, and the native mixer runs the witnessed staggered cohort
	// walk for the elapsed ticks -- each placed marker re-evaluates every 8th 62.5 Hz
	// tick [orig: Entity_UpdateAllEntities @ 0x4c225a pool-2 walk;
	// Entity_UpdateEnvSoundEmitter @ 0x4a8080]. The first world-driven tick rebases the
	// clock so an editor session that free-ran before Play keeps its slot lifetimes.
	void advance_ticks(int64_t p_logic_tick);
	// Occlusion provider (the Simulation) -- emitter/one-shot distances inflate
	// through the witnessed two-ray LOS so occluded sources sound farther [orig:
	// Sound_ApplyOcclusionDistance @ 0x529970]. Optional: tests and the menu run
	// without a sim and mix unoccluded.
	void set_simulation(const Ref<Simulation> &p_sim);
	// Test-injection seam: a Callable(listener, source, dist_q16, source_id) -> int
	// occlusion override, consulted by the bank and the mixer only when no
	// Simulation is set. Replaces the deleted duck-typed provider stubs.
	void set_occlusion_override(const Callable &p_override);
	// The per-frame ambient mix pass [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0,
	// called once per render frame from the Game Loop render callback @ 0x521341]:
	// the native mixer (engine/runtime/audio AmbientMixer) ranks the LIVE emitter slots --
	// registered at the witnessed staggered tick cadence via advance_ticks -- through
	// the two-radius member-0 curve with occlusion, and this node binds the loudest
	// MIX_CHANNELS to reusable players (D-SND-6/D-SND-8 reimpl territory). Stable
	// candidate IDs let selected incumbents continue while an entrant restarts,
	// matching transient registration. `delta` free-runs the autonomous 62.5 Hz eval
	// clock only for owners that never push logic ticks (editor idle) -- see
	// docs/audio/lwf-dbf-sound-re.md §driver cadence (D-SND-16, ported).
	void tick(const Vector3 &p_camera_pos, double p_delta = 0.0);
	// Mission unload: undo the reverb, stop and free every voice child, forget
	// the mission state. The owner frees the node itself.
	void teardown();
	// The recent positional one-shots, oldest first (see recent_fires_).
	TypedArray<FiredSoundset> recent_fired_soundsets() const;

	// The bound signal target: the active dialog voice finished.
	void _on_dialog_finished();

protected:
	static void _bind_methods();

private:
	Ref<Simulation> _simulation() const;
	bool _record_fire(const String &p_set_name, const Vector3 &p_world_pos, int p_source_bms_id,
			const String &p_exclusive_key, bool p_slot, bool p_played);
    void _play_listener_relative(const String &p_name, int32_t p_distance_q16,
            int32_t p_bearing, const Transform3D &p_camera_xform);
	void _attach_under(Node3D *p_container);
	void _free_voice_nodes();
	std::vector<std::string> _resolve_dialog_sets(int p_wav_id) const;
	void _pump_dialog_queue();
	AudioStreamPlayer *_dialog_voice_node() const;
	AudioStreamPlayer *_wac_voice_node() const;
	void _on_script_voice_finished(int64_t p_serial, int64_t p_player_id);
	void _stop_script_voice(bool p_report_finished);
	Ref<AudioStreamWAV> _resolve_wav(const String &p_filename);
	Ref<AudioStreamWAV> _resolve_candidate_stream(const Ref<AmbientLayer> &p_descriptor);
	Ref<AudioStreamWAV> _validate_candidate_stream(int p_candidate_id,
			const Ref<AmbientLayer> &p_descriptor);
	Ref<MissionAudioChannel> _free_or_new_channel();
	void _stop_all_ambient_channels();
	void _reset_mission_playback_state();
	void _feed_mixer();
	void _flush_sound_emitters(int64_t p_final_tick);
	void _forget_dynamic_emitter(const String &p_key);
	int _allocate_dynamic_candidate_id();
	void _prune_dynamic_emitter_states();
	void _release_retired_candidate_ids();
	static double _candidate_pitch_scale(const Ref<MissionAudioCandidate> &p_candidate);
	static double _hhmm_to_hours(double p_hhmm);
	void _load_bank(const String &p_lwf_name);
	void _apply_reverb(int p_reverb_id);
	void _apply_music(int p_music_id);

	Ref<ResourceRoot> resource_root_;
	Ref<ItemDatabase> item_db_;
	ObjectID simulation_id_; // occlusion LOS; optional
	// Test-injection seam (Callable(listener, source, dist_q16, source_id) -> int),
	// forwarded to the bank and every fresh mixer; production uses the sim.
	Callable occlusion_override_;
	Ref<SoundBank> bank_;
	Ref<DbfData> dbf_; // mission co-named dialog bank; null if absent
	// True while this node has a SceneTree home (setup / set_markers with a
	// container) -- the audio root exists; teardown clears it.
	bool root_attached_ = false;
	Vector<Ref<MissionAudioMarker>> markers_;
	// The native emitter system (engine/runtime/audio AmbientMixer): staggered tick&7 marker
	// eval/registration on the logic-tick clock + the per-frame live-slot ranking
	// (docs/audio/lwf-dbf-sound-re.md §driver cadence, D-SND-16). This node keeps the
	// per-candidate descriptors for stream resolution and the voice binding below.
	Ref<AmbientMixer> mixer_;
	HashMap<int, Ref<MissionAudioCandidateBinding>> candidate_lookup_; // candidate_id -> binding
	// Portable systems emit short-lived registrations before the GameWorld audio
	// pass advances the mixer clock. Queue them so catch-up ticks age the previous
	// registrations first, then the newest per-tick refresh lands at the current
	// clock [orig: SoundEmitter_Register @0x529270 before the render-frame
	// SoundEmitter_UpdateAndMixTop8 @0x5284a0].
	std::vector<opennova::world::SoundEmitterEvent> queued_sound_emitters_;
	// "source_spawn_id:lane" -> DynamicEmitter. Candidate IDs remain stable across
	// per-tick refreshes so an incumbent physical channel does not restart; a set
	// change or explicit clear retires the old IDs.
	HashMap<String, Ref<MissionAudioDynamicEmitter>> dynamic_emitter_states_;
	// Latched once a world-driven logic tick arrives (advance_ticks): the world tick owns
	// the eval clock; until then tick(delta) free-runs an autonomous 62.5 Hz clock
	// (editor-idle owners -- the weather world-driven/autonomous split).
	bool world_driven_ticks_ = false;
	int64_t world_driven_tick_offset_ = 0;
	Vector<Ref<MissionAudioChannel>> channels_; // at most MIX_CHANNELS
	int next_candidate_id_ = 1;
	Vector<int> free_candidate_ids_;
	Vector<int> retired_candidate_ids_;
	HashSet<int> failed_candidate_ids_;
	HashSet<int> validated_candidate_ids_;
	bool warned_ambient_decode_failure_ = false;
	// The "ambience disabled" arm (the dialog-vs-ambient probe): banks and the
	// .DBF still load, no marker resolves.
	bool ambient_markers_enabled_ = true;
	Ref<MissionAudioStats> stats_;
	double time_of_day_hhmm_ = 1200.0; // HHMM like MissionEnvironment.time_of_day; noon default
	Vector3 last_camera_pos_; // listener at the last tick; INF until first tick
	// The serialized dialog playback (engine: runtime/audio/dialog_queue.h) and
	// the voice its active line plays on.
	opennova::audio::DialogQueue dialog_queue_;
	ObjectID dialog_voice_id_;
	// Standalone preview player; mission script ownership lives in World.
	ObjectID wac_voice_id_;
	ObjectID script_voice_id_;
	opennova::world::ScriptVoiceChannel::Frame script_voice_frame_;
	HashMap<String, Ref<AudioStreamWAV>> wac_wav_cache_; // filename(lower) -> stream (or null)
	int64_t perf_tick_us_ = 0;
	int perf_markers_ = 0;
	int perf_voice_writes_ = 0;
	// The last RECENT_FIRES positional one-shots (fire_soundset / slot_soundset),
	// oldest first: a bounded diagnostic ring the F3 audio rows and the GUT pins
	// read (ADR 0018 read seam); the players themselves live under this node.
	Vector<Ref<FiredSoundset>> recent_fires_;
};

} // namespace godot
