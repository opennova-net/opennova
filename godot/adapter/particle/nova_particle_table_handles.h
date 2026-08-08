#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <particle/particle.h>

namespace godot {

// Wraps opennova::particle::TableEditHandles. Editor-only metadata for a
// [tabledef] (handlecount, tightness). Runtime ignores these fields.
class NovaParticleTableHandles : public Resource {
	GDCLASS(NovaParticleTableHandles, Resource)

private:
	String table_id;
	int handlecount = 0;
	int tightness = 0;

protected:
	static void _bind_methods();

public:
	void set_table_id(const String &p_value);
	String get_table_id() const;
	void set_handlecount(int p_value);
	int get_handlecount() const;
	void set_tightness(int p_value);
	int get_tightness() const;

	void copy_from_native(const opennova::particle::TableEditHandles &h);
	opennova::particle::TableEditHandles to_native() const;
};

} // namespace godot
