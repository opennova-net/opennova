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

// Typed diagnostic counters of the fire present pass (FirePresenter, ADR
// 0017): fires presented (the local player's own excluded), sounds the bank
// played, muzzle effects spawned, and the peak live tracer channel count —
// probes assert the presentation legs actually ran.
class FirePresentStats : public RefCounted {
	GDCLASS(FirePresentStats, RefCounted)

public:
	STAT_FIELD(fires)
	STAT_FIELD(sounds)
	STAT_FIELD(effects)
	STAT_FIELD(tracer_peak)

protected:
	static void _bind_methods();
};

// Typed diagnostic counters of the destruction present pass
// (DestructionPresenter): husk swaps presented, swaps with no husk graft
// (the intact graphic keeps standing), the peak live piece count, the
// resolved debris triangles and glass points, effects and sounds played,
// and the wreck-fire crackle rolls that fired.
class DestructionPresentStats : public RefCounted {
	GDCLASS(DestructionPresentStats, RefCounted)

public:
	STAT_FIELD(husk_swaps)
	STAT_FIELD(no_husk)
	STAT_FIELD(pieces_peak)
	STAT_FIELD(debris_triangles)
	STAT_FIELD(effects)
	STAT_FIELD(sounds)
	STAT_FIELD(glass_points)
	STAT_FIELD(crackles)

protected:
	static void _bind_methods();
};

// Typed diagnostic snapshot of the throwable present pass
// (ThrowablePresenter): the live item models, the live round-bound move
// groups, and the transforms their anchors resolve.
class ThrowablePresentStats : public RefCounted {
	GDCLASS(ThrowablePresentStats, RefCounted)

public:
	STAT_FIELD(live)
	STAT_FIELD(move_effects)
	STAT_FIELD(move_effect_transforms)

protected:
	static void _bind_methods();
};

// Typed diagnostic counters of the scar present pass (ScarPresenter's
// present_frame leg): the sim's slot census (live, fog-culled, rings
// leased), the device's batch/surface/mesh counts, the strips whose TGA is
// missing or whose mode word is neither shipped drawer state, and the
// entity-ring owners the pass could not resolve to a live node.
class ScarPresentStats : public RefCounted {
	GDCLASS(ScarPresentStats, RefCounted)

public:
	STAT_FIELD(slots_live)
	STAT_FIELD(slots_culled)
	STAT_FIELD(rings_leased)
	STAT_FIELD(batches)
	STAT_FIELD(world_surfaces)
	STAT_FIELD(entity_meshes)
	STAT_FIELD(textures_missing)
	STAT_FIELD(strips_unsupported)
	STAT_FIELD(owners_unresolved)

protected:
	static void _bind_methods();
};

#undef STAT_FIELD

} // namespace godot
