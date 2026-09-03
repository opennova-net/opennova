#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>

#include "audio/mission_audio_records.h"
#include "mission/mission_perf_counters.h"
#include "mission/mission_placement_stats.h"
#include "terrain/foliage_frame_stats.h"

namespace godot {

// The world's last-frame probe counters (GameWorld.get_runtime_perf_counters,
// built per call; the former Dictionary of world_device_frame.gd, ADR 0043
// slice G10): the four world-tick legs in microseconds (the whole tick, the
// foliage leg, the session-runtime leg, the audio leg), the nested typed
// reports (the mission root's counters, the foliage frame stats, the
// placement census, the mission audio counters), the placer's live static
// population count, and the instance-uniform geometry estimate. The two
// device reports that are Dictionaries at their producers (the foliage
// dispatcher's and FrameFx's backend reports) ride only the JSON edge.
// Typed record per ADR 0017; to_json_value() is the probe/MCP JSON edge and
// reproduces the former Dictionary shape key for key.
#define RUNTIME_PERF_COUNTER_FIELDS(X) \
	X(tick_us)                         \
	X(foliage_us)                      \
	X(runtime_us)                      \
	X(audio_us)

// Godot reserves this many vec4 values of the global shader buffer per
// geometry instance whose shader declares instance uniforms, visible or not,
// and prints "Too many instances using shader instance variables. Increase
// buffer size in Project Settings." once the buffer_size budget is exhausted
// (16384 instances with the project's setting;
// shader_resource_validation_test.gd pins it). Godot does not expose the live
// allocation, so the estimate sums the retained instance-uniform geometry the
// shell itself owns: the foliage draw pools (FoliageDispatcher), the placer's
// static populations (visible batches plus their shadow twins), and every
// surface instance of every live ObjectModel scene. Terrain patches, water,
// and the per-model shadow twins the placer parents under animated models are
// not counted: read the total as a floor on the allocation, not the exact
// figure.
#define RUNTIME_PERF_ESTIMATE_FIELDS(X) \
	X(estimate_total)                   \
	X(estimate_budget)                  \
	X(estimate_foliage_pool)            \
	X(estimate_static_populations)      \
	X(estimate_object_geometry)

class RuntimePerfCounters : public RefCounted {
	GDCLASS(RuntimePerfCounters, RefCounted)

public:
	static constexpr int INSTANCE_UNIFORM_VALUES_PER_GEOMETRY = 16;

#define RUNTIME_PERF_COUNTER_ACCESSORS(m_name)                  \
	int64_t get_##m_name() const { return m_name##_; }         \
	void set_##m_name(int64_t p_value) { m_name##_ = p_value; }
	RUNTIME_PERF_COUNTER_FIELDS(RUNTIME_PERF_COUNTER_ACCESSORS)
	RUNTIME_PERF_ESTIMATE_FIELDS(RUNTIME_PERF_COUNTER_ACCESSORS)
#undef RUNTIME_PERF_COUNTER_ACCESSORS

	Ref<MissionPerfCounters> get_runtime() const { return runtime_; }
	void set_runtime(const Ref<MissionPerfCounters> &p_value) { runtime_ = p_value; }
	Ref<FoliageFrameStats> get_foliage() const { return foliage_; }
	void set_foliage(const Ref<FoliageFrameStats> &p_value) { foliage_ = p_value; }
	Ref<MissionPlacementStats> get_mission_placement() const { return mission_placement_; }
	void set_mission_placement(const Ref<MissionPlacementStats> &p_value) { mission_placement_ = p_value; }
	Ref<MissionAudioPerf> get_audio() const { return audio_; }
	void set_audio(const Ref<MissionAudioPerf> &p_value) { audio_ = p_value; }
	int get_static_live_populations() const { return static_live_populations_; }
	void set_static_live_populations(int p_value) { static_live_populations_ = p_value; }
	// The two device backend reports (C++ only): they reach GDScript through
	// to_json_value() alone.
	void set_foliage_backend(const Dictionary &p_value) { foliage_backend_ = p_value; }
	void set_framefx(const Dictionary &p_value) { framefx_ = p_value; }

	Dictionary to_json_value() const;

protected:
	static void _bind_methods();

private:
#define RUNTIME_PERF_COUNTER_MEMBER(m_name) int64_t m_name##_ = 0;
	RUNTIME_PERF_COUNTER_FIELDS(RUNTIME_PERF_COUNTER_MEMBER)
	RUNTIME_PERF_ESTIMATE_FIELDS(RUNTIME_PERF_COUNTER_MEMBER)
#undef RUNTIME_PERF_COUNTER_MEMBER
	Ref<MissionPerfCounters> runtime_;
	Ref<FoliageFrameStats> foliage_;
	Ref<MissionPlacementStats> mission_placement_;
	Ref<MissionAudioPerf> audio_;
	int static_live_populations_ = 0;
	Dictionary foliage_backend_;
	Dictionary framefx_;
};

} // namespace godot
