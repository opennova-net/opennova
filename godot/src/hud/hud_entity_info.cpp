#include "hud/hud_entity_info.h"

#include "hud/player_hud_weapon_def.h"
#include "simulation/player_local_view.h"
#include "simulation/player_weapon_view.h"

using namespace godot;

Ref<HudEntityInfo> HudEntityInfo::build(int p_health, int p_max_health,
		const Ref<PlayerLocalView> &p_view, const Ref<PlayerWeaponView> &p_weapon_view,
		const Ref<PlayerHudWeaponDef> &p_weapon) {
	opennova::hud::HudEntityWeaponDef weapon;
	if (p_weapon.is_valid()) {
		weapon.clipsize = p_weapon->get_clipsize();
		weapon.scoped_selector = p_weapon->get_scoped_selector();
		weapon.sighted_selector = p_weapon->get_sighted_selector();
		weapon.has_sights = !p_weapon->get_sights().is_empty();
	}
	Ref<HudEntityInfo> out;
	out.instantiate();
	out->value_ = opennova::hud::local_player_entity_info(p_health, p_max_health,
			p_view.is_valid() ? &p_view->native_frame() : nullptr,
			p_weapon_view.is_valid() ? &p_weapon_view->value() : nullptr,
			p_weapon.is_valid() ? &weapon : nullptr);
	return out;
}

void HudEntityInfo::_bind_methods() {
	ClassDB::bind_static_method("HudEntityInfo",
			D_METHOD("build", "health", "max_health", "view", "weapon_view", "weapon"),
			&HudEntityInfo::build);
#define HUD_ENTITY_INFO_BIND(m_type, m_name, m_variant)                         \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &HudEntityInfo::get_##m_name); \
	ADD_PROPERTY(PropertyInfo(Variant::m_variant, #m_name), "", "get_" #m_name);
	HUD_ENTITY_INFO_FIELDS(HUD_ENTITY_INFO_BIND)
#undef HUD_ENTITY_INFO_BIND
}
