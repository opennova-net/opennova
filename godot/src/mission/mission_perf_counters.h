#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>

namespace godot {

// The mission root's per-frame probe counters (MissionRoot.get_perf_counters,
// built per call): the session frame's tick/sim/present/effects spans, the
// did-tick edge and the catch-up tick count, plus the sim's own runtime
// counters nested under "sim" at the JSON edge (the perf probes and the
// world's get_runtime_perf_counters). Typed record per ADR 0017;
// to_json_value() is the probe/MCP JSON edge.
#define MISSION_PERF_COUNTER_FIELDS(X) \
	X(tick_us)                         \
	X(sim_us)                          \
	X(present_us)                      \
	X(effects_us)

class MissionPerfCounters : public RefCounted {
	GDCLASS(MissionPerfCounters, RefCounted)

public:
#define MISSION_PERF_COUNTER_ACCESSORS(m_name)                  \
	int64_t get_##m_name() const { return m_name##_; }         \
	void set_##m_name(int64_t p_value) { m_name##_ = p_value; }
	MISSION_PERF_COUNTER_FIELDS(MISSION_PERF_COUNTER_ACCESSORS)
#undef MISSION_PERF_COUNTER_ACCESSORS
	bool get_did_tick() const { return did_tick_; }
	void set_did_tick(bool p_value) { did_tick_ = p_value; }
	int get_ticks() const { return ticks_; }
	void set_ticks(int p_value) { ticks_ = p_value; }
	// The sim's Simulation.get_runtime_perf_counters() snapshot; reaches
	// GDScript only through to_json_value() (the "sim" key).
	void set_sim_counters(const Dictionary &p_value) { sim_counters_ = p_value; }

	Dictionary to_json_value() const;

protected:
	static void _bind_methods();

private:
#define MISSION_PERF_COUNTER_MEMBER(m_name) int64_t m_name##_ = 0;
	MISSION_PERF_COUNTER_FIELDS(MISSION_PERF_COUNTER_MEMBER)
#undef MISSION_PERF_COUNTER_MEMBER
	bool did_tick_ = false;
	int ticks_ = 0;
	Dictionary sim_counters_;
};

} // namespace godot
