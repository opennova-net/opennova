#include "nova_foliage_dispatcher.h"

#include "nova_terrain_data.h"

#include <foliage/fd_bake.h>
#include <foliage/far_mesh_emitter.h>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace godot {

namespace {

constexpr float INVALID_HEIGHT_THRESHOLD = -1.0e6f;

// The witnessed model render-height scale: the static VB halves the model Y
// (pos.y = y * 0.5) [orig: Foliage_FillInstancedModelBuffers @ 0x5ffa20].
// The XZ counterpart is opennova::foliage::MODEL_FOOTPRINT_SCALE (0.75).
constexpr float MODEL_RENDER_HEIGHT_SCALE = 0.5f;

// FAR collect/draw constants, all witnessed in retail Jointops.exe:
//   - 42.0: both the traversal pregate (flt_7DEA3C @ 0x60906d) and the
//     per-leaf clamped-AABB distance gate (@ 0x603f5c) that admit a cell
//     into the foliage key list.
//   - 128: the per-frame cap of both collect lists (@ 0x603f98 / 0x603ff1).
//   - fade: c6.a = 1 through distance 20 (flt_7D8E60), then
//     1 - (d - 20) * (1/22) (flt_7DF1BC) [orig: @ 0x60a45d..0x60a483].
//   - pass split: high under 33.0 (flt_7DF1C0 @ 0x60a171), alpha-test ref
//     180 high / 8 low [orig: Terrain_SetupSectorModelDraw @ 0x6008f5 /
//     0x600931].
constexpr float FAR_COLLECT_RADIUS = 42.0f;
constexpr int FAR_COLLECT_CAP = 128;
constexpr float FAR_FADE_START = 20.0f;
constexpr float FAR_FADE_INV_RANGE = 1.0f / 22.0f;
constexpr float FAR_HIGH_PASS_DISTANCE = 33.0f;
constexpr float FAR_ALPHA_REF_HIGH = 180.0f;
constexpr float FAR_ALPHA_REF_LOW = 8.0f;
// Host pool residency. Retail keeps baked cells resident in per-def VB slot
// pools and LRU-evicts by frame stamp [orig: Foliage_UpdateFarCellSlots
// @ 0x601b30]; the collect cap bounds the set a frame can touch, so the host
// sizes its pool to it.
constexpr size_t FAR_POOL_CAP = 128;

constexpr float FOLIAGE_CELL_SIZE = 16.0f;

inline float far_fade_for_distance(float distance) {
	// [orig: render_terrain_lightmaps @ 0x60a45d..0x60a483] c6.a: 1 through
	// 20, then 1 - (d - 20)/22 (reaches 0 exactly at the 42.0 collect edge).
	if (distance <= FAR_FADE_START) {
		return 1.0f;
	}
	return std::max(0.0f, 1.0f - (distance - FAR_FADE_START) * FAR_FADE_INV_RANGE);
}

inline float far_alpha_ref_for_distance(float distance) {
	// [orig: render_terrain_lightmaps @ 0x60a171] high pass under 33.0.
	return distance < FAR_HIGH_PASS_DISTANCE ? FAR_ALPHA_REF_HIGH : FAR_ALPHA_REF_LOW;
}

} // namespace

NovaFoliageDispatcher::NovaFoliageDispatcher() = default;
NovaFoliageDispatcher::~NovaFoliageDispatcher() = default;

void NovaFoliageDispatcher::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_foliage_defs", "defs"), &NovaFoliageDispatcher::set_foliage_defs);
	ClassDB::bind_method(D_METHOD("get_foliage_defs"), &NovaFoliageDispatcher::get_foliage_defs);
	ClassDB::bind_method(D_METHOD("set_slot_meshes", "meshes"), &NovaFoliageDispatcher::set_slot_meshes);
	ClassDB::bind_method(D_METHOD("get_slot_meshes"), &NovaFoliageDispatcher::get_slot_meshes);
	ClassDB::bind_method(D_METHOD("set_slot_fd_textures", "textures"),
	                     &NovaFoliageDispatcher::set_slot_fd_textures);
	ClassDB::bind_method(D_METHOD("get_slot_fd_textures"), &NovaFoliageDispatcher::get_slot_fd_textures);
	ClassDB::bind_method(D_METHOD("set_model_anchors", "anchors"),
	                     &NovaFoliageDispatcher::set_model_anchors);
	ClassDB::bind_method(D_METHOD("get_model_anchors"), &NovaFoliageDispatcher::get_model_anchors);
	ClassDB::bind_method(D_METHOD("set_model_anchor_range", "range"),
	                     &NovaFoliageDispatcher::set_model_anchor_range);
	ClassDB::bind_method(D_METHOD("get_model_anchor_range"),
	                     &NovaFoliageDispatcher::get_model_anchor_range);
	ClassDB::bind_method(D_METHOD("set_model_view_fov", "fov_y_deg", "aspect"),
	                     &NovaFoliageDispatcher::set_model_view_fov);
	ClassDB::bind_static_method("NovaFoliageDispatcher", D_METHOD("bake_fd_image", "image"),
	                            &NovaFoliageDispatcher::bake_fd_image);
	ClassDB::bind_method(D_METHOD("get_model_tile_debug", "slot"),
	                     &NovaFoliageDispatcher::get_model_tile_debug);
	ClassDB::bind_method(D_METHOD("get_model_draw_debug"),
	                     &NovaFoliageDispatcher::get_model_draw_debug);
	ClassDB::bind_method(D_METHOD("get_far_tile_debug", "slot"),
	                     &NovaFoliageDispatcher::get_far_tile_debug);
	ClassDB::bind_method(D_METHOD("set_height_sampler", "sampler"), &NovaFoliageDispatcher::set_height_sampler);
	ClassDB::bind_method(D_METHOD("get_height_sampler"), &NovaFoliageDispatcher::get_height_sampler);
	ClassDB::bind_method(D_METHOD("set_foliage_sampler", "sampler"), &NovaFoliageDispatcher::set_foliage_sampler);
	ClassDB::bind_method(D_METHOD("get_foliage_sampler"), &NovaFoliageDispatcher::get_foliage_sampler);
	ClassDB::bind_method(D_METHOD("set_surface_sampler", "sampler"), &NovaFoliageDispatcher::set_surface_sampler);
	ClassDB::bind_method(D_METHOD("get_surface_sampler"), &NovaFoliageDispatcher::get_surface_sampler);
	ClassDB::bind_method(D_METHOD("set_terrain_data", "data"), &NovaFoliageDispatcher::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"), &NovaFoliageDispatcher::get_terrain_data);
	ClassDB::bind_method(D_METHOD("set_colormap_source", "data"), &NovaFoliageDispatcher::set_colormap_source);
	ClassDB::bind_method(D_METHOD("get_colormap_source"), &NovaFoliageDispatcher::get_colormap_source);
	ClassDB::bind_method(D_METHOD("dispatch", "centre", "view_xform"),
	                     &NovaFoliageDispatcher::dispatch, DEFVAL(Transform3D()));
	ClassDB::bind_method(D_METHOD("reset"), &NovaFoliageDispatcher::reset);
	ClassDB::bind_method(D_METHOD("get_total_instances"), &NovaFoliageDispatcher::get_total_instances);
	ClassDB::bind_method(D_METHOD("get_cached_cells"), &NovaFoliageDispatcher::get_cached_cells);
	ClassDB::bind_method(D_METHOD("get_dispatch_stats"), &NovaFoliageDispatcher::get_dispatch_stats);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "foliage_defs"), "set_foliage_defs", "get_foliage_defs");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "slot_meshes", PROPERTY_HINT_ARRAY_TYPE, "Mesh"),
	             "set_slot_meshes", "get_slot_meshes");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "slot_fd_textures", PROPERTY_HINT_ARRAY_TYPE, "Texture2D"),
	             "set_slot_fd_textures", "get_slot_fd_textures");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "model_anchors"),
	             "set_model_anchors", "get_model_anchors");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "model_anchor_range"),
	             "set_model_anchor_range", "get_model_anchor_range");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "height_sampler"), "set_height_sampler", "get_height_sampler");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "foliage_sampler"), "set_foliage_sampler", "get_foliage_sampler");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "surface_sampler"), "set_surface_sampler", "get_surface_sampler");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data", PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainData"),
	             "set_terrain_data", "get_terrain_data");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "colormap_source", PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainData"),
	             "set_colormap_source", "get_colormap_source");
}

void NovaFoliageDispatcher::set_foliage_defs(const Array &p_defs) {
	foliage_defs_ = p_defs;
	reset();
}

Array NovaFoliageDispatcher::get_foliage_defs() const { return foliage_defs_; }

