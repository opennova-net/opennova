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
	ClassDB::bind_method(D_METHOD("set_terrain_tint", "tint"), &NovaFoliageDispatcher::set_terrain_tint);
	ClassDB::bind_method(D_METHOD("get_terrain_tint"), &NovaFoliageDispatcher::get_terrain_tint);
	ClassDB::bind_method(D_METHOD("set_dispatch_algorithm", "algorithm"), &NovaFoliageDispatcher::set_dispatch_algorithm);
	ClassDB::bind_method(D_METHOD("get_dispatch_algorithm"), &NovaFoliageDispatcher::get_dispatch_algorithm);
	ClassDB::bind_method(D_METHOD("set_cell_grid_radius", "radius"), &NovaFoliageDispatcher::set_cell_grid_radius);
	ClassDB::bind_method(D_METHOD("get_cell_grid_radius"), &NovaFoliageDispatcher::get_cell_grid_radius);
	ClassDB::bind_method(D_METHOD("set_preview_cell_radius", "radius"), &NovaFoliageDispatcher::set_preview_cell_radius);
	ClassDB::bind_method(D_METHOD("get_preview_cell_radius"), &NovaFoliageDispatcher::get_preview_cell_radius);
	ClassDB::bind_method(D_METHOD("set_lru_capacity", "capacity"), &NovaFoliageDispatcher::set_lru_capacity);
	ClassDB::bind_method(D_METHOD("get_lru_capacity"), &NovaFoliageDispatcher::get_lru_capacity);
	ClassDB::bind_method(D_METHOD("set_quad_half_width", "width"), &NovaFoliageDispatcher::set_quad_half_width);
	ClassDB::bind_method(D_METHOD("get_quad_half_width"), &NovaFoliageDispatcher::get_quad_half_width);
	ClassDB::bind_method(D_METHOD("set_surface_offset", "offset"), &NovaFoliageDispatcher::set_surface_offset);
	ClassDB::bind_method(D_METHOD("get_surface_offset"), &NovaFoliageDispatcher::get_surface_offset);
	ClassDB::bind_method(D_METHOD("set_engine_view_radius_fixed", "radius"),
	                     &NovaFoliageDispatcher::set_engine_view_radius_fixed);
	ClassDB::bind_method(D_METHOD("get_engine_view_radius_fixed"),
	                     &NovaFoliageDispatcher::get_engine_view_radius_fixed);
	ClassDB::bind_method(D_METHOD("dispatch", "centre", "view_xform"),
	                     &NovaFoliageDispatcher::dispatch, DEFVAL(Transform3D()));
	ClassDB::bind_method(D_METHOD("dispatch_centers", "centers", "view_xform"),
	                     &NovaFoliageDispatcher::dispatch_centers, DEFVAL(Transform3D()));
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
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "terrain_tint"), "set_terrain_tint", "get_terrain_tint");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "colormap_source", PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainData"),
	             "set_colormap_source", "get_colormap_source");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "dispatch_algorithm", PROPERTY_HINT_ENUM,
	                          "Engine Centers,Cell Grid"),
	             "set_dispatch_algorithm", "get_dispatch_algorithm");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cell_grid_radius"), "set_cell_grid_radius", "get_cell_grid_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "preview_cell_radius"), "set_preview_cell_radius", "get_preview_cell_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lru_capacity"), "set_lru_capacity", "get_lru_capacity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "quad_half_width"), "set_quad_half_width", "get_quad_half_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "surface_offset"), "set_surface_offset", "get_surface_offset");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "engine_view_radius_fixed"),
	             "set_engine_view_radius_fixed", "get_engine_view_radius_fixed");

	BIND_CONSTANT(DISPATCH_ALGORITHM_ENGINE_CENTERS);
	BIND_CONSTANT(DISPATCH_ALGORITHM_CELL_GRID);
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
	// Texture-only swap: refresh materials, keep every placement cache.
	for (Ref<ShaderMaterial> &material : foliage_materials_) {
		material.unref();
	}
	for (Ref<ShaderMaterial> &material : foliage_model_materials_) {
		material.unref();
	}
	mm_dirty_ = true;
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

bool NovaFoliageDispatcher::bake_fd_image(const Ref<Image> &p_image) {
	// [orig: Foliage_LoadDefAssets @ 0x601260 tail] - the ":fd" bake over the
	// model's own diffuse; see libs/foliage/fd_bake.h for the kernel.
	if (p_image.is_null() || p_image->get_format() != Image::FORMAT_RGBA8) {
		return false;
	}
	const int w = p_image->get_width();
	const int h = p_image->get_height();
	PackedByteArray data = p_image->get_data();
	if (data.size() < static_cast<int64_t>(w) * h * 4) {
		return false;
	}
	if (!opennova::foliage::bake_fd_rgba(data.ptrw(), w, h)) {
		return false;
	}
	p_image->set_data(w, h, false, Image::FORMAT_RGBA8, data);
	return true;
}

