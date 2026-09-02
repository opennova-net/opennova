#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <runtime/world/entity.h>

// The AI overlay's per-frame payload (Simulation::get_ai_debug): one row per
// brain with its state, alert, target, aim direction, muzzle, ranges and
// timers; the nav channels as node runs; the group rows; the system
// counters. Godot space throughout — aim directions are unit vectors computed
// natively, the view does no BAM math. Read-write so the view test authors a
// payload. The engine join is inspect::AiDebugReport (runtime/world/inspect.h).

#define AI_DEBUG_ACCESSORS(m_type, m_name, m_default)         \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define AI_DEBUG_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

#define AI_DEBUG_ROW_FIELDS(X)                                          \
	X(int, ai_index, -1)                                                \
	X(int, handle, opennova::world::EntityHandle::kInvalid)             \
	X(String, name, String())                                           \
	X(int, group, 0)                                                    \
	X(bool, alive, false)                                               \
	X(bool, infantry, false)                                            \
	X(Vector3, pos, Vector3())                                          \
	X(int, state, 0)                                                    \
	X(String, state_name, String())                                     \
	X(int, alert, 0)                                                    \
	X(int, move_mode, 0)                                                \
	X(int, out_speed, 0)                                                \
	X(int, wp_channel, 0)                                               \
	X(int, wp_node, 0)                                                  \
	X(bool, target_valid, false)                                        \
	X(int, target_handle, opennova::world::EntityHandle::kInvalid)      \
	X(Vector3, target_pos, Vector3())                                   \
	X(String, target_name, String())                                    \
	X(bool, aim_valid, false)                                           \
	X(Vector3, aim_dir, Vector3())                                      \
	X(bool, muzzle_valid, false)                                        \
	X(Vector3, muzzle, Vector3())                                       \
	X(float, sight_range, 0.0f)                                         \
	X(float, attack_range, 0.0f)                                        \
	X(int, combat_timer, 0)                                             \
	X(int, fire_delay, 0)                                               \
	X(int, damage_timer, 0)                                             \
	X(int, combat_move_timer, 0)

class AiDebugRow : public RefCounted {
	GDCLASS(AiDebugRow, RefCounted)

public:
	AI_DEBUG_ROW_FIELDS(AI_DEBUG_ACCESSORS)

protected:
	static void _bind_methods();

private:
	AI_DEBUG_ROW_FIELDS(AI_DEBUG_MEMBER)
};

// One nav channel: the node run with its radii; `once` = loopflag bit 0
// (terminate at the path end); `followers` = brains walking it.
#define AI_DEBUG_CHANNEL_FIELDS(X)                        \
	X(int, index, 0)                                      \
	X(bool, once, false)                                  \
	X(int, followers, 0)                                  \
	X(PackedVector3Array, nodes, PackedVector3Array())    \
	X(PackedFloat32Array, radii, PackedFloat32Array())

class AiDebugChannel : public RefCounted {
	GDCLASS(AiDebugChannel, RefCounted)

public:
	AI_DEBUG_CHANNEL_FIELDS(AI_DEBUG_ACCESSORS)

protected:
	static void _bind_methods();

private:
	AI_DEBUG_CHANNEL_FIELDS(AI_DEBUG_MEMBER)
};

#define AI_DEBUG_GROUP_FIELDS(X) \
	X(int, id, 0)                \
	X(int, alert, 0)             \
	X(int, initial_count, 0)     \
	X(int, live_count, 0)

class AiDebugGroup : public RefCounted {
	GDCLASS(AiDebugGroup, RefCounted)

public:
	AI_DEBUG_GROUP_FIELDS(AI_DEBUG_ACCESSORS)
	static Ref<AiDebugGroup> make(int p_id, int p_alert, int p_initial_count, int p_live_count);

protected:
	static void _bind_methods();

private:
	AI_DEBUG_GROUP_FIELDS(AI_DEBUG_MEMBER)
};

// `valid` false without a kernel and on a joiner (the tooling AI pool never
// joins the decoded view); an unloaded kernel reports valid with no rows.
#define AI_DEBUG_REPORT_FIELDS(X)      \
	X(bool, valid, false)              \
	X(int64_t, logic_tick, 0)          \
	X(int, brain_count, 0)             \
	X(int, scheduler_budget, 0)        \
	X(int, event_count, 0)             \
	X(int, unported_calls, 0)          \
	X(int, rel_ops, 0)                 \
	X(int, find_target_calls, 0)

class AiDebugReport : public RefCounted {
	GDCLASS(AiDebugReport, RefCounted)

public:
	AI_DEBUG_REPORT_FIELDS(AI_DEBUG_ACCESSORS)
	TypedArray<AiDebugRow> get_rows() const { return rows_; }
	void set_rows(const TypedArray<AiDebugRow> &p_value) { rows_ = p_value; }
	TypedArray<AiDebugChannel> get_channels() const { return channels_; }
	void set_channels(const TypedArray<AiDebugChannel> &p_value) { channels_ = p_value; }
	TypedArray<AiDebugGroup> get_groups() const { return groups_; }
	void set_groups(const TypedArray<AiDebugGroup> &p_value) { groups_ = p_value; }
	void add_row(const Ref<AiDebugRow> &p_row) { rows_.push_back(p_row); }
	void add_channel(const Ref<AiDebugChannel> &p_row) { channels_.push_back(p_row); }
	void add_group(const Ref<AiDebugGroup> &p_row) { groups_.push_back(p_row); }

protected:
	static void _bind_methods();

private:
	AI_DEBUG_REPORT_FIELDS(AI_DEBUG_MEMBER)
	TypedArray<AiDebugRow> rows_;
	TypedArray<AiDebugChannel> channels_;
	TypedArray<AiDebugGroup> groups_;
};

} // namespace godot

#undef AI_DEBUG_ACCESSORS
#undef AI_DEBUG_MEMBER
