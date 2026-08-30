#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <runtime/renderer/q3_frame.h>

#include "render/q3_geometry_cache.h"

namespace godot {

class GeometryInstance3D;
class Node;
class Viewport;

// One surface of a registered Q3 source with the material block the focused
// compile hands the compiler. An object surface caches the block and re-reads
// it only when its material's parameter version moved
// (Q3SourceRegistry::invalidate_object_material, called by ObjectModel's
// runtime parameter writes) or the source's surfaces were refreshed; water
// and celestial surfaces belong to per-frame producers and are re-read at
// every in-frustum sight.
struct Q3SurfaceRecord {
	int surface = 0;
	Ref<ShaderMaterial> material;
	std::uint64_t material_id = 0;
	// The material parameter version the cached block was read at; 0 = never.
	std::uint64_t parameter_version = 0;
	opennova::renderer::ObjectMaterialClassification classification{};
	Q3PackParameters pack;
	opennova::renderer::Q3ObjectMaterialParameters object{};
	opennova::renderer::Q3WaterMaterialParameters water{};
	opennova::renderer::Q3CelestialMaterialParameters celestial{};
	Ref<Texture2D> primary_texture_resource;
	Ref<Texture2D> secondary_texture_resource;
	Ref<Texture2D> tertiary_texture_resource;
	RID primary_texture;
	RID secondary_texture;
	RID tertiary_texture;
};

// One live (non-carved) MultiMesh row in world space.
struct Q3InstanceRow {
	Transform3D world_transform;
	AABB world_bounds;
};

// The latest producer-published CPU arrays for one source surface.
struct Q3PublishedGeometry {
	Q3SurfaceArrays arrays;
	std::uint64_t generation = 0;
};

// The persistent record of one registered Q3 source. Producers and the
// node's tree/visibility signals mark what changed; the compile refreshes
// only the marked state, probes the transform and layer mask once per
// visible record (Godot exposes no transform-changed signal for an
// engine-class node to an extension), and frustum-tests the cached bounds.
struct Q3SourceRecord {
	std::uint64_t node_id = 0;
	// Dereferenced only while `in_tree`: a node inside the tree leaves it
	// (tree_exited) before it can be freed, and a record outside the tree is
	// checked against ObjectDB before any use.
	GeometryInstance3D *node = nullptr;
	opennova::renderer::Q3Source source = opennova::renderer::Q3Source::Object;
	std::uint64_t material_id = 0;
	// Celestial sources: bit i set = surface i was installed with the
	// additive celestial material, so its Q3 disc draw adds.
	std::uint32_t additive_surfaces = 0;
	// Bumped by invalidate_source (a rebuilt mesh): the geometry cache
	// re-reads and re-packs the source's surfaces once when it moves.
	std::uint64_t geometry_generation = 1;
	// Bumped by invalidate_instances (rewritten MultiMesh rows): the rows
	// and the population bounds are re-read; the packed surfaces stay.
	std::uint64_t instance_generation = 1;
	// Cleared by unregister_source or a re-registration whose material
	// carries no glow (an ObjectModel surface slot swapped onto such a
	// level): the compile skips the record but it keeps its generations, so
	// a later re-registration of the same node never hands the geometry
	// cache a generation it has already seen.
	bool active = true;
	bool signals_connected = false;

	// Scene state.
	bool in_tree = false;
	std::size_t dormant_index = static_cast<std::size_t>(-1);
	Viewport *viewport = nullptr;
	// Scope membership, cached per scope node (the parent walk runs once).
	std::uint64_t scope_id = 0;
	bool in_scope = false;
	bool visible = false;
	bool visibility_dirty = true;
	bool transform_valid = false;
	Transform3D global_transform;
	bool bounds_dirty = true;
	AABB world_bounds;

	// Surfaces.
	bool surfaces_dirty = true;
	bool is_multimesh = false;
	Ref<ArrayMesh> mesh;
	Ref<MultiMesh> multimesh;
	std::vector<Q3SurfaceRecord> surfaces;