void NovaFoliageDispatcher::set_height_sampler(const Callable &p_sampler) {
	height_sampler_ = p_sampler;
	// FAR vertices bake the sampled terrain height into their dynamic mesh.
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
	mm_dirty_ = true;
}

void NovaFoliageDispatcher::set_terrain_tint(const Color &p_tint) {
	if (terrain_tint_ == p_tint) {
		return;
	}
	// Compatibility surface only. The earlier terrain-color interpretation
	// was disproven by the unconditional red wind-weight write @ 0x60030A.
	terrain_tint_ = p_tint;
}

Color NovaFoliageDispatcher::get_terrain_tint() const {
	return terrain_tint_;
}

Ref<NovaTerrainData> NovaFoliageDispatcher::get_colormap_source() const { return colormap_source_; }

void NovaFoliageDispatcher::set_dispatch_algorithm(int p_algorithm) {
	const int clamped = std::clamp(p_algorithm,
	                               static_cast<int>(DISPATCH_ALGORITHM_ENGINE_CENTERS),
	                               static_cast<int>(DISPATCH_ALGORITHM_CELL_GRID));
	if (dispatch_algorithm_ == clamped) {
		return;
	}
	dispatch_algorithm_ = clamped;
	reset();
}

int NovaFoliageDispatcher::get_dispatch_algorithm() const { return dispatch_algorithm_; }

void NovaFoliageDispatcher::set_cell_grid_radius(int p_radius) {
	const int clamped = p_radius < 0 ? 0 : p_radius;
	if (cell_grid_radius_ == clamped) {
		return;
	}
	cell_grid_radius_ = clamped;
	reset();
}

int NovaFoliageDispatcher::get_cell_grid_radius() const { return cell_grid_radius_; }

void NovaFoliageDispatcher::set_preview_cell_radius(int p_radius) {
	set_cell_grid_radius(p_radius);
}

int NovaFoliageDispatcher::get_preview_cell_radius() const { return get_cell_grid_radius(); }

void NovaFoliageDispatcher::set_lru_capacity(int p_capacity) {
	lru_capacity_ = p_capacity < 1 ? 1 : p_capacity;
	_invalidate_dispatch_coverage();
}

int NovaFoliageDispatcher::get_lru_capacity() const { return lru_capacity_; }

void NovaFoliageDispatcher::set_quad_half_width(float p_width) {
	// Compatibility surface: retail FAR emits the authored mesh at XZ scale 1.
	quad_half_width_ = p_width;
}

float NovaFoliageDispatcher::get_quad_half_width() const { return quad_half_width_; }

void NovaFoliageDispatcher::set_surface_offset(float p_offset) {
	// Compatibility surface: retail anchors every source vertex directly at
	// terrain_height + source_y*0.5, with no extra lift.
	surface_offset_ = p_offset;
}

float NovaFoliageDispatcher::get_surface_offset() const { return surface_offset_; }

void NovaFoliageDispatcher::set_engine_view_radius_fixed(int p_radius) {
	engine_view_radius_fixed_ = p_radius < 0 ? 0 : p_radius;
	reset();
}

int NovaFoliageDispatcher::get_engine_view_radius_fixed() const { return engine_view_radius_fixed_; }

bool NovaFoliageDispatcher::_has_sampling_source() const {
	return terrain_data_.is_valid() ||
	       (height_sampler_.is_valid() &&
	        (foliage_sampler_.is_valid() || surface_sampler_.is_valid()));
}

void NovaFoliageDispatcher::reset() {
	lru_.clear();
	touch_counter_ = 0;
	dispatch_stats_ = DispatchStats{};
	_invalidate_dispatch_coverage();
	engine_frame_counter_ = 0;
	render_algorithm_ = dispatch_algorithm_;
	engine_center_states_.clear();
	for (auto &slot_placements : engine_placements_) {
		slot_placements.clear();
	}
	model_frame_counter_ = 0;
	model_wind_counter_ = 0;
	for (auto &dispatcher : model_dispatchers_) {
		dispatcher.reset();
	}
	for (auto &slot_instances : model_instances_) {
		slot_instances.clear();
	}
	model_draw_batches_.clear();
	mm_dirty_ = true;
	_clear_children();
}

