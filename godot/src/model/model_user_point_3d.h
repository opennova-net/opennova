#pragma once

#include <godot_cpp/classes/marker3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

// One USRP row, named "UPcNN <label>" per the scene naming contract: the
// type word (71 gameplay, 83 effect), the owning part, the label the file
// carries as the point's name, and the authored direction (a unit vector).
// The node position is the point in the presentation frame. A plain Marker3D
// or Node3D carrying the contract name exports the same way, its direction
// taken from the node's -Z axis.
class ModelUserPoint3D : public Marker3D {
	GDCLASS(ModelUserPoint3D, Marker3D)

	int point_type_ = 71;
	int part_index_ = 0;
	String label_;
	Vector3 direction_ = Vector3(0, 0, -1);

protected:
	static void _bind_methods();

public:
	void set_point_type(int p_value) { point_type_ = p_value; }
	int get_point_type() const { return point_type_; }
	void set_part_index(int p_value) { part_index_ = p_value; }
	int get_part_index() const { return part_index_; }
	void set_label(const String &p_value) { label_ = p_value; }
	String get_label() const { return label_; }
	void set_direction(const Vector3 &p_value) { direction_ = p_value; }
	Vector3 get_direction() const { return direction_; }
};

} // namespace godot
