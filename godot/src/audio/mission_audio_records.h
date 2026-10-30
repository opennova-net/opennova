#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

#include <runtime/audio/ambient_channel_pool.h> // kAmbientMixChannels

// The typed records of the mission audio device (audio/mission_audio, ADR
// 0017): the setup statistics, the per-frame perf counters and the fired
// one-shot the diagnostics ring keeps. The ambient markers, lanes and their
// candidates are the engine's (runtime/audio/ambient_emitters.h).

namespace godot {

// The ambient emitter mix budget (the engine's channel pool carries the witness).
inline constexpr int kMissionAudioMixChannels = opennova::audio::kAmbientMixChannels;

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
	bool is_slot() const { return slot_; }
	void set_slot(bool p_value) { slot_ = p_value; }
	bool is_played() const { return played_; }
	void set_played(bool p_value) { played_ = p_value; }
};

} // namespace godot
