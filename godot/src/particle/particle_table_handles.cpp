#include "particle/particle_table_handles.h"

using namespace godot;

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