int NovaFoliageDispatcher::get_cached_cells() const {
	if (render_algorithm_ == DISPATCH_ALGORITHM_ENGINE_CENTERS) {
		int total = 0;
		for (const auto &entry : engine_center_states_) {
			total += entry.second.cached_cells();
		}
		return total;
	}
	return static_cast<int>(lru_.size());
}

int NovaFoliageDispatcher::get_total_instances() const {
	if (render_algorithm_ == DISPATCH_ALGORITHM_ENGINE_CENTERS) {
		int total = 0;
		for (const auto &slot_instances : engine_placements_) {
			total += static_cast<int>(slot_instances.size());
		}
		return total;
	}

	int total = 0;
	for (const auto &kv : lru_) {
		total += static_cast<int>(kv.second.placements.size());
	}
	return total;
}

Dictionary NovaFoliageDispatcher::get_dispatch_stats() const {
	Dictionary out;
	out["dispatch_calls"] = dispatch_stats_.dispatch_calls;
	out["coverage_skips"] = dispatch_stats_.coverage_skips;
	out["rebuilt_slots"] = dispatch_stats_.rebuilt_slots;
	out["instance_uploads"] = dispatch_stats_.instance_uploads;
	out["cell_cache_hits"] = dispatch_stats_.cell_cache_hits;
	out["cell_cache_misses"] = dispatch_stats_.cell_cache_misses;
	out["cached_cells"] = get_cached_cells();
	out["total_instances"] = get_total_instances();
	// Model tier.
	out["model_anchors_in_range"] = dispatch_stats_.model_anchors_in_range;
	out["model_tiles_emitted"] = dispatch_stats_.model_tiles_emitted;
	out["model_instances"] = dispatch_stats_.model_instances;
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
	for (const auto &inst : model_instances_[p_slot]) {
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
		d["anchor"] = batch.anchor;
		d["view_depth"] = batch.view_depth;
		d["anchor_distance"] = batch.anchor_distance;
		d["alpha_ref"] = batch.alpha_ref;
		d["wind_counter"] = batch.wind_counter;
		d["wind_phase"] = batch.wind_phase;
		d["instance_count"] = static_cast<int64_t>(batch.instances.size());
		out.push_back(d);
	}
	return out;
}

Array NovaFoliageDispatcher::get_far_tile_debug(int p_slot) const {
	Array out;
	if (p_slot < 0 || p_slot >= opennova::FOLIAGE_MAX_DEFS) {
		return out;
	}
	auto emit = [&out](
	                const std::vector<opennova::foliage::PlacementInstance> &placements) {
		for (const auto &placement : placements) {
			Dictionary d;
			d["center"] = Vector3(
			    static_cast<float>(placement.world_x_fixed) * opennova::foliage::FIXED_TO_FLOAT,
			    0.0f,
			    static_cast<float>(placement.world_z_fixed) * opennova::foliage::FIXED_TO_FLOAT);
			d["yaw"] = placement.rotation_radians;
			out.push_back(d);
		}
	};
	if (render_algorithm_ == DISPATCH_ALGORITHM_ENGINE_CENTERS) {
		emit(engine_placements_[p_slot]);
	} else {
		for (const auto &kv : lru_) {
			if (kv.first.slot == p_slot) {
				emit(kv.second.placements);
			}
		}
	}
	return out;
}

void NovaFoliageDispatcher::_invalidate_dispatch_coverage() {
	last_cell_grid_base_valid_ = false;
	last_cell_grid_base_x_ = 0;
	last_cell_grid_base_z_ = 0;
}

void NovaFoliageDispatcher::dispatch(Vector3 centre, Transform3D view_xform) {
	++dispatch_stats_.dispatch_calls;
	if (foliage_defs_.is_empty()) {
		return;
	}

	// FAR coverage skip: unchanged 16u-cell check. The model tier still
	// ticks below (its stagger/cache is frame-driven and its anchors move
	// independently of the FAR coverage cell).
	bool far_coverage_current = false;
	if (dispatch_algorithm_ == DISPATCH_ALGORITHM_CELL_GRID && terrain_data_.is_valid() &&
	    render_algorithm_ == DISPATCH_ALGORITHM_CELL_GRID && !mm_dirty_) {
		const int base_x = static_cast<int>(std::floor(centre.x / 16.0f) * 16.0f);
		const int base_z = static_cast<int>(std::floor(centre.z / 16.0f) * 16.0f + 16.0f);
		if (last_cell_grid_base_valid_ &&
		    last_cell_grid_base_x_ == base_x &&
		    last_cell_grid_base_z_ == base_z) {
			++dispatch_stats_.coverage_skips;
			far_coverage_current = true;
		}
	}

	Dictionary defs_by_match = _build_defs_by_match();
	if (!_has_sampling_source()) {
		return;
	}

	if (!far_coverage_current) {
		if (dispatch_algorithm_ == DISPATCH_ALGORITHM_CELL_GRID) {
			_dispatch_cell_grid(centre);
		} else {
			PackedVector3Array centers;
			centers.push_back(centre);
			_dispatch_engine_centers(centers, view_xform);
		}
	}

	// FAR geometry is static until placement coverage changes, but the retail
	// wind phase advances every draw. Refresh the four small material parameter
	// sets even when CELL_GRID legitimately skips a mesh rebuild.
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		if (mm_by_slot_[s] != nullptr) {
			_update_slot_material(s);
		}
	}

	_dispatch_model_tier(view_xform, defs_by_match);
}

