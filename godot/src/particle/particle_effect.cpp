#include "nova_particle_effect.h"

using namespace godot;

void ParticleEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_id", "value"), &ParticleEffect::set_id);
	ClassDB::bind_method(D_METHOD("get_id"), &ParticleEffect::get_id);
	ClassDB::bind_method(D_METHOD("set_pdefs", "value"), &ParticleEffect::set_pdefs);
	ClassDB::bind_method(D_METHOD("get_pdefs"), &ParticleEffect::get_pdefs);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "id"), "set_id", "get_id");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "pdefs"), "set_pdefs", "get_pdefs");
}

void ParticleEffect::set_id(const String &p_value) {
	id = p_value;
	emit_changed();
}
String ParticleEffect::get_id() const { return id; }

void ParticleEffect::set_pdefs(const PackedStringArray &p_value) {
	pdefs = p_value;
	emit_changed();
}
PackedStringArray ParticleEffect::get_pdefs() const { return pdefs; }

void ParticleEffect::copy_from_native(const opennova::particle::EffectDef &effect) {
	id = String::utf8(effect.id.c_str());
	pdefs.clear();
	pdefs.resize(static_cast<int>(effect.pdefs.size()));
	for (int i = 0; i < static_cast<int>(effect.pdefs.size()); ++i) {
		pdefs[i] = String::utf8(effect.pdefs[static_cast<size_t>(i)].c_str());
	}
}

opennova::particle::EffectDef ParticleEffect::to_native() const {
	opennova::particle::EffectDef out;
	out.id = id.utf8().get_data();
	out.pdefs.reserve(static_cast<size_t>(pdefs.size()));
	for (int i = 0; i < pdefs.size(); ++i) {
		out.pdefs.emplace_back(pdefs[i].utf8().get_data());
	}
	return out;
}
