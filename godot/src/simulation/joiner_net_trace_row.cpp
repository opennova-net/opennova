#include "simulation/joiner_net_trace_row.h"

#include "util/record_bind.h"

using namespace godot;
namespace im = opennova::inmatch;

String JoinerNetTraceRow::get_pump_loop() const {
	return String(im::send_pump_loop_name(value_.pump_loop));
}

Dictionary JoinerNetTraceRow::to_json_value() const {
	Dictionary out;
	out["seq"] = get_seq();
	out["frame_tick"] = get_frame_tick();
	out["client_tick"] = get_client_tick();
	out["wall_ms"] = get_wall_ms();
	out["pump_loop"] = get_pump_loop();
	out["countdown_before"] = get_countdown_before();
	out["holdoff_ticks"] = get_holdoff_ticks();
	out["block_open"] = get_block_open();
	out["uplink_sent"] = get_uplink_sent();
	out["frames_applied"] = get_frames_applied();
	out["quality_folded"] = get_quality_folded();
	out["fold_fps"] = get_fold_fps();
	out["fold_deploy_hold"] = get_fold_deploy_hold();
	out["fold_spawn_gate"] = get_fold_spawn_gate();
	out["fold_preround"] = get_fold_preround();
	out["fold_bandwidth"] = get_fold_bandwidth();
	out["fold_ping"] = get_fold_ping();
	out["window_quality"] = get_window_quality();
	out["host_metric"] = get_host_metric();
	out["level"] = get_level();
	out["report_queued"] = get_report_queued();
	out["report_level"] = get_report_level();
	return out;
}

void JoinerNetTraceRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, seq)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, frame_tick)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, client_tick)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, wall_ms)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::STRING, pump_loop)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, countdown_before)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, holdoff_ticks)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::BOOL, block_open)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::BOOL, uplink_sent)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, frames_applied)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::BOOL, quality_folded)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, fold_fps)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::BOOL, fold_deploy_hold)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::BOOL, fold_spawn_gate)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::BOOL, fold_preround)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, fold_bandwidth)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, fold_ping)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, window_quality)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, host_metric)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, level)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::BOOL, report_queued)
	OPENNOVA_RECORD_READ_ONLY(JoinerNetTraceRow, Variant::INT, report_level)
	ClassDB::bind_method(D_METHOD("to_json_value"), &JoinerNetTraceRow::to_json_value);
}