void NovaFoliageDispatcher::set_slot_meshes(const Array &p_meshes) {
	slot_meshes_ = p_meshes;
	for (Ref<ShaderMaterial> &material : foliage_materials_) {
		material.unref();
	}
	for (Ref<ShaderMaterial> &material : foliage_model_materials_) {
		material.unref();
	}
	_refresh_slot_bounds();
	// Mesh data is placement input in both tiers: FAR replicates its vertices,
	// while MODEL derives its footprint from the bounds.
	reset();
}

Array NovaFoliageDispatcher::get_slot_meshes() const { return slot_meshes_; }

void NovaFoliageDispatcher::set_slot_fd_textures(const Array &p_textures) {
	slot_fd_textures_ = p_textures;
	// Texture-only swap: refresh materials, keep every placement cache. The
	// FAR pool nodes re-adopt the recreated shared material on their next
	// active frame; MODEL draw nodes rebuild their per-draw clones.
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		const bool had_material = foliage_materials_[s].is_valid();
		foliage_materials_[s].unref();
		if (had_material) {
			// Recreate the shared slot material NOW: the resident pool's
			// lazy re-adopt (_apply_far_cell_state) and the per-frame
			// wind-phase refresh both key off its validity - deferring to
			// the next NEW cell bake left resident cells on the stale
			// texture with a frozen u_far_wind_phase.
			_update_slot_material(s);
		}
	}
	for (Ref<ShaderMaterial> &material : foliage_model_materials_) {
		material.unref();
	}
	for (ModelDrawNodeState &state : model_draw_node_states_) {
		state.material.unref();
		state.slot = -1;  // force a refresh on the next update
	}
}

Array NovaFoliageDispatcher::get_slot_fd_textures() const { return slot_fd_textures_; }

void NovaFoliageDispatcher::set_model_anchors(const PackedVector3Array &p_anchors) {
	// Per-tick input; never invalidates caches (the model tile cache is keyed
	// by tile, not anchor).
	model_anchors_ = p_anchors;
}

PackedVector3Array NovaFoliageDispatcher::get_model_anchors() const { return model_anchors_; }

void NovaFoliageDispatcher::set_model_anchor_range(float p_range) {
	model_anchor_range_ = p_range > 0.0f ? p_range : 0.0f;
}

float NovaFoliageDispatcher::get_model_anchor_range() const { return model_anchor_range_; }

void NovaFoliageDispatcher::set_model_view_fov(float p_fov_y_deg, float p_aspect) {
	if (p_fov_y_deg <= 0.0f || p_aspect <= 0.0f) {
		model_view_tan_half_h_ = 0.0f;
		model_view_tan_half_v_ = 0.0f;
		return;
	}
	const float half_v = Math::deg_to_rad(p_fov_y_deg) * 0.5f;
	model_view_tan_half_v_ = Math::tan(half_v);
	model_view_tan_half_h_ = model_view_tan_half_v_ * p_aspect;
}

bool NovaFoliageDispatcher::bake_fd_image(const Ref<Image> &p_image) {
	// [orig: Foliage_LoadDefAssets @ 0x601260 tail] - the ":fd" bake over the
	// model's own diffuse; see libs/foliage/fd_bake.h for the kernel.
	if (p_image.is_null() || p_image->get_format() != Image::FORMAT_RGBA8) {
		return false;
	}
	const int w = p_image->get_width();
	const int h = p_image->get_height();
	PackedByteArray data = p_image->get_data();
	const int64_t level0_size = static_cast<int64_t>(w) * h * 4;
	if (data.size() < level0_size) {
		return false;
	}
	if (data.size() > level0_size) {
		// Mipmapped input: bake level 0 and drop the mip tail (callers
		// regenerate mips from the baked result).
		data.resize(level0_size);
	}
	if (!opennova::foliage::bake_fd_rgba(data.ptrw(), w, h)) {
		return false;
	}
	p_image->set_data(w, h, false, Image::FORMAT_RGBA8, data);
	return true;
}

void NovaFoliageDispatcher::set_height_sampler(const Callable &p_sampler) {
	height_sampler_ = p_sampler;
	// FAR vertices bake the sampled terrain height into their cell meshes.
	reset();
}
Callable NovaFoliageDispatcher::get_height_sampler() const { return height_sampler_; }

void NovaFoliageDispatcher::set_foliage_sampler(const Callable &p_sampler) {
	foliage_sampler_ = p_sampler;
	reset();
}

Callable NovaFoliageDispatcher::get_foliage_sampler() const { return foliage_sampler_; }

void NovaFoliageDispatcher::set_surface_sampler(const Callable &p_sampler) {
	surface_sampler_ = p_sampler;
	reset();
}

Callable NovaFoliageDispatcher::get_surface_sampler() const { return surface_sampler_; }

void NovaFoliageDispatcher::set_terrain_data(const Ref<NovaTerrainData> &p_data) {
	if (terrain_data_ == p_data) {
		return;
	}
	terrain_data_ = p_data;
	reset();
}

Ref<NovaTerrainData> NovaFoliageDispatcher::get_terrain_data() const { return terrain_data_; }

void NovaFoliageDispatcher::set_colormap_source(const Ref<NovaTerrainData> &p_data) {
	if (colormap_source_ == p_data) {
		return;
	}
	colormap_source_ = p_data;
	for (Ref<ShaderMaterial> &material : foliage_materials_) {
		material.unref();
	}
}

Ref<NovaTerrainData> NovaFoliageDispatcher::get_colormap_source() const { return colormap_source_; }

bool NovaFoliageDispatcher::_has_sampling_source() const {
	return terrain_data_.is_valid() ||
	       (height_sampler_.is_valid() &&
	        (foliage_sampler_.is_valid() || surface_sampler_.is_valid()));
}

void NovaFoliageDispatcher::reset() {
	dispatch_stats_ = DispatchStats{};
	far_frame_counter_ = 0;
	far_visible_.clear();
	model_frame_counter_ = 0;
	model_wind_counter_ = 0;
	for (auto &dispatcher : model_dispatchers_) {
		dispatcher.reset();
	}

	model_draw_batches_.clear();
	_clear_children();
}

int NovaFoliageDispatcher::get_cached_cells() const {
	int total = 0;
	for (const auto &pool : far_cells_) {
		total += static_cast<int>(pool.size());
	}
	return total;
}

int NovaFoliageDispatcher::get_total_instances() const {
	int total = 0;
	for (const auto &pool : far_cells_) {
		for (const auto &kv : pool) {
			if (kv.second.active) {
				total += static_cast<int>(kv.second.placements.size());
			}
		}
	}
	return total;
}

Dictionary NovaFoliageDispatcher::get_dispatch_stats() const {
	Dictionary out;
	out["dispatch_calls"] = dispatch_stats_.dispatch_calls;
	out["far_cells_visible"] = dispatch_stats_.far_cells_visible;
	out["far_pool_hits"] = dispatch_stats_.far_pool_hits;
	out["far_pool_misses"] = dispatch_stats_.far_pool_misses;
	out["far_cells_baked"] = dispatch_stats_.far_cells_baked;
	out["far_instances_baked"] = dispatch_stats_.far_instances_baked;
	out["cached_cells"] = get_cached_cells();
	out["total_instances"] = get_total_instances();
	// Model tier.
	out["model_anchors_in_range"] = dispatch_stats_.model_anchors_in_range;
	out["model_tiles_emitted"] = dispatch_stats_.model_tiles_emitted;
	out["model_batches"] = static_cast<int64_t>(model_draw_batches_.size());
	out["model_instances"] = dispatch_stats_.model_instances;
	out["model_uploads"] = dispatch_stats_.model_uploads;
	int64_t model_hits = 0;
	int64_t model_misses = 0;
	int64_t model_regens = 0;
	int model_cached_tiles = 0;
	for (const auto &dispatcher : model_dispatchers_) {
		model_hits += dispatcher.cache_hits();
		model_misses += dispatcher.cache_misses();
		model_regens += dispatcher.regenerations();
		model_cached_tiles += dispatcher.cache_occupancy();
	}
	out["far_us"] = dispatch_stats_.far_us;
	out["material_us"] = dispatch_stats_.material_us;
	out["model_us"] = dispatch_stats_.model_us;
	out["model_cache_hits"] = model_hits;
	out["model_cache_misses"] = model_misses;
	out["model_regenerations"] = model_regens;
	out["model_cached_tiles"] = model_cached_tiles;
	return out;
}

