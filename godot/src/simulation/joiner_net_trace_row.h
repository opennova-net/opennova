#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/inmatch/joiner_net_trace.h>

#include <cstdint>

namespace godot {

// One joiner net readout record (Simulation::debug_joiner_net_trace; engine
// runtime/inmatch/joiner_net_trace.h, a debug instrument): a typed row (ADR 0017, ADR 0042
// d5) whose loop reads as its name; to_json_value() is the joiner_net_trace probe's JSON edge.
class JoinerNetTraceRow : public RefCounted {
	GDCLASS(JoinerNetTraceRow, RefCounted)

	opennova::inmatch::JoinerNetTraceRecord value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::inmatch::JoinerNetTraceRecord &p_value) { value_ = p_value; }

	int64_t get_seq() const { return value_.seq; }
	int64_t get_frame_tick() const { return value_.frame_tick; }
	int64_t get_client_tick() const { return value_.client_tick; }
	int64_t get_wall_ms() const { return value_.wall_ms; }
	String get_pump_loop() const;
	int64_t get_countdown_before() const { return value_.countdown_before; }
	int64_t get_holdoff_ticks() const { return value_.holdoff_ticks; }
	bool get_block_open() const { return value_.block_open; }
	bool get_uplink_sent() const { return value_.uplink_sent; }
	int64_t get_frames_applied() const { return value_.frames_applied; }
	bool get_quality_folded() const { return value_.quality_folded; }
	int get_fold_fps() const { return value_.fold_fps; }
	bool get_fold_deploy_hold() const { return value_.fold_deploy_hold; }
	bool get_fold_spawn_gate() const { return value_.fold_spawn_gate; }
	bool get_fold_preround() const { return value_.fold_preround; }
	int get_fold_bandwidth() const { return value_.fold_bandwidth; }
	int get_fold_ping() const { return value_.fold_ping; }
	int get_window_quality() const { return value_.window_quality; }
	int get_host_metric() const { return value_.host_metric; }
	int get_level() const { return value_.level; }
	bool get_report_queued() const { return value_.report_queued; }
	int get_report_level() const { return value_.report_level; }

	Dictionary to_json_value() const;
};

} // namespace godot
