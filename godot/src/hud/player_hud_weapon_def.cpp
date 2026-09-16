#include "hud/player_hud_weapon_def.h"

#include <runtime/hud/scope_circle_mask.h>

using namespace godot;

Ref<PlayerHudWeaponDef> PlayerHudWeaponDef::from_weapon_def(const Ref<WeaponDef> &p_def) {
	if (p_def.is_null()) {
		return Ref<PlayerHudWeaponDef>();
	}
	Ref<PlayerHudWeaponDef> out;
	out.instantiate();
	out->weapon_name_ = p_def->get_name();
	out->round_type_ = p_def->get_round_type();
	out->clipsize_ = p_def->get_clipsize();
	out->error_deg_ = p_def->get_error();
	out->clipgfx_texture_ = p_def->get_hudclipgfx_texture();
	out->clipgfx_offset_ = p_def->get_hudclipgfx_offset();
	out->rndgfx_texture_ = p_def->get_hudrndgfx_texture();
	out->rndgfx_offset_ = p_def->get_hudrndgfx_offset();
	const Vector3i layout = p_def->get_hudrndgfx_layout();
	out->rndgfx_step_ = Vector2i(layout.x, layout.y);
	out->rounds_per_icon_ = rounds_per_icon_from_layout(layout);
	out->sights_ = p_def->get_sights();
	out->sight_slide_multiplier_ = p_def->get_sight_slide_multiplier();
	// The card selectors' def halves; the engine owns the bit policy.
	out->scoped_selector_ = opennova::hud::scoped_selector_from_def(
			static_cast<uint32_t>(p_def->get_flags()),
			static_cast<uint32_t>(p_def->get_flags2()));
	out->sighted_selector_ = opennova::hud::sighted_selector_from_def(
			static_cast<uint32_t>(p_def->get_flags()), false);
	return out;
}

int PlayerHudWeaponDef::rounds_per_icon_from_layout(const Vector3i &p_layout) {
	return p_layout.z & 0xFF;
}

float PlayerHudWeaponDef::error_row_deg(int p_row) const {
	if (p_row >= 0 && p_row < error_deg_.size()) {
		return error_deg_[p_row];
	}
	return 0.0f;
}

void PlayerHudWeaponDef::_bind_methods() {
	ClassDB::bind_static_method("PlayerHudWeaponDef", D_METHOD("from_weapon_def", "def"),
			&PlayerHudWeaponDef::from_weapon_def);
	ClassDB::bind_static_method("PlayerHudWeaponDef", D_METHOD("rounds_per_icon_from_layout", "layout"),
			&PlayerHudWeaponDef::rounds_per_icon_from_layout);
	ClassDB::bind_method(D_METHOD("error_row_deg", "row"), &PlayerHudWeaponDef::error_row_deg);
#define PLAYER_HUD_WEAPON_DEF_PROPERTY(m_type, m_name)                                                \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PlayerHudWeaponDef::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &PlayerHudWeaponDef::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(m_type, #m_name), "set_" #m_name, "get_" #m_name);
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::STRING, weapon_name)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::STRING, round_type)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::INT, clipsize)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::PACKED_FLOAT32_ARRAY, error_deg)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::STRING, clipgfx_texture)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::VECTOR2I, clipgfx_offset)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::STRING, rndgfx_texture)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::VECTOR2I, rndgfx_offset)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::VECTOR2I, rndgfx_step)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::INT, rounds_per_icon)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::INT, sight_slide_multiplier)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::BOOL, scoped_selector)
	PLAYER_HUD_WEAPON_DEF_PROPERTY(Variant::BOOL, sighted_selector)
#undef PLAYER_HUD_WEAPON_DEF_PROPERTY
	ClassDB::bind_method(D_METHOD("get_sights"), &PlayerHudWeaponDef::get_sights);
	ClassDB::bind_method(D_METHOD("set_sights", "value"), &PlayerHudWeaponDef::set_sights);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "sights", PROPERTY_HINT_ARRAY_TYPE, "WeaponSightRow"),
			"set_sights", "get_sights");
}
