#include "particle/particle_effect.h"
#include "util/string_convert.h"

using namespace godot;

void ParticleEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_id"), &ParticleEffect::get_id);
	ClassDB::bind_method(D_METHOD("get_pdefs"), &ParticleEffect::get_pdefs);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "id", PROPERTY_HINT_NONE, "",
			PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_id");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "pdefs", PROPERTY_HINT_NONE, "",
			PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_pdefs");
}

String ParticleEffect::get_id() const { return opennova::to_gd(value_.id); }

PackedStringArray ParticleEffect::get_pdefs() const {
	PackedStringArray out;
	for (const auto &id : value_.pdefs) out.push_back(opennova::to_gd(id));
	return out;
}
