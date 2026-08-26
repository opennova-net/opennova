#include "nova_present_stats.h"

namespace godot {

#define NOVA_STAT_BIND(klass, field)                                          \
	ClassDB::bind_method(D_METHOD("get_" #field), &klass::get_##field);       \
	ClassDB::bind_method(D_METHOD("set_" #field, "value"),                    \
			&klass::set_##field);                                             \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #field), "set_" #field,           \
			"get_" #field);

void MissionPresentStats::_bind_methods() {
	NOVA_STAT_BIND(MissionPresentStats, moved)
	NOVA_STAT_BIND(MissionPresentStats, posed)
	NOVA_STAT_BIND(MissionPresentStats, hidden)
	NOVA_STAT_BIND(MissionPresentStats, muzzles)
	NOVA_STAT_BIND(MissionPresentStats, plan_rebuilds)
	NOVA_STAT_BIND(MissionPresentStats, transform_builds)
	NOVA_STAT_BIND(MissionPresentStats, aim_dispatches)
	NOVA_STAT_BIND(MissionPresentStats, rhc_dispatches)
	NOVA_STAT_BIND(MissionPresentStats, part_dispatches)
	NOVA_STAT_BIND(MissionPresentStats, control_dispatches)
	NOVA_STAT_BIND(MissionPresentStats, body_dispatches)
	NOVA_STAT_BIND(MissionPresentStats, muzzle_queries)
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
	NOVA_STAT_BIND(WirePresentStats, live)
	NOVA_STAT_BIND(WirePresentStats, spawned)
	NOVA_STAT_BIND(WirePresentStats, unresolved)
	NOVA_STAT_BIND(WirePresentStats, pending)
}

void ScarPresenterStats::_bind_methods() {
	NOVA_STAT_BIND(ScarPresenterStats, world_surfaces)
	NOVA_STAT_BIND(ScarPresenterStats, entity_meshes)
	NOVA_STAT_BIND(ScarPresenterStats, batches)
	NOVA_STAT_BIND(ScarPresenterStats, vertices)
	NOVA_STAT_BIND(ScarPresenterStats, textures_missing)
	NOVA_STAT_BIND(ScarPresenterStats, strips_unsupported)
}

#undef NOVA_STAT_BIND

} // namespace godot
