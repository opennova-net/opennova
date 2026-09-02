#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

// The round hit-detection reality for the F3 hitbox view
// (Simulation::get_hitbox_debug): one row per nearby static/vehicle entity
// with its transformed CFAC triangles, one row per posed person section
// sphere. Read-write with make() factories so the view test authors a
// payload; positions are Godot world space.

#define HITBOX_DEBUG_ACCESSORS(m_type, m_name, m_default)     \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define HITBOX_DEBUG_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

// One entity's hit mesh: the triangle list with its per-face material and
// flag words; `has_faces` false = the bound-sphere stand-in.
#define HITBOX_DEBUG_ENTITY_FIELDS(X)                    \
	X(int, entity_handle, 0)                             \
	X(Vector3, pos, Vector3())                           \
	X(float, bound_radius, 0.0f)                         \
	X(bool, husk, false)                                 \
	X(bool, has_faces, true)                             \
	X(int, face_total, 0)                                \
	X(PackedVector3Array, tris, PackedVector3Array())    \
	X(PackedByteArray, materials, PackedByteArray())     \
	X(PackedInt32Array, flags, PackedInt32Array())

class HitboxDebugEntity : public RefCounted {
	GDCLASS(HitboxDebugEntity, RefCounted)

public:
	HITBOX_DEBUG_ENTITY_FIELDS(HITBOX_DEBUG_ACCESSORS)

protected:
	static void _bind_methods();

private:
	HITBOX_DEBUG_ENTITY_FIELDS(HITBOX_DEBUG_MEMBER)
};

// One posed person section sphere; `fallback` marks the bounded stand-in
// for an entity whose graphic supplies no usable authored sections.
#define HITBOX_DEBUG_ORGANIC_FIELDS(X) \
	X(int, entity_handle, 0)           \
	X(int, section, 0)                 \
	X(Vector3, pos, Vector3())         \
	X(float, radius, 0.0f)             \
	X(float, authored_radius, 0.0f)    \
	X(bool, masked, false)             \
	X(bool, fallback, false)

class HitboxDebugOrganic : public RefCounted {
	GDCLASS(HitboxDebugOrganic, RefCounted)

public:
	HITBOX_DEBUG_ORGANIC_FIELDS(HITBOX_DEBUG_ACCESSORS)
	static Ref<HitboxDebugOrganic> make(int p_entity_handle, int p_section, const Vector3 &p_pos,
			float p_radius, float p_authored_radius, bool p_masked, bool p_fallback);

protected:
	static void _bind_methods();

private:
	HITBOX_DEBUG_ORGANIC_FIELDS(HITBOX_DEBUG_MEMBER)
};

class HitboxDebugReport : public RefCounted {
	GDCLASS(HitboxDebugReport, RefCounted)

public:
	TypedArray<HitboxDebugEntity> get_entities() const { return entities_; }
	void set_entities(const TypedArray<HitboxDebugEntity> &p_value) { entities_ = p_value; }
	TypedArray<HitboxDebugOrganic> get_organics() const { return organics_; }
	void set_organics(const TypedArray<HitboxDebugOrganic> &p_value) { organics_ = p_value; }
	void add_entity(const Ref<HitboxDebugEntity> &p_row) { entities_.push_back(p_row); }
	void add_organic(const Ref<HitboxDebugOrganic> &p_row) { organics_.push_back(p_row); }
	static Ref<HitboxDebugReport> make(const TypedArray<HitboxDebugEntity> &p_entities,
			const TypedArray<HitboxDebugOrganic> &p_organics);

protected:
	static void _bind_methods();

private:
	TypedArray<HitboxDebugEntity> entities_;
	TypedArray<HitboxDebugOrganic> organics_;
};

} // namespace godot

#undef HITBOX_DEBUG_ACCESSORS
#undef HITBOX_DEBUG_MEMBER