void NovaFoliageDispatcher::dispatch_centers(PackedVector3Array centers, Transform3D view_xform) {
	++dispatch_stats_.dispatch_calls;
	if (foliage_defs_.is_empty() || centers.is_empty()) {
		return;
	}

	Dictionary defs_by_match = _build_defs_by_match();
	if (!_has_sampling_source()) {
		return;
	}

	_dispatch_engine_centers(centers, view_xform);
	_dispatch_model_tier(view_xform, defs_by_match);
}

void NovaFoliageDispatcher::_dispatch_cell_grid(Vector3 centre) {
	render_algorithm_ = DISPATCH_ALGORITHM_CELL_GRID;
	++touch_counter_;

	// CELL_GRID algorithm: scan a wider 16u cell grid around the supplied center.
	const float base_x = std::floor(centre.x / 16.0f) * 16.0f;
	const float base_z = std::floor(centre.z / 16.0f) * 16.0f + 16.0f;
	last_cell_grid_base_valid_ = true;
	last_cell_grid_base_x_ = static_cast<int>(base_x);
	last_cell_grid_base_z_ = static_cast<int>(base_z);

	const int num_slots = foliage_defs_.size();
	for (int slot_index = 0; slot_index < num_slots && slot_index < opennova::FOLIAGE_MAX_DEFS; ++slot_index) {
		Ref<NovaTerrainFoliageDef> def = foliage_defs_[slot_index];
		if (def.is_null()) {
			continue;
		}
		for (int dz = -cell_grid_radius_; dz <= cell_grid_radius_; ++dz) {
			for (int dx = -cell_grid_radius_; dx <= cell_grid_radius_; ++dx) {
				const int cell_x = static_cast<int>(base_x) + dx * 16;
				const int cell_z = static_cast<int>(base_z) + dz * 16;

				CellKey key{cell_x, cell_z, slot_index};
				auto it = lru_.find(key);
				if (it != lru_.end()) {
					it->second.touch = touch_counter_;
					++dispatch_stats_.cell_cache_hits;
					continue;
				}
				++dispatch_stats_.cell_cache_misses;

				std::vector<opennova::foliage::PlacementInstance> placements;
				const bool any =
				    _scatter_cell(slot_index, cell_x, cell_z, def, placements);
				if (!any) {
					continue;
				}
				LRUEntry entry;
				entry.placements = std::move(placements);
				entry.touch = touch_counter_;
				lru_.emplace(key, std::move(entry));
				mm_dirty_ = true;
			}
		}
	}

	const int num_slots_used = foliage_defs_.size();
	const int grid_side = 2 * cell_grid_radius_ + 1;
	const int required_capacity = grid_side * grid_side * num_slots_used * 2;
	const int effective_capacity = std::max(lru_capacity_, required_capacity);

	while (static_cast<int>(lru_.size()) > effective_capacity) {
		auto oldest = lru_.begin();
		for (auto it = lru_.begin(); it != lru_.end(); ++it) {
			if (it->second.touch < oldest->second.touch) {
				oldest = it;
			}
		}
		lru_.erase(oldest);
		mm_dirty_ = true;
	}

	if (mm_dirty_) {
		_rebuild_multimeshes();
		mm_dirty_ = false;
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

void NovaFoliageDispatcher::_dispatch_engine_centers(const PackedVector3Array &centers,
                                                     const Transform3D &view_xform) {
	render_algorithm_ = DISPATCH_ALGORITHM_ENGINE_CENTERS;
	NovaTerrainData *td = terrain_data_.ptr();
	if (td == nullptr && (!height_sampler_.is_valid() || !surface_sampler_.is_valid())) {
		return;
	}

	for (auto &slot_placements : engine_placements_) {
		slot_placements.clear();
	}

	opennova::foliage::PlacementConfig config;
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		if (s < foliage_defs_.size()) {
			Ref<NovaTerrainFoliageDef> def = foliage_defs_[s];
			if (def.is_valid()) {
				config.attrib_flags[s] = static_cast<uint8_t>(def->get_attrib_flags());
			}
		}
	}

	opennova::foliage::PlacementSamplers samplers;
	samplers.path_blocked = nullptr;  // sub_5C6450 remains deferred until the ambient-source registry exists.
	samplers.height_at = [this, td](opennova::foliage::Fixed16_16 wx,
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
		return static_cast<opennova::foliage::Fixed16_16>(y * 65536.0f);
	};
	samplers.slot_mask_at = [this, td](opennova::foliage::Fixed16_16 wx,
	                                  opennova::foliage::Fixed16_16 wz) -> uint32_t {
		const float wx_f = static_cast<float>(wx) * opennova::foliage::FIXED_TO_FLOAT;
		// place_cell() already supplies the witnessed Terrain_GetSurfaceTypeAtFixedPoint
		// boundary coordinate (world X, -world Z).
		const float native_z =
		    static_cast<float>(wz) * opennova::foliage::FIXED_TO_FLOAT;
		int surface_mask = 0;
		if (td != nullptr) {
			surface_mask = td->get_surface_mask_world(wx_f, native_z);
		} else if (surface_sampler_.is_valid()) {
			Array args;
			args.push_back(wx_f);
			args.push_back(native_z);
			surface_mask = static_cast<int>(surface_sampler_.callv(args));
		}
		return static_cast<uint32_t>(surface_mask) & 0xFFu;
	};

	++engine_frame_counter_;

	auto center_key_for = [](const Vector3 &center) {
		return EngineCenterKey{
		    static_cast<opennova::foliage::Fixed16_16>(
		        std::lround(static_cast<double>(center.x) * 65536.0)),
		    static_cast<opennova::foliage::Fixed16_16>(
		        std::lround(static_cast<double>(center.z) * 65536.0))};
	};

	// Retain the first 256 unique centers in caller order, matching
	// NovaTerrain's production patch-pool bound. Overflow centers still get
	// independent transient state for this dispatch, preserving output parity
	// without allowing persistent cache growth.
	std::vector<EngineCenterKey> retained_keys;
	retained_keys.reserve(std::min(static_cast<size_t>(centers.size()),
	                              ENGINE_CENTER_CACHE_CAPACITY));
	for (int center_index = 0; center_index < centers.size(); ++center_index) {
		const EngineCenterKey key = center_key_for(centers[center_index]);
		if (std::find(retained_keys.begin(), retained_keys.end(), key) == retained_keys.end() &&
		    retained_keys.size() < ENGINE_CENTER_CACHE_CAPACITY) {
			retained_keys.push_back(key);
		}
	}
	auto retains_key = [&retained_keys](const EngineCenterKey &key) {
		return std::find(retained_keys.begin(), retained_keys.end(), key) != retained_keys.end();
	};
	for (auto it = engine_center_states_.begin(); it != engine_center_states_.end();) {
		if (!retains_key(it->first)) {
			it = engine_center_states_.erase(it);
		} else {
			++it;
		}
	}

	for (int center_index = 0; center_index < centers.size(); ++center_index) {
		const Vector3 centre = centers[center_index];
		const EngineCenterKey center_key = center_key_for(centre);
		const auto centre_x_fixed = center_key.x;
		const auto centre_z_fixed = center_key.z;

		EngineCenterState *center_state = nullptr;
		std::unique_ptr<EngineCenterState> transient_state;
		if (retains_key(center_key)) {
			auto [it, inserted] = engine_center_states_.try_emplace(center_key);
			(void)inserted;
			center_state = &it->second;
		} else {
			transient_state = std::make_unique<EngineCenterState>();
			center_state = transient_state.get();
		}

		// Engine near-plane reject. sub_5C1940 @ 0x5c19a3 calls
		// Math_TransformPoint(view_matrix, center, ...) and rejects the whole
		// dispatch when transformed Z >= 38.0. Godot cameras look down -Z, so
		// camera-local Z is negated into the engine's +Z-forward convention.
		float engine_view_camera_z = opennova::foliage::DISPATCHER_NEAR_Z;
		if (!(view_xform == Transform3D())) {
			const Vector3 view_local = view_xform.affine_inverse().xform(centre);
			engine_view_camera_z = -view_local.z;
		}

		for (int slot_index = 0;
		     slot_index < foliage_defs_.size() && slot_index < opennova::FOLIAGE_MAX_DEFS;
		     ++slot_index) {
			Ref<NovaTerrainFoliageDef> def = foliage_defs_[slot_index];
			if (def.is_null()) {
				continue;
			}

			std::vector<opennova::foliage::ZSortInstance> zsort;
			center_state->dispatcher_for_slot(slot_index).dispatch(
			    slot_index,
			    centre_x_fixed,
			    centre_z_fixed,
			    engine_view_camera_z,
			    engine_view_radius_fixed_,
			    engine_frame_counter_,
			    config,
			    samplers,
			    zsort);
			auto &slot_placements = engine_placements_[slot_index];
			slot_placements.reserve(slot_placements.size() + zsort.size());
			for (const auto &z_instance : zsort) {
				slot_placements.push_back(z_instance.instance);
			}
		}
	}

	_rebuild_multimeshes();
	mm_dirty_ = false;
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
		return static_cast<opennova::foliage::Fixed16_16>(y * 65536.0f);
	};

	// The model tier's witnessed gate is the FOLIAGEMAP byte
	// [orig: Foliage_SampleFoliageMapMask @ 0x606620]; the host analog is the
	// foliage-map index -> def-slot mask chain. Dictionary captured by value
	// (COW ref) so the samplers
	// outlive the caller's local.
	out_samplers.slot_mask_at = [this, td, defs_by_match](opennova::foliage::Fixed16_16 wx,
	                                                      opennova::foliage::Fixed16_16 wz) -> uint32_t {
		const float wx_f = static_cast<float>(wx) * opennova::foliage::FIXED_TO_FLOAT;
		const float wz_f = static_cast<float>(wz) * opennova::foliage::FIXED_TO_FLOAT;
		int painted = 0;
		if (td != nullptr) {
			painted = td->get_foliage_index_world(wx_f, wz_f);
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

	bool had_instances = false;
	for (auto &slot_instances : model_instances_) {
		had_instances = had_instances || !slot_instances.empty();
		slot_instances.clear();
	}
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
		if (had_instances) {
			_rebuild_model_multimeshes();
		}
		return;
	}

	PlacementSamplers samplers;
	_make_model_sampler_bindings(defs_by_match, samplers);

	++model_frame_counter_;

	const bool has_view = !(view_xform == Transform3D());
	const Transform3D view_inv = has_view ? view_xform.affine_inverse() : Transform3D();

	std::vector<ModelTileDraw> draws;

	for (int a = 0; a < model_anchors_.size(); ++a) {
		const Vector3 anchor = model_anchors_[a];

		// Per-anchor view depth, engine +Z-forward convention (Godot cameras
		// look down -Z) - the same mapping the quad ENGINE_CENTERS path uses.
		// Identity view transform passes the gate (headless/tests).
		float view_depth = MODEL_DEPTH_GATE;
		if (has_view) {
			view_depth = -view_inv.xform(anchor).z;
		}
		if (view_depth < MODEL_DEPTH_GATE) {
			continue;  // the witnessed >= 38.0 gate (also enforced in the libs walk)
		}
		// Host visibility mapping: retail only dispatches VISIBLE sector
		// entities (sector render + occlusion test
		// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50]); the host
		// approximates with a view-depth range gate.
		if (view_depth > model_anchor_range_) {
			continue;
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
				ModelDrawBatch batch;
				batch.slot = s;
				batch.tile_key = draw.tile_key;
				batch.anchor = anchor;
				batch.view_depth = view_depth;
				batch.anchor_distance = anchor_distance;
				batch.alpha_ref = static_cast<float>(alpha_ref_byte);
				batch.wind_counter = ++model_wind_counter_;
				batch.wind_phase = static_cast<float>(
				    static_cast<double>(batch.wind_counter) * 0.001);
				batch.instances.reserve(static_cast<size_t>(draw.result.count));
				auto &slot_instances = model_instances_[s];
				for (int i = 0; i < draw.result.count; ++i) {
					const ModelInstance &instance = draw.result.instances[i];
					batch.instances.push_back(instance);
					slot_instances.push_back(instance);
				}
				model_draw_batches_.push_back(std::move(batch));
			}
		}
	}

	for (const auto &slot_instances : model_instances_) {
		dispatch_stats_.model_instances += static_cast<int64_t>(slot_instances.size());
	}

	_rebuild_model_multimeshes();
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
	// Per-draw clones replace these defaults with the anchor-derived alpha
	// reference and the tile-draw wind phase.
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
}

