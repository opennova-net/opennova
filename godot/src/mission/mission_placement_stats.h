#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>

namespace godot {

// One MissionObjectPlacer.place / place_entities result: the placed /
// batched / animated / unresolved split, the marker and graphic counts, the
// static-population census (bins, batches, retained instances, LOD and live
// populations, shadow twins), the authored OOBJ occluder count and the three
// per-stage usec spans. `batched` and `animated` are the honest
// individual/batched split: a multi-RLOD graphic never leaves the batched
// count on its own. Read-write so a stub world authors one; to_json_value()
// is the perf-counter / MCP embed (spans nested as before).
#define MISSION_PLACEMENT_STATS_FIELDS(X)  \
	X(placed)                              \
	X(batched)                             \
	X(animated)                            \
	X(unresolved)                          \
	X(markers)                             \
	X(graphics)                            \
	X(batches)                             \
	X(static_bins)                         \
	X(static_binned_batches)               \
	X(static_global_batches)               \
	X(static_instances_retained)           \
	X(static_lod_populations)              \
	X(static_live_populations)             \
	X(static_shadow_batches)               \
	X(authored_occluder_models)

#define MISSION_PLACEMENT_SPANS(X) \
	X(bucket_entities)             \
	X(static_batches)              \
	X(animated_models)

class MissionPlacementStats : public RefCounted {
	GDCLASS(MissionPlacementStats, RefCounted)

public:
#define MISSION_PLACEMENT_STATS_ACCESSORS(m_name)          \
	int get_##m_name() const { return m_name##_; }        \
	void set_##m_name(int p_value) { m_name##_ = p_value; }
	MISSION_PLACEMENT_STATS_FIELDS(MISSION_PLACEMENT_STATS_ACCESSORS)
#undef MISSION_PLACEMENT_STATS_ACCESSORS
#define MISSION_PLACEMENT_SPAN_ACCESSORS(m_name)                                  \
	int64_t get_span_##m_name##_usec() const { return span_##m_name##_usec_; }   \
	void set_span_##m_name##_usec(int64_t p_value) { span_##m_name##_usec_ = p_value; }
	MISSION_PLACEMENT_SPANS(MISSION_PLACEMENT_SPAN_ACCESSORS)
#undef MISSION_PLACEMENT_SPAN_ACCESSORS

	Dictionary to_json_value() const;

protected:
	static void _bind_methods();

private:
#define MISSION_PLACEMENT_STATS_MEMBER(m_name) int m_name##_ = 0;
	MISSION_PLACEMENT_STATS_FIELDS(MISSION_PLACEMENT_STATS_MEMBER)
#undef MISSION_PLACEMENT_STATS_MEMBER
#define MISSION_PLACEMENT_SPAN_MEMBER(m_name) int64_t span_##m_name##_usec_ = 0;
	MISSION_PLACEMENT_SPANS(MISSION_PLACEMENT_SPAN_MEMBER)
#undef MISSION_PLACEMENT_SPAN_MEMBER
};

} // namespace godot
