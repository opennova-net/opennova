#include "nova_particle_effect.h"

using namespace godot;

void NovaParticleEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_id", "value"), &NovaParticleEffect::set_id);
	ClassDB::bind_method(D_METHOD("get_id"), &NovaParticleEffect::get_id);
	ClassDB::bind_method(D_METHOD("set_pdefs", "value"), &NovaParticleEffect::set_pdefs);
	ClassDB::bind_method(D_METHOD("get_pdefs"), &NovaParticleEffect::get_pdefs);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "id"), "set_id", "get_id");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "pdefs"), "set_pdefs", "get_pdefs");
}

void NovaParticleEffect::set_id(const String &p_value) {
	id = p_value;
	emit_changed();
}
String NovaParticleEffect::get_id() const { return id; }

void NovaParticleEffect::set_pdefs(const PackedStringArray &p_value) {
	pdefs = p_value;
	emit_changed();
}
PackedStringArray NovaParticleEffect::get_pdefs() const { return pdefs; }

void NovaParticleEffect::copy_from_native(const opennova::particle::EffectDef &effect) {
	id = String::utf8(effect.id.c_str());
	pdefs.clear();
	pdefs.resize(static_cast<int>(effect.pdefs.size()));
	for (int i = 0; i < static_cast<int>(effect.pdefs.size()); ++i) {
		pdefs[i] = String::utf8(effect.pdefs[static_cast<size_t>(i)].c_str());
	}
}

opennova::particle::EffectDef NovaParticleEffect::to_native() const {
	opennova::particle::EffectDef out;
	out.id = id.utf8().get_data();
	out.pdefs.reserve(static_cast<size_t>(pdefs.size()));
	for (int i = 0; i < pdefs.size(); ++i) {
		out.pdefs.emplace_back(pdefs[i].utf8().get_data());
	}
	return out;
}
