#include "simulation/use_press_row.h"

#include "util/record_bind.h"

#include <godot_cpp/variant/array.hpp>

using namespace godot;
namespace w = opennova::world;

String UsePressRow::get_event() const { return String(w::use_press_event_name(value_.event)); }

String UsePressRow::get_arm() const { return String(w::use_item_arm_name(value_.arm)); }

String UsePressRow::get_outcome() const {
	return String(w::use_toggle_outcome_name(value_.outcome));
}

String UsePressRow::get_scan_gate() const { return String(w::use_scan_gate_name(value_.scan.gate)); }

static Array q16_triple(const int32_t (&words)[3]) {
	Array out;
	for (const int32_t word : words) out.push_back(word);
	return out;
}

Dictionary UsePressRow::to_json_value() const {
	Dictionary out;
	out["seq"] = get_seq();
	out["logic_tick"] = get_logic_tick();
	out["event"] = get_event();
	out["arm"] = get_arm();
	out["has_player"] = get_has_player();
	out["flags"] = get_flags();
	out["engine_flags"] = get_engine_flags();
	out["mounted"] = get_mounted();
	out["has_slot"] = get_has_slot();
	out["slot_current"] = get_slot_current();
	out["slot_next"] = get_slot_next();
	out["allowed"] = get_allowed();
	out["outcome"] = get_outcome();
	out["candidate"] = get_candidate();
	out["vehicle"] = get_vehicle();
	out["seat_index"] = get_seat_index();
	out["bone"] = get_bone();
	out["attach_sent_before"] = get_attach_sent_before();
	out["watch"] = get_watch();
	out["watch_present"] = get_watch_present();
	out["watch_flags"] = get_watch_flags();
	out["watch_engine_flags"] = get_watch_engine_flags();
	out["watch_in_table"] = get_watch_in_table();
	out["watch_table_radius_q16"] = get_watch_table_radius_q16();
	out["watch_in_slice"] = get_watch_in_slice();
	out["slice_count"] = get_slice_count();
	out["scan_gate"] = get_scan_gate();
	out["scan_seats"] = get_scan_seats();
	out["scan_seat_index"] = get_scan_seat_index();
	out["scan_bone"] = get_scan_bone();
	out["scan_reach_q16"] = get_scan_reach_q16();
	out["scan_aim_bam"] = get_scan_aim_bam();
	out["scan_point_q16"] = q16_triple(value_.scan.point_q16);
	out["scan_eye_q16"] = q16_triple(value_.scan.eye_q16);
	out["scan_view_heading"] = get_scan_view_heading();
	out["scan_view_pitch"] = get_scan_view_pitch();
	return out;
}

void UsePressRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, seq)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, logic_tick)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::STRING, event)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::STRING, arm)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::BOOL, has_player)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, flags)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, engine_flags)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::BOOL, mounted)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::BOOL, has_slot)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, slot_current)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, slot_next)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::BOOL, allowed)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::STRING, outcome)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::BOOL, candidate)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, vehicle)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, seat_index)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, bone)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, attach_sent_before)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, watch)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::BOOL, watch_present)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, watch_flags)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, watch_engine_flags)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::BOOL, watch_in_table)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, watch_table_radius_q16)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::BOOL, watch_in_slice)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, slice_count)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::STRING, scan_gate)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, scan_seats)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, scan_seat_index)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, scan_bone)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, scan_reach_q16)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, scan_aim_bam)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, scan_view_heading)
	OPENNOVA_RECORD_READ_ONLY(UsePressRow, Variant::INT, scan_view_pitch)
	ClassDB::bind_method(D_METHOD("to_json_value"), &UsePressRow::to_json_value);
}
