#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/hud/hud_entity_info.h>

namespace godot {

class PlayerHudWeaponDef;
class PlayerLocalView;
class PlayerWeaponView;

// The local player's per-frame HUD info record, read-only: the engine fills it
// (hud::local_player_entity_info, runtime/hud/hud_entity_info.h carries the
// witnesses) from the frame the HUD presents, the equipped weapon's FSM view,
// its def slice and the health pair; GameHudPresenter writes its fields into
// the overlay, the SIGHTS card, the circle mask and the view effects.
class HudEntityInfo : public RefCounted {
	GDCLASS(HudEntityInfo, RefCounted)

	opennova::hud::HudEntityInfo value_;

protected:
	static void _bind_methods();

public:
	// Null view / weapon view / def each read as absent.
	static Ref<HudEntityInfo> build(int p_health, int p_max_health, const Ref<PlayerLocalView> &p_view,
			const Ref<PlayerWeaponView> &p_weapon_view, const Ref<PlayerHudWeaponDef> &p_weapon);

#define HUD_ENTITY_INFO_FIELDS(X)                    \
	X(float, health_fraction, FLOAT)                 \
	X(int, stance, INT)                              \
	X(float, fov_h_deg, FLOAT)                       \
	X(bool, weapon_drawn, BOOL)                      \
	X(int, clip, INT)                                \
	X(int, reserve, INT)                             \
	X(int, heat, INT)                                \
	X(int, spread_fp16, INT)                         \
	X(bool, aimed_shot_available, BOOL)              \
	X(bool, keep_crosshair_while_aimed, BOOL)        \
	X(bool, windup_active, BOOL)                     \
	X(int, windup_held_ticks, INT)                   \
	X(int, ammo_bucket, INT)                         \
	X(int, ammo_class_id, INT)                       \
	X(bool, sights_card_up, BOOL)                    \
	X(bool, nvg_sights_in_scene, BOOL)               \
	X(bool, scope_mask_up, BOOL)                     \
	X(bool, scope_mask_draw_crosshair, BOOL)         \
	X(bool, scope_mask_nvg_lens, BOOL)               \
	X(bool, binoculars_view_active, BOOL)            \
	X(int, binocular_range, INT)                     \
	X(bool, nvg_mask, BOOL)                          \
	X(int, nvg_gain, INT)                            \
	X(int, flash_white, INT)                         \
	X(int, flash_red, INT)                           \
	X(int, flash_revive, INT)                        \
	X(int, flash_revive_channel, INT)                \
	X(bool, overlays_suppressed, BOOL)

#define HUD_ENTITY_INFO_GETTER(m_type, m_name, m_variant) \
	m_type get_##m_name() const { return value_.m_name; }
	HUD_ENTITY_INFO_FIELDS(HUD_ENTITY_INFO_GETTER)
#undef HUD_ENTITY_INFO_GETTER
};

} // namespace godot
