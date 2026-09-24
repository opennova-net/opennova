#include "render/q3_source_registry.h"
#include "render/material_params.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/string_name.hpp>

using namespace godot;
using namespace opennova::renderer;

namespace {

constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);
// Material ids are never reused, so a stale table row can only leak; the
// sweep that drops the rows of freed materials runs this often.
constexpr std::uint64_t kMaterialSweepPeriod = 256;

struct RegisteredMaterial {
	ObjectMaterialClassification classification{};
	// Moves on every re-registration and invalidate_object_material.
	std::uint64_t version = 1;
};

std::recursive_mutex g_mutex;
std::unordered_map<std::uint64_t, RegisteredMaterial> g_materials;
std::unordered_map<std::uint64_t, std::unique_ptr<Q3SourceRecord>> g_records;
// In-tree records ordered by node id, and the records outside the tree
// (registered before their add_child, or removed and not yet freed).
std::vector<Q3SourceRecord *> g_live;
std::vector<Q3SourceRecord *> g_dormant;
std::uint64_t g_frames = 0;

std::uint64_t object_id(const Ref<RefCounted> &p_object) {
	return p_object.is_valid() ? p_object->get_instance_id() : 0;
}

Q3ResourceLease lease_for(const Ref<RefCounted> &p_object) {
	return {object_id(p_object), 1};
}

Q3SourceRecord *find_record(std::uint64_t p_node_id) {
	const auto found = g_records.find(p_node_id);
	return found != g_records.end() ? found->second.get() : nullptr;
}

Q3SourceRecord *find_record(GeometryInstance3D *p_source) {
	return p_source != nullptr ? find_record(p_source->get_instance_id()) : nullptr;
}

void dormant_add(Q3SourceRecord &r_record) {
	if (r_record.dormant_index != kNoIndex)
		return;
	r_record.dormant_index = g_dormant.size();
	g_dormant.push_back(&r_record);
}

void dormant_remove(Q3SourceRecord &r_record) {
	const std::size_t index = r_record.dormant_index;
	if (index == kNoIndex)
		return;
	Q3SourceRecord *last = g_dormant.back();
	g_dormant[index] = last;
	last->dormant_index = index;
	g_dormant.pop_back();
	r_record.dormant_index = kNoIndex;
}

std::vector<Q3SourceRecord *>::iterator live_lower_bound(std::uint64_t p_node_id) {
	return std::lower_bound(g_live.begin(), g_live.end(), p_node_id,
			[](const Q3SourceRecord *p_record, std::uint64_t p_id) {
				return p_record->node_id < p_id;
			});
}

void live_add(Q3SourceRecord &r_record) {
	const auto at = live_lower_bound(r_record.node_id);
	if (at != g_live.end() && (*at)->node_id == r_record.node_id)
		return;
	g_live.insert(at, &r_record);
}

void live_remove(Q3SourceRecord &r_record) {
	const auto at = live_lower_bound(r_record.node_id);
	if (at != g_live.end() && (*at)->node_id == r_record.node_id)
		g_live.erase(at);
}

// The node entered the tree (or was registered inside it): every cached
// scene fact is re-read by the next compile.
void activate(Q3SourceRecord &r_record) {
	if (r_record.in_tree || r_record.node == nullptr)
		return;
	r_record.in_tree = true;
	r_record.viewport = r_record.node->get_viewport();
	r_record.scope_id = 0;
	r_record.in_scope = false;
	r_record.visibility_dirty = true;
	r_record.transform_valid = false;
	r_record.bounds_dirty = true;
	r_record.surfaces_dirty = true;
	r_record.rows_world_dirty = true;
	dormant_remove(r_record);
	live_add(r_record);
}

void deactivate(Q3SourceRecord &r_record) {
	if (!r_record.in_tree)
		return;
	r_record.in_tree = false;
	r_record.viewport = nullptr;
	live_remove(r_record);
	dormant_add(r_record);
}

void erase_record(Q3SourceRecord &r_record) {
	const std::uint64_t id = r_record.node_id;
	live_remove(r_record);
	dormant_remove(r_record);
	g_records.erase(id);
}

