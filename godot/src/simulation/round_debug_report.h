#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/world/entity.h>

#include <cstdint>

// The round-simulation debug trail for the F3 rounds view
// (Simulation::get_round_debug): one event per resolved projectile step,
// oldest first, positions in Godot space. Read-write so the view test authors
// events. The engine ring is world::RoundSim::debug_trail.

#define ROUND_DEBUG_ACCESSORS(m_type, m_name, m_default)      \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define ROUND_DEBUG_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

// `kind`: 0 organic, 1 item face, 2 item sphere, 3 terrain, 4 expired,
// 5 face miss (kind_name carries the label). `section` is the primary posed
// section (reactions/death), `secondary_section` the damage-zone section;
// `fallback` marks the neutral stand-in sphere; `t` the graze parameter.
#define ROUND_DEBUG_EVENT_FIELDS(X)                                        \
	X(int64_t, tick, 0)                                                    \
	X(int, kind, 4)                                                        \
	X(String, kind_name, String())                                         \
	X(int, material, 0)                                                    \
	X(int, section, -1)                                                    \
	X(int, secondary_section, -1)                                          \
	X(bool, fallback, false)                                               \
	X(int, face, -1)                                                       \
	X(int, effect_tag, -1)                                                 \
	X(String, effect_tag_name, String())                                   \
	X(int, entity_handle, opennova::world::EntityHandle::kInvalid)         \
	X(int, shooter_handle, opennova::world::EntityHandle::kInvalid)        \
	X(int, ammo_index, -1)                                                 \
	X(bool, husk, false)                                                   \
	X(float, t, 0.0f)                                                      \
	X(Vector3, p0, Vector3())                                              \
	X(Vector3, p1, Vector3())                                              \
	X(Vector3, hit, Vector3())                                             \
	X(String, entity_name, String())

class RoundDebugEvent : public RefCounted {
	GDCLASS(RoundDebugEvent, RefCounted)

public:
	ROUND_DEBUG_EVENT_FIELDS(ROUND_DEBUG_ACCESSORS)

protected:
	static void _bind_methods();

private:
	ROUND_DEBUG_EVENT_FIELDS(ROUND_DEBUG_MEMBER)
};

class RoundDebugReport : public RefCounted {
	GDCLASS(RoundDebugReport, RefCounted)

public:
	int64_t get_tick() const { return tick_; }
	void set_tick(int64_t p_value) { tick_ = p_value; }
	TypedArray<RoundDebugEvent> get_events() const { return events_; }
	void set_events(const TypedArray<RoundDebugEvent> &p_value) { events_ = p_value; }
	void add_event(const Ref<RoundDebugEvent> &p_event) { events_.push_back(p_event); }

protected:
	static void _bind_methods();

private:
	int64_t tick_ = 0;
	TypedArray<RoundDebugEvent> events_;
};

} // namespace godot

#undef ROUND_DEBUG_ACCESSORS
#undef ROUND_DEBUG_MEMBER