Array NovaFoliageDispatcher::get_model_tile_debug(int p_slot) const {
	Array out;
	if (p_slot < 0 || p_slot >= opennova::FOLIAGE_MAX_DEFS) {
		return out;
	}
	using opennova::foliage::FIXED_TO_FLOAT;
	const SlotModelBounds &bounds = slot_bounds_[p_slot];
	for (const auto &batch : model_draw_batches_) {
		if (batch.slot != p_slot || batch.instances == nullptr) {
			continue;
		}
		for (int bi = 0; bi < batch.instance_count; ++bi) {
			const auto &inst = batch.instances[bi];
		const float hbase = opennova::foliage::model_instance_hbase(inst);
		Dictionary d;
		d["center"] = Vector3(static_cast<float>(inst.center_x_fixed) * FIXED_TO_FLOAT, hbase,
		                      static_cast<float>(inst.center_z_fixed) * FIXED_TO_FLOAT);
		d["hbase"] = hbase;
		d["yaw"] = inst.yaw_radians;
		PackedVector3Array corners;
		for (int k = 0; k < 4; ++k) {
			corners.push_back(Vector3(static_cast<float>(inst.corner_x_fixed[k]) * FIXED_TO_FLOAT,
			                          inst.corner_height[k],
			                          static_cast<float>(inst.corner_z_fixed[k]) * FIXED_TO_FLOAT));
		}
		d["corners"] = corners;
		d["transform"] = _model_instance_transform(inst, hbase, bounds);
		d["color"] = Color(inst.corner_height[0] - hbase, inst.corner_height[2] - hbase,
		                   inst.corner_height[1] - hbase, inst.corner_height[3] - hbase);
		d["custom"] = Color(inst.fold_e_b, inst.fold_t_b, inst.fold_e_a, inst.fold_t_a);
		d["bound_center"] = Vector2(bounds.center_x, bounds.center_z);
		d["bound_radius"] = bounds.radius;
		out.push_back(d);
		}
	}
	return out;
}

Array NovaFoliageDispatcher::get_model_draw_debug() const {
	Array out;
	for (int i = 0; i < static_cast<int>(model_draw_batches_.size()); ++i) {
		const ModelDrawBatch &batch = model_draw_batches_[i];
		Dictionary d;
		d["draw_index"] = i;
		d["slot"] = batch.slot;
		d["tile_key"] = static_cast<int64_t>(batch.tile_key);
		d["generation"] = batch.generation;
		d["anchor"] = batch.anchor;
		d["view_depth"] = batch.view_depth;
		d["anchor_distance"] = batch.anchor_distance;
		d["alpha_ref"] = batch.alpha_ref;
		d["wind_counter"] = batch.wind_counter;
		d["wind_phase"] = batch.wind_phase;
		d["instance_count"] = static_cast<int64_t>(batch.instance_count);
		d["submissions"] = batch.submissions;
		out.push_back(d);
	}
	return out;
}

Array NovaFoliageDispatcher::get_far_tile_debug(int p_slot) const {
	Array out;
	if (p_slot < 0 || p_slot >= opennova::FOLIAGE_MAX_DEFS) {
		return out;
	}
	for (const auto &kv : far_cells_[p_slot]) {
		const FarCellEntry &entry = kv.second;
		if (!entry.active) {
			continue;
		}
		for (const auto &placement : entry.placements) {
			Dictionary d;
			d["center"] = Vector3(
			    static_cast<float>(placement.world_x_fixed) * opennova::foliage::FIXED_TO_FLOAT,
			    0.0f,
			    static_cast<float>(placement.world_z_fixed) * opennova::foliage::FIXED_TO_FLOAT);
			d["yaw"] = placement.rotation_radians;
			d["cell_key"] = static_cast<int64_t>(kv.first);
			d["distance"] = entry.distance;
			d["fade"] = far_fade_for_distance(entry.distance);
			d["alpha_ref"] = far_alpha_ref_for_distance(entry.distance);
			out.push_back(d);
		}
	}
	return out;
}

void NovaFoliageDispatcher::dispatch(Vector3 centre, Transform3D view_xform) {
	++dispatch_stats_.dispatch_calls;
	if (foliage_defs_.is_empty()) {
		return;
	}

	Dictionary defs_by_match = _build_defs_by_match();
	if (!_has_sampling_source()) {
		return;
	}

	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	_dispatch_far_tier(centre);
	const uint64_t t1 = Time::get_singleton()->get_ticks_usec();

	// The FAR wind phase advances every draw; refresh the small per-slot
	// material parameter sets each frame.
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		if (foliage_materials_[s].is_valid()) {
			_update_slot_material(s);
		}
	}

	const uint64_t t2 = Time::get_singleton()->get_ticks_usec();
	_dispatch_model_tier(view_xform, defs_by_match);
	const uint64_t t3 = Time::get_singleton()->get_ticks_usec();
	dispatch_stats_.far_us = static_cast<int64_t>(t1 - t0);
	dispatch_stats_.material_us = static_cast<int64_t>(t2 - t1);
	dispatch_stats_.model_us = static_cast<int64_t>(t3 - t2);
}

// --- The FAR tier ------------------------------------------------------------

float NovaFoliageDispatcher::_sample_height_world(float p_world_x, float p_world_z) const {
	if (NovaTerrainData *td = terrain_data_.ptr()) {
		return td->get_height_world_bilinear(Vector3(p_world_x, 0.0f, p_world_z));
	}
	if (height_sampler_.is_valid()) {
		Array args;
		args.push_back(p_world_x);
		args.push_back(p_world_z);
		return static_cast<float>(static_cast<double>(height_sampler_.callv(args)));
	}
	return INVALID_HEIGHT_THRESHOLD;
}

void NovaFoliageDispatcher::_collect_far_cells(const Vector3 &camera_pos) {
	// Host analog of the traversal collect [orig: Terrain_TraverseQuadtreeNode
	// @ 0x60905c -> Terrain_CollectNearFoliagePatches @ 0x603e60]: every 16u
	// leaf cell whose clamped-box 3D distance from the camera is <= 42.0 joins
	// the frame's key list (<= 128). Retail measures leaf AABBs during the
	// frustum-culled traversal; the host enumerates the 42u disc directly and
	// lets Godot's per-node culling drop the off-screen draws, and it uses the
	// terrain height under the cell center as the leaf's Y metric.
	far_visible_.clear();

	const float r = FAR_COLLECT_RADIUS;
	// A cell keyed (kx, kz) covers world x in [kx, kx+16] and z in [kz-16, kz]
	// (the witnessed +16 Z key bias; the placement B axis runs negative world
	// Z).
	const int kx_min = static_cast<int>(std::floor((camera_pos.x - r) / FOLIAGE_CELL_SIZE)) * 16;
	const int kx_max = static_cast<int>(std::floor((camera_pos.x + r) / FOLIAGE_CELL_SIZE)) * 16;
	const int kz_min = static_cast<int>(std::floor((camera_pos.z - r) / FOLIAGE_CELL_SIZE)) * 16 + 16;
	const int kz_max = static_cast<int>(std::floor((camera_pos.z + r) / FOLIAGE_CELL_SIZE)) * 16 + 16;

	for (int kz = kz_min; kz <= kz_max; kz += 16) {
		for (int kx = kx_min; kx <= kx_max; kx += 16) {
			// Clamped-box XZ distance to the cell footprint.
			float dx = 0.0f;
			if (camera_pos.x < static_cast<float>(kx)) {
				dx = static_cast<float>(kx) - camera_pos.x;
			} else if (camera_pos.x > static_cast<float>(kx) + FOLIAGE_CELL_SIZE) {
				dx = camera_pos.x - (static_cast<float>(kx) + FOLIAGE_CELL_SIZE);
			}
			float dz = 0.0f;
			if (camera_pos.z < static_cast<float>(kz) - FOLIAGE_CELL_SIZE) {
				dz = (static_cast<float>(kz) - FOLIAGE_CELL_SIZE) - camera_pos.z;
			} else if (camera_pos.z > static_cast<float>(kz)) {
				dz = camera_pos.z - static_cast<float>(kz);
			}
			if (dx * dx + dz * dz > r * r) {
				continue;
			}

			// Y metric: terrain height under the cell center. No terrain here
			// means no key - the OOB analog of the 0x80000000 empty marker.
			const float height = _sample_height_world(static_cast<float>(kx) + 8.0f,
			                                          static_cast<float>(kz) - 8.0f);
			if (height <= INVALID_HEIGHT_THRESHOLD) {
				continue;
			}
			const float dy = camera_pos.y - height;
			const float dist_sq = dx * dx + dz * dz + dy * dy;
			if (dist_sq > r * r) {
				continue;
			}

			FarVisibleCell cell;
			cell.cell_x_int = kx;
			cell.cell_z_int = kz;
			cell.key = opennova::foliage::pack_cell_key(
			    static_cast<opennova::foliage::Fixed16_16>(kx) << 16,
			    static_cast<opennova::foliage::Fixed16_16>(kz) << 16);
			cell.distance = std::sqrt(dist_sq);
			far_visible_.push_back(cell);
			if (far_visible_.size() >= static_cast<size_t>(FAR_COLLECT_CAP)) {
				return;  // the witnessed 128-entry list cap
			}
		}
	}
}

