#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

// Three Simulation diagnostic cards the probes and tests read (ADR 0042 d5):
// the WAC VM state, the native pose-path health counters and the per-entity
// destruction gate inputs. Read-write so a stub sim authors one.

#define DEBUG_CARD_ACCESSORS(m_type, m_name, m_default)       \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define DEBUG_CARD_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

// The installed WAC program's VM state (Simulation::get_wac_state).
#define WAC_STATE_FIELDS(X)     \
	X(bool, loaded, false)      \
	X(bool, paused, false)      \
	X(int64_t, runs, 0)         \
	X(int, event_count, 0)      \
	X(int, code_size, 0)

class WacState : public RefCounted {
	GDCLASS(WacState, RefCounted)

public:
	WAC_STATE_FIELDS(DEBUG_CARD_ACCESSORS)

protected:
	static void _bind_methods();

private:
	WAC_STATE_FIELDS(DEBUG_CARD_MEMBER)
};

// Native pose-path health (Simulation::debug_native_pose_stats): cumulative
// queries/declines for the collision provider and the mounted resolver, plus
// the installed mounted model sources. The soak gates on declines == 0.
#define NATIVE_POSE_STATS_FIELDS(X)                \
	X(int64_t, collision_queries, 0)               \
	X(int64_t, collision_declines, 0)              \
	X(int64_t, muzzle_queries, 0)                  \
	X(int64_t, muzzle_resolves, 0)                 \
	X(int64_t, mounted_queries, 0)                 \
	X(int64_t, mounted_declines, 0)                \
	X(int64_t, mounted_evaluations, 0)             \
	X(int64_t, mounted_cache_hits, 0)              \
	X(int64_t, mounted_rest_cache_entries, 0)      \
	X(int64_t, mounted_graphic_sources, 0)

class NativePoseStats : public RefCounted {
	GDCLASS(NativePoseStats, RefCounted)

public:
	NATIVE_POSE_STATS_FIELDS(DEBUG_CARD_ACCESSORS)

protected:
	static void _bind_methods();

private:
	NATIVE_POSE_STATS_FIELDS(DEBUG_CARD_MEMBER)
};

// Per-entity destruction diagnostics by placed bms_id
// (Simulation::get_destruction_debug): the entity identity and health, the
// items.def death traits when authored (armor, KZ / bridge-DEAD anchors, the
// glass userpoints) — the §24 damage-chain gate inputs. `found` false = no
// entity carries that bms_id; `has_death_traits` false leaves the trait
// fields at their defaults. Points are model-local.
#define DESTRUCTION_DEBUG_CARD_FIELDS(X)                              \
	X(bool, found, false)                                             \
	X(int, bms_id, 0)                                                 \
	X(int, net_id, 0)                                                 \
	X(int, kind, 0)                                                   \
	X(int, pool, 0)                                                   \
	X(int, item_id, 0)                                                \
	X(int, health, 0)                                                 \
	X(int, health_max, 0)                                             \
	X(bool, alive, false)                                             \
	X(float, bound_radius, 0.0f)                                      \
	X(int64_t, engine_flags, 0)                                       \
	X(bool, is_ai_capable, false)                                     \
	X(bool, has_collision_instance, false)                            \
	X(bool, has_death_traits, false)                                  \
	X(int, armor_impact, 0)                                           \
	X(int, armor_blast, 0)                                            \
	X(int, unit_type, 0)                                              \
	X(int, kz, 0)                                                     \
	X(bool, has_husk, false)                                          \
	X(bool, husk_model_loaded, false)                                 \
	X(int, kz_point_count, 0)                                         \
	X(PackedVector3Array, kz_points, PackedVector3Array())            \
	X(int, bridge_dead_point_count, 0)                                \
	X(PackedVector3Array, bridge_dead_points, PackedVector3Array())   \
	X(int, glass_point_count, 0)                                      \
	X(PackedVector3Array, glass_point_positions, PackedVector3Array()) \
	X(PackedVector3Array, glass_point_directions, PackedVector3Array()) \
	X(Vector3, pos, Vector3())

class DestructionDebugCard : public RefCounted {
	GDCLASS(DestructionDebugCard, RefCounted)

public:
	DESTRUCTION_DEBUG_CARD_FIELDS(DEBUG_CARD_ACCESSORS)

protected:
	static void _bind_methods();

private:
	DESTRUCTION_DEBUG_CARD_FIELDS(DEBUG_CARD_MEMBER)
};

} // namespace godot

#undef DEBUG_CARD_ACCESSORS
#undef DEBUG_CARD_MEMBER
