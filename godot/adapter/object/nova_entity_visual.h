#pragma once

// The TYPED entity-visual contract (ADR 0007's surface, de-duck-typed per
// ADR 0033 R2): the node a present pass drives implements this class, and
// every dispatcher reaches it through a cast + direct calls — no has_method
// probes, no StringName call tables, no capability bitmasks. Script
// implementers (NovaObjectModel) override the `_`-prefixed virtuals; the
// public wrappers carry the one copy of the graceful-degradation ladder the
// GDScript call sites used to re-derive per caller (blend -> clip-at -> clip,
// slot-at -> slot, variant-at-time -> variant -> clip).
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

class NovaEntityVisual : public Node3D {
	GDCLASS(NovaEntityVisual, Node3D)

protected:
	static void _bind_methods();

	// --- The script-overridable surface ---------------------------------
	// PANM (procedural part anim) + CTRL registers
	GDVIRTUAL3(_play_part_anim, int, int, double)
	GDVIRTUAL3(_restart_part_anim, int, int, double)
	GDVIRTUAL2(_set_part_phase, int, int)
	GDVIRTUAL1(_clear_part_phase, int)
	GDVIRTUAL0(_begin_ctrl_update)
	GDVIRTUAL0(_end_ctrl_update)
	GDVIRTUAL2(_set_ctrl_value, String, int)
	GDVIRTUAL1(_clear_ctrl_value, String)
	GDVIRTUAL3(_set_ctrl_override, String, String, int)
	GDVIRTUAL2(_clear_ctrl_override, String, String)
	// Main-body skeletal channels
	GDVIRTUAL1(_play_body_clip, String)
	GDVIRTUAL2(_play_body_clip_variant, String, int)
	GDVIRTUAL3(_play_body_clip_variant_at_time, String, int, double)
	GDVIRTUAL2(_play_body_clip_at, String, int)
	GDVIRTUAL5(_play_body_blend_at, String, int, String, int, double)
	GDVIRTUAL1(_play_body_anim, int)
	GDVIRTUAL2(_play_body_anim_at, int, int)
	GDVIRTUAL1(_set_animation_time, double)
	GDVIRTUAL4R(bool, _apply_remote_body_state, int, String, int, int)
	GDVIRTUAL0(_reset_remote_body_state)
	GDVIRTUAL1R(bool, _advance_remote_body_blend_tick, int)
	GDVIRTUAL2(_set_weapon_channel, String, int)
	GDVIRTUAL1(_set_aim_overlay, Array)
	GDVIRTUAL1(_set_right_hand_collapsed, bool)
	// Muzzle + camera-submission seam
	GDVIRTUAL0RC(bool, _has_muzzle)
	GDVIRTUAL0RC(Vector3, _get_muzzle_world_position)
	GDVIRTUAL1(_set_submission_registry, Dictionary)
	// Construction-time wiring (the placer's legs)
	GDVIRTUAL1(_set_skeletal_anim, Variant)
	GDVIRTUAL0RC(Object *, _get_skeleton)
	GDVIRTUAL1(_set_environment_node, Object *)

public:
	// --- Typed dispatch, one wrapper per leg -----------------------------
	void play_part_anim(int p_channel, int p_play_type, double p_time_s);
	void restart_part_anim(int p_channel, int p_play_type, double p_time_s);
	void set_part_phase(int p_channel, int p_phase);
	void clear_part_phase(int p_channel);
	void begin_ctrl_update();
	void end_ctrl_update();
	void set_ctrl_value(const String &p_name, int p_value);
	void clear_ctrl_value(const String &p_name);
	void set_ctrl_override(const String &p_owner, const String &p_name,
			int p_value);
	void clear_ctrl_override(const String &p_owner, const String &p_name);

	void play_body_clip(const String &p_key);
	// Falls back down the ladder when an implementer leaves the richer leg
	// unoverridden: variant_at_time -> variant -> clip; clip_at -> clip;
	// blend_at -> clip_at(target); anim_at -> anim.
	void play_body_clip_variant(const String &p_key, int p_variant);
	void play_body_clip_variant_at_time(const String &p_key, int p_variant,
			double p_seconds);
	void play_body_clip_at(const String &p_key, int p_phase_ticks);
	void play_body_blend_at(const String &p_source_key, int p_source_phase_ticks,
			const String &p_target_key, int p_target_phase_ticks,
			double p_weight);
	void play_body_anim(int p_slot);
	void play_body_anim_at(int p_slot, int p_phase_ticks);
	void set_animation_time(double p_seconds);
	bool apply_remote_body_state(int p_state_id, const String &p_key,
			int p_flags, int p_phase_ticks = -1);
	void reset_remote_body_state();
	bool advance_remote_body_blend_tick(int p_state_id);
	void set_weapon_channel(const String &p_key, int p_phase_ticks);
	void set_aim_overlay(const Array &p_deltas);
	void set_right_hand_collapsed(bool p_collapsed);

	bool has_muzzle() const;
	Vector3 get_muzzle_world_position() const;
	void set_submission_registry(const Dictionary &p_registry);

	void set_skeletal_anim(const Variant &p_skeletal);
	Skeleton3D *get_visual_skeleton() const;
	void set_environment_node(Object *p_node);
};

} // namespace godot