void NovaFoliageDispatcher::_dispatch_far_tier(const Vector3 &camera_pos) {
	++far_frame_counter_;
	_collect_far_cells(camera_pos);
	dispatch_stats_.far_cells_visible = static_cast<int64_t>(far_visible_.size());

	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		auto &pool = far_cells_[s];

		Ref<NovaTerrainFoliageDef> def;
		if (s < foliage_defs_.size()) {
			def = foliage_defs_[s];
		}
		Ref<Mesh> slot_mesh;
		if (s < slot_meshes_.size()) {
			slot_mesh = slot_meshes_[s];
		}
		const bool slot_enabled = def.is_valid() && slot_mesh.is_valid();

		if (slot_enabled) {
			// New-key bake budget: the engine's per-call new-texture list is a
			// 64-entry stack array (dedup into `ib_and_new_textures[1..64]`,
			// no growth) [orig: Foliage_UpdateFarCellSlots @ 0x601b30 — the
			// _WORD *[65] frame local]. A fresh view fills over successive
			// frames instead of spiking one; misses beyond the budget stay
			// unbaked and re-collect next frame.
			int bake_budget = 64;
			for (const FarVisibleCell &cell : far_visible_) {
				auto it = pool.find(cell.key);
				if (it == pool.end()) {
					if (bake_budget <= 0) {
						continue;
					}
					--bake_budget;
					++dispatch_stats_.far_pool_misses;
					it = pool.emplace(cell.key, FarCellEntry{}).first;
					_bake_far_cell_into(s, cell, def, it->second);
				} else {
					++dispatch_stats_.far_pool_hits;
				}
				FarCellEntry &entry = it->second;
				entry.last_touched = far_frame_counter_;
				entry.active = true;
				entry.distance = cell.distance;
				_apply_far_cell_state(s, entry, cell.distance);
			}
		}

		// Hide pooled cells that fell out of this frame's collect (or the
		// whole slot when it lost its def/mesh).
		for (auto &kv : pool) {
			FarCellEntry &entry = kv.second;
			if (entry.last_touched != far_frame_counter_ && entry.active) {
				entry.active = false;
				if (entry.node != nullptr) {
					entry.node->set_visible(false);
				}
			}
		}

		_evict_far_overflow(s);
	}
}

