#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

// One authored .3di LGHT record as the effect-light director spawns it
// (ObjectData.get_light_info): the model-space offset, the attenuation range,
// the two generator colors (BGR bytes -> Color), the falloff angle, the owning
// subobject, the three disable flags, the color-generator style/phase/rate and
// the target-type bit. Read-write so a test authors a transient record; the
// engine's ThreediLight layout carries the field witnesses.
class ModelLight : public RefCounted {
	GDCLASS(ModelLight, RefCounted)

#define MODEL_LIGHT_FIELDS(X)                  \
	X(String, name, String())                  \
	X(Vector3, position, Vector3())            \
	X(float, atten_start, 0.0f)                \
	X(float, atten_end, 0.0f)                  \
	X(Color, color_start, Color(1, 1, 1, 1))   \
	X(Color, color_end, Color(1, 1, 1, 1))     \
	X(int, falloff_deg, 0)                     \
	X(int, subobject, 0)                       \
	X(bool, disable_corona, false)             \
	X(bool, disable_lightterrain, false)       \
	X(bool, disable_lightobjects, false)       \
	X(int, colorgen_style, 0)                  \
	X(int, colorgen_phase, 0)                  \
	X(int, colorgen_rate, 0)                   \
	X(int, light_type, 0)

#define MODEL_LIGHT_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;
	MODEL_LIGHT_FIELDS(MODEL_LIGHT_MEMBER)
#undef MODEL_LIGHT_MEMBER

protected:
	static void _bind_methods();

public:
#define MODEL_LIGHT_ACCESSORS(m_type, m_name, m_default)                 \
	m_type get_##m_name() const { return m_name##_; }                  \
	void set_##m_name(const m_type &p_value) { m_name##_ = p_value; }
	MODEL_LIGHT_FIELDS(MODEL_LIGHT_ACCESSORS)
#undef MODEL_LIGHT_ACCESSORS
};

} // namespace godot
