#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// Typed semantic identity of the actual first-person arms submit (the former
// first_person_arms_witness.gd, ADR 0043 slice G8): the authority-stamped
// character id, the submitted arms graphic and its raw camo triplet, or the
// reason no visible arms submit exists. Transport owners (the capture probes)
// read these fields only at their serialization boundary (ADR 0017).
class FirstPersonArmsWitness : public RefCounted {
	GDCLASS(FirstPersonArmsWitness, RefCounted)

	int character_id_ = 0;
	String arms_graphic_;
	PackedInt32Array arms_camo_;
	String error_;

protected:
	static void _bind_methods();

public:
	int get_character_id() const { return character_id_; }
	void set_character_id(int p_value) { character_id_ = p_value; }
	String get_arms_graphic() const { return arms_graphic_; }
	void set_arms_graphic(const String &p_value) { arms_graphic_ = p_value; }
	PackedInt32Array get_arms_camo() const { return arms_camo_; }
	void set_arms_camo(const PackedInt32Array &p_value) { arms_camo_ = p_value; }
	String get_error() const { return error_; }
	void set_error(const String &p_value) { error_ = p_value; }

	bool is_valid() const {
		return error_.is_empty() && character_id_ != 0 && !arms_graphic_.is_empty() &&
				arms_camo_.size() == 3;
	}
};

} // namespace godot
