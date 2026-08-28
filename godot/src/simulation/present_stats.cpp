#include "simulation/present_stats.h"

namespace godot {

#define STAT_BIND(klass, field)                                          \
	ClassDB::bind_method(D_METHOD("get_" #field), &klass::get_##field);       \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #field), "", "get_" #field);

void MissionPresentStats::_bind_methods() {
	STAT_BIND(MissionPresentStats, moved)
	STAT_BIND(MissionPresentStats, posed)
	STAT_BIND(MissionPresentStats, hidden)
	STAT_BIND(MissionPresentStats, plan_rebuilds)
	STAT_BIND(MissionPresentStats, transform_builds)
	STAT_BIND(MissionPresentStats, aim_dispatches)
	STAT_BIND(MissionPresentStats, rhc_dispatches)
	STAT_BIND(MissionPresentStats, part_dispatches)
	STAT_BIND(MissionPresentStats, control_dispatches)
	STAT_BIND(MissionPresentStats, body_dispatches)
}

Ref<WirePresentStats> WirePresentStats::create(int64_t p_live,
		int64_t p_spawned, int64_t p_unresolved, int64_t p_pending) {
	Ref<WirePresentStats> stats;
	stats.instantiate();
	stats->live = p_live;
	stats->spawned = p_spawned;
	stats->unresolved = p_unresolved;
	stats->pending = p_pending;
	return stats;
}

void WirePresentStats::_bind_methods() {
	ClassDB::bind_static_method("WirePresentStats",
			D_METHOD("create", "live", "spawned", "unresolved", "pending"),
			&WirePresentStats::create, DEFVAL(0));
	STAT_BIND(WirePresentStats, live)
	STAT_BIND(WirePresentStats, spawned)
	STAT_BIND(WirePresentStats, unresolved)
	STAT_BIND(WirePresentStats, pending)
}

void ScarPresenterStats::_bind_methods() {
	STAT_BIND(ScarPresenterStats, world_surfaces)
	STAT_BIND(ScarPresenterStats, entity_meshes)
	STAT_BIND(ScarPresenterStats, batches)
	STAT_BIND(ScarPresenterStats, vertices)
	STAT_BIND(ScarPresenterStats, textures_missing)
	STAT_BIND(ScarPresenterStats, strips_unsupported)
}

#undef STAT_BIND

} // namespace godot