void NovaFoliageDispatcher::_bake_far_cell_into(int slot_index,
                                                const FarVisibleCell &cell,
                                                const Ref<NovaTerrainFoliageDef> &def,
                                                FarCellEntry &entry) {
	// Bake once per newly-resident key [orig: Foliage_UpdateFarCellSlots
	// @ 0x601b30 -> generate_foliage_instances_0 @ 0x5ffdd0]. A cell that
	// bakes empty stays resident with no node - retail keeps zero-count slots
	// and draws nothing for them.
	std::vector<opennova::foliage::PlacementInstance> placements;
	_scatter_cell(slot_index, cell.cell_x_int, cell.cell_z_int, def, placements);
	++dispatch_stats_.far_cells_baked;
	dispatch_stats_.far_instances_baked += static_cast<int64_t>(placements.size());

	entry.placements = std::move(placements);
	if (entry.placements.empty()) {
		return;
	}

	Ref<Mesh> cell_mesh = _build_far_mesh(slot_index, entry.placements);
	if (cell_mesh.is_null()) {
		return;
	}

	_update_slot_material(slot_index);

	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_name(String("FarCell") + String::num_int64(slot_index) + String("_") +
	             String::num_uint64(cell.key));
	add_child(mi);
	// The baked vertices are world-space; a top-level node keeps the mesh AABB
	// authoritative for Godot's frustum culling (the host stand-in for the
	// traversal's frustum gate).
	mi->set_as_top_level(true);
	mi->set_mesh(cell_mesh);
	if (foliage_materials_[slot_index].is_valid()) {
		mi->set_material_override(foliage_materials_[slot_index]);
	}
	// SHADOW attribute bit (Jointops.exe FoliageDef +532 bit 1).
	const int attribs = def.is_valid() ? def->get_attrib_flags() : 0;
	mi->set_cast_shadows_setting((attribs & opennova::FOLIAGE_ATTRIB_SHADOW)
	                                 ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON
	                                 : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	mi->set_extra_cull_margin(1.0f);  // wind sway margin (amplitude 0.03)
	entry.node = mi;
}

void NovaFoliageDispatcher::_apply_far_cell_state(int slot_index, FarCellEntry &entry,
                                                  float distance) {
	if (entry.node == nullptr) {
		return;
	}
	// A texture swap recreates the shared slot material; re-adopt it lazily.
	if (foliage_materials_[slot_index].is_valid() &&
	    entry.node->get_material_override() != foliage_materials_[slot_index]) {
		entry.node->set_material_override(foliage_materials_[slot_index]);
	}
	entry.node->set_visible(true);
	// The witnessed per-cell draw state: c6.a distance fade and the high/low
	// alpha-test ref [orig: render_terrain_lightmaps @ 0x60a171..0x60a53b].
	entry.node->set_instance_shader_parameter("u_cell_fade",
	                                          far_fade_for_distance(distance));
	entry.node->set_instance_shader_parameter("u_cell_alpha_ref",
	                                          far_alpha_ref_for_distance(distance));
}

void NovaFoliageDispatcher::_evict_far_overflow(int slot_index) {
	auto &pool = far_cells_[slot_index];
	while (pool.size() > FAR_POOL_CAP) {
		// Evict the stalest inactive entry [orig: the frame-stamp LRU scan in
		// Foliage_UpdateFarCellSlots @ 0x601c05..0x601c27].
		auto oldest = pool.end();
		for (auto it = pool.begin(); it != pool.end(); ++it) {
			if (it->second.active) {
				continue;
			}
			if (oldest == pool.end() || it->second.last_touched < oldest->second.last_touched) {
				oldest = it;
			}
		}
		if (oldest == pool.end()) {
			return;  // every resident entry is active this frame
		}
		if (oldest->second.node != nullptr) {
			if (oldest->second.node->get_parent() == this) {
				remove_child(oldest->second.node);
			}
			oldest->second.node->queue_free();
		}
		pool.erase(oldest);
	}
}

void NovaFoliageDispatcher::_clear_far_cells() {
	for (auto &pool : far_cells_) {
		for (auto &kv : pool) {
			if (kv.second.node != nullptr) {
				if (kv.second.node->get_parent() == this) {
					remove_child(kv.second.node);
				}
				kv.second.node->queue_free();
			}
		}
		pool.clear();
	}
}

Dictionary NovaFoliageDispatcher::_build_defs_by_match() const {
	Dictionary out;
	const int n = foliage_defs_.size();
	for (int i = 0; i < n; ++i) {
		Ref<NovaTerrainFoliageDef> def = foliage_defs_[i];
		if (def.is_null()) {
			continue;
		}
		const int match = def->get_match();
		if (match <= 0) {
			continue;
		}
		Array list;
		if (out.has(match)) {
			list = out[match];
		}
		list.push_back(i);
		out[match] = list;
	}
	return out;
}

// --- The NEAR/MODEL tier -----------------------------------------------------

void NovaFoliageDispatcher::_make_model_sampler_bindings(
		const Dictionary &defs_by_match,
		opennova::foliage::PlacementSamplers &out_samplers) const {
	NovaTerrainData *td = terrain_data_.ptr();

	// sub_606490 (path/ambient-source spacing) remains deferred until the
	// ambient-source registry exists (D-FOLIAGE-7).
	out_samplers.path_blocked = nullptr;

	out_samplers.height_at = [this, td](opennova::foliage::Fixed16_16 wx,
	                                    opennova::foliage::Fixed16_16 wz) -> opennova::foliage::Fixed16_16 {
		const float wx_f = static_cast<float>(wx) * opennova::foliage::FIXED_TO_FLOAT;
		const float wz_f = static_cast<float>(wz) * opennova::foliage::FIXED_TO_FLOAT;
		float y = INVALID_HEIGHT_THRESHOLD;
		if (td != nullptr) {
			y = td->get_height_world_bilinear(Vector3(wx_f, 0.0f, wz_f));
		} else if (height_sampler_.is_valid()) {
			Array args;
			args.push_back(wx_f);
			args.push_back(wz_f);
			y = static_cast<float>(static_cast<double>(height_sampler_.callv(args)));
		}
		if (y <= INVALID_HEIGHT_THRESHOLD) {
			// "No terrain here": hand the libs consumers their skip sentinel
			// (a raw *65536 cast of the float sentinel overflowed int32).
			return opennova::foliage::HEIGHT_INVALID;
		}
		return static_cast<opennova::foliage::Fixed16_16>(y * 65536.0f);
	};

	// The model tier's witnessed gate is the FOLIAGEMAP byte
	// [orig: Foliage_SampleFoliageMapMask @ 0x606620]; the host analog is the
	// foliage-map index -> def-slot mask chain. Dictionary captured by value
	// (COW ref) so the samplers outlive the caller's local.
	out_samplers.slot_mask_at = [this, td, defs_by_match](opennova::foliage::Fixed16_16 wx,
	                                                      opennova::foliage::Fixed16_16 wz) -> uint32_t {
		const float wx_f = static_cast<float>(wx) * opennova::foliage::FIXED_TO_FLOAT;
		const float wz_f = static_cast<float>(wz) * opennova::foliage::FIXED_TO_FLOAT;
		int painted = 0;
		if (td != nullptr) {
			// Placement candidates are RENDER-space; get_foliage_index_world
			// expects NATIVE z (the engine's sampler indexes (-z) internally
			// [orig: Foliage_SampleFarMapMask @ 0x6066d0 — ((x>>16)&1023,
			// (-z>>16)&1023)], and the Godot-side API keeps the native arg).
			// Passing render z here read the MIRRORED map: painted clusters
			// gated empty and ~mirror-accident instances landed elsewhere —
			// the 00TRg sparse-coverage bug the capture probe caught.
			painted = td->get_foliage_index_world(wx_f, -wz_f);
		} else if (foliage_sampler_.is_valid()) {
			Array args;
			args.push_back(wx_f);
			args.push_back(wz_f);
			painted = static_cast<int>(foliage_sampler_.callv(args));
		}
		if (painted == 0 || !defs_by_match.has(painted)) {
			return 0u;
		}
		uint32_t mask = 0u;
		Array def_list = defs_by_match[painted];
		for (int i = 0; i < def_list.size(); ++i) {
			const int slot_index = static_cast<int>(def_list[i]);
			if (slot_index >= 0 && slot_index < 32) {
				mask |= (1u << slot_index);
			}
		}
		return mask;
	};
}

// The source-model -> Godot mapping witnessed in the MODEL draw path:
//
//   MODEL TRANSFORM = translate(center, hbase) * rotY(yaw + pi/2)
//                   * scale(0.75, 0.5, 0.75) * translate(-(cx, 0, cz))
//
// The libs generator emits engine-space corners k = 0..3 at local coords
// (A, B) = (k&1 ? +F : -F, k&2 ? +F : -F) rotated by yaw, with
// world deltas dX = A cos - B sin, dZ = -(A sin + B cos) (the B axis runs
// NEGATIVE world Z). With Godot rotY(yaw + pi/2), engine (A, B) maps from
// mesh (z, x), so mesh corners (-1,-1),(+1,-1),(-1,+1),(+1,+1) land on
// c0,c2,c1,c3. COLOR is packed (h0,h2,h1,h3), and swapping the two fit axes
// packs CUSTOM as (E_B,T_B,E_A,T_A). Model scale: the VB normalization
// x_n = (x - cx)/(2R) + 0.5
// spanning footprint corners at +-0.75R gives the witnessed effective
// render scale 0.75 on XZ [orig: Foliage_FillInstancedModelBuffers
// @ 0x5ffa20 (the normalize)], and pos.y = y * 0.5 - the model height is
// HALVED in the VB. hbase = the fit at the instance center (corner average
// + the sag-fold center E_A + E_B); the vertex shader adds only the
// per-vertex DELTA, so the transform carries the base.
Basis NovaFoliageDispatcher::_engine_yaw_basis(float yaw_radians) const {
	return Basis(Vector3(0, 1, 0),
	             yaw_radians + static_cast<float>(Math_PI) * 0.5f);
}

Transform3D NovaFoliageDispatcher::_model_instance_transform(
		const opennova::foliage::ModelInstance &inst,
		float hbase,
		const SlotModelBounds &bounds) const {
	using opennova::foliage::FIXED_TO_FLOAT;
	const float wx = static_cast<float>(inst.center_x_fixed) * FIXED_TO_FLOAT;
	const float wz = static_cast<float>(inst.center_z_fixed) * FIXED_TO_FLOAT;
	Basis basis = _engine_yaw_basis(inst.yaw_radians);
	basis = basis * Basis::from_scale(Vector3(opennova::foliage::MODEL_FOOTPRINT_SCALE,
	                                          MODEL_RENDER_HEIGHT_SCALE,
	                                          opennova::foliage::MODEL_FOOTPRINT_SCALE));
	const Vector3 bound_center(bounds.center_x, 0.0f, bounds.center_z);
	const Vector3 origin = Vector3(wx, hbase, wz) - basis.xform(bound_center);
	return Transform3D(basis, origin);
}

void NovaFoliageDispatcher::_dispatch_model_tier(const Transform3D &view_xform,
                                                 const Dictionary &defs_by_match) {
	using namespace opennova::foliage;

	dispatch_stats_.model_anchors_in_range = 0;
	dispatch_stats_.model_tiles_emitted = 0;
	dispatch_stats_.model_instances = 0;

	model_draw_batches_.clear();

	// Which slots can stamp models: a def AND a mesh with usable bounds.
	ModelPlacementConfig config;
	bool slot_active[opennova::FOLIAGE_MAX_DEFS] = {};
	bool any_slot = false;
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS && s < foliage_defs_.size(); ++s) {
		Ref<NovaTerrainFoliageDef> def = foliage_defs_[s];
		if (def.is_null() || !slot_bounds_[s].valid) {
			continue;
		}
		config.attrib_flags[s] = static_cast<uint8_t>(def->get_attrib_flags());
		// footprint = 0.75 * Chebyshev bound radius
		// [orig: Foliage_DefTable_Footprint @ 0x316207C].
		config.footprint[s] = MODEL_FOOTPRINT_SCALE * slot_bounds_[s].radius;
		slot_active[s] = true;
		any_slot = true;
	}

	if (model_anchors_.is_empty() || !any_slot) {
		_update_model_draw_nodes();
		return;
	}

	PlacementSamplers samplers;
	_make_model_sampler_bindings(defs_by_match, samplers);

	++model_frame_counter_;

	const bool has_view = !(view_xform == Transform3D());
	const Transform3D view_inv = has_view ? view_xform.affine_inverse() : Transform3D();

	std::vector<ModelTileDraw> draws;
	// Per-frame (slot, tile) -> rendered-batch index. Retail submits a shared
	// tile once per qualifying sector entity and each later immediate-mode
	// draw overwrites the earlier one (z-write on, ZFUNC LESSEQUAL)
	// [orig: Foliage_DrawModelTileSlot @ 0x601e33]; two coexisting retained
	// copies would z-fight instead, so the host keeps ONE batch per
	// (slot, tile) carrying the LAST submission's draw state
	// (docs/foliage/foliage-re.md D-FOLIAGE-10). The wind counter still
	// advances once per SUBMISSION [orig:
	// Foliage_UploadModelTileVSConstants @ 0x600f00 pre-increments per call].
	std::unordered_map<uint64_t, size_t> frame_batches;

	for (int a = 0; a < model_anchors_.size(); ++a) {
		const Vector3 anchor = model_anchors_[a];

		// Per-anchor view depth, engine +Z-forward convention (Godot cameras
		// look down -Z). Identity view transform passes the gate
		// (headless/tests).
		float view_depth = MODEL_DEPTH_GATE;
		Vector3 view_pos;
		if (has_view) {
			view_pos = view_inv.xform(anchor);
			view_depth = -view_pos.z;
		}
		if (view_depth < MODEL_DEPTH_GATE) {
			continue;  // the witnessed >= 38.0 gate (also enforced in the libs walk)
		}
		// Host visibility mapping: retail only dispatches VISIBLE sector
		// entities (sector render + occlusion test
		// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50]); the host
		// gates on view depth + the view frustum (occlusion un-hosted — the
		// host culls strictly less than retail). The margin covers the
		// cluster's spread around its anchor: quadrant tiles snap up to 32u
		// out, plus the footprint jitter.
		if (view_depth > model_anchor_range_) {
			continue;
		}
		if (has_view && model_view_tan_half_h_ > 0.0f) {
			constexpr float MODEL_CLUSTER_MARGIN = 40.0f;
			if (Math::abs(view_pos.x) > view_depth * model_view_tan_half_h_ + MODEL_CLUSTER_MARGIN ||
			    Math::abs(view_pos.y) > view_depth * model_view_tan_half_v_ + MODEL_CLUSTER_MARGIN) {
				continue;
			}
		}
		++dispatch_stats_.model_anchors_in_range;
		const float anchor_distance =
		    has_view ? anchor.distance_to(view_xform.origin) : anchor.length();
		const float distance_units = std::floor(std::max(anchor_distance, 0.0f));
		const int alpha_ref_byte = std::clamp(
		    static_cast<int>(4096.0f / (distance_units + 1.0f)), 8, 128);

		const auto anchor_x_fixed =
		    static_cast<Fixed16_16>(std::lround(static_cast<double>(anchor.x) * 65536.0));
		const auto anchor_z_fixed =
		    static_cast<Fixed16_16>(std::lround(static_cast<double>(anchor.z) * 65536.0));

		for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
			if (!slot_active[s]) {
				continue;
			}
			draws.clear();
			model_dispatchers_[s].walk(s, anchor_x_fixed, anchor_z_fixed, view_depth,
			                           model_frame_counter_, config, samplers, draws);
			for (const auto &draw : draws) {
				++dispatch_stats_.model_tiles_emitted;
				const int64_t wind_counter = ++model_wind_counter_;
				const float wind_phase = static_cast<float>(
				    static_cast<double>(wind_counter) * 0.001);
				const uint64_t batch_key =
				    (static_cast<uint64_t>(static_cast<uint32_t>(s)) << 32) |
				    static_cast<uint64_t>(draw.tile_key);
				auto seen = frame_batches.find(batch_key);
				if (seen != frame_batches.end()) {
					// A later submission of the same tile: retail's second
					// draw overwrites the first in the framebuffer - the
					// rendered batch takes the LAST submission's anchor
					// state (alpha ref, wind phase) AND, on stagger frames,
					// its regenerated instance list (the per-anchor regen
					// replaced the cache entry the earlier borrowed view
					// pointed into - re-borrow so nothing reads the
					// reallocated buffer).
					ModelDrawBatch &existing = model_draw_batches_[seen->second];
					existing.generation = draw.generation;
					existing.instances = draw.instances;
					existing.instance_count = draw.count;
					existing.anchor = anchor;
					existing.view_depth = view_depth;
					existing.anchor_distance = anchor_distance;
					existing.alpha_ref = static_cast<float>(alpha_ref_byte);
					existing.wind_counter = wind_counter;
					existing.wind_phase = wind_phase;
					++existing.submissions;
					continue;
				}
				ModelDrawBatch batch;
				batch.slot = s;
				batch.tile_key = draw.tile_key;
				batch.generation = draw.generation;
				batch.anchor = anchor;
				batch.view_depth = view_depth;
				batch.anchor_distance = anchor_distance;
				batch.alpha_ref = static_cast<float>(alpha_ref_byte);
				batch.wind_counter = wind_counter;
				batch.wind_phase = wind_phase;
				// Borrowed from the dispatcher cache (stable until its next
				// walk) - the per-frame instance copies were the model tier's
				// dominant CPU cost at jungle-map density.
				batch.instances = draw.instances;
				batch.instance_count = draw.count;
				frame_batches.emplace(batch_key, model_draw_batches_.size());
				model_draw_batches_.push_back(std::move(batch));
			}
		}
	}

	for (const auto &batch : model_draw_batches_) {
		dispatch_stats_.model_instances += static_cast<int64_t>(batch.instance_count);
	}

	_update_model_draw_nodes();
}

