#pragma once

#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

#include "audio/ambient_layer.h"

// The typed records of the mission audio orchestrator (audio/mission_audio,
// the former mission_audio.gd inner classes, ADR 0017): the placed marker,
// the physical channel, the candidate binding and ranked row, the dynamic
// emitter lane, the setup statistics, the per-frame perf counters and the
// fired one-shot the diagnostics ring keeps.

namespace godot {

// The ambient emitter mix budget: the engine sorts every in-range emitter voice
// by computed volume each frame and keeps the loudest 8 on real channels
// [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0, channel table @ 0x24D6688].
inline constexpr int kMissionAudioMixChannels = 8;

// One placed ambient marker ("snd:" item): data, not a scene node. The four
// time-of-day slot set names ("" = silent in that region) and, per distinct
// set, the LWF layer descriptors (SoundBank.describe_ambient's AmbientLayer
// rows, the documented layer transport edge), each carrying a stable
// `candidate_id` for the mission's lifetime so an incumbent keeps its channel
// across ranking ticks. Sets keep their insertion order (the mixer's slot keys
// index it).
class MissionAudioMarker : public RefCounted {
	GDCLASS(MissionAudioMarker, RefCounted)

	Vector3 pos_;
	int source_bms_id_ = 0;
	PackedStringArray slot_sets_;
	// The marker's pool-slot nibble: the native mixer derives the walk cohort
	// AND the (slot << 11) Q16-hours clock stagger from it
	// [orig: tick & 7 @ 0x4c225a; (poolHandle & 0xF) << 11 @ 0x408158].
	int stagger_slot_ = 0;
	Vector<String> set_names_;
	Vector<TypedArray<AmbientLayer>> set_layers_;

protected:
	static void _bind_methods();

public:
	Vector3 get_pos() const { return pos_; }
	void set_pos(const Vector3 &p_value) { pos_ = p_value; }
	int get_source_bms_id() const { return source_bms_id_; }
	void set_source_bms_id(int p_value) { source_bms_id_ = p_value; }
	PackedStringArray get_slot_sets() const { return slot_sets_; }
	void set_slot_sets(const PackedStringArray &p_value) { slot_sets_ = p_value; }
	int get_stagger_slot() const { return stagger_slot_; }
	void set_stagger_slot(int p_value) { stagger_slot_ = p_value; }
	// The layer descriptors of one set (replaces an earlier entry of the same
	// name in place, else appends).
	void set_layers(const String &p_set_name, const TypedArray<AmbientLayer> &p_layers);
	PackedStringArray get_set_names() const;
	TypedArray<AmbientLayer> get_layers(const String &p_set_name) const;

	// The C++ walk in insertion order.
	int set_count() const { return set_names_.size(); }
	const String &set_name_at(int p_index) const { return set_names_[p_index]; }
	const TypedArray<AmbientLayer> &layers_at(int p_index) const { return set_layers_[p_index]; }
};

// One of the MIX_CHANNELS physical channels: a reusable player and the
// candidate bound to it (-1 = free).
class MissionAudioChannel : public RefCounted {
	GDCLASS(MissionAudioChannel, RefCounted)

	ObjectID player_id_;
	int candidate_id_ = -1;

protected:
	static void _bind_methods();

public:
	// The player, or null once it was freed from under the channel.
	AudioStreamPlayer3D *get_player() const;
	void set_player(AudioStreamPlayer3D *p_player);
	int get_candidate_id() const { return candidate_id_; }
	void set_candidate_id(int p_value) { candidate_id_ = p_value; }
};

// A live candidate's descriptor and bus, by candidate id: marker layers ride
// the Ambient bus, dynamic emitter layers the SFX bus.
class MissionAudioCandidateBinding : public RefCounted {
	GDCLASS(MissionAudioCandidateBinding, RefCounted)

	Ref<AmbientLayer> descriptor_;
	StringName bus_;

protected:
	static void _bind_methods();

public:
	Ref<AmbientLayer> get_descriptor() const { return descriptor_; }
	void set_descriptor(const Ref<AmbientLayer> &p_value) { descriptor_ = p_value; }
	StringName get_bus() const { return bus_; }
	void set_bus(const StringName &p_value) { bus_ = p_value; }
};

// One ranked mix row the native mixer returned this frame, joined to its
// binding; `resolved_stream` is filled for an entrant that reaches the top
// eight.
class MissionAudioCandidate : public RefCounted {
	GDCLASS(MissionAudioCandidate, RefCounted)

	int candidate_id_ = 0;
	Ref<AmbientLayer> descriptor_;
	StringName bus_;
	Vector3 pos_;
	int vol_ = 0;
	int pitch_q16_ = 0;
	Ref<AudioStreamWAV> resolved_stream_;

protected:
	static void _bind_methods();

public:
	int get_candidate_id() const { return candidate_id_; }
	void set_candidate_id(int p_value) { candidate_id_ = p_value; }
	Ref<AmbientLayer> get_descriptor() const { return descriptor_; }
	void set_descriptor(const Ref<AmbientLayer> &p_value) { descriptor_ = p_value; }
	StringName get_bus() const { return bus_; }
	void set_bus(const StringName &p_value) { bus_ = p_value; }
	Vector3 get_pos() const { return pos_; }
	void set_pos(const Vector3 &p_value) { pos_ = p_value; }
	int get_vol() const { return vol_; }
	void set_vol(int p_value) { vol_ = p_value; }
	int get_pitch_q16() const { return pitch_q16_; }
	void set_pitch_q16(int p_value) { pitch_q16_ = p_value; }
	Ref<AudioStreamWAV> get_resolved_stream() const { return resolved_stream_; }
	void set_resolved_stream(const Ref<AudioStreamWAV> &p_value) { resolved_stream_ = p_value; }
};

// One dynamic emitter lane's live registration ((source lifetime, lane) key):
// the set it plays, its stable candidate ids, and the tick it expires.
class MissionAudioDynamicEmitter : public RefCounted {
	GDCLASS(MissionAudioDynamicEmitter, RefCounted)

