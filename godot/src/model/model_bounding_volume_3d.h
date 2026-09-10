#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/plane.hpp>

namespace godot {

// One BVOL of a collision section, named "<TYPE>NN-colonly" per the scene
// naming contract: the collidable type and flag words, the AABB and the
// bounding planes that carve the convex region, all in the presentation
// frame local to the section (a Plane's d is the point-on-plane distance:
// `normal.dot(p) == d`, the negated on-disk radius). `set_box` is the
// authoring shortcut for the six-plane axis box every CB volume is.
class ModelBoundingVolume3D : public Node3D {
	GDCLASS(ModelBoundingVolume3D, Node3D)

	int collidable_type_ = 1;
	int flags_ = 0;
	AABB bounds_;
	Array planes_;

protected:
	static void _bind_methods();

public:
	void set_collidable_type(int p_value) { collidable_type_ = p_value; }
	int get_collidable_type() const { return collidable_type_; }
	void set_flags(int p_value) { flags_ = p_value; }
	int get_flags() const { return flags_; }
	void set_bounds(const AABB &p_value) { bounds_ = p_value; }
	AABB get_bounds() const { return bounds_; }
	void set_planes(const Array &p_value) { planes_ = p_value; }
	Array get_planes() const { return planes_; }

	// The bounds and the six outward planes of an axis box.
	void set_box(const AABB &p_box);
};

} // namespace godot