void NovaFoliageDispatcher::_refresh_slot_bounds() {
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		slot_bounds_[s] = SlotModelBounds{};
		Ref<Mesh> mesh;
		if (s < slot_meshes_.size()) {
			mesh = slot_meshes_[s];
		}
		if (mesh.is_null()) {
			continue;
		}
		// The host analog of the retail def-table bounds: XZ min/max over the
		// raw 3DI verts -> center (cx, 0, cz), radius = max(half-extent X,
		// half-extent Z) (Chebyshev, not Euclidean)
		// [orig: Foliage_LoadDefAssets @ 0x601260]. The mesh AABB carries the
		// same min/max (the 3DI->Godot X negation mirrors them; extents and
		// the per-axis midpoint survive).
		const AABB aabb = mesh->get_aabb();
		SlotModelBounds bounds;
		bounds.center_x = aabb.position.x + aabb.size.x * 0.5f;
		bounds.center_z = aabb.position.z + aabb.size.z * 0.5f;
		bounds.radius = std::max(aabb.size.x, aabb.size.z) * 0.5f;
		bounds.max_y = aabb.position.y + aabb.size.y;
		if (bounds.radius <= 0.0f || bounds.max_y <= 0.0f) {
			continue;  // degenerate MODEL bounds; FAR validates surfaces independently
		}
		bounds.valid = true;
		slot_bounds_[s] = bounds;
	}
}

Ref<Mesh> NovaFoliageDispatcher::_build_far_mesh(
		int slot_index,
		const std::vector<opennova::foliage::PlacementInstance> &placements) const {
	using namespace opennova::foliage;
	if (slot_index < 0 || slot_index >= slot_meshes_.size() || placements.empty()) {
		return Ref<Mesh>();
	}
	Ref<Mesh> source_mesh = slot_meshes_[slot_index];
	if (source_mesh.is_null()) {
		return Ref<Mesh>();
	}

	NovaTerrainData *td = terrain_data_.ptr();
	HeightFn height_at = [this, td](Fixed16_16 wx, Fixed16_16 wz) -> Fixed16_16 {
		const float x = static_cast<float>(wx) * FIXED_TO_FLOAT;
		const float z = static_cast<float>(wz) * FIXED_TO_FLOAT;
		float height = INVALID_HEIGHT_THRESHOLD;
		if (td != nullptr) {
			height = td->get_height_world_bilinear(Vector3(x, 0.0f, z));
		} else if (height_sampler_.is_valid()) {
			Array args;
			args.push_back(x);
			args.push_back(z);
			height = static_cast<float>(static_cast<double>(height_sampler_.callv(args)));
		}
		if (height <= INVALID_HEIGHT_THRESHOLD) {
			// "No terrain here": the emitter drops the instance (placement.h
			// contract) instead of bending a blade to the sentinel.
			return opennova::foliage::HEIGHT_INVALID;
		}
		return static_cast<Fixed16_16>(height * FIXED_SCALE);
	};

	Ref<ArrayMesh> emitted_mesh;
	emitted_mesh.instantiate();
	for (int surface = 0; surface < source_mesh->get_surface_count(); ++surface) {
		const Array source_arrays = source_mesh->surface_get_arrays(surface);
		if (source_arrays.size() < Mesh::ARRAY_MAX) {
			continue;
		}
		const PackedVector3Array source_positions = source_arrays[Mesh::ARRAY_VERTEX];
		if (source_positions.is_empty()) {
			continue;
		}
		const PackedVector2Array source_uvs = source_arrays[Mesh::ARRAY_TEX_UV];
		const PackedInt32Array source_indices = source_arrays[Mesh::ARRAY_INDEX];

		FarSourceMesh source;
		source.vertices.reserve(static_cast<size_t>(source_positions.size()));
		for (int i = 0; i < source_positions.size(); ++i) {
			const Vector3 p = source_positions[i];
			const Vector2 uv = i < source_uvs.size() ? source_uvs[i] : Vector2();
			source.vertices.push_back({p.x, p.y, p.z, uv.x, uv.y});
		}
		if (source_indices.is_empty()) {
			source.indices.reserve(static_cast<size_t>(source_positions.size()));
			for (int i = 0; i < source_positions.size(); ++i) {
				source.indices.push_back(static_cast<uint32_t>(i));
			}
		} else {
			source.indices.reserve(static_cast<size_t>(source_indices.size()));
			for (int index : source_indices) {
				if (index < 0) {
					return Ref<Mesh>();
				}
				source.indices.push_back(static_cast<uint32_t>(index));
			}
		}

		PackedVector3Array out_positions;
		PackedVector2Array out_uvs;
		PackedColorArray out_colors;
		PackedInt32Array out_indices;
		uint32_t vertex_base = 0;
		for (size_t first = 0; first < placements.size(); first += FAR_CELL_CAP) {
			PlacementResult chunk;
			chunk.count = static_cast<int>(
			    std::min<size_t>(FAR_CELL_CAP, placements.size() - first));
			for (int i = 0; i < chunk.count; ++i) {
				chunk.instances[i] = placements[first + static_cast<size_t>(i)];
			}
			FarMesh emitted;
			if (!emit_far_mesh(source, chunk, height_at, emitted)) {
				return Ref<Mesh>();
			}
			for (const FarVertex &vertex : emitted.vertices) {
				out_positions.push_back(Vector3(vertex.x, vertex.y, vertex.z));
				out_uvs.push_back(Vector2(vertex.u, vertex.v));
				const float wind =
				    static_cast<float>((vertex.color >> 16) & 0xFFu) / 255.0f;
				out_colors.push_back(Color(wind, 0.0f, 0.0f, 0.0f));
			}
			for (uint32_t index : emitted.indices) {
				if (index > static_cast<uint32_t>(INT32_MAX) - vertex_base) {
					return Ref<Mesh>();
				}
				out_indices.push_back(static_cast<int32_t>(vertex_base + index));
			}
			vertex_base += static_cast<uint32_t>(emitted.vertices.size());
		}

		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = out_positions;
		arrays[Mesh::ARRAY_COLOR] = out_colors;
		arrays[Mesh::ARRAY_TEX_UV] = out_uvs;
		arrays[Mesh::ARRAY_INDEX] = out_indices;
		emitted_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	}
	return emitted_mesh->get_surface_count() > 0 ? Ref<Mesh>(emitted_mesh) : Ref<Mesh>();
}

