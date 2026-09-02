#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

// The two LightScene spawn requests as typed records (ADR 0042 d5): the
// LGHT-record model light and the transient glow. `make` seeds the required
// facts; the chain helpers (`owned_by`, `fading`, `masking`, `attached`,
// `in_blink_box`) return the record so a caller composes the optional ones
// inline. Every position is Godot world space; the witnesses live on
// engine/runtime/renderer/light_scene.h.

#define LIGHT_SPAWN_ACCESSORS(m_type, m_name, m_default)      \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define LIGHT_SPAWN_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

// A transient glow (muzzle / impact / death / round legs): radius in world
// units; fade_mode / fade_duration as the retail instance words (mode 1 =
// permanent, 2 = the impact countdown, 3 = the muzzle re-arm); owner_entity /
// owner_section scope the light to its owner's draws; the disable_* flags
// are the record render masks; corona_lower_half_radius is render flag
// 0x100 (the impact flash's corona re-centre).
#define GLOW_SPAWN_FIELDS(X)                     \
	X(Vector3, position, Vector3())              \
	X(float, radius, 0.0f)                       \
	X(Color, color, Color(1, 1, 1))              \
	X(int, fade_mode, 1)                         \
	X(int, fade_duration, -1)                    \
	X(int64_t, owner_entity, 0)                  \
	X(int, owner_section, 0)                     \
	X(bool, disable_corona, false)               \
	X(bool, disable_terrain, false)              \
	X(bool, disable_objects, false)              \
	X(bool, corona_lower_half_radius, false)

class GlowSpawn : public RefCounted {
	GDCLASS(GlowSpawn, RefCounted)

public:
	GLOW_SPAWN_FIELDS(LIGHT_SPAWN_ACCESSORS)

	static Ref<GlowSpawn> make(const Vector3 &p_position, float p_radius, const Color &p_color);
	Ref<GlowSpawn> owned_by(int64_t p_owner_entity, int p_owner_section);
	Ref<GlowSpawn> fading(int p_fade_mode, int p_fade_duration);
	Ref<GlowSpawn> masking(bool p_disable_corona, bool p_disable_terrain, bool p_disable_objects);

protected:
	static void _bind_methods();

private:
	GLOW_SPAWN_FIELDS(LIGHT_SPAWN_MEMBER)
};

// One LGHT record's light: atten_end in world units (the spawner scales it
// by 65536), the colorgen style / phase / rate with its start / end colors
// (style 0 = no gen block), and the owner-attach facts
// opennova::renderer::resolve_model_light_owner decides from — the record's
// authored subobject, the spawning entity and whether it is a building, and
// the blink box the spawning entity stands in (0 = none).
#define MODEL_LIGHT_SPAWN_FIELDS(X)              \
	X(Vector3, position, Vector3())              \
	X(float, atten_end, 0.0f)                    \
	X(float, intensity, 1.0f)                    \
	X(int, style, 0)                             \
	X(int, phase, 0)                             \
	X(int, rate, 0)                              \
	X(Color, color_start, Color(1, 1, 1))        \
	X(Color, color_end, Color(1, 1, 1))          \
	X(int, attach_bone, 0)                       \
	X(int64_t, spawning_entity, 0)               \
	X(bool, spawner_is_building, false)          \
	X(int64_t, blink_owner_entity, 0)            \
	X(int, blink_section, 0)                     \
	X(bool, disable_corona, false)               \
	X(bool, disable_terrain, false)              \
	X(bool, disable_objects, false)

class ModelLightSpawn : public RefCounted {
	GDCLASS(ModelLightSpawn, RefCounted)

public:
	MODEL_LIGHT_SPAWN_FIELDS(LIGHT_SPAWN_ACCESSORS)

	static Ref<ModelLightSpawn> make(const Vector3 &p_position, float p_atten_end);
	Ref<ModelLightSpawn> attached(int p_attach_bone, int64_t p_spawning_entity,
			bool p_spawner_is_building);
	Ref<ModelLightSpawn> in_blink_box(int64_t p_owner_entity, int p_section);
	Ref<ModelLightSpawn> masking(bool p_disable_corona, bool p_disable_terrain,
			bool p_disable_objects);

protected:
	static void _bind_methods();

private:
	MODEL_LIGHT_SPAWN_FIELDS(LIGHT_SPAWN_MEMBER)
};

} // namespace godot
