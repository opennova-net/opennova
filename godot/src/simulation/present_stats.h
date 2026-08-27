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
	NOVA_STAT_FIELD(plan_rebuilds)
	NOVA_STAT_FIELD(transform_builds)
	NOVA_STAT_FIELD(aim_dispatches)
	NOVA_STAT_FIELD(rhc_dispatches)
	NOVA_STAT_FIELD(part_dispatches)
	NOVA_STAT_FIELD(control_dispatches)
	NOVA_STAT_FIELD(body_dispatches)

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
	NOVA_STAT_FIELD(pending) // cold rows the spawn budget deferred last frame

	static Ref<WirePresentStats> create(int64_t p_live, int64_t p_spawned,
			int64_t p_unresolved, int64_t p_pending = 0);

protected:
	static void _bind_methods();
};

// Typed diagnostic snapshot for the ScarPresenter device: world_surfaces,
// entity_meshes, batches, vertices, textures_missing (strips drawn this
// present whose TGA has not resolved — re-tried every present),
// strips_unsupported (strips drawn this present whose mode word decodes to
// neither shipped drawer state; they draw in the scorch state).
class ScarPresenterStats : public RefCounted {
	GDCLASS(ScarPresenterStats, RefCounted)

public:
	NOVA_STAT_FIELD(world_surfaces)
	NOVA_STAT_FIELD(entity_meshes)
	NOVA_STAT_FIELD(batches)
	NOVA_STAT_FIELD(vertices)
	NOVA_STAT_FIELD(textures_missing)
	NOVA_STAT_FIELD(strips_unsupported)

protected:
	static void _bind_methods();
};

#undef NOVA_STAT_FIELD

} // namespace godot
