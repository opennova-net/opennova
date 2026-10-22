#include "simulation/use_press_row.h"

#include "util/record_bind.h"

using namespace godot;
namespace w = opennova::world;

String UsePressRow::get_event() const { return String(w::use_press_event_name(value_.event)); }

String UsePressRow::get_arm() const { return String(w::use_item_arm_name(value_.arm)); }

String UsePressRow::get_outcome() const {
	return String(w::use_toggle_outcome_name(value_.outcome));
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
	ClassDB::bind_method(D_METHOD("to_json_value"), &UsePressRow::to_json_value);
}