void connect_signals(Q3SourceRecord &r_record, GeometryInstance3D *p_source) {
	if (r_record.signals_connected)
		return;
	const std::uint64_t id = r_record.node_id;
	const Error entered = p_source->connect("tree_entered",
			callable_mp_static(&Q3SourceRegistry::on_tree_entered).bind(id));
	// tree_exiting, not tree_exited: a node freed inside an ancestor's
	// exit-tree handler (the shell's world releasing the water strip while
	// the shell leaves the tree) never emits tree_exited, because its parent
	// is already outside the tree when remove_child runs; tree_exiting is
	// emitted for every node of the departing subtree first.
	const Error exiting = p_source->connect("tree_exiting",
			callable_mp_static(&Q3SourceRegistry::on_tree_exiting).bind(id));
	const Error visibility = p_source->connect("visibility_changed",
			callable_mp_static(&Q3SourceRegistry::on_visibility_changed).bind(id));
	// Connected only when every signal took: a record whose tree_exiting
	// connect failed must not enter the live set (the walk dereferences a
	// live record's node), and the next registration retries the connects.
	r_record.signals_connected = entered == OK && exiting == OK && visibility == OK;
	ERR_FAIL_COND_MSG(!r_record.signals_connected,
			"A Q3 source refused its tree/visibility signals; its record cannot follow it");
}

// Registration shared by every source kind: one record per node, its
// signals connected once, activated at once when the node is already inside
// the tree (the placer's populations, the water strip) and otherwise at its
// tree_entered (ObjectModel registers before add_child).
Q3SourceRecord &register_record(GeometryInstance3D *p_source) {
	const std::uint64_t id = p_source->get_instance_id();
	std::unique_ptr<Q3SourceRecord> &slot = g_records[id];
	if (!slot) {
		slot = std::make_unique<Q3SourceRecord>();
		slot->node_id = id;
		slot->node = p_source;
		dormant_add(*slot);
	}
	Q3SourceRecord &record = *slot;
	record.surfaces_dirty = true;
	record.bounds_dirty = true;
	connect_signals(record, p_source);
	// A record without its signals stays dormant: nothing would deactivate
	// it when the node leaves the tree.
	if (record.signals_connected && p_source->is_inside_tree())
		activate(record);
	return record;
}

} // namespace

void Q3SourceRegistry::register_object_material(const Ref<Material> &p_material,
		const ObjectMaterialClassification &p_classification) {
	if (p_material.is_null())
		return;
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	RegisteredMaterial &material = g_materials[p_material->get_instance_id()];
	material.classification = p_classification;
	++material.version;
}

void Q3SourceRegistry::clone_object_material(const Ref<Material> &p_source,
		const Ref<Material> &p_clone) {
	if (p_source.is_null() || p_clone.is_null())
		return;
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	const auto found = g_materials.find(p_source->get_instance_id());
	if (found == g_materials.end())
		return;
	RegisteredMaterial &clone = g_materials[p_clone->get_instance_id()];
	clone.classification = found->second.classification;
	++clone.version;
}

bool Q3SourceRegistry::object_material_classification(
		const Ref<Material> &p_material,
		ObjectMaterialClassification &r_classification) {
	if (p_material.is_null())
		return false;
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	const auto found = g_materials.find(p_material->get_instance_id());
	if (found == g_materials.end())
		return false;
	r_classification = found->second.classification;
	return true;
}

void Q3SourceRegistry::invalidate_object_material(const Ref<Material> &p_material) {
	if (p_material.is_null())
		return;
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	const auto found = g_materials.find(p_material->get_instance_id());
	if (found != g_materials.end())
		++found->second.version;
}

void Q3SourceRegistry::register_object_source(GeometryInstance3D *p_source,
		const Ref<Material> &p_material) {
	if (p_source == nullptr)
		return;
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	const auto classification = p_material.is_valid() ?
			g_materials.find(p_material->get_instance_id()) : g_materials.end();
	Q3SourceRecord *existing = find_record(p_source);
	if (classification == g_materials.end() ||
			!classification->second.classification.is_glow_capable) {
		// A source whose material carries no glow is never compiled; one
		// registered before (a surface slot swapped onto such a level) is
		// parked inactive in place.
		if (existing != nullptr)
			existing->active = false;
		return;
	}
	if (existing != nullptr) {
		// The same node re-registered with the level's mesh and material
		// swapped onto it: keep the record and bump the geometry generation
		// so the cache re-packs the surfaces once (register_record marks the
		// surface list and bounds for a re-read).
		++existing->geometry_generation;
	}
	Q3SourceRecord &record = register_record(p_source);
	record.source = Q3Source::Object;
	record.material_id = p_material->get_instance_id();
	record.additive_surfaces = 0;
	record.active = true;
}

