#include "particle/particle_table_handles.h"

using namespace godot;

void ParticleTableHandles::_bind_methods() {
	// No GDScript surface: the class stays registered only as the typed
	// .ptl edithandles record ParticleFile carries through load/save.
}

void ParticleTableHandles::set_table_id(const String &p_value) {
	table_id = p_value;
	emit_changed();
}
String ParticleTableHandles::get_table_id() const { return table_id; }

void ParticleTableHandles::set_handlecount(int p_value) {
	handlecount = p_value;
	emit_changed();
}
int ParticleTableHandles::get_handlecount() const { return handlecount; }

void ParticleTableHandles::set_tightness(int p_value) {
	tightness = p_value;
	emit_changed();
}
int ParticleTableHandles::get_tightness() const { return tightness; }

void ParticleTableHandles::copy_from_native(const opennova::particle::TableEditHandles &h) {
	table_id = String::utf8(h.table_id.c_str());
	handlecount = h.handlecount;
	tightness = h.tightness;
}

opennova::particle::TableEditHandles ParticleTableHandles::to_native() const {
	opennova::particle::TableEditHandles out;
	out.table_id = table_id.utf8().get_data();
	out.handlecount = handlecount;
	out.tightness = tightness;
	return out;
}
