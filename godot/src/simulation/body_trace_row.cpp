#include "simulation/body_trace_row.h"

#include "util/record_bind.h"

using namespace godot;
namespace w = opennova::world;

namespace {

PackedInt32Array triple(const int32_t (&p_words)[3]) {
	PackedInt32Array out;
	out.resize(3);
	for (int i = 0; i < 3; ++i) out.set(i, p_words[i]);
	return out;
}

} // namespace

String BodyTraceRow::get_event() const { return String(w::body_trace_event_name(value_.event)); }

PackedInt32Array BodyTraceRow::get_pos() const { return triple(value_.pos); }

PackedInt32Array BodyTraceRow::get_vel() const { return triple(value_.vel); }

PackedInt32Array BodyTraceRow::get_source() const { return triple(value_.source); }

PackedInt32Array BodyTraceRow::get_eye() const { return triple(value_.eye); }

PackedInt32Array BodyTraceRow::get_resolve_push() const {
	PackedInt32Array out;
	out.resize(2);
	out.set(0, value_.resolve_push[0]);
	out.set(1, value_.resolve_push[1]);
	return out;
}

Dictionary BodyTraceRow::to_json_value() const {
	Dictionary out;
	out["seq"] = get_seq();
	out["logic_tick"] = get_logic_tick();
	out["event"] = get_event();
	out["handle"] = get_handle();
	out["present"] = get_present();
	out["pos"] = get_pos();
	out["vel"] = get_vel();
	out["flags"] = get_flags();
	out["engine_flags"] = get_engine_flags();
	out["health"] = get_health();
	out["mounted"] = get_mounted();
	out["mount_target"] = get_mount_target();
	out["mount_seat"] = get_mount_seat();
	out["mount_bone"] = get_mount_bone();
	out["ground_target"] = get_ground_target();
	out["collision_model"] = get_collision_model();
	out["probe_z"] = get_probe_z();
	out["other"] = get_other();
	out["hit_type"] = get_hit_type();
	out["force_type"] = get_force_type();
	out["source"] = get_source();
	out["damage"] = get_damage();
	out["health_before"] = get_health_before();
	out["brain_state"] = get_brain_state();
	out["brain_pend"] = get_brain_pend();
	out["think_clock"] = get_think_clock();
	out["ai_event"] = get_ai_event();
	out["eye"] = get_eye();
	out["eye_source"] = get_eye_source();
	out["anim_state"] = get_anim_state();
	out["anim_playing"] = get_anim_playing();
	out["anim_prev"] = get_anim_prev();
	out["anim_blend_milli"] = get_anim_blend_milli();
	out["clip_phase"] = get_clip_phase();
	out["capsule_bottom"] = get_capsule_bottom();
	out["airborne"] = get_airborne();
	out["resolve_push"] = get_resolve_push();
	out["contact_item"] = get_contact_item();
	return out;
}

void BodyTraceRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, seq)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, logic_tick)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::STRING, event)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, handle)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::BOOL, present)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::PACKED_INT32_ARRAY, pos)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::PACKED_INT32_ARRAY, vel)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, flags)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, engine_flags)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, health)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::BOOL, mounted)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, mount_target)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, mount_seat)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, mount_bone)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, ground_target)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, collision_model)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, probe_z)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, other)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, hit_type)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, force_type)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::PACKED_INT32_ARRAY, source)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, damage)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, health_before)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, brain_state)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, brain_pend)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, think_clock)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, ai_event)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::PACKED_INT32_ARRAY, eye)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, eye_source)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, anim_state)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, anim_playing)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, anim_prev)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, anim_blend_milli)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, clip_phase)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, capsule_bottom)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::BOOL, airborne)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::PACKED_INT32_ARRAY, resolve_push)
	OPENNOVA_RECORD_READ_ONLY(BodyTraceRow, Variant::INT, contact_item)
	ClassDB::bind_method(D_METHOD("to_json_value"), &BodyTraceRow::to_json_value);
}
