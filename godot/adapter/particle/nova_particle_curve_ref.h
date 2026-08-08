#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <particle/particle.h>

namespace godot {

// Wraps opennova::particle::CurveRef. CurveRefs reference a [tabledef] by
// name; reverse/inverse modifiers were decoded from
// CParticleDef_ParseProperties @ 0x5ea320 (e.g. scale_func @ 0x5eafdd).
class NovaParticleCurveRef : public Resource {
	GDCLASS(NovaParticleCurveRef, Resource)

private:
	String name;
	bool reverse = false;
	bool inverse = false;
	bool present = false;

protected:
	static void _bind_methods();

public:
	void set_name(const String &p_value);
	String get_name() const;
	void set_reverse(bool p_value);
	bool get_reverse() const;
	void set_inverse(bool p_value);
	bool get_inverse() const;
	void set_present(bool p_value);
	bool get_present() const;

	void copy_from_native(const opennova::particle::CurveRef &ref);
	opennova::particle::CurveRef to_native() const;
};

} // namespace godot
