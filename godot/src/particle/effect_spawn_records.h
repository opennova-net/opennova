#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <godot_cpp/variant/variant.hpp>

#include <cstdint>

// The EffectScene.spawn seam as three typed records (ADR 0042 d5): the
// game-level options a caller hands EffectWorld.spawn_effect_request, the
// request EffectWorld builds from them, and the receipt the portable scene
// answers (particle::EffectSpawnReceipt). Read-write so stubs author them.

#define EFFECT_SPAWN_ACCESSORS(m_type, m_name, m_default)     \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define EFFECT_SPAWN_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

// admission / binding / render_domain / kill_plane are EffectScene's bound
// enums (Admission, Binding, RenderDomain, KillPlane); slot_token /
// owner_token are the interned identities the shell holds; lod_divisor is
// clamped to >= 1 by the scene.
#define EFFECT_SPAWN_REQUEST_FIELDS(X)                                                             \
	X(int64_t, effect_handle, 0)                                                                   \
	X(Transform3D, transform, Transform3D())                                                       \
	X(int, admission, 0)                                                                           \
	X(int, binding, 0)                                                                             \
	X(int, render_domain, 0)                                                                       \
	X(int64_t, slot_token, 0)                                                                      \
	X(int64_t, owner_token, 0)                                                                     \
	X(Transform3D, owner_relative_transform, Transform3D())                                        \
	X(int, force_zone, 0)                                                                          \
	X(int, initial_age_ticks, 0)                                                                   \
	X(int64_t, source_tick, 0)                                                                     \
	X(int64_t, source_order, 0)                                                                    \
	X(float, spring_const, 0.0f)                                                                   \
	X(int, lod_divisor, 1)                                                                         \
	X(int, kill_plane, 0)                                                                          \
	X(float, kill_plane_y, 0.0f)

class EffectSpawnRequest : public RefCounted {
	GDCLASS(EffectSpawnRequest, RefCounted)

public:
	EFFECT_SPAWN_REQUEST_FIELDS(EFFECT_SPAWN_ACCESSORS)
	// The minimal request: the interned effect at a pose, every other field
	// at its default (a world-bound, always-admitted transient).
	static Ref<EffectSpawnRequest> make(int64_t p_effect_handle, const Transform3D &p_transform);

protected:
	static void _bind_methods();

private:
	EFFECT_SPAWN_REQUEST_FIELDS(EFFECT_SPAWN_MEMBER)
};

// `status` is EffectScene::SpawnStatus (-1 invalid request); `spawned` =
// Spawned, `accepted` = Spawned or Suppressed; `message` names an invalid
// request's failing field.
#define EFFECT_SPAWN_RECEIPT_FIELDS(X)     \
	X(int, status, -1)                     \
	X(String, status_name, String())       \
	X(String, message, String())           \
	X(int64_t, effect_handle, 0)           \
	X(int64_t, group_id, 0)                \
	X(int64_t, replaced_group_id, 0)       \
	X(bool, spawned, false)                \
	X(bool, accepted, false)

class EffectSpawnReceipt : public RefCounted {
	GDCLASS(EffectSpawnReceipt, RefCounted)

public:
	EFFECT_SPAWN_RECEIPT_FIELDS(EFFECT_SPAWN_ACCESSORS)
	// A stub's receipt: spawned (status Spawned, accepted) or rejected
	// (status InvalidHandle) with the identities it reports.
	static Ref<EffectSpawnReceipt> make(bool p_spawned, int64_t p_effect_handle, int64_t p_group_id);

protected:
	static void _bind_methods();

private:
	EFFECT_SPAWN_RECEIPT_FIELDS(EFFECT_SPAWN_MEMBER)
};

// The game-level half of one effect spawn (the former effect_spawn_options.gd):
// the admission / binding / render-domain policy (EffectScene's enums), the
// slot and owner KEYS EffectWorld interns to native tokens (any Variant; null
// = no slot / no owner), the owner pose it seeds around the spawn (before
// the spawn, after it for ReplaceOwned so the predecessor detaches at its
// own last pose) when `has_owner_transform`, and the tick provenance.
// EffectWorld composes the native EffectSpawnRequest from this plus the
// interned effect handle; the request's remaining fields (tint, spring, LOD
// divisor, kill plane) keep their native defaults.
#define EFFECT_SPAWN_OPTIONS_FIELDS(X)                                                             \
	X(int, admission, 0)                                                                           \
	X(int, binding, 0)                                                                             \
	X(int, render_domain, 0)                                                                       \
	X(Transform3D, owner_transform, Transform3D())                                                 \
	X(bool, has_owner_transform, false)                                                            \
	X(Transform3D, owner_relative_transform, Transform3D())                                        \
	X(int, force_zone, 0)                                                                          \
	X(int, initial_age_ticks, 0)                                                                   \
	X(int64_t, source_tick, 0)                                                                     \
	X(int64_t, source_order, 0)

class EffectSpawnOptions : public RefCounted {
	GDCLASS(EffectSpawnOptions, RefCounted)

public:
	EFFECT_SPAWN_OPTIONS_FIELDS(EFFECT_SPAWN_ACCESSORS)
	Variant get_slot_key() const { return slot_key_; }
	void set_slot_key(const Variant &p_value) { slot_key_ = p_value; }
	Variant get_owner_key() const { return owner_key_; }
	void set_owner_key(const Variant &p_value) { owner_key_ = p_value; }

protected:
	static void _bind_methods();

private:
	EFFECT_SPAWN_OPTIONS_FIELDS(EFFECT_SPAWN_MEMBER)
	Variant slot_key_;
	Variant owner_key_;
};

} // namespace godot

#undef EFFECT_SPAWN_ACCESSORS
#undef EFFECT_SPAWN_MEMBER