void NovaFoliageDispatcher::_rebuild_model_multimeshes() {
	using opennova::foliage::model_instance_hbase;

	size_t node_index = 0;
	int slot_draw_index[opennova::FOLIAGE_MAX_DEFS] = {};
	bool slot_material_ready[opennova::FOLIAGE_MAX_DEFS] = {};

	for (const ModelDrawBatch &batch : model_draw_batches_) {
		const int s = batch.slot;
		if (s < 0 || s >= opennova::FOLIAGE_MAX_DEFS || batch.instances.empty()) {
			continue;
		}

		Ref<Mesh> slot_mesh;
		if (s < slot_meshes_.size()) {
			slot_mesh = slot_meshes_[s];
		}
		if (slot_mesh.is_null()) {
			continue;
		}

		if (!slot_material_ready[s]) {
			_update_model_slot_material(s);
			slot_material_ready[s] = true;
		}

		MultiMeshInstance3D *mmi = nullptr;
		Ref<MultiMesh> mm;
		if (node_index < model_draw_nodes_.size()) {
			mmi = model_draw_nodes_[node_index];
			mm = mmi->get_multimesh();
		} else {
			mmi = memnew(MultiMeshInstance3D);
			mmi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			mmi->set_extra_cull_margin(8.0f);
			add_child(mmi);
			model_draw_nodes_.push_back(mmi);
		}
		++node_index;

		const int draw_index = slot_draw_index[s]++;
		String node_name = String("FoliageModelSlot") + String::num_int64(s);
		if (draw_index > 0) {
			node_name += String("Draw") + String::num_int64(draw_index);
		}
		mmi->set_name(node_name);

		if (mm.is_null()) {
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_use_colors(true);
			mm->set_use_custom_data(true);
			mmi->set_multimesh(mm);
		}
		if (mm->get_mesh() != slot_mesh) {
			mm->set_mesh(slot_mesh);
		}

		Ref<ShaderMaterial> base_material = foliage_model_materials_[s];
		if (base_material.is_valid()) {
			Ref<ShaderMaterial> material = base_material->duplicate();
			if (material.is_valid()) {
				material->set_shader_parameter("u_model_alpha_ref", batch.alpha_ref);
				material->set_shader_parameter("u_model_wind_phase", batch.wind_phase);
				mmi->set_material_override(material);
			}
		}

		const int count = static_cast<int>(batch.instances.size());
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
	}

	// Keep the live prefix as a stable node pool. Retire only surplus draws;
	// stable dispatches update their existing MultiMeshes in place instead of
	// allocating and orphaning a full batch set every frame.
	for (size_t i = model_draw_nodes_.size(); i > node_index; --i) {
		MultiMeshInstance3D *node = model_draw_nodes_[i - 1];
		if (node != nullptr) {
			if (node->get_parent() == this) {
				remove_child(node);
			}
			node->queue_free();
		}
	}
	model_draw_nodes_.resize(node_index);
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
			return static_cast<Fixed16_16>(INVALID_HEIGHT_THRESHOLD * 65536.0f);
		}
		return static_cast<Fixed16_16>(y * 65536.0f);
	};

	samplers.slot_mask_at = [this, td](Fixed16_16 wx, Fixed16_16 wz) -> uint32_t {
		const float wx_f = static_cast<float>(wx) * opennova::foliage::FIXED_TO_FLOAT;
		// place_cell() already supplies the witnessed Terrain_GetSurfaceTypeAtFixedPoint
		// boundary coordinate (world X, -world Z).
		const float native_z =
		    static_cast<float>(wz) * opennova::foliage::FIXED_TO_FLOAT;
		int surface_mask = 0;
		if (td != nullptr) {
			surface_mask = td->get_surface_mask_world(wx_f, native_z);
		} else if (surface_sampler_.is_valid()) {
			Array args;
			args.push_back(wx_f);
			args.push_back(native_z);
			surface_mask = static_cast<int>(surface_sampler_.callv(args));
		}
		return static_cast<uint32_t>(surface_mask) & 0xFFu;
	};

	opennova::foliage::PlacementConfig config;
	config.attrib_flags[slot_index] = static_cast<uint8_t>(def.is_valid() ? def->get_attrib_flags() : 0);

	const Fixed16_16 cell_x_fixed = static_cast<Fixed16_16>(cell_x_int) << 16;
	const Fixed16_16 cell_z_fixed = static_cast<Fixed16_16>(cell_z_int) << 16;
	const uint32_t cell_key = opennova::foliage::pack_cell_key(cell_x_fixed, cell_z_fixed);
	constexpr int32_t HUGE_RADIUS = 0x40000000;

	const opennova::foliage::PlacementResult result =
	    opennova::foliage::place_cell(slot_index,
	                                  cell_key,
	                                  cell_x_fixed + (1 << 19),
	                                  cell_z_fixed - (1 << 19),
	                                  HUGE_RADIUS,
	                                  config,
	                                  samplers);

	out_placements.assign(result.instances.begin(), result.instances.begin() + result.count);

	return !out_placements.empty();
}