void Q3SourceRegistry::unregister_source(GeometryInstance3D *p_source) {
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (Q3SourceRecord *record = find_record(p_source))
		record->active = false;
}

void Q3SourceRegistry::register_source(GeometryInstance3D *p_source,
		Q3Source p_kind, std::uint32_t p_additive_surfaces) {
	if (p_source == nullptr || p_kind == Q3Source::Object ||
			p_kind == Q3Source::LightCorona)
		return;
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	Q3SourceRecord &record = register_record(p_source);
	record.source = p_kind;
	record.material_id = 0;
	record.additive_surfaces = p_additive_surfaces;
	record.geometry_generation = 1;
	record.instance_generation = 1;
}

void Q3SourceRegistry::publish_geometry(GeometryInstance3D *p_source,
		int p_surface, const Array &p_arrays) {
	if (p_source == nullptr || p_surface < 0)
		return;
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	Q3SourceRecord *record = find_record(p_source);
	if (record == nullptr)
		return;
	Q3PublishedGeometry &published = record->published[p_surface];
	published.arrays = Q3SurfaceArrays::from_mesh_arrays(p_arrays);
	++published.generation;
	// The producer rebuilt the surface it publishes: its mesh bounds are
	// re-read, and so is its surface list when the record does not list the
	// surface yet (a strip that came back after a clear).
	record->bounds_dirty = true;
	const bool listed = std::any_of(record->surfaces.begin(),
			record->surfaces.end(), [p_surface](const Q3SurfaceRecord &p_row) {
				return p_row.surface == p_surface;
			});
	if (!listed)
		record->surfaces_dirty = true;
}

void Q3SourceRegistry::invalidate_source(GeometryInstance3D *p_source) {
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	Q3SourceRecord *record = find_record(p_source);
	if (record == nullptr)
		return;
	++record->geometry_generation;
	record->surfaces_dirty = true;
	record->bounds_dirty = true;
}

void Q3SourceRegistry::invalidate_instances(GeometryInstance3D *p_source) {
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	Q3SourceRecord *record = find_record(p_source);
	if (record == nullptr)
		return;
	++record->instance_generation;
	record->bounds_dirty = true;
}

void Q3SourceRegistry::cleanup_statics() {
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	g_live.clear();
	g_dormant.clear();
	g_records.clear();
	g_materials.clear();
}

std::recursive_mutex &Q3SourceRegistry::mutex() {
	return g_mutex;
}

void Q3SourceRegistry::begin_frame(std::vector<std::uint64_t> &r_dead_sources) {
	r_dead_sources.clear();
	// A node inside the tree cannot die without leaving it first, so only
	// the dormant records are checked against ObjectDB.
	for (std::size_t index = 0; index < g_dormant.size();) {
		Q3SourceRecord *record = g_dormant[index];
		if (ObjectDB::get_instance(record->node_id) != nullptr) {
			++index;
			continue;
		}
		r_dead_sources.push_back(record->node_id);
		erase_record(*record);
	}
	if (++g_frames % kMaterialSweepPeriod == 0) {
		// A cached ShaderMaterial may deliberately outlive every current
		// source and later be reused: ObjectDB lifetime, not the live-source
		// set, is the pruning authority.
		for (auto it = g_materials.begin(); it != g_materials.end();) {
			if (ObjectDB::get_instance(it->first) == nullptr)
				it = g_materials.erase(it);
			else
				++it;
		}
	}
}

const std::vector<Q3SourceRecord *> &Q3SourceRegistry::live_records() {
	return g_live;
}

std::size_t Q3SourceRegistry::record_count() {
	return g_records.size();
}

void Q3SourceRegistry::refresh_surfaces(Q3SourceRecord &r_record,
		FrameCounters &r_counters) {
	r_record.surfaces_dirty = false;
	r_record.surfaces.clear();
	r_record.mesh.unref();
	r_record.multimesh.unref();
	r_record.is_multimesh = false;
	++r_counters.records_touched;
	GeometryInstance3D *source = r_record.node;
	Ref<Mesh> mesh;
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(source)) {
		mesh = mesh_instance->get_mesh();
	} else if (MultiMeshInstance3D *multi_instance =
			Object::cast_to<MultiMeshInstance3D>(source)) {
		r_record.multimesh = multi_instance->get_multimesh();
		r_record.is_multimesh = true;
		if (r_record.multimesh.is_valid())
			mesh = r_record.multimesh->get_mesh();
	}
	r_record.mesh = mesh;
	if (r_record.mesh.is_null())
		return;
	const int surface_count = r_record.mesh->get_surface_count();
	for (int surface = 0; surface < surface_count; ++surface) {
		const Ref<ShaderMaterial> material = active_material(source, mesh, surface);
		if (material.is_null())
			continue;
		Q3SurfaceRecord row;
		row.surface = surface;
		row.material = material;
		row.material_id = material->get_instance_id();
		if (r_record.source == Q3Source::Object) {
			const auto classification = g_materials.find(row.material_id);
			if (classification == g_materials.end())
				continue;
			row.classification = classification->second.classification;
		}
		r_record.surfaces.push_back(row);
	}
}

