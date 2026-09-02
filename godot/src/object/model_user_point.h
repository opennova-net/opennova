#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

// One authored .3di userpoint (USRP row) as the shell reads it: the name, the
// decoded model-space position and direction in the public model frame, the
// owning subobject and the authored type word (ObjectData.get_user_point_info).
// The decode is the engine's (formats/threedi threedi_user_point_position /
// _direction); the row is read-only.
class ModelUserPoint : public RefCounted {
	GDCLASS(ModelUserPoint, RefCounted)

	String name_;
	Vector3 position_;
	Vector3 rotation_;
	int subobject_ = 0;
	int point_type_ = 0;

protected:
	static void _bind_methods();

public:
	void assign(const String &p_name, const Vector3 &p_position, const Vector3 &p_rotation,
			int p_subobject, int p_point_type);

	String get_name() const { return name_; }
	Vector3 get_position() const { return position_; }
	// The authored direction (a unit vector in model space).
	Vector3 get_rotation() const { return rotation_; }
	int get_subobject() const { return subobject_; }
	int get_point_type() const { return point_type_; }
};

} // namespace godot