Ref<Texture2D> NovaFoliageDispatcher::_slot_fd_texture(int slot_index) const {
	if (slot_index < 0 || slot_index >= slot_fd_textures_.size()) {
		return Ref<Texture2D>();
	}
	return slot_fd_textures_[slot_index];
}

void NovaFoliageDispatcher::_update_model_slot_material(int slot_index) {
	if (slot_index < 0 || slot_index >= opennova::FOLIAGE_MAX_DEFS) {
		return;
	}

	if (foliage_model_shader_.is_null()) {
		foliage_model_shader_ =
		    ResourceLoader::get_singleton()->load("res://shaders/foliage_model.gdshader", "Shader");
	}
	if (foliage_model_shader_.is_null()) {
		return;
	}

	Ref<ShaderMaterial> material = foliage_model_materials_[slot_index];
	if (material.is_null()) {
		material.instantiate();
		material->set_shader(foliage_model_shader_);
		foliage_model_materials_[slot_index] = material;
	} else if (material->get_shader() != foliage_model_shader_) {
		material->set_shader(foliage_model_shader_);
	}

	Ref<Texture2D> fd_tex = _slot_fd_texture(slot_index);
	material->set_shader_parameter("u_fd_texture", fd_tex);

	// The bound square the vertex shader re-normalizes against (pre-scale
	// Godot mesh space) [orig: Foliage_FillInstancedModelBuffers @ 0x5ffa20].
	const SlotModelBounds &bounds = slot_bounds_[slot_index];
	material->set_shader_parameter("u_bound_center", Vector2(bounds.center_x, bounds.center_z));
	material->set_shader_parameter("u_bound_radius", bounds.radius);
	// Per-draw node materials replace these defaults with the anchor-derived
	// alpha reference and the tile-draw wind phase.
	material->set_shader_parameter("u_model_alpha_ref", 8.0f);
	material->set_shader_parameter("u_model_wind_phase", 0.0f);
}

void NovaFoliageDispatcher::_clear_model_draw_nodes() {
	for (MultiMeshInstance3D *node : model_draw_nodes_) {
		if (node == nullptr) {
			continue;
		}
		if (node->get_parent() == this) {
			remove_child(node);
		}
		node->queue_free();
	}
	model_draw_nodes_.clear();
	model_draw_node_states_.clear();
	model_node_by_key_.clear();
}

// Acquire the pooled draw node for (slot, tile_key): the keyed lookup first, then
// LRU reuse of a node idle this dispatch, then a fresh node. Mirrors the retained
// ownership of the original's tile cache — a tile's GPU content survives while its
// cache entry lives, so steady-state dispatches upload NOTHING
// [orig: Foliage_UpdateModelTiles @ 0x601f50 — hits restamp and draw; only the
// 8-frame-stagger regeneration or an LRU adoption rewrites an entry].
size_t NovaFoliageDispatcher::_acquire_model_draw_node(uint64_t p_key) {
	auto found = model_node_by_key_.find(p_key);
	if (found != model_node_by_key_.end()) {
		return found->second;
	}
	// LRU among nodes not used this dispatch.
	size_t reuse = model_draw_nodes_.size();
	uint64_t oldest = UINT64_MAX;
	for (size_t i = 0; i < model_draw_node_states_.size(); ++i) {
		const ModelDrawNodeState &st = model_draw_node_states_[i];
		if (st.in_use) {
			continue;
		}
		if (st.last_used_frame < oldest) {
			oldest = st.last_used_frame;
			reuse = i;
		}
	}
	if (reuse == model_draw_nodes_.size()) {
		MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
		mmi->set_name(String("FoliageModelDraw") +
		              String::num_int64(static_cast<int64_t>(model_draw_nodes_.size())));
		mmi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		mmi->set_extra_cull_margin(8.0f);
		add_child(mmi);
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_colors(true);
		mm->set_use_custom_data(true);
		mmi->set_multimesh(mm);
		model_draw_nodes_.push_back(mmi);
		model_draw_node_states_.push_back(ModelDrawNodeState{});
	} else {
		// Steal the idle node: drop its old key mapping so the pool stays 1:1.
		const ModelDrawNodeState &st = model_draw_node_states_[reuse];
		if (st.slot >= 0) {
			model_node_by_key_.erase((static_cast<uint64_t>(static_cast<uint32_t>(st.slot)) << 32) |
			                         st.tile_key);
		}
	}
	model_node_by_key_[p_key] = reuse;
	return reuse;
}

void NovaFoliageDispatcher::_update_model_draw_nodes() {
	using opennova::foliage::model_instance_hbase;

	++model_draw_frame_;
	for (ModelDrawNodeState &st : model_draw_node_states_) {
		st.in_use = false;
	}

	for (const ModelDrawBatch &batch : model_draw_batches_) {
		const int s = batch.slot;
		if (s < 0 || s >= opennova::FOLIAGE_MAX_DEFS || batch.instance_count <= 0 ||
		    batch.instances == nullptr) {
			continue;
		}

		Ref<Mesh> slot_mesh;
		if (s < slot_meshes_.size()) {
			slot_mesh = slot_meshes_[s];
		}
		if (slot_mesh.is_null()) {
			continue;
		}

		const uint64_t key =
		    (static_cast<uint64_t>(static_cast<uint32_t>(s)) << 32) | batch.tile_key;
		const size_t node_index = _acquire_model_draw_node(key);
		MultiMeshInstance3D *mmi = model_draw_nodes_[node_index];
		ModelDrawNodeState &state = model_draw_node_states_[node_index];
		state.in_use = true;
		state.last_used_frame = model_draw_frame_;

		Ref<MultiMesh> mm = mmi->get_multimesh();
		if (mm.is_null()) {
			continue;
		}

		// Per-draw material: retail submits alpha ref + wind phase per tile
		// draw; each pooled node keeps its own material and only those two
		// parameters change on stable draws.
		if (state.slot != s || state.material.is_null()) {
			_update_model_slot_material(s);
			Ref<ShaderMaterial> base_material = foliage_model_materials_[s];
			if (base_material.is_valid()) {
				state.material = base_material->duplicate();
				mmi->set_material_override(state.material);
			}
		}
		if (state.material.is_valid()) {
			state.material->set_shader_parameter("u_model_alpha_ref", batch.alpha_ref);
			state.material->set_shader_parameter("u_model_wind_phase", batch.wind_phase);
		}

		const int count = batch.instance_count;
		// With keyed nodes, content changes only when the tile's instance list
		// actually regenerated (the witnessed 8-frame stagger / LRU adoption
		// bumps generation) or the slot mesh swapped.
		const bool content_changed = state.slot != s ||
		                             state.tile_key != batch.tile_key ||
		                             state.generation != batch.generation ||
		                             state.count != count ||
		                             mm->get_mesh() != slot_mesh;
		if (content_changed) {
			++dispatch_stats_.model_uploads;
			if (mm->get_mesh() != slot_mesh) {
				mm->set_mesh(slot_mesh);
			}
			mm->set_instance_count(count);
			const SlotModelBounds &bounds = slot_bounds_[s];
			for (int i = 0; i < count; ++i) {
				const auto &inst = batch.instances[i];
				const float hbase = model_instance_hbase(inst);
				mm->set_instance_transform(i, _model_instance_transform(inst, hbase, bounds));
				// Source-model (x,z) maps to engine fit axes (B,A), hence the
				// corner permutation c0,c2,c1,c3 and the swapped fold families.
				mm->set_instance_color(i, Color(inst.corner_height[0] - hbase,
				                                inst.corner_height[2] - hbase,
				                                inst.corner_height[1] - hbase,
				                                inst.corner_height[3] - hbase));
				mm->set_instance_custom_data(i, Color(inst.fold_e_b, inst.fold_t_b,
				                                      inst.fold_e_a, inst.fold_t_a));
			}
			state.slot = s;
			state.tile_key = batch.tile_key;
			state.generation = batch.generation;
			state.count = count;
		}
		if (!mmi->is_visible()) {
			mmi->set_visible(true);
		}
	}

	// Hide idle nodes; their content stays valid for reuse when the tile
	// scrolls back into a quadrant (no reallocation, no re-upload).
	for (size_t i = 0; i < model_draw_nodes_.size(); ++i) {
		if (!model_draw_node_states_[i].in_use) {
			MultiMeshInstance3D *node = model_draw_nodes_[i];
			if (node != nullptr && node->is_visible()) {
				node->set_visible(false);
			}
		}
	}
}