void Q3SourceRegistry::refresh_rows(Q3SourceRecord &r_record,
		FrameCounters &r_counters) {
	if (r_record.multimesh.is_null()) {
		r_record.local_rows.clear();
		r_record.rows.clear();
		r_record.rows_generation = r_record.instance_generation;
		r_record.rows_world_dirty = false;
		return;
	}
	if (r_record.rows_generation != r_record.instance_generation) {
		// Rows are read once per instance generation; a carved (zero-scaled)
		// row never enters the live list, so nothing iterates it later.
		MultiMesh *multimesh = r_record.multimesh.ptr();
		const int instance_count = multimesh->get_instance_count();
		const int visible = multimesh->get_visible_instance_count();
		const int count = visible < 0 ? instance_count :
				std::min(visible, instance_count);
		r_record.local_rows.clear();
		r_record.local_rows.reserve(static_cast<std::size_t>(std::max(count, 0)));
		for (int instance = 0; instance < count; ++instance) {
			const Transform3D local = multimesh->get_instance_transform(instance);
			if (std::abs(local.basis.determinant()) <= 1.0e-8f)
				continue;
			r_record.local_rows.push_back(local);
		}
		r_record.rows_generation = r_record.instance_generation;
		r_record.rows_world_dirty = true;
		++r_counters.instance_row_reads;
		++r_counters.records_touched;
	}
	if (!r_record.rows_world_dirty)
		return;
	const AABB mesh_bounds = r_record.mesh.is_valid() ? r_record.mesh->get_aabb() : AABB();
	r_record.rows.resize(r_record.local_rows.size());
	for (std::size_t index = 0; index < r_record.local_rows.size(); ++index) {
		Q3InstanceRow &row = r_record.rows[index];
		row.world_transform = r_record.global_transform * r_record.local_rows[index];
		row.world_bounds = row.world_transform.xform(mesh_bounds);
	}
	r_record.rows_world_dirty = false;
}

