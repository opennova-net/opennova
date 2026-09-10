#include "model/model_user_point_3d.h"

using namespace godot;

void ModelUserPoint3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_point_type", "value"), &ModelUserPoint3D::set_point_type);
	ClassDB::bind_method(D_METHOD("get_point_type"), &ModelUserPoint3D::get_point_type);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "point_type", PROPERTY_HINT_ENUM, "Gameplay:71,Effect:83"),
			"set_point_type", "get_point_type");
	ClassDB::bind_method(D_METHOD("set_part_index", "value"), &ModelUserPoint3D::set_part_index);
	ClassDB::bind_method(D_METHOD("get_part_index"), &ModelUserPoint3D::get_part_index);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "part_index"), "set_part_index", "get_part_index");
	ClassDB::bind_method(D_METHOD("set_label", "value"), &ModelUserPoint3D::set_label);
	ClassDB::bind_method(D_METHOD("get_label"), &ModelUserPoint3D::get_label);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "label"), "set_label", "get_label");
	ClassDB::bind_method(D_METHOD("set_direction", "value"), &ModelUserPoint3D::set_direction);
	ClassDB::bind_method(D_METHOD("get_direction"), &ModelUserPoint3D::get_direction);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "direction"), "set_direction", "get_direction");
}
