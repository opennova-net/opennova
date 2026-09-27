#pragma once

#include <godot_cpp/classes/ref.hpp>
#include "util/string_convert.h"
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/world/player_weapon.h> // LocalPlayerWeaponView

namespace godot {

// The local player's equipped-weapon FSM view for one tick (world/weapon_fsm.h,
// world/player_weapon.h), a value wrapper over the engine's
// LocalPlayerWeaponView the sim fills (world::local_player_weapon_view in
// engine/runtime/world/player_weapon_view.cpp carries the field witnesses;
// ADR 0043 d10). `active` false = no weapon FSM
// installed (every other field reads its default). The one static make()
// exists for the comparison-probe fixture's sim double.
class PlayerWeaponView : public RefCounted {
	GDCLASS(PlayerWeaponView, RefCounted)

	opennova::world::LocalPlayerWeaponView value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::LocalPlayerWeaponView &p_value) { value_ = p_value; }
	const opennova::world::LocalPlayerWeaponView &value() const { return value_; }
	static Ref<PlayerWeaponView> make(bool p_active, int p_clip = 0, int p_reserve = 0);

// The field list: (Godot property type, name, Variant::Type). The X-macro
// generates only FORWARDERS over value_ and the bindings (ADR 0043 d10);
// the members live on the engine struct.
#define PLAYER_WEAPON_VIEW_FIELDS(X)                                        \
	X(bool, active, BOOL)                                                   \
	/* world::weapon_action ids (0 idle .. 11 overheated), the switch */   \
	X(int, current_action, INT)                                             \
	X(int, next_action, INT)                                                \
	X(int, phase, INT)                                                      \
	X(int, switch_deferred_action, INT)                                     \
	X(bool, switch_in_flight, BOOL)                                         \
	X(int, pending_combo, INT)                                              \
	/* the FP clip channel */                                               \
	X(String, anim_key, STRING)                                             \
	X(int, anim_variant, INT)                                               \
	X(int, anim_advance_ticks, INT)                                         \
	/* a loop wrap's fade toward the slot's next ring entry */              \
	X(bool, anim_blending, BOOL)                                            \
	X(String, anim_blend_key, STRING)                                       \
	X(int, anim_blend_variant, INT)                                         \
	X(int, anim_blend_ticks, INT)                                           \
	X(float, anim_blend_weight, FLOAT)                                      \
	X(int, play_serial, INT)                                                \
	/* the last-started action's legs (ordered delivery is the event drain) */ \
	X(int, action_serial, INT)                                              \
	X(int, action_started, INT)                                             \
	X(String, action_soundset, STRING)                                      \
	X(String, action_particle, STRING)                                      \
	X(String, action_particle_userpoint, STRING)                            \
	X(int, action_end_serial, INT)                                          \
	X(String, action_end_soundset, STRING)                                  \
	/* the PowerThrow windup for the HUD charge bar */                      \
	X(bool, windup_active, BOOL)                                            \
	X(int, windup_held_ticks, INT)                                          \
	/* event serials */                                                     \
	X(int, fired_serial, INT)                                               \
	X(int, tracer_counter, INT)                                             \
	X(int, dry_serial, INT)                                                 \
	X(int, reload_serial, INT)                                              \
	X(int, reload_applied_serial, INT)                                      \
	X(int, reload_received_serial, INT)                                     \
	X(int, reload_received_entity, INT)                                     \
	X(int, reload_received_param, INT)                                      \
	X(int, unscope_serial, INT)                                             \
	X(int, rescope_serial, INT)                                             \
	/* the magazine, recoil kick, and the crosshair spread domains */       \
	X(int, clip, INT)                                                       \
	X(int, reserve, INT)                                                    \
	X(int, kick, INT)                                                       \
	X(int, recoil_pitch_bam, INT)                                           \
	X(int, weapon_weight_spread_bam, INT)                                   \
	X(bool, aimed_shot_available, BOOL)                                     \
	X(int, hud_spread_row, INT)                                             \
	X(int, hud_spread_fp16, INT)                                            \
	/* heat: the HUD clamp and the CTRL-bus HEAT_GLOW endpoint */           \
	X(int, heat, INT)                                                       \
	X(int, heat_glow, INT)                                                  \
	X(bool, borrowed_usegun_slot, BOOL)                                     \
	X(int, usegun_mount_handle, INT)                                        \
	/* the emplaced gun's controls when the local body gunners it */        \
	X(bool, emplaced_controls_valid, BOOL)                                  \
	X(int, emplaced_gun_yaw, INT)                                           \
	X(int, emplaced_gun_pitch, INT)                                         \
	X(int, emplaced_spin_phase, INT)                                        \
	/* the FIRE -> RoundData_AddRound seam diagnostics */                   \
	X(int, round_ring_count, INT)                                           \
	X(int, last_round_flags, INT)                                           \
	X(int, last_round_subtype, INT)                                         \
	X(int, last_round_slot_byte, INT)                                       \
	X(int, last_round_seq, INT)                                             \
	/* the 3P body's weapon channel (empty key = override gate off) */      \
	X(String, body_anim_key, STRING)                                        \
	X(int, body_anim_phase, INT)                                            \
	X(String, body_anim_prev_key, STRING)                                   \
	X(int, body_anim_prev_phase, INT)                                       \
	X(float, body_anim_blend_weight, FLOAT)                                 \
	X(int, body_anim_variant, INT)                                          \
	X(int, body_anim_prev_variant, INT)

	// The per-type forwarders: an engine int/bool/float reads straight, an
	// engine std::string as a Godot String.
	static int forward(int32_t v) { return v; }
	static bool forward(bool v) { return v; }
	static float forward(float v) { return v; }
	static String forward(const std::string &v) { return opennova::to_gd(v); }

#define PLAYER_WEAPON_VIEW_GETTER(m_type, m_name, m_variant) \
	m_type get_##m_name() const { return forward(value_.m_name); }
	PLAYER_WEAPON_VIEW_FIELDS(PLAYER_WEAPON_VIEW_GETTER)
#undef PLAYER_WEAPON_VIEW_GETTER

	// The MCP status boundary's JSON shape (one key per field).
	Dictionary to_json_value() const;
};

} // namespace godot
