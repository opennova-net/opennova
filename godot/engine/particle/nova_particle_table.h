#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <particle/particle.h>

namespace godot {

// Wraps opennova::particle::TableDef — a 32×8 uint8 LUT used as a curve
// lookup for color/alpha/scale animation. `sample(t)` returns a uint8 by
// linear interpolation across the 256-byte logical curve.
//
// Engine writers: CParticleTableDef_WriteToFile @ 0x5e27e0.
class NovaParticleTable : public Resource {
	GDCLASS(NovaParticleTable, Resource)

private:
	String id;
	// Stored as one PackedByteArray of length 256 (32 rows × 8 cols, row-major).
	// Editor exposure as a flat array keeps inspector simple; sample()/get_row()
	// give per-row access.
	PackedByteArray data;

protected:
	static void _bind_methods();

public:
	NovaParticleTable();

	void set_id(const String &p_value);
	String get_id() const;
	void set_data(const PackedByteArray &p_data);
	PackedByteArray get_data() const;

	int row_count() const;
	PackedByteArray get_row(int row) const;
	void set_row(int row, const PackedByteArray &values);

	// Sample the 256-byte curve at t in [0, 1]. Linear interpolation between
	// adjacent bytes; clamped at the ends.
	int sample(float t) const;

	void copy_from_native(const opennova::particle::TableDef &table);
	opennova::particle::TableDef to_native() const;
};

} // namespace godot
