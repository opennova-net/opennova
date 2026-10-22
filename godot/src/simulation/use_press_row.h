#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/world/use_press_trace.h>

#include <cstdint>

namespace godot {

// One USE readout record (Simulation::debug_use_press_trace; engine
// runtime/world/use_press_trace.h, a debug instrument): a typed row (ADR 0017,
// ADR 0042 d5) whose enums read as their names; to_json_value() is the
// use_press_trace probe's JSON edge.
class UsePressRow : public RefCounted {
	GDCLASS(UsePressRow, RefCounted)

	opennova::world::UsePressRecord value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::UsePressRecord &p_value) { value_ = p_value; }

	int64_t get_seq() const { return value_.seq; }
	int64_t get_logic_tick() const { return value_.logic_tick; }
	String get_event() const;
	String get_arm() const;
	bool get_has_player() const { return value_.has_player; }
	int64_t get_flags() const { return value_.flags; }
	int64_t get_engine_flags() const { return value_.engine_flags; }
	bool get_mounted() const { return value_.mounted; }
	bool get_has_slot() const { return value_.has_slot; }
	int get_slot_current() const { return value_.slot_current; }
	int get_slot_next() const { return value_.slot_next; }
	bool get_allowed() const { return value_.allowed; }
	String get_outcome() const;
	bool get_candidate() const { return value_.candidate; }
	int get_vehicle() const { return value_.vehicle; }
	int get_seat_index() const { return value_.seat_index; }
	int get_bone() const { return value_.bone; }
	int64_t get_attach_sent_before() const { return value_.attach_sent_before; }

	Dictionary to_json_value() const;
};

} // namespace godot
