#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

namespace godot {

// The counters FoliageDispatcher accumulates while applying one compiled
// foliage frame. The same X-macro lays out the dispatcher's private counter
// struct, so the record can never drift from what the dispatcher counts.
#define FOLIAGE_FRAME_COUNTERS(X)               \
	X(int64_t, frame_calls)                     \
	X(int64_t, detail_cells)                    \
	X(int64_t, silhouette_anchors_input)        \
	X(int64_t, silhouette_anchors_visible)      \
	X(int64_t, runtime_detail_intents)          \
	X(int64_t, runtime_silhouette_intents)      \
	X(int64_t, detail_high_instances)           \
	X(int64_t, detail_low_instances)            \
	X(int64_t, silhouette_instances)            \
	X(int64_t, detail_vertices)                 \
	X(int64_t, silhouette_vertices)             \
	X(int64_t, render_batches)                  \
	X(int64_t, detail_cache_hits)               \
	X(int64_t, detail_cache_misses)             \
	X(int64_t, detail_cache_regenerations)      \
	X(int64_t, detail_cache_evictions)          \
	X(int64_t, detail_cache_residents)          \
	X(int64_t, detail_cache_submissions)        \
	X(int64_t, model_cache_hits)                \
	X(int64_t, model_cache_misses)              \
	X(int64_t, model_cache_regenerations)       \
	X(int64_t, model_cache_evictions)           \
	X(int64_t, model_cache_residents)           \
	X(int64_t, model_cache_submissions)         \
	X(int64_t, detail_mesh_hits)                \
	X(int64_t, detail_mesh_uploads)             \
	X(int64_t, backend_instance_creates)        \
	X(int64_t, backend_scenario_writes)         \
	X(int64_t, backend_configuration_writes)    \
	X(int64_t, backend_base_writes)             \
	X(int64_t, backend_material_writes)         \
	X(int64_t, backend_material_parameter_writes) \
	X(int64_t, backend_uniform_writes)          \
	X(int64_t, backend_visibility_writes)       \
	X(int64_t, terrain_scene_counter)           \
	X(bool, native_detail_source)               \
	X(bool, preview_detail_source)              \
	X(bool, path_blocker_available)

// One FoliageDispatcher.get_frame_stats snapshot: the frame counters plus the
// retained draw-pool census, the backend name, the summed RenderingServer
// writes and the authored/enabled/disabled foliage slot counts. Read-write so
// a stub authors one; to_json_value() is the perf-counter JSON embed.
#define FOLIAGE_FRAME_STATS_FIELDS(X)  \
	FOLIAGE_FRAME_COUNTERS(X)          \
	X(String, foliage_backend)         \
	X(int64_t, backend_pool_size)      \
	X(int64_t, backend_active_draws)   \
	X(int64_t, backend_visible_draws)  \
	X(int64_t, backend_server_writes)  \
	X(int64_t, authored_slots)         \
	X(int64_t, enabled_slots)          \
	X(int64_t, disabled_slots)

class FoliageFrameStats : public RefCounted {
	GDCLASS(FoliageFrameStats, RefCounted)

public:
#define FOLIAGE_FRAME_STATS_ACCESSORS(m_type, m_name)      \
	m_type get_##m_name() const { return m_name##_; }     \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
	FOLIAGE_FRAME_STATS_FIELDS(FOLIAGE_FRAME_STATS_ACCESSORS)
#undef FOLIAGE_FRAME_STATS_ACCESSORS

	Dictionary to_json_value() const;

protected:
	static void _bind_methods();

private:
#define FOLIAGE_FRAME_STATS_MEMBER(m_type, m_name) m_type m_name##_{};
	FOLIAGE_FRAME_STATS_FIELDS(FOLIAGE_FRAME_STATS_MEMBER)
#undef FOLIAGE_FRAME_STATS_MEMBER
};

} // namespace godot
