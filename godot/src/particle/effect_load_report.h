#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/particle/effect_scene.h>

namespace godot {

// One EffectScene.open result: the engine's EffectLoadReport counters
// (runtime/particle/effect_scene.h) plus the input / ignored (null) document
// counts of the call. Read-only, assigned by EffectScene::open.
#define EFFECT_LOAD_REPORT_FIELDS(X)         \
	X(document_count)                        \
	X(effect_count)                          \
	X(particle_definition_count)             \
	X(table_definition_count)                \
	X(duplicate_effect_count)                \
	X(duplicate_particle_count)              \
	X(unresolved_particle_reference_count)   \
	X(input_document_count)                  \
	X(ignored_document_count)

class EffectLoadReport : public RefCounted {
	GDCLASS(EffectLoadReport, RefCounted)

public:
	void assign(const opennova::particle::EffectLoadReport &p_report, int p_input_document_count,
			int p_ignored_document_count);

#define EFFECT_LOAD_REPORT_GETTER(m_name) int get_##m_name() const { return m_name##_; }
	EFFECT_LOAD_REPORT_FIELDS(EFFECT_LOAD_REPORT_GETTER)
#undef EFFECT_LOAD_REPORT_GETTER

protected:
	static void _bind_methods();

private:
#define EFFECT_LOAD_REPORT_MEMBER(m_name) int m_name##_ = 0;
	EFFECT_LOAD_REPORT_FIELDS(EFFECT_LOAD_REPORT_MEMBER)
#undef EFFECT_LOAD_REPORT_MEMBER
};

} // namespace godot
