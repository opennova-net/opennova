#include "particle/particle_table_handles.h"

using namespace godot;

void ParticleTableHandles::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_table_id", "value"), &ParticleTableHandles::set_table_id);
	ClassDB::bind_method(D_METHOD("get_table_id"), &ParticleTableHandles::get_table_id);
	ClassDB::bind_method(D_METHOD("set_handlecount", "value"), &ParticleTableHandles::set_handlecount);
	ClassDB::bind_method(D_METHOD("get_handlecount"), &ParticleTableHandles::get_handlecount);
	ClassDB::bind_method(D_METHOD("set_tightness", "value"), &ParticleTableHandles::set_tightness);
	ClassDB::bind_method(D_METHOD("get_tightness"), &ParticleTableHandles::get_tightness);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "table_id"), "set_table_id", "get_table_id");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "handlecount"), "set_handlecount", "get_handlecount");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tightness"), "set_tightness", "get_tightness");
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
