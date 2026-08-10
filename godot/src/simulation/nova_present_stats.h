#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

// One int64 stat field with its property accessors.
#define NOVA_STAT_FIELD(name)                                                 \
	int64_t name = 0;                                                         \
	int64_t get_##name() const { return name; }                               \
	void set_##name(int64_t p_value) { name = p_value; }

// Typed diagnostic snapshot for the native mission-present row applier
// (ADR 0017: consumers receive this record, not the transport Dictionary).
class MissionPresentStats : public RefCounted {
	GDCLASS(MissionPresentStats, RefCounted)

public:
	NOVA_STAT_FIELD(moved)
	NOVA_STAT_FIELD(posed)
	NOVA_STAT_FIELD(hidden)
	NOVA_STAT_FIELD(muzzles)
	NOVA_STAT_FIELD(plan_rebuilds)
	NOVA_STAT_FIELD(transform_builds)
	NOVA_STAT_FIELD(aim_dispatches)
	NOVA_STAT_FIELD(rhc_dispatches)
	NOVA_STAT_FIELD(part_dispatches)
	NOVA_STAT_FIELD(control_dispatches)
	NOVA_STAT_FIELD(body_dispatches)
	NOVA_STAT_FIELD(muzzle_queries)

protected:
	static void _bind_methods();
};

// Typed diagnostic snapshot for the wire present pass.
class WirePresentStats : public RefCounted {
	GDCLASS(WirePresentStats, RefCounted)

public:
	NOVA_STAT_FIELD(live)
	NOVA_STAT_FIELD(spawned)
	NOVA_STAT_FIELD(unresolved)

	static Ref<WirePresentStats> create(int64_t p_live, int64_t p_spawned,
			int64_t p_unresolved);

protected:
	static void _bind_methods();
};

#undef NOVA_STAT_FIELD

} // namespace godot
