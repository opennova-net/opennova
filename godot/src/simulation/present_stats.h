#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

// One int64 stat field with its read-only property accessor (C++ writers
// assign the public field directly).
#define STAT_FIELD(name)                                                 \
	int64_t name = 0;                                                         \
	int64_t get_##name() const { return name; }

// Typed diagnostic snapshot for the native mission-present row applier
// (ADR 0017: consumers receive this record, not the transport Dictionary).
class MissionPresentStats : public RefCounted {
	GDCLASS(MissionPresentStats, RefCounted)

public:
	STAT_FIELD(moved)
	STAT_FIELD(posed)
	STAT_FIELD(hidden)
	STAT_FIELD(plan_rebuilds)
	STAT_FIELD(transform_builds)
	STAT_FIELD(aim_dispatches)
	STAT_FIELD(rhc_dispatches)
	STAT_FIELD(part_dispatches)
	STAT_FIELD(control_dispatches)
	STAT_FIELD(body_dispatches)

protected:
	static void _bind_methods();
};

// Typed diagnostic snapshot for the wire present pass.
class WirePresentStats : public RefCounted {
	GDCLASS(WirePresentStats, RefCounted)

public:
	STAT_FIELD(live)
	STAT_FIELD(spawned)
	STAT_FIELD(unresolved)
	STAT_FIELD(pending) // cold rows the spawn budget deferred last frame

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
	STAT_FIELD(world_surfaces)
	STAT_FIELD(entity_meshes)
	STAT_FIELD(batches)
	STAT_FIELD(vertices)
	STAT_FIELD(textures_missing)
	STAT_FIELD(strips_unsupported)

protected:
	static void _bind_methods();
};

#undef STAT_FIELD

} // namespace godot