	String set_name_;
	PackedInt32Array candidate_ids_;
	int64_t expires_tick_ = 0;

protected:
	static void _bind_methods();

public:
	String get_set_name() const { return set_name_; }
	void set_set_name(const String &p_value) { set_name_ = p_value; }
	PackedInt32Array get_candidate_ids() const { return candidate_ids_; }
	void set_candidate_ids(const PackedInt32Array &p_value) { candidate_ids_ = p_value; }
	int64_t get_expires_tick() const { return expires_tick_; }
	void set_expires_tick(int64_t p_value) { expires_tick_ = p_value; }
};

// The setup statistics: a typed record like the sibling present passes' Stats
// (ADR 0017); to_json_value() is the probe/MCP JSON edge.
#define MISSION_AUDIO_STATS_FIELDS(X)                            \
	X(markers_total, 0)                                          \
	X(markers_resolved, 0)                                       \
	X(banks_loaded, 0)                                           \
	X(ambient_candidates, 0)                                     \
	X(ambient_candidates_validated, 0)                           \
	X(ambient_decode_failures, 0)                                \
	X(physical_channels, 0)                                      \
	X(channel_budget, kMissionAudioMixChannels)                  \
	X(dialogs, 0)

class MissionAudioStats : public RefCounted {
	GDCLASS(MissionAudioStats, RefCounted)

public:
#define MISSION_AUDIO_STATS_ACCESSORS(m_name, m_default)  \
	int get_##m_name() const { return m_name##_; }         \
	void set_##m_name(int p_value) { m_name##_ = p_value; }
	MISSION_AUDIO_STATS_FIELDS(MISSION_AUDIO_STATS_ACCESSORS)
#undef MISSION_AUDIO_STATS_ACCESSORS

	Dictionary to_json_value() const;

protected:
	static void _bind_methods();

private:
#define MISSION_AUDIO_STATS_MEMBER(m_name, m_default) int m_name##_ = m_default;
	MISSION_AUDIO_STATS_FIELDS(MISSION_AUDIO_STATS_MEMBER)
#undef MISSION_AUDIO_STATS_MEMBER
};

// The per-frame counters of the mission audio tick (the world's audio leg,
// the F3 rows and the perf probe): the tick wall time, the marker count, the
// player property writes, the physical pool size and how many channels are
// bound, and the decode failures excluded from the mix. to_json_value() is
// the perf-counter JSON edge.
#define MISSION_AUDIO_PERF_FIELDS(X)   \
	X(tick_us)                         \
	X(markers)                         \
	X(voice_writes)                    \
	X(physical_channels)               \
	X(active_channels)                 \
	X(ambient_decode_failures)

class MissionAudioPerf : public RefCounted {
	GDCLASS(MissionAudioPerf, RefCounted)

public:
#define MISSION_AUDIO_PERF_ACCESSORS(m_name)                    \
	int64_t get_##m_name() const { return m_name##_; }         \
	void set_##m_name(int64_t p_value) { m_name##_ = p_value; }
	MISSION_AUDIO_PERF_FIELDS(MISSION_AUDIO_PERF_ACCESSORS)
#undef MISSION_AUDIO_PERF_ACCESSORS

	Dictionary to_json_value() const;

protected:
	static void _bind_methods();

private:
#define MISSION_AUDIO_PERF_MEMBER(m_name) int64_t m_name##_ = 0;
	MISSION_AUDIO_PERF_FIELDS(MISSION_AUDIO_PERF_MEMBER)
#undef MISSION_AUDIO_PERF_MEMBER
};

// One positional one-shot MissionAudio fired (fire_soundset / slot_soundset):
// the value the recent-fires ring keeps for diagnostics and the GUT pins
// (ADR 0018 read seam; the players themselves live under the audio root).
class FiredSoundset : public RefCounted {
	GDCLASS(FiredSoundset, RefCounted)

	String set_name_;
	Vector3 position_;
	int source_bms_id_ = 0;
	String exclusive_key_; // the every-tick refire slot key (slot sounds only)
	bool slot_ = false;    // slot_soundset (true) vs fire_soundset (false)
	bool played_ = false;  // the bank found the set and spawned a player

protected:
	static void _bind_methods();

public:
	String get_set_name() const { return set_name_; }
	void set_set_name(const String &p_value) { set_name_ = p_value; }
	Vector3 get_position() const { return position_; }
	void set_position(const Vector3 &p_value) { position_ = p_value; }
	int get_source_bms_id() const { return source_bms_id_; }
	void set_source_bms_id(int p_value) { source_bms_id_ = p_value; }
	String get_exclusive_key() const { return exclusive_key_; }
	void set_exclusive_key(const String &p_value) { exclusive_key_ = p_value; }
	bool is_slot() const { return slot_; }
	void set_slot(bool p_value) { slot_ = p_value; }
	bool is_played() const { return played_; }
	void set_played(bool p_value) { played_ = p_value; }
};

} // namespace godot