void NovaFoliageDispatcher::_retire_far_node(int slot_index) {
	if (slot_index < 0 || slot_index >= opennova::FOLIAGE_MAX_DEFS) {
		return;
	}
	MultiMeshInstance3D *node = mm_by_slot_[slot_index];
	if (node == nullptr) {
		return;
	}
	if (Node *parent = node->get_parent()) {
		parent->remove_child(node);
	}
	node->queue_free();
	mm_by_slot_[slot_index] = nullptr;
}

void NovaFoliageDispatcher::_clear_children() {
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		_retire_far_node(s);
	}
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

	// FAR T1: the terrain light/colormap texture projected from world XZ.
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
	// c6 is per-sector state; neutral RGB/full fade is explicit until the
	// terrain renderer exposes that draw input (D-FOLIAGE-7).
	material->set_shader_parameter("u_far_pass_color", Color(1.0f, 1.0f, 1.0f, 1.0f));
	// Explicit slot-wide host approximation: retail selects per visible sector
	// at 33u (with a caller force-low override), and its high path additionally
	// submits an alpha-ref-8 wireframe pass. This combined batch has neither
	// boundary, so do not claim selector parity here.
	material->set_shader_parameter("u_far_alpha_ref", 180.0f);

	if (mm_by_slot_[slot_index] != nullptr) {
		mm_by_slot_[slot_index]->set_material_override(material);
	}
}

