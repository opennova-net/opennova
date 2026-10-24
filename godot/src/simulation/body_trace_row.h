#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/world/body_trace.h>

#include <cstdint>

namespace godot {

// One body readout record (Simulation::debug_body_trace; engine runtime/world/body_trace.h, a
// debug instrument): a typed row (ADR 0017, ADR 0042 d5) whose event reads as its name;
// to_json_value() is the body_trace probe's JSON edge.
class BodyTraceRow : public RefCounted {
	GDCLASS(BodyTraceRow, RefCounted)

	opennova::world::BodyTraceRecord value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::BodyTraceRecord &p_value) { value_ = p_value; }

	int64_t get_seq() const { return value_.seq; }
	int64_t get_logic_tick() const { return value_.logic_tick; }
	String get_event() const;
	int get_handle() const { return value_.handle; }
	bool get_present() const { return value_.present; }
	PackedInt32Array get_pos() const;
	PackedInt32Array get_vel() const;
	int64_t get_flags() const { return value_.flags; }
	int64_t get_engine_flags() const { return value_.engine_flags; }
	int get_health() const { return value_.health; }
	bool get_mounted() const { return value_.mounted; }
	int get_mount_target() const { return value_.mount_target; }
	int get_mount_seat() const { return value_.mount_seat; }
	int get_mount_bone() const { return value_.mount_bone; }
	int get_ground_target() const { return value_.ground_target; }
	int get_collision_model() const { return value_.collision_model; }
	int get_probe_z() const { return value_.probe_z; }
	int get_other() const { return value_.other; }
	int get_hit_type() const { return value_.hit_type; }
	int get_force_type() const { return value_.force_type; }
	PackedInt32Array get_source() const;
	int get_damage() const { return value_.damage; }
	int get_health_before() const { return value_.health_before; }
	int get_brain_state() const { return value_.brain_state; }
	int get_brain_pend() const { return value_.brain_pend; }
	int get_think_clock() const { return value_.think_clock; }
	int get_ai_event() const { return value_.ai_event; }
	PackedInt32Array get_eye() const;
	int get_eye_source() const { return value_.eye_source; }
	int get_anim_state() const { return value_.anim_state; }
	int get_anim_playing() const { return value_.anim_playing; }
	int get_anim_prev() const { return value_.anim_prev; }
	int get_anim_blend_milli() const { return value_.anim_blend_milli; }
	int get_clip_phase() const { return value_.clip_phase; }
	int get_capsule_bottom() const { return value_.capsule_bottom; }
	bool get_airborne() const { return value_.airborne; }
	PackedInt32Array get_resolve_push() const;
	int get_contact_item() const { return value_.contact_item; }

	Dictionary to_json_value() const;
};

} // namespace godot
