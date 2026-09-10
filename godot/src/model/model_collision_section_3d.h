#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

// One COBJ of a model's collision block, named "CO##" (the section pairs with
// render part ## by ordinal). The node position is the section's authored
// offset (CXLT) in the presentation frame; `parent_part` is the COBJ parent
// word (the parent of the paired part, the root naming itself). A bone
// sphere (the skeletal rigs' per-bone sections) carries its center and
// radius here and no faces or volumes; every other section derives its
// bounds from the "CO## faces" mesh and the volume children.
class ModelCollisionSection3D : public Node3D {
	GDCLASS(ModelCollisionSection3D, Node3D)

	int parent_part_ = 0;
	bool sphere_ = false;
	Vector3 sphere_center_;
	float sphere_radius_ = 0.0f;

protected:
	static void _bind_methods();

public:
	void set_parent_part(int p_value) { parent_part_ = p_value; }
	int get_parent_part() const { return parent_part_; }
	void set_sphere(bool p_value) { sphere_ = p_value; }
	bool get_sphere() const { return sphere_; }
	void set_sphere_center(const Vector3 &p_value) { sphere_center_ = p_value; }
	Vector3 get_sphere_center() const { return sphere_center_; }
	void set_sphere_radius(float p_value) { sphere_radius_ = p_value; }
	float get_sphere_radius() const { return sphere_radius_; }
};

} // namespace godot
