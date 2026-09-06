#include "model/model_bounding_volume_3d.h"

using namespace godot;

void ModelBoundingVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_collidable_type", "value"), &ModelBoundingVolume3D::set_collidable_type);
	ClassDB::bind_method(D_METHOD("get_collidable_type"), &ModelBoundingVolume3D::get_collidable_type);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collidable_type"), "set_collidable_type", "get_collidable_type");
	ClassDB::bind_method(D_METHOD("set_flags", "value"), &ModelBoundingVolume3D::set_flags);
	ClassDB::bind_method(D_METHOD("get_flags"), &ModelBoundingVolume3D::get_flags);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "flags"), "set_flags", "get_flags");
	ClassDB::bind_method(D_METHOD("set_bounds", "value"), &ModelBoundingVolume3D::set_bounds);
	ClassDB::bind_method(D_METHOD("get_bounds"), &ModelBoundingVolume3D::get_bounds);
	ADD_PROPERTY(PropertyInfo(Variant::AABB, "bounds"), "set_bounds", "get_bounds");
	ClassDB::bind_method(D_METHOD("set_planes", "value"), &ModelBoundingVolume3D::set_planes);
	ClassDB::bind_method(D_METHOD("get_planes"), &ModelBoundingVolume3D::get_planes);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "planes", PROPERTY_HINT_ARRAY_TYPE, "Plane"), "set_planes",
			"get_planes");
	ClassDB::bind_method(D_METHOD("set_box", "box"), &ModelBoundingVolume3D::set_box);
}

void ModelBoundingVolume3D::set_box(const AABB &p_box) {
	bounds_ = p_box;
	const Vector3 lo = p_box.position;
	const Vector3 hi = p_box.position + p_box.size;
	planes_.clear();
	planes_.push_back(Plane(Vector3(1, 0, 0), hi.x));
	planes_.push_back(Plane(Vector3(-1, 0, 0), -lo.x));
	planes_.push_back(Plane(Vector3(0, 1, 0), hi.y));
	planes_.push_back(Plane(Vector3(0, -1, 0), -lo.y));
	planes_.push_back(Plane(Vector3(0, 0, 1), hi.z));
	planes_.push_back(Plane(Vector3(0, 0, -1), -lo.z));
}