void NovaFoliageDispatcher::_rebuild_multimeshes() {
	std::vector<opennova::foliage::PlacementInstance>
	    per_slot[opennova::FOLIAGE_MAX_DEFS];

	if (render_algorithm_ == DISPATCH_ALGORITHM_ENGINE_CENTERS) {
		for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
			per_slot[s] = engine_placements_[s];
		}
	} else {
		for (const auto &kv : lru_) {
			const int s = kv.first.slot;
			if (s < 0 || s >= opennova::FOLIAGE_MAX_DEFS) {
				continue;
			}
			const auto &entry = kv.second;
			per_slot[s].insert(per_slot[s].end(),
			                   entry.placements.begin(), entry.placements.end());
		}
	}

	// FAR streams a terrain-bent copy of every source-mesh surface for every
	// accepted placement. MODEL uses the same source mesh through its separate
	// per-tile-draw MultiMesh path.
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		const int count = static_cast<int>(per_slot[s].size());
		if (count == 0) {
			_retire_far_node(s);
			continue;
		}
		++dispatch_stats_.rebuilt_slots;
		dispatch_stats_.instance_uploads += count;

		Ref<Mesh> slot_mesh = _build_far_mesh(s, per_slot[s]);
		if (slot_mesh.is_null()) {
			_retire_far_node(s);
			continue;
		}
		if (mm_by_slot_[s] == nullptr) {
			MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
			mmi->set_name(String("FoliageSlot") + String::num_int64(s));
			add_child(mmi);
			mmi->set_as_top_level(true);
			mm_by_slot_[s] = mmi;
		}

		// Retail streams a dynamic vertex/index buffer. The one-instance
		// MultiMesh wrapper preserves the public node shape; its Mesh is the
		// replicated, terrain-anchored buffer and carries vertex COLOR.r.
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(slot_mesh);
		mm->set_instance_count(1);
		mm->set_instance_transform(0, Transform3D());
		mm_by_slot_[s]->set_multimesh(mm);

		// SHADOW attribute bit (Jointops.exe FoliageDef +532 bit 1, engine_spec_foliage §3.1).
		// The bit is a shadow-render opt-in; when set the slot's instances cast shadows,
		// otherwise we disable the pass entirely. Applied every rebuild so def edits
		// propagate without forcing a full scene reload.
		_update_slot_material(s);

		int shadow_attrib = 0;
		if (s < foliage_defs_.size()) {
			Ref<NovaTerrainFoliageDef> def = foliage_defs_[s];
			if (def.is_valid()) {
				shadow_attrib = def->get_attrib_flags();
			}
		}
		mm_by_slot_[s]->set_cast_shadows_setting(
		    (shadow_attrib & opennova::FOLIAGE_ATTRIB_SHADOW)
		        ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON
		        : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);

	}
}

} // namespace godot
