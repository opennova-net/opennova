#include "player/player_viewmodel_def.h"

#include "object/weapon_def.h"

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<float>() { return Variant::FLOAT; }
template <>
constexpr Variant::Type variant_type_of<String>() { return Variant::STRING; }
template <>
constexpr Variant::Type variant_type_of<Vector3>() { return Variant::VECTOR3; }

} // namespace

Ref<PlayerViewmodelDef> PlayerViewmodelDef::from_weapon_def(const Ref<WeaponDef> &p_def) {
	if (p_def.is_null()) {
		return Ref<PlayerViewmodelDef>();
	}
	Ref<PlayerViewmodelDef> out;
	out.instantiate();
	out->weapon_name_ = p_def->get_name();
	out->gfx1_ = p_def->get_gfx1();
	out->gfx3_ = p_def->get_gfx3();
	out->animadm_ = p_def->get_animadm();
	out->pos_units_ = p_def->get_pos_units();
	out->rot_bias_deg_ = p_def->get_rot_bias_deg();
	out->tpos_units_ = p_def->get_tpos_units();
	out->renderfov_h_deg_ = p_def->get_renderfov();
	out->flags_ = p_def->get_flags();
	out->scope_max_mag_ = p_def->get_scope_max_mag();
	out->clipsize_ = p_def->get_clipsize();
	return out;
}

void PlayerViewmodelDef::_bind_methods() {
	ClassDB::bind_static_method("PlayerViewmodelDef", D_METHOD("from_weapon_def", "def"),
			&PlayerViewmodelDef::from_weapon_def);
#define PLAYER_VIEWMODEL_DEF_BIND(m_type, m_name, m_default)                                        \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PlayerViewmodelDef::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &PlayerViewmodelDef::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
	PLAYER_VIEWMODEL_DEF_FIELDS(PLAYER_VIEWMODEL_DEF_BIND)
#undef PLAYER_VIEWMODEL_DEF_BIND
}
