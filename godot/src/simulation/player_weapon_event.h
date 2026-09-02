#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/world/player_weapon.h>

#include <cstdint>

namespace godot {

// One logic tick's ordered presentation outputs from the local weapon FSM
// (world::WeaponPresentationEvent): the clip start, the ACTION begin leg, the
// ACTION end leg, the recoil-row DIRECT effect leg, and a committed weapon
// switch. `age_ticks` is the catch-up pre-age for the sound/effect legs (the FP
// CLIP is posed from the sim's anim_advance_ticks, never from this);
// `world_position` is the production-tick shooter position in Godot space so
// delayed 3D audio stays spatially faithful. Drained in order by
// Simulation.drain_local_player_weapon_events; the engine event carries the
// witnesses.
class PlayerWeaponEvent : public RefCounted {
	GDCLASS(PlayerWeaponEvent, RefCounted)

	opennova::world::WeaponPresentationEvent value_;
	int age_ticks_ = 0;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::WeaponPresentationEvent &p_value, uint32_t p_now_tick);

	int get_age_ticks() const { return age_ticks_; }
	Vector3 get_world_position() const;
	String get_anim_key() const;
	int get_anim_variant() const { return value_.anim_variant; }
	// The started action's slot id (world::weapon_action; -1 = none) and its legs.
	int get_action_started() const { return value_.action_started; }
	String get_action_soundset() const;
	String get_action_particle() const;
	String get_action_particle_userpoint() const;
	bool get_scope_settled() const { return value_.scope_settled; }
	bool get_third_person() const { return value_.third_person; }
	bool get_vehicle_attack_context() const { return value_.vehicle_attack_context; }
	int get_action_finished() const { return value_.action_finished; }
	String get_action_end_soundset() const;
	// The recoil-row DIRECT effect leg (no scope gate, no live-handle suppression).
	int get_action_effect() const { return value_.action_effect; }
	String get_effect_particle() const;
	String get_effect_particle_userpoint() const;
	// A committed weapon switch: the newly equipped weapon.def name (empty = none),
	// a target with no definition, the UseGun slot-preserve rule, and the
	// switch-walk wrap-around refusal.
	String get_switch_to_weapon() const;
	bool get_clear_weapon() const { return value_.clear_weapon; }
	bool get_preserve_slot_state() const { return value_.preserve_slot_state; }
	bool get_switch_denied() const { return value_.switch_denied; }
};

} // namespace godot
