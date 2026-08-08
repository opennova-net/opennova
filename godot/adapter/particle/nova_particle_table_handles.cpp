#include "nova_particle_table_handles.h"

using namespace godot;

void NovaParticleTableHandles::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_table_id", "value"), &NovaParticleTableHandles::set_table_id);
	ClassDB::bind_method(D_METHOD("get_table_id"), &NovaParticleTableHandles::get_table_id);
	ClassDB::bind_method(D_METHOD("set_handlecount", "value"), &NovaParticleTableHandles::set_handlecount);
	ClassDB::bind_method(D_METHOD("get_handlecount"), &NovaParticleTableHandles::get_handlecount);
	ClassDB::bind_method(D_METHOD("set_tightness", "value"), &NovaParticleTableHandles::set_tightness);
	ClassDB::bind_method(D_METHOD("get_tightness"), &NovaParticleTableHandles::get_tightness);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "table_id"), "set_table_id", "get_table_id");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "handlecount"), "set_handlecount", "get_handlecount");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tightness"), "set_tightness", "get_tightness");
}

void NovaParticleTableHandles::set_table_id(const String &p_value) {
	table_id = p_value;
	emit_changed();
}
String NovaParticleTableHandles::get_table_id() const { return table_id; }

void NovaParticleTableHandles::set_handlecount(int p_value) {
	handlecount = p_value;
	emit_changed();
}
int NovaParticleTableHandles::get_handlecount() const { return handlecount; }

void NovaParticleTableHandles::set_tightness(int p_value) {
	tightness = p_value;
	emit_changed();
}
int NovaParticleTableHandles::get_tightness() const { return tightness; }

void NovaParticleTableHandles::copy_from_native(const opennova::particle::TableEditHandles &h) {
	table_id = String::utf8(h.table_id.c_str());
	handlecount = h.handlecount;
	tightness = h.tightness;
}

opennova::particle::TableEditHandles NovaParticleTableHandles::to_native() const {
	opennova::particle::TableEditHandles out;
	out.table_id = table_id.utf8().get_data();
	out.handlecount = handlecount;
	out.tightness = tightness;
	return out;
}
