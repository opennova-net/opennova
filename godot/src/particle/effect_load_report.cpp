#include "particle/effect_load_report.h"

using namespace godot;

void EffectLoadReport::assign(const opennova::particle::EffectLoadReport &p_report,
		int p_input_document_count, int p_ignored_document_count) {
	document_count_ = static_cast<int>(p_report.document_count);
	effect_count_ = static_cast<int>(p_report.effect_count);
	particle_definition_count_ = static_cast<int>(p_report.particle_definition_count);
	table_definition_count_ = static_cast<int>(p_report.table_definition_count);
	duplicate_effect_count_ = static_cast<int>(p_report.duplicate_effect_count);
	duplicate_particle_count_ = static_cast<int>(p_report.duplicate_particle_count);
	unresolved_particle_reference_count_ =
			static_cast<int>(p_report.unresolved_particle_reference_count);
	input_document_count_ = p_input_document_count;
	ignored_document_count_ = p_ignored_document_count;
}

void EffectLoadReport::_bind_methods() {
#define EFFECT_LOAD_REPORT_BIND(m_name)                                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &EffectLoadReport::get_##m_name);            \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name, PROPERTY_HINT_NONE, "",                    \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),                    \
			"", "get_" #m_name);
	EFFECT_LOAD_REPORT_FIELDS(EFFECT_LOAD_REPORT_BIND)
#undef EFFECT_LOAD_REPORT_BIND
}
