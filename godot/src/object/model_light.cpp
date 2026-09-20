#include "object/model_light.h"
#include "util/variant_type_of.h"

using namespace godot;

void ModelLight::_bind_methods() {
#define MODEL_LIGHT_BIND(m_type, m_name, m_default)                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &ModelLight::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &ModelLight::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
	MODEL_LIGHT_FIELDS(MODEL_LIGHT_BIND)
#undef MODEL_LIGHT_BIND
}