bool NovaFoliageDispatcher::_scatter_cell(int slot_index,
                                          int cell_x_int,
                                          int cell_z_int,
                                          const Ref<NovaTerrainFoliageDef> &def,
	                                      std::vector<opennova::foliage::PlacementInstance> &out_placements) {
	using opennova::foliage::Fixed16_16;

	opennova::foliage::PlacementSamplers samplers;
	samplers.path_blocked = nullptr;

	NovaTerrainData *td = terrain_data_.ptr();

	samplers.height_at = [this, td](Fixed16_16 wx, Fixed16_16 wz) -> Fixed16_16 {
		const float wx_f = static_cast<float>(wx) * opennova::foliage::FIXED_TO_FLOAT;
		const float wz_f = static_cast<float>(wz) * opennova::foliage::FIXED_TO_FLOAT;
		float y;
		if (td != nullptr) {
			y = td->get_height_world_bilinear(Vector3(wx_f, 0.0f, wz_f));
		} else if (height_sampler_.is_valid()) {
			Array args;
			args.push_back(wx_f);
			args.push_back(wz_f);
			y = static_cast<float>(static_cast<double>(height_sampler_.callv(args)));
		} else {
			return opennova::foliage::HEIGHT_INVALID;
		}
		if (y <= INVALID_HEIGHT_THRESHOLD) {
			return opennova::foliage::HEIGHT_INVALID;
		}
		return static_cast<Fixed16_16>(y * 65536.0f);
	};

	samplers.slot_mask_at = [this, td](Fixed16_16 wx, Fixed16_16 wz) -> uint32_t {
		const float wx_f = static_cast<float>(wx) * opennova::foliage::FIXED_TO_FLOAT;
		// Sign chain, measured end-to-end on 00TRg (the diag probe's
		// m_code/f cross-check): FAR cells are keyed in Godot RENDER space;
		// libs place_cell pre-negates its candidate z before this sampler
		// [orig: the (x, -z) call @ 0x600065..0x600079 into
		// Foliage_SampleFarMapMask @ 0x6066d0], so the incoming `wz` is
		// NATIVE z. get_foliage_far_mask_world negates ONCE internally to
		// feed get_foliage_index_world, whose resolve chain wants NATIVE z —
		// so this seam must hand it RENDER z (= -wz) for the two negations
		// to cancel onto the painted texel. Passing native straight through
		// (the previous form, rationalized off the parameter's misleading
		// "native_z" name) read the z-MIRRORED map: painted clusters gated
		// empty, ~2 stray instances/cell everywhere else — the 00TRg
		// sparse-coverage bug.
		const float native_z = static_cast<float>(wz) * opennova::foliage::FIXED_TO_FLOAT;
		int far_mask = 0;
		if (td != nullptr) {
			// NovaTerrainData's chain: far_mask negates once internally and
			// get_foliage_index_world's resolve wants NATIVE z — hand it
			// RENDER z (= -incoming) so the negations cancel onto the painted
			// texel (measured end-to-end on 00TRg).
			far_mask = td->get_foliage_far_mask_world(wx_f, -native_z);
		} else if (surface_sampler_.is_valid()) {
			// The editor-preview seam keeps the witnessed (x, -z) boundary:
			// the Callable receives NATIVE z; the preview's far-mask wrapper
			// un-negates it to render z, and its index sampler negates again
			// into NovaTerrainData's runtime wrap kernel — landing on the
			// SAME gate texel as the TD fast path above
			// (terrain_foliage_preview.gd _sample_far_mask /
			// _sample_foliage_index; the editor's old EditorTerrainMesh
			// chain resolved the un-negated row = the z-MIRRORED map).
			Array args;
			args.push_back(wx_f);
			args.push_back(native_z);
			far_mask = static_cast<int>(surface_sampler_.callv(args));
		}
		return static_cast<uint32_t>(far_mask) & 0xFFu;
	};

	opennova::foliage::PlacementConfig config;
	config.attrib_flags[slot_index] = static_cast<uint8_t>(def.is_valid() ? def->get_attrib_flags() : 0);

	const Fixed16_16 cell_x_fixed = static_cast<Fixed16_16>(cell_x_int) << 16;
	const Fixed16_16 cell_z_fixed = static_cast<Fixed16_16>(cell_z_int) << 16;
	const uint32_t cell_key = opennova::foliage::pack_cell_key(cell_x_fixed, cell_z_fixed);

	const opennova::foliage::PlacementResult result =
	    opennova::foliage::place_cell(slot_index, cell_key, config, samplers);

	out_placements.assign(result.instances.begin(), result.instances.begin() + result.count);

	return !out_placements.empty();
}

void NovaFoliageDispatcher::_clear_children() {
	_clear_far_cells();
	_clear_model_draw_nodes();
}

void NovaFoliageDispatcher::_update_slot_material(int slot_index) {
	if (slot_index < 0 || slot_index >= opennova::FOLIAGE_MAX_DEFS) {
		return;
	}

	if (foliage_far_shader_.is_null()) {
		foliage_far_shader_ =
		    ResourceLoader::get_singleton()->load("res://shaders/foliage_far.gdshader", "Shader");
	}
	if (foliage_far_shader_.is_null()) {
		return;
	}

	Ref<ShaderMaterial> material = foliage_materials_[slot_index];
	if (material.is_null()) {
		material.instantiate();
		material->set_shader(foliage_far_shader_);
		foliage_materials_[slot_index] = material;
	} else if (material->get_shader() != foliage_far_shader_) {
		material->set_shader(foliage_far_shader_);
	}

	Ref<Texture2D> fd_tex = _slot_fd_texture(slot_index);
	material->set_shader_parameter("u_fd_texture", fd_tex);

	// FAR T1: retail binds the per-tile terrain render target, and the tile
	// BAKE writes rgb ~= the colormap only (MODULATE2X with diffuse
	// 0x808080; NO detail splat, NO noise) with alpha = saturate(N.L)
	// [orig: PolyTrn_RenderTile @ 0x60dce5 / 0x60e38a;
	// Terrain_FindSectorTileRT @ 0x6042a0]. The host binds the colormap as
	// that stand-in; its alpha is the same fold input the host ground
	// include consumes (the exact N.L alpha rides the terrain normal-map
	// generator port, tracked with D-FOLIAGE-7).
	Ref<NovaTerrainData> cm_src = terrain_data_.is_valid() ? terrain_data_ : colormap_source_;
	Ref<Texture2D> terrain_light =
	    cm_src.is_valid() ? cm_src->get_colormap() : Ref<Texture2D>();
	material->set_shader_parameter("u_terrain_light_texture", terrain_light);

	// [orig: setup_water_vertex_shader_constants @ 0x600450] c24.x clock
	// term. The FAR shader associates the registered NovaWeather ring-head
	// global with this value before adding world X.
	const double ticks = static_cast<double>(Time::get_singleton()->get_ticks_msec());
	material->set_shader_parameter("u_far_wind_phase",
	                               static_cast<float>(ticks * 0.003));
	// c6.rgb rides the shader default (0.5 neutral): with the flat-0x808080
	// ":fd" RGB and the PS *8 that reproduces the fixed-function fallback's
	// 2*T0*diffuse level, so foliage brightness tracks the untinted terrain
	// surface (env #19: the terrain_rgb texture-bake consumer is dead code).
	// The exact per-frame c6 float chain stays on D-FOLIAGE-7.
}

} // namespace godot
