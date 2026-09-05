#include "object/model_light.h"

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
template <>
constexpr Variant::Type variant_type_of<Vector3>() { return Variant::VECTOR3; }
template <>
constexpr Variant::Type variant_type_of<Color>() { return Variant::COLOR; }

} // namespace

void ModelLight::_bind_methods() {
#define MODEL_LIGHT_BIND(m_type, m_name, m_default)                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &ModelLight::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &ModelLight::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
	MODEL_LIGHT_FIELDS(MODEL_LIGHT_BIND)
#undef MODEL_LIGHT_BIND
}