	// MultiMesh rows: the live local rows, read at the population's first
	// in-frustum sight per instance generation, and their world form.
	std::uint64_t rows_generation = 0;
	bool rows_world_dirty = true;
	std::vector<Transform3D> local_rows;
	std::vector<Q3InstanceRow> rows;

	std::map<int, Q3PublishedGeometry> published;
};

// The typed Q3 producer registry: persistent per-source records (above),
// the object material classification table with its parameter versions, and
// the compile-side refresh of a record's cached state. Every method runs on
// the main thread; the compile holds `mutex()` across begin_frame and its
// walk so a producer call and the walk never interleave.
class Q3SourceRegistry {
public:
	struct FrameCounters {
		// Records whose cached scene or surface state was refreshed.
		std::size_t records_touched = 0;
		// MultiMesh sources whose rows were (re-)read this frame.
		std::size_t instance_row_reads = 0;
		// Surfaces whose material block was read through the material.
		std::size_t material_reads = 0;
	};

	// Producer API.
	static void register_object_material(const Ref<Material> &p_material,
			const opennova::renderer::ObjectMaterialClassification &p_classification);
	static void clone_object_material(const Ref<Material> &p_source,
			const Ref<Material> &p_clone);
	static bool object_material_classification(const Ref<Material> &p_material,
			opennova::renderer::ObjectMaterialClassification &r_classification);
	// A producer rewrote one of the material's Q3 parameters (u_diffuse,
	// u_detail, u_rgb_mod, u_alpha_mod, u_reflect_color, the UV rows): every
	// object surface on it re-reads its block at its next sight.
	static void invalidate_object_material(const Ref<Material> &p_material);
	// Register (or re-register) an object surface instance with the material
	// it currently draws: a glow-capable material makes it a compiled source
	// (a node already registered keeps its record and bumps its geometry
	// generation, so the swapped mesh is re-packed once); any other material
	// parks an existing record inactive. unregister_source parks a record
	// the same way without touching its generations.
	static void register_object_source(GeometryInstance3D *p_source,
			const Ref<Material> &p_material);
	static void unregister_source(GeometryInstance3D *p_source);
	static void register_source(GeometryInstance3D *p_source,
			opennova::renderer::Q3Source p_kind, std::uint32_t p_additive_surfaces);
	static void publish_geometry(GeometryInstance3D *p_source, int p_surface,
			const Array &p_arrays);
	static void invalidate_source(GeometryInstance3D *p_source);
	static void invalidate_instances(GeometryInstance3D *p_source);

	// Extension teardown: drops every record and material row while the
	// servers their materials, textures and meshes belong to still exist.
	static void cleanup_statics();

	// Compile side; hold mutex() from begin_frame through the walk.
	static std::recursive_mutex &mutex();
	// Reaps the records whose node died outside the tree (their ids are
	// returned for cache eviction) and sweeps the material table every
	// kMaterialSweepPeriod frames.
	static void begin_frame(std::vector<std::uint64_t> &r_dead_sources);
	// The in-tree records, ordered by node id (creation order).
	static const std::vector<Q3SourceRecord *> &live_records();
	static std::size_t record_count();
	// Re-reads the record's mesh, surface materials and classifications.
	static void refresh_surfaces(Q3SourceRecord &r_record, FrameCounters &r_counters);
	// Reads the live rows of a MultiMesh record once per instance
	// generation and rebuilds their world form after a move.
	static void refresh_rows(Q3SourceRecord &r_record, FrameCounters &r_counters);
	// Reads the surface's material block when the material's parameter
	// version moved (object) or unconditionally (water, celestial).
	static void refresh_surface_parameters(const Q3SourceRecord &p_record,
			Q3SurfaceRecord &r_surface, FrameCounters &r_counters);

	// The source node's signal targets (bound to its id at registration):
	// tree_entered/tree_exited move the record between the live and dormant
	// sets, visibility_changed marks its visibility for the next compile.
	static void on_tree_entered(std::uint64_t p_node_id);
	static void on_tree_exited(std::uint64_t p_node_id);
	static void on_visibility_changed(std::uint64_t p_node_id);
};

} // namespace godot
