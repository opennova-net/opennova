#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

// The dynamic point-light pool's diagnostic snapshot (LightScene::get_report):
// the pool census, the selection facts of the last render frame, the static
// row census and one EffectLightRow per selected light. Read-write so a stub
// authors one; to_json_value() is the renderer-diagnostics JSON boundary.

#define EFFECT_LIGHT_ACCESSORS(m_type, m_name, m_default)     \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define EFFECT_LIGHT_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

// One selected light: Godot-space position, the color, range = atten_end *
// 1.25 and the quadratic attenuation term, the opaque lease handle gameplay
// holds and the retail slot word it wraps.
#define EFFECT_LIGHT_ROW_FIELDS(X)               \
	X(Vector3, position, Vector3())              \
	X(Color, color, Color(1, 1, 1, 1))           \
	X(float, range, 0.0f)                        \
	X(float, attenuation_quadratic, 0.0f)        \
	X(int, handle, 0)                            \
	X(int, retail_handle, 0)

class EffectLightRow : public RefCounted {
	GDCLASS(EffectLightRow, RefCounted)

public:
	EFFECT_LIGHT_ROW_FIELDS(EFFECT_LIGHT_ACCESSORS)
	Dictionary to_json_value() const;

protected:
	static void _bind_methods();

private:
	EFFECT_LIGHT_ROW_FIELDS(EFFECT_LIGHT_MEMBER)
};

#define EFFECT_LIGHT_REPORT_FIELDS(X)             \
	X(int, live, 0)                               \
	X(int, high_water, 0)                         \
	X(int, last_query, 0)                         \
	X(int, selected, 0)                           \
	X(String, selection_mode, String("none"))     \
	X(String, owner_isolation, String("none"))    \
	X(int, models, 0)                             \
	X(int, lit_models, 0)                         \
	X(int, static_rows, 0)                        \
	X(int, static_draws, 0)                       \
	X(int, lit_static_draws, 0)

class EffectLightReport : public RefCounted {
	GDCLASS(EffectLightReport, RefCounted)

public:
	EFFECT_LIGHT_REPORT_FIELDS(EFFECT_LIGHT_ACCESSORS)
	TypedArray<EffectLightRow> get_rows() const { return rows_; }
	void set_rows(const TypedArray<EffectLightRow> &p_value) { rows_ = p_value; }
	void add_row(const Ref<EffectLightRow> &p_row) { rows_.push_back(p_row); }
	Dictionary to_json_value() const;

protected:
	static void _bind_methods();

private:
	EFFECT_LIGHT_REPORT_FIELDS(EFFECT_LIGHT_MEMBER)
	TypedArray<EffectLightRow> rows_;
};

} // namespace godot

#undef EFFECT_LIGHT_ACCESSORS
#undef EFFECT_LIGHT_MEMBER
