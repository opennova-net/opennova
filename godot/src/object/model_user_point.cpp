#include "object/model_user_point.h"

using namespace godot;

void ModelUserPoint::assign(const String &p_name, const Vector3 &p_position,
		const Vector3 &p_rotation, int p_subobject, int p_point_type) {
	name_ = p_name;
	position_ = p_position;
	rotation_ = p_rotation;
	subobject_ = p_subobject;
	point_type_ = p_point_type;
}

void ModelUserPoint::_bind_methods() {
#define MODEL_USER_POINT_FIELD(m_variant, m_name)                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &ModelUserPoint::get_##m_name);        \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name, PROPERTY_HINT_NONE, "",                 \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),              \
			"", "get_" #m_name);
	MODEL_USER_POINT_FIELD(Variant::STRING, name)
	MODEL_USER_POINT_FIELD(Variant::VECTOR3, position)
	MODEL_USER_POINT_FIELD(Variant::VECTOR3, rotation)
	MODEL_USER_POINT_FIELD(Variant::INT, subobject)
	MODEL_USER_POINT_FIELD(Variant::INT, point_type)
#undef MODEL_USER_POINT_FIELD
}
