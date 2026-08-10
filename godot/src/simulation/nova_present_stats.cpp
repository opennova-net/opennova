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
		int64_t p_spawned, int64_t p_unresolved) {
	Ref<WirePresentStats> stats;
	stats.instantiate();
	stats->live = p_live;
	stats->spawned = p_spawned;
	stats->unresolved = p_unresolved;
	return stats;
}

void WirePresentStats::_bind_methods() {
	ClassDB::bind_static_method("WirePresentStats",
			D_METHOD("create", "live", "spawned", "unresolved"),
			&WirePresentStats::create);
	NOVA_STAT_BIND(WirePresentStats, live)
	NOVA_STAT_BIND(WirePresentStats, spawned)
	NOVA_STAT_BIND(WirePresentStats, unresolved)
}

#undef NOVA_STAT_BIND

} // namespace godot
