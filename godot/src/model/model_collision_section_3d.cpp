#include "model/model_collision_section_3d.h"

using namespace godot;

void ModelCollisionSection3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_parent_part", "value"), &ModelCollisionSection3D::set_parent_part);
	ClassDB::bind_method(D_METHOD("get_parent_part"), &ModelCollisionSection3D::get_parent_part);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "parent_part"), "set_parent_part", "get_parent_part");
	ClassDB::bind_method(D_METHOD("set_sphere", "value"), &ModelCollisionSection3D::set_sphere);
	ClassDB::bind_method(D_METHOD("get_sphere"), &ModelCollisionSection3D::get_sphere);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sphere"), "set_sphere", "get_sphere");
	ClassDB::bind_method(D_METHOD("set_sphere_center", "value"), &ModelCollisionSection3D::set_sphere_center);
	ClassDB::bind_method(D_METHOD("get_sphere_center"), &ModelCollisionSection3D::get_sphere_center);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "sphere_center"), "set_sphere_center", "get_sphere_center");
	ClassDB::bind_method(D_METHOD("set_sphere_radius", "value"), &ModelCollisionSection3D::set_sphere_radius);
	ClassDB::bind_method(D_METHOD("get_sphere_radius"), &ModelCollisionSection3D::get_sphere_radius);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sphere_radius"), "set_sphere_radius", "get_sphere_radius");
}
