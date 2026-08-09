// NovaEntityVisual — the typed entity-visual base (ADR 0033 R2: the
// duck-typed dispatch dies). Wrappers vcall the script override and carry
// the single copy of the degradation ladder.

#include "object/nova_entity_visual.h"

namespace godot {

void NovaEntityVisual::_bind_methods() {
	ClassDB::bind_method(D_METHOD("play_part_anim", "channel", "play_type", "time_s"),
			&NovaEntityVisual::play_part_anim);
	ClassDB::bind_method(D_METHOD("restart_part_anim", "channel", "play_type", "time_s"),
			&NovaEntityVisual::restart_part_anim);
	ClassDB::bind_method(D_METHOD("set_part_phase", "channel", "phase"),
			&NovaEntityVisual::set_part_phase);
	ClassDB::bind_method(D_METHOD("clear_part_phase", "channel"),
			&NovaEntityVisual::clear_part_phase);
	ClassDB::bind_method(D_METHOD("begin_ctrl_update"),
			&NovaEntityVisual::begin_ctrl_update);
	ClassDB::bind_method(D_METHOD("end_ctrl_update"),
			&NovaEntityVisual::end_ctrl_update);
	ClassDB::bind_method(D_METHOD("set_ctrl_value", "name", "value"),
			&NovaEntityVisual::set_ctrl_value);
	ClassDB::bind_method(D_METHOD("clear_ctrl_value", "name"),
			&NovaEntityVisual::clear_ctrl_value);
	ClassDB::bind_method(D_METHOD("set_ctrl_override", "owner", "name", "value"),
			&NovaEntityVisual::set_ctrl_override);
	ClassDB::bind_method(D_METHOD("clear_ctrl_override", "owner", "name"),
			&NovaEntityVisual::clear_ctrl_override);

	ClassDB::bind_method(D_METHOD("play_body_clip", "key"),
			&NovaEntityVisual::play_body_clip);
	ClassDB::bind_method(D_METHOD("play_body_clip_variant", "key", "variant"),
			&NovaEntityVisual::play_body_clip_variant);
	ClassDB::bind_method(
			D_METHOD("play_body_clip_variant_at_time", "key", "variant", "seconds"),
			&NovaEntityVisual::play_body_clip_variant_at_time);
	ClassDB::bind_method(D_METHOD("play_body_clip_at", "key", "phase_ticks"),
			&NovaEntityVisual::play_body_clip_at);
	ClassDB::bind_method(
			D_METHOD("play_body_blend_at", "source_key", "source_phase_ticks",
					"target_key", "target_phase_ticks", "weight"),
			&NovaEntityVisual::play_body_blend_at);
	ClassDB::bind_method(D_METHOD("play_body_anim", "slot"),
			&NovaEntityVisual::play_body_anim);
	ClassDB::bind_method(D_METHOD("play_body_anim_at", "slot", "phase_ticks"),
			&NovaEntityVisual::play_body_anim_at);
	ClassDB::bind_method(D_METHOD("set_animation_time", "seconds"),
			&NovaEntityVisual::set_animation_time);
	ClassDB::bind_method(
			D_METHOD("apply_remote_body_state", "state_id", "key", "flags",
					"phase_ticks"),
			&NovaEntityVisual::apply_remote_body_state, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("reset_remote_body_state"),
			&NovaEntityVisual::reset_remote_body_state);
	ClassDB::bind_method(D_METHOD("advance_remote_body_blend_tick", "state_id"),
			&NovaEntityVisual::advance_remote_body_blend_tick);
	ClassDB::bind_method(D_METHOD("set_weapon_channel", "key", "phase_ticks"),
			&NovaEntityVisual::set_weapon_channel);
	ClassDB::bind_method(D_METHOD("set_aim_overlay", "deltas"),
			&NovaEntityVisual::set_aim_overlay);
	ClassDB::bind_method(D_METHOD("set_right_hand_collapsed", "collapsed"),
			&NovaEntityVisual::set_right_hand_collapsed);

	ClassDB::bind_method(D_METHOD("has_muzzle"), &NovaEntityVisual::has_muzzle);
	ClassDB::bind_method(D_METHOD("get_muzzle_world_position"),
			&NovaEntityVisual::get_muzzle_world_position);
	ClassDB::bind_method(D_METHOD("set_submission_registry", "registry"),
			&NovaEntityVisual::set_submission_registry);

	ClassDB::bind_method(D_METHOD("set_skeletal_anim", "skeletal"),
			&NovaEntityVisual::set_skeletal_anim);
	ClassDB::bind_method(D_METHOD("get_visual_skeleton"),
			&NovaEntityVisual::get_visual_skeleton);
	ClassDB::bind_method(D_METHOD("set_environment_node", "node"),
			&NovaEntityVisual::set_environment_node);

	GDVIRTUAL_BIND(_play_part_anim, "channel", "play_type", "time_s");
	GDVIRTUAL_BIND(_restart_part_anim, "channel", "play_type", "time_s");
	GDVIRTUAL_BIND(_set_part_phase, "channel", "phase");
	GDVIRTUAL_BIND(_clear_part_phase, "channel");
	GDVIRTUAL_BIND(_begin_ctrl_update);
	GDVIRTUAL_BIND(_end_ctrl_update);
	GDVIRTUAL_BIND(_set_ctrl_value, "name", "value");
	GDVIRTUAL_BIND(_clear_ctrl_value, "name");
	GDVIRTUAL_BIND(_set_ctrl_override, "owner", "name", "value");
	GDVIRTUAL_BIND(_clear_ctrl_override, "owner", "name");
	GDVIRTUAL_BIND(_play_body_clip, "key");
	GDVIRTUAL_BIND(_play_body_clip_variant, "key", "variant");
	GDVIRTUAL_BIND(_play_body_clip_variant_at_time, "key", "variant", "seconds");
	GDVIRTUAL_BIND(_play_body_clip_at, "key", "phase_ticks");
	GDVIRTUAL_BIND(_play_body_blend_at, "source_key", "source_phase_ticks",
			"target_key", "target_phase_ticks", "weight");
	GDVIRTUAL_BIND(_play_body_anim, "slot");
	GDVIRTUAL_BIND(_play_body_anim_at, "slot", "phase_ticks");
	GDVIRTUAL_BIND(_set_animation_time, "seconds");
	GDVIRTUAL_BIND(_apply_remote_body_state, "state_id", "key", "flags",
			"phase_ticks");
	GDVIRTUAL_BIND(_reset_remote_body_state);
	GDVIRTUAL_BIND(_advance_remote_body_blend_tick, "state_id");
	GDVIRTUAL_BIND(_set_weapon_channel, "key", "phase_ticks");
	GDVIRTUAL_BIND(_set_aim_overlay, "deltas");
	GDVIRTUAL_BIND(_set_right_hand_collapsed, "collapsed");
	GDVIRTUAL_BIND(_has_muzzle);
	GDVIRTUAL_BIND(_get_muzzle_world_position);
	GDVIRTUAL_BIND(_set_submission_registry, "registry");
	GDVIRTUAL_BIND(_set_skeletal_anim, "skeletal");
	GDVIRTUAL_BIND(_get_skeleton);
	GDVIRTUAL_BIND(_set_environment_node, "node");
}

void NovaEntityVisual::play_part_anim(int p_channel, int p_play_type,
		double p_time_s) {
	GDVIRTUAL_CALL(_play_part_anim, p_channel, p_play_type, p_time_s);
}

void NovaEntityVisual::restart_part_anim(int p_channel, int p_play_type,
		double p_time_s) {
	GDVIRTUAL_CALL(_restart_part_anim, p_channel, p_play_type, p_time_s);
}

void NovaEntityVisual::set_part_phase(int p_channel, int p_phase) {
	GDVIRTUAL_CALL(_set_part_phase, p_channel, p_phase);
}

void NovaEntityVisual::clear_part_phase(int p_channel) {
	GDVIRTUAL_CALL(_clear_part_phase, p_channel);
}

void NovaEntityVisual::begin_ctrl_update() {
	GDVIRTUAL_CALL(_begin_ctrl_update);
}

void NovaEntityVisual::end_ctrl_update() {
	GDVIRTUAL_CALL(_end_ctrl_update);
}

void NovaEntityVisual::set_ctrl_value(const String &p_name, int p_value) {
	GDVIRTUAL_CALL(_set_ctrl_value, p_name, p_value);
}

void NovaEntityVisual::clear_ctrl_value(const String &p_name) {
	GDVIRTUAL_CALL(_clear_ctrl_value, p_name);
}

void NovaEntityVisual::set_ctrl_override(const String &p_owner,
		const String &p_name, int p_value) {
	// Owner-aware CTRL is the production surface; an implementer carrying only
	// the original pair takes the legacy write (the former VISUAL_CTRL_LEGACY
	// dispatch, now the base's one fallback).
	if (!GDVIRTUAL_CALL(_set_ctrl_override, p_owner, p_name, p_value)) {
		set_ctrl_value(p_name, p_value);
	}
}

void NovaEntityVisual::clear_ctrl_override(const String &p_owner,
		const String &p_name) {
	if (!GDVIRTUAL_CALL(_clear_ctrl_override, p_owner, p_name)) {
		clear_ctrl_value(p_name);
	}
}

void NovaEntityVisual::play_body_clip(const String &p_key) {
	GDVIRTUAL_CALL(_play_body_clip, p_key);
}

void NovaEntityVisual::play_body_clip_variant(const String &p_key,
		int p_variant) {
	if (!GDVIRTUAL_CALL(_play_body_clip_variant, p_key, p_variant)) {
		play_body_clip(p_key);
	}
}

void NovaEntityVisual::play_body_clip_variant_at_time(const String &p_key,
		int p_variant, double p_seconds) {
	if (!GDVIRTUAL_CALL(_play_body_clip_variant_at_time, p_key, p_variant,
				p_seconds)) {
		play_body_clip_variant(p_key, p_variant);
	}
}

void NovaEntityVisual::play_body_clip_at(const String &p_key,
		int p_phase_ticks) {
	if (!GDVIRTUAL_CALL(_play_body_clip_at, p_key, p_phase_ticks)) {
		play_body_clip(p_key);
	}
}

void NovaEntityVisual::play_body_blend_at(const String &p_source_key,
		int p_source_phase_ticks, const String &p_target_key,
		int p_target_phase_ticks, double p_weight) {
	if (!GDVIRTUAL_CALL(_play_body_blend_at, p_source_key, p_source_phase_ticks,
				p_target_key, p_target_phase_ticks, p_weight)) {
		play_body_clip_at(p_target_key, p_target_phase_ticks);
	}
}

void NovaEntityVisual::play_body_anim(int p_slot) {
	GDVIRTUAL_CALL(_play_body_anim, p_slot);
}

void NovaEntityVisual::play_body_anim_at(int p_slot, int p_phase_ticks) {
	if (!GDVIRTUAL_CALL(_play_body_anim_at, p_slot, p_phase_ticks)) {
		play_body_anim(p_slot);
	}
}

void NovaEntityVisual::set_animation_time(double p_seconds) {
	GDVIRTUAL_CALL(_set_animation_time, p_seconds);
}

bool NovaEntityVisual::apply_remote_body_state(int p_state_id,
		const String &p_key, int p_flags, int p_phase_ticks) {
	bool applied = false;
	GDVIRTUAL_CALL(_apply_remote_body_state, p_state_id, p_key, p_flags,
			p_phase_ticks, applied);
	return applied;
}

void NovaEntityVisual::reset_remote_body_state() {
	GDVIRTUAL_CALL(_reset_remote_body_state);
}

bool NovaEntityVisual::advance_remote_body_blend_tick(int p_state_id) {
	bool advanced = false;
	GDVIRTUAL_CALL(_advance_remote_body_blend_tick, p_state_id, advanced);
	return advanced;
}

void NovaEntityVisual::set_weapon_channel(const String &p_key,
		int p_phase_ticks) {
	GDVIRTUAL_CALL(_set_weapon_channel, p_key, p_phase_ticks);
}

void NovaEntityVisual::set_aim_overlay(const Array &p_deltas) {
	GDVIRTUAL_CALL(_set_aim_overlay, p_deltas);
}

void NovaEntityVisual::set_right_hand_collapsed(bool p_collapsed) {
	GDVIRTUAL_CALL(_set_right_hand_collapsed, p_collapsed);
}

bool NovaEntityVisual::has_muzzle() const {
	bool result = false;
	GDVIRTUAL_CALL(_has_muzzle, result);
	return result;
}

Vector3 NovaEntityVisual::get_muzzle_world_position() const {
	Vector3 result;
	GDVIRTUAL_CALL(_get_muzzle_world_position, result);
	return result;
}

void NovaEntityVisual::set_submission_registry(const Dictionary &p_registry) {
	GDVIRTUAL_CALL(_set_submission_registry, p_registry);
}

void NovaEntityVisual::set_skeletal_anim(const Variant &p_skeletal) {
	GDVIRTUAL_CALL(_set_skeletal_anim, p_skeletal);
}

Skeleton3D *NovaEntityVisual::get_visual_skeleton() const {
	Object *result = nullptr;
	GDVIRTUAL_CALL(_get_skeleton, result);
	return Object::cast_to<Skeleton3D>(result);
}

void NovaEntityVisual::set_environment_node(Object *p_node) {
	GDVIRTUAL_CALL(_set_environment_node, p_node);
}

} // namespace godot
