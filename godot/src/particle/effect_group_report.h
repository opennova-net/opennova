#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

// EffectWorld.get_debug_group_report as typed rows (ADR 0017): one
// EffectGroupReport per live group carrying one EffectEmitterReport per
// emitter — the portable simulation values joined to the renderer's
// draw-list bounds by emitter id. Value-only: no particle/render Node
// escapes through the facade. Read-write so a harness authors a row.

#define EFFECT_REPORT_ACCESSORS(m_type, m_name, m_default)            \
	m_type get_##m_name() const { return m_name##_; }                \
	void set_##m_name(const m_type &p_value) { m_name##_ = p_value; }
#define EFFECT_REPORT_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

// name = the emitter's particle definition; alive = its live particle count;
// emitting = still emitting; rendered = the quads the last draw compiled for
// it, bounds / bounds_valid the renderer-owned box of those quads; position /
// forward / age the emitter's simulation pose and age; emit_rate /
// spawn_y_offset / camera_pull are its live parameterized values; kill_plane /
// kill_plane_y its water-plane clip (EffectScene.KillPlane).
#define EFFECT_EMITTER_REPORT_FIELDS(X)     \
	X(String, name, String())               \
	X(int, alive, 0)                        \
	X(bool, emitting, false)                \
	X(int, rendered, 0)                     \
	X(AABB, bounds, AABB())                 \
	X(bool, bounds_valid, false)            \
	X(int64_t, emitter_id, 0)               \
	X(Vector3, position, Vector3())         \
	X(Vector3, forward, Vector3(0, 0, -1))  \
	X(float, age, 0.0f)                     \
	X(float, emit_rate, 0.0f)               \
	X(float, spawn_y_offset, 0.0f)          \
	X(float, camera_pull, 0.0f)             \
	X(int, kill_plane, 0)                   \
	X(float, kill_plane_y, 0.0f)

class EffectEmitterReport : public RefCounted {
	GDCLASS(EffectEmitterReport, RefCounted)

public:
	EFFECT_EMITTER_REPORT_FIELDS(EFFECT_REPORT_ACCESSORS)

protected:
	static void _bind_methods();

private:
	EFFECT_EMITTER_REPORT_FIELDS(EFFECT_REPORT_MEMBER)
};

// id = the native group id; name = the interned effect; owner_key = the
// spawn's owner key (null for unowned groups); source = the defining
// document's file name; forever = an attached group carrying a FOREVEREMIT
// emitter; admission / binding / render_domain = EffectScene's enums;
// detached = stopped and draining; transform = the group pose; source_tick /
// source_order = the spawn's tick provenance; section_tagged = the spawn
// carried an owner tag, so the group takes the building-section gate.
#define EFFECT_GROUP_REPORT_FIELDS(X)         \
	X(int64_t, id, 0)                         \
	X(String, name, String())                 \
	X(String, source, String())               \
	X(bool, forever, false)                   \
	X(int, admission, 0)                      \
	X(int, binding, 0)                        \
	X(int, render_domain, 0)                  \
	X(bool, detached, false)                  \
	X(Transform3D, transform, Transform3D())  \
	X(int64_t, source_tick, 0)                \
	X(int64_t, source_order, 0)               \
	X(bool, section_tagged, false)

class EffectGroupReport : public RefCounted {
	GDCLASS(EffectGroupReport, RefCounted)

public:
	EFFECT_GROUP_REPORT_FIELDS(EFFECT_REPORT_ACCESSORS)
	Variant get_owner_key() const { return owner_key_; }
	void set_owner_key(const Variant &p_value) { owner_key_ = p_value; }
	TypedArray<EffectEmitterReport> get_emitters() const { return emitters_; }
	void set_emitters(const TypedArray<EffectEmitterReport> &p_value) { emitters_ = p_value; }
	void add_emitter(const Ref<EffectEmitterReport> &p_row) { emitters_.push_back(p_row); }

protected:
	static void _bind_methods();

private:
	EFFECT_GROUP_REPORT_FIELDS(EFFECT_REPORT_MEMBER)
	Variant owner_key_;
	TypedArray<EffectEmitterReport> emitters_;
};

} // namespace godot

#undef EFFECT_REPORT_ACCESSORS
#undef EFFECT_REPORT_MEMBER
