#include "simulation/player_weapon_view.h"

#include "util/record_bind.h"

using namespace godot;

Ref<PlayerWeaponView> PlayerWeaponView::make(bool p_active, int p_clip, int p_reserve) {
	opennova::world::LocalPlayerWeaponView v;
	v.active = p_active;
	v.clip = p_clip;
	v.reserve = p_reserve;
	Ref<PlayerWeaponView> out;
	out.instantiate();
	out->assign(v);
	return out;
}

Dictionary PlayerWeaponView::to_json_value() const {
	Dictionary out;
#define PLAYER_WEAPON_VIEW_JSON(m_type, m_name, m_variant) out[#m_name] = get_##m_name();
	PLAYER_WEAPON_VIEW_FIELDS(PLAYER_WEAPON_VIEW_JSON)
#undef PLAYER_WEAPON_VIEW_JSON
	return out;
}

void PlayerWeaponView::_bind_methods() {
	ClassDB::bind_static_method("PlayerWeaponView", D_METHOD("make", "active", "clip", "reserve"),
			&PlayerWeaponView::make, DEFVAL(0), DEFVAL(0));
#define PLAYER_WEAPON_VIEW_BIND(m_type, m_name, m_variant) \
	OPENNOVA_RECORD_READ_ONLY(PlayerWeaponView, Variant::m_variant, m_name)
	PLAYER_WEAPON_VIEW_FIELDS(PLAYER_WEAPON_VIEW_BIND)
#undef PLAYER_WEAPON_VIEW_BIND
	ClassDB::bind_method(D_METHOD("to_json_value"), &PlayerWeaponView::to_json_value);
}