void Q3SourceRegistry::refresh_surface_parameters(const Q3SourceRecord &p_record,
		Q3SurfaceRecord &r_surface, FrameCounters &r_counters) {
	const Ref<ShaderMaterial> &material = r_surface.material;
	if (material.is_null())
		return;
	if (p_record.source == Q3Source::Object) {
		const auto registered = g_materials.find(r_surface.material_id);
		const std::uint64_t version = registered != g_materials.end() ?
				registered->second.version : 1;
		if (r_surface.parameter_version == version)
			return;
		r_surface.parameter_version = version;
	}
	++r_counters.material_reads;
	r_surface.pack = Q3PackParameters();
	r_surface.pack.source = p_record.source;
	if (p_record.source == Q3Source::Object) {
		r_surface.pack.uv_u = vector3_parameter(material, "u_uv_transform_u",
				Vector3(1, 0, 0));
		r_surface.pack.uv_v = vector3_parameter(material, "u_uv_transform_v",
				Vector3(0, 1, 0));
		Q3ObjectMaterialParameters &object = r_surface.object;
		object.classification = r_surface.classification;
		const Ref<Texture2D> base = texture_parameter(material, "u_diffuse");
		const Ref<Texture2D> detail = texture_parameter(material, "u_detail");
		object.base_texture = lease_for(base);
		object.detail_texture = lease_for(detail);
		const Vector4 reflect = vector4_parameter(material, "u_reflect_color",
				Vector4(0.75f, 0.75f, 0.75f, 0.75f));
		object.reflect_color = {reflect.x, reflect.y, reflect.z, reflect.w};
		const Vector3 self_lum = vector3_parameter(material, "u_rgb_mod",
				Vector3(1, 1, 1));
		object.self_lum_color = {self_lum.x, self_lum.y, self_lum.z, 1.0f};
		object.alpha_mod = float_parameter(material, "u_alpha_mod", 1.0f);
		object.diffuse_max_lod = float_parameter(material, "u_diffuse_max_lod",
				kQ3NoMipCeiling);
		object.detail_max_lod = float_parameter(material, "u_detail_max_lod",
				kQ3NoMipCeiling);
		r_surface.primary_texture_resource = base;
		r_surface.secondary_texture_resource = detail;
		r_surface.tertiary_texture_resource.unref();
		r_surface.primary_texture = server_rid(base);
		r_surface.secondary_texture = server_rid(detail);
		r_surface.tertiary_texture = RID();
		return;
	}
	if (p_record.source == Q3Source::Water) {
		Q3WaterMaterialParameters &water = r_surface.water;
		const Ref<Texture2D> reflection = texture_parameter(material, "u_reflection");
		const Ref<Texture2D> noise_color = texture_parameter(material, "u_noise_color");
		const Ref<Texture2D> noise_normal = texture_parameter(material, "u_noise_normal");
		water.has_reflection = bool_parameter(material, "u_has_reflection",
				reflection.is_valid());
		water.reflection_texture = lease_for(reflection);
		water.noise_color_texture = lease_for(noise_color);
		water.noise_normal_texture = lease_for(noise_normal);
		const Vector3 water_color = vector3_parameter(material, "u_water_color",
				Vector3(0.408f, 0.314f, 0.224f));
		water.water_color = {water_color.x, water_color.y, water_color.z};
		const Vector4 uv = vector4_parameter(material, "u_water_uv",
				Vector4(1.0f, 0.2f, 0.0f, 0.0f));
		water.water_uv = {uv.x, uv.y, uv.z, uv.w};
		r_surface.pack.water_uv = uv;
		const Vector2 scale = vector2_parameter(material, "u_reflection_uv_scale",
				Vector2(1, 1));
		water.reflection_uv_scale = {scale.x, scale.y};
		water.underwater_view = bool_parameter(material, "u_underwater_view", false);
		r_surface.primary_texture_resource = noise_color;
		r_surface.secondary_texture_resource = noise_normal;
		r_surface.tertiary_texture_resource = reflection;
		r_surface.primary_texture = server_rid(noise_color);
		r_surface.secondary_texture = server_rid(noise_normal);
		r_surface.tertiary_texture = server_rid(reflection);
		return;
	}
	Q3CelestialMaterialParameters &celestial = r_surface.celestial;
	const Ref<Texture2D> diffuse = texture_parameter(material, "u_diffuse");
	celestial.diffuse_texture = lease_for(diffuse);
	const Vector3 tint = vector3_parameter(material, "u_tint", Vector3(1, 1, 1));
	celestial.tint = {tint.x, tint.y, tint.z};
	// Every celestial producer publishes its bloom-pass opacity as
	// u_q3_opacity (the moon's fog-shader leg, the sun's body alpha, the
	// glare's occlusion-free peak); an unpublished row draws nothing rather
	// than borrowing the beauty formula.
	celestial.opacity = float_parameter(material, "u_q3_opacity", 0.0f);
	const Vector3 glare = vector3_parameter(material, "u_glare_direction",
			Vector3(0, 1, 0));
	celestial.glare_direction = {glare.x, glare.y, glare.z};
	celestial.glare_view_fade = bool_parameter(material, "u_glare_view_fade", false);
	// The blend is the one the Celestial installed for this surface
	// (registered beside the source), never the source kind: the authored
	// sun/moon FF_ST_AD_LUM discs add like the glare.
	celestial.additive = r_surface.surface < 32 &&
			((p_record.additive_surfaces >> r_surface.surface) & 1u) != 0u;
	r_surface.primary_texture_resource = diffuse;
	r_surface.secondary_texture_resource.unref();
	r_surface.tertiary_texture_resource.unref();
	r_surface.primary_texture = server_rid(diffuse);
	r_surface.secondary_texture = RID();
	r_surface.tertiary_texture = RID();
}

void Q3SourceRegistry::on_tree_entered(std::uint64_t p_node_id) {
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (Q3SourceRecord *record = find_record(p_node_id))
		activate(*record);
}

void Q3SourceRegistry::on_tree_exiting(std::uint64_t p_node_id) {
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (Q3SourceRecord *record = find_record(p_node_id))
		deactivate(*record);
}

void Q3SourceRegistry::on_visibility_changed(std::uint64_t p_node_id) {
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (Q3SourceRecord *record = find_record(p_node_id))
		record->visibility_dirty = true;
}
