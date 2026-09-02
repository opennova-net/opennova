#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// The local player's equipped-weapon FSM view for one tick (world/weapon_fsm.h,
// world/local_player_weapon.h): the action ladder position, the FP clip channel,
// the last action's audio/effect legs, the event serials, the magazine, heat and
// recoil, the HUD crosshair spread in retail's integer domains, the PowerThrow
// windup, the emplaced-gun controls, the round-ring diagnostics and the 3P body
// weapon channel. `active` false = no weapon FSM installed (every other field
// reads its default). Read-write so a stub sim authors one; produced by
// Simulation.get_local_player_weapon_state. Field witnesses live on the engine
// state the builder reads (Simulation::get_local_player_weapon_state).
class PlayerWeaponView : public RefCounted {
	GDCLASS(PlayerWeaponView, RefCounted)

#define PLAYER_WEAPON_VIEW_FIELDS(X)                                     \
	X(bool, active, false)                                              \
	/* world::weapon_action ids (0 idle .. 11 overheated), the switch */ \
	X(int, current_action, 0)                                           \
	X(int, next_action, 0)                                              \
	X(int, phase, 0)                                                    \
	X(int, switch_deferred_action, -1)                                  \
	X(bool, switch_in_flight, false)                                    \
	X(int, pending_combo, 0)                                            \
	/* the FP clip channel */                                           \
	X(String, anim_key, String())                                       \
	X(int, anim_variant, 0)                                             \
	X(int, anim_advance_ticks, 0)                                       \
	X(int, play_serial, 0)                                              \
	/* the last-started action's legs (ordered delivery is the event drain) */ \
	X(int, action_serial, 0)                                            \
	X(int, action_started, -1)                                          \
	X(String, action_soundset, String())                                \
	X(String, action_particle, String())                                \
	X(String, action_particle_userpoint, String())                      \
	X(int, action_end_serial, 0)                                        \
	X(String, action_end_soundset, String())                            \
	/* the PowerThrow windup for the HUD charge bar */                  \
	X(bool, windup_active, false)                                       \
	X(int, windup_held_ticks, 0)                                        \
	/* event serials */                                                 \
	X(int, fired_serial, 0)                                             \
	X(int, tracer_counter, 0)                                           \
	X(int, dry_serial, 0)                                               \
	X(int, reload_serial, 0)                                            \
	X(int, reload_applied_serial, 0)                                    \
	X(int, reload_received_serial, 0)                                   \
	X(int, reload_received_entity, 0)                                   \
	X(int, reload_received_param, 0)                                    \
	X(int, unscope_serial, 0)                                           \
	X(int, rescope_serial, 0)                                           \
	/* the magazine, recoil kick, and the crosshair spread domains */   \
	X(int, clip, 0)                                                     \
	X(int, reserve, 0)                                                  \
	X(int, kick, 0)                                                     \
	X(int, recoil_pitch_bam, 0)                                         \
	X(int, weapon_weight_spread_bam, 0)                                 \
	X(bool, aimed_shot_available, false)                                \
	X(int, hud_spread_row, 0)                                           \
	X(int, hud_spread_fp16, 0)                                          \
	/* heat: the HUD clamp and the CTRL-bus HEAT_GLOW endpoint */       \
	X(int, heat, 0)                                                     \
	X(int, heat_glow, 0)                                                \
	X(bool, borrowed_usegun_slot, false)                                \
	/* the emplaced gun's controls when the local body gunners it */    \
	X(bool, emplaced_controls_valid, false)                             \
	X(int, emplaced_gun_yaw, 0)                                         \
	X(int, emplaced_gun_pitch, 0)                                       \
	/* the FIRE -> RoundData_AddRound seam diagnostics */               \
	X(int, round_ring_count, 0)                                         \
	X(int, last_round_flags, 0)                                         \
	X(int, last_round_subtype, 0)                                       \
	X(int, last_round_slot_byte, 0)                                     \
	X(int, last_round_seq, 0)                                           \
	/* the 3P body's weapon channel (empty key = override gate off) */  \
	X(String, body_anim_key, String())                                  \
	X(int, body_anim_phase, 0)                                          \
	X(String, body_anim_prev_key, String())                             \
	X(int, body_anim_prev_phase, 0)                                     \
	X(float, body_anim_blend_weight, 1.0f)                              \
	X(int, body_anim_variant, 0)                                        \
	X(int, body_anim_prev_variant, 0)

#define PLAYER_WEAPON_VIEW_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;
	PLAYER_WEAPON_VIEW_FIELDS(PLAYER_WEAPON_VIEW_MEMBER)
#undef PLAYER_WEAPON_VIEW_MEMBER

protected:
	static void _bind_methods();

public:
#define PLAYER_WEAPON_VIEW_ACCESSORS(m_type, m_name, m_default)          \
	m_type get_##m_name() const { return m_name##_; }                  \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
	PLAYER_WEAPON_VIEW_FIELDS(PLAYER_WEAPON_VIEW_ACCESSORS)
#undef PLAYER_WEAPON_VIEW_ACCESSORS

	// The MCP status boundary's JSON shape (one key per field).
	Dictionary to_json_value() const;
};

} // namespace godot
