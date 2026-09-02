#include "simulation/player_weapon_view.h"

#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<bool>() { return Variant::BOOL; }
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<float>() { return Variant::FLOAT; }
template <>
constexpr Variant::Type variant_type_of<String>() { return Variant::STRING; }

} // namespace

Dictionary PlayerWeaponView::to_json_value() const {
	Dictionary out;
#define PLAYER_WEAPON_VIEW_JSON(m_type, m_name, m_default) out[#m_name] = m_name##_;
	PLAYER_WEAPON_VIEW_FIELDS(PLAYER_WEAPON_VIEW_JSON)
#undef PLAYER_WEAPON_VIEW_JSON
	return out;
}

void PlayerWeaponView::_bind_methods() {
#define PLAYER_WEAPON_VIEW_BIND(m_type, m_name, m_default)                                        \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PlayerWeaponView::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &PlayerWeaponView::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
	PLAYER_WEAPON_VIEW_FIELDS(PLAYER_WEAPON_VIEW_BIND)
#undef PLAYER_WEAPON_VIEW_BIND
	ClassDB::bind_method(D_METHOD("to_json_value"), &PlayerWeaponView::to_json_value);
}
