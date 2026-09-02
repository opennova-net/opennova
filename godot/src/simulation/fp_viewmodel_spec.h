#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/simassets/fp_viewmodel_spec.h>

namespace godot {

// The first-person submit spec (simassets::FpViewmodelSpec): the gun and arms
// graphics, the clip .adm, and whether the arms submit at all. Produced by
// Simulation.fp_viewmodel_spec; the struct carries the witnesses.
class FpViewmodelSpec : public RefCounted {
	GDCLASS(FpViewmodelSpec, RefCounted)

	opennova::simassets::FpViewmodelSpec value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::simassets::FpViewmodelSpec &p_value) { value_ = p_value; }

	// Empty = a resolved def with no fpModel intentionally submits no gun.
	String get_gun() const;
	// The selected character's arms graphic; empty = no arms submit.
	String get_arms() const;
	String get_adm() const;
	// False when the mount is emplaced or no character arms resolved.
	bool get_show_arms() const { return value_.show_arms; }
};

} // namespace godot
