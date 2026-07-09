#include "nova_foliage_dispatcher.h"

#include "nova_terrain_data.h"

#include <foliage/fd_bake.h>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace godot {

namespace {

constexpr float INVALID_HEIGHT_THRESHOLD = -1.0e6f;

// The witnessed model render-height scale: the static VB halves the model Y
// (pos.y = y * 0.5) [orig: Foliage_FillInstancedModelBuffers @ 0x5ffa20].
// The XZ counterpart is opennova::foliage::MODEL_FOOTPRINT_SCALE (0.75).
constexpr float MODEL_RENDER_HEIGHT_SCALE = 0.5f;

// Both tiers derive their ground color IN-SHADER from the tinted colormap
// sample (the quad-emitter half-plus-bias form; foliage_model.gdshader) -
// the per-pixel form supersedes the old CPU per-instance average and sits
// closer to the witnessed per-VERTEX quad colors; the exact emitter form
// remains the D-FOLIAGE-1 render-emitter parity leg.

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
	ClassDB::bind_method(D_METHOD("get_far_tile_debug", "slot"),
	                     &NovaFoliageDispatcher::get_far_tile_debug);
	ClassDB::bind_method(D_METHOD("set_height_sampler", "sampler"), &NovaFoliageDispatcher::set_height_sampler);
	ClassDB::bind_method(D_METHOD("get_height_sampler"), &NovaFoliageDispatcher::get_height_sampler);
	ClassDB::bind_method(D_METHOD("set_foliage_sampler", "sampler"), &NovaFoliageDispatcher::set_foliage_sampler);
	ClassDB::bind_method(D_METHOD("get_foliage_sampler"), &NovaFoliageDispatcher::get_foliage_sampler);
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
	mm_dirty_ = true;
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
	_invalidate_dispatch_coverage();
}
Callable NovaFoliageDispatcher::get_height_sampler() const { return height_sampler_; }

void NovaFoliageDispatcher::set_foliage_sampler(const Callable &p_sampler) {
	foliage_sampler_ = p_sampler;
	reset();
}

Callable NovaFoliageDispatcher::get_foliage_sampler() const { return foliage_sampler_; }

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
	// Tints are baked into the cached instance colors at scatter time, so refresh.
	reset();
}

void NovaFoliageDispatcher::set_terrain_tint(const Color &p_tint) {
	if (terrain_tint_ == p_tint) {
		return;
	}
	// The env terrain_rgb tint, derived once like the terrain-init global
	// [orig: PolyTrn_SetTerrainTintColors @ 0x605e20 <- Terrain_Init
	// @ 0x60fc42]. Both tiers apply the FULL form in-shader
	// (min(texel * tint * 255/128, 1) - the u_terrain_tint uniform).
	terrain_tint_ = p_tint;
	reset();
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
	quad_half_width_ = p_width;
	far_patch_mesh_.unref();  // rebuilt lazily at the next rebuild
	reset();
}

float NovaFoliageDispatcher::get_quad_half_width() const { return quad_half_width_; }

void NovaFoliageDispatcher::set_surface_offset(float p_offset) {
	surface_offset_ = p_offset;
	reset();
}

float NovaFoliageDispatcher::get_surface_offset() const { return surface_offset_; }

void NovaFoliageDispatcher::set_engine_view_radius_fixed(int p_radius) {
	engine_view_radius_fixed_ = p_radius < 0 ? 0 : p_radius;
	reset();
}

int NovaFoliageDispatcher::get_engine_view_radius_fixed() const { return engine_view_radius_fixed_; }

bool NovaFoliageDispatcher::_has_sampling_source() const {
	return terrain_data_.is_valid() || (height_sampler_.is_valid() && foliage_sampler_.is_valid());
}

void NovaFoliageDispatcher::reset() {
	lru_.clear();
	touch_counter_ = 0;
	dispatch_stats_ = DispatchStats{};
	_invalidate_dispatch_coverage();
	engine_frame_counter_ = 0;
	render_algorithm_ = dispatch_algorithm_;
	for (auto &dispatcher : engine_dispatchers_) {
		dispatcher.reset();
	}
	for (auto &slot_transforms : engine_transforms_) {
		slot_transforms.clear();
	}
	for (auto &slot_colors : engine_colors_) {
		slot_colors.clear();
	}
	for (auto &slot_customs : engine_customs_) {
		slot_customs.clear();
	}
	model_frame_counter_ = 0;
	model_wind_counter_ = 0;
	for (auto &dispatcher : model_dispatchers_) {
		dispatcher.reset();
	}
	for (auto &slot_instances : model_instances_) {
		slot_instances.clear();
	}
	mm_dirty_ = true;
	_clear_children();
}

int NovaFoliageDispatcher::get_cached_cells() const {
	if (render_algorithm_ == DISPATCH_ALGORITHM_ENGINE_CENTERS) {
		int total = 0;
		for (const auto &dispatcher : engine_dispatchers_) {
			total += dispatcher.lru_occupancy();
		}
		return total;
	}
	return static_cast<int>(lru_.size());
}

int NovaFoliageDispatcher::get_total_instances() const {
	if (render_algorithm_ == DISPATCH_ALGORITHM_ENGINE_CENTERS) {
		int total = 0;
		for (const auto &slot_instances : engine_transforms_) {
			total += static_cast<int>(slot_instances.size());
		}
		return total;
	}

	int total = 0;
	for (const auto &kv : lru_) {
		total += static_cast<int>(kv.second.transforms.size());
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
		d["color"] = Color(inst.corner_height[1] - hbase, inst.corner_height[0] - hbase,
		                   inst.corner_height[3] - hbase, inst.corner_height[2] - hbase);
		d["custom"] = Color(inst.fold_e_a, -inst.fold_t_a, inst.fold_e_b, inst.fold_t_b);
		d["bound_center"] = Vector2(bounds.center_x, bounds.center_z);
		d["bound_radius"] = bounds.radius;
		out.push_back(d);
	}
	return out;
}

Array NovaFoliageDispatcher::get_far_tile_debug(int p_slot) const {
	Array out;
	if (p_slot < 0 || p_slot >= opennova::FOLIAGE_MAX_DEFS) {
		return out;
	}
	auto emit = [&out](const std::vector<Transform3D> &transforms,
	                   const std::vector<Color> &colors,
	                   const std::vector<Color> &customs) {
		const size_t n = transforms.size();
		if (colors.size() != n || customs.size() != n) {
			return;
		}
		for (size_t i = 0; i < n; ++i) {
			Dictionary d;
			d["transform"] = transforms[i];
			d["color"] = colors[i];
			d["custom"] = customs[i];
			out.push_back(d);
		}
	};
	if (render_algorithm_ == DISPATCH_ALGORITHM_ENGINE_CENTERS) {
		emit(engine_transforms_[p_slot], engine_colors_[p_slot], engine_customs_[p_slot]);
	} else {
		for (const auto &kv : lru_) {
			if (kv.first.slot == p_slot) {
				emit(kv.second.transforms, kv.second.colors, kv.second.customs);
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

	// Quad-tier coverage skip: unchanged 16u-cell check. The model tier still
	// ticks below (its stagger/cache is frame-driven and its anchors move
	// independently of the quad coverage cell).
	bool quad_coverage_current = false;
	if (dispatch_algorithm_ == DISPATCH_ALGORITHM_CELL_GRID && terrain_data_.is_valid() &&
	    render_algorithm_ == DISPATCH_ALGORITHM_CELL_GRID && !mm_dirty_) {
		const int base_x = static_cast<int>(std::floor(centre.x / 16.0f) * 16.0f);
		const int base_z = static_cast<int>(std::floor(centre.z / 16.0f) * 16.0f + 16.0f);
		if (last_cell_grid_base_valid_ &&
		    last_cell_grid_base_x_ == base_x &&
		    last_cell_grid_base_z_ == base_z) {
			++dispatch_stats_.coverage_skips;
			quad_coverage_current = true;
		}
	}

	Dictionary defs_by_match = _build_defs_by_match();
	if (defs_by_match.is_empty()) {
		return;
	}

	if (!_has_sampling_source()) {
		return;
	}

	if (!quad_coverage_current) {
		if (dispatch_algorithm_ == DISPATCH_ALGORITHM_CELL_GRID) {
			_dispatch_cell_grid(centre, defs_by_match);
		} else {
			PackedVector3Array centers;
			centers.push_back(centre);
			_dispatch_engine_centers(centers, view_xform, defs_by_match);
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
	if (defs_by_match.is_empty() || !_has_sampling_source()) {
		return;
	}

	_dispatch_engine_centers(centers, view_xform, defs_by_match);
	_dispatch_model_tier(view_xform, defs_by_match);
}

void NovaFoliageDispatcher::_dispatch_cell_grid(Vector3 centre, const Dictionary &defs_by_match) {
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

				std::vector<Transform3D> transforms;
				std::vector<Color> colors;
				std::vector<Color> customs;
				const bool any = _scatter_cell(slot_index, cell_x, cell_z, def, defs_by_match,
				                               transforms, colors, customs);
				if (!any) {
					continue;
				}
				LRUEntry entry;
				entry.transforms = std::move(transforms);
				entry.colors = std::move(colors);
				entry.customs = std::move(customs);
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

float NovaFoliageDispatcher::_sample_height(float world_x, float world_z) const {
	if (!height_sampler_.is_valid()) {
		return INVALID_HEIGHT_THRESHOLD;
	}
	Array args;
	args.push_back(world_x);
	args.push_back(world_z);
	Variant result = height_sampler_.callv(args);
	return static_cast<float>(static_cast<double>(result));
}

void NovaFoliageDispatcher::_append_render_instance(const opennova::foliage::PlacementInstance &inst,
                                                    std::vector<Transform3D> &out_transforms,
                                                    std::vector<Color> &out_colors,
                                                    std::vector<Color> &out_customs) const {
	using opennova::foliage::FIXED_TO_FLOAT;

	const float wx = static_cast<float>(inst.world_x_fixed) * FIXED_TO_FLOAT;
	const float wz = static_cast<float>(inst.world_z_fixed) * FIXED_TO_FLOAT;

	// The quad placement's OWN witnessed ground fit: patch_control IS the
	// (E_A, T_A, E_B, T_B) fold of the corner/midpoint heights - the same
	// family the model tier feeds its grid-placement VS (pinned by the
	// foliage_quad_fold ctest) [orig: Foliage_BuildPatchData @ 0x5C0240,
	// 0x5C06B8..0x5C07FC; retail generate_foliage_instances_0 @ 0x600197].
	const float e_a = inst.patch_control[0];
	const float t_a = inst.patch_control[1];
	const float e_b = inst.patch_control[2];
	const float t_b = inst.patch_control[3];
	float h[4];
	for (int k = 0; k < 4; ++k) {
		h[k] = static_cast<float>(inst.corner_y_fixed[k]) * FIXED_TO_FLOAT;
	}
	// hbase = the fit at the patch center (corner average + E_A + E_B); the
	// shader adds only the per-vertex delta, like the model tier.
	const float hbase = (h[0] + h[1] + h[2] + h[3]) * 0.25f + e_a + e_b;

	// Ground-conforming, upright, yaw-only: conformance comes per-vertex
	// from the height fold, never from a basis tilt. The quad corner layout
	// shares the model tier's axis form (A -> +X form, B -> -Z form; the
	// patch mesh spans mesh (x, z) = (-A, +B)), so the SAME rotY(yaw + pi)
	// mapping, the same (1, 0, 3, 2) COLOR corner order, and the same T_A
	// sign flip apply (derivation at _model_instance_transform).
	out_transforms.emplace_back(_engine_yaw_basis(inst.rotation_radians),
	                            Vector3(wx, hbase + surface_offset_, wz));
	out_colors.emplace_back(Color(h[1] - hbase, h[0] - hbase, h[3] - hbase, h[2] - hbase));
	out_customs.emplace_back(Color(e_a, -t_a, e_b, t_b));
}

void NovaFoliageDispatcher::_dispatch_engine_centers(const PackedVector3Array &centers,
                                                     const Transform3D &view_xform,
                                                     const Dictionary &defs_by_match) {
	render_algorithm_ = DISPATCH_ALGORITHM_ENGINE_CENTERS;
	NovaTerrainData *td = terrain_data_.ptr();
	if (td == nullptr && (!height_sampler_.is_valid() || !foliage_sampler_.is_valid())) {
		return;
	}

	for (auto &slot_transforms : engine_transforms_) {
		slot_transforms.clear();
	}
	for (auto &slot_colors : engine_colors_) {
		slot_colors.clear();
	}
	for (auto &slot_customs : engine_customs_) {
		slot_customs.clear();
	}

	opennova::foliage::PlacementConfig config;
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		config.quad_half_width[s] = quad_half_width_;
		if (s < foliage_defs_.size()) {
			Ref<NovaTerrainFoliageDef> def = foliage_defs_[s];
			if (def.is_valid()) {
				config.attrib_flags[s] = static_cast<uint8_t>(def->get_attrib_flags());
				config.color_lower[s] = def->get_color_lower();
				config.color_upper[s] = def->get_color_upper();
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
	samplers.slot_mask_at = [this, td, &defs_by_match](opennova::foliage::Fixed16_16 wx,
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

	++engine_frame_counter_;

	for (int center_index = 0; center_index < centers.size(); ++center_index) {
		const Vector3 centre = centers[center_index];
		const auto centre_x_fixed =
		    static_cast<opennova::foliage::Fixed16_16>(std::lround(static_cast<double>(centre.x) * 65536.0));
		const auto centre_z_fixed =
		    static_cast<opennova::foliage::Fixed16_16>(std::lround(static_cast<double>(centre.z) * 65536.0));

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
			engine_dispatchers_[slot_index].dispatch(slot_index,
			                                         centre_x_fixed,
			                                         centre_z_fixed,
			                                         engine_view_camera_z,
			                                         engine_view_radius_fixed_,
			                                         engine_frame_counter_,
			                                         config,
			                                         samplers,
			                                         zsort);
			auto &slot_transforms = engine_transforms_[slot_index];
			auto &slot_colors = engine_colors_[slot_index];
			auto &slot_customs = engine_customs_[slot_index];
			slot_transforms.reserve(slot_transforms.size() + zsort.size());
			slot_colors.reserve(slot_colors.size() + zsort.size());
			slot_customs.reserve(slot_customs.size() + zsort.size());
			for (const auto &z_instance : zsort) {
				_append_render_instance(z_instance.instance, slot_transforms, slot_colors, slot_customs);
			}
		}
	}

	_rebuild_multimeshes();
	mm_dirty_ = false;
}

// --- The NEAR/MODEL tier -----------------------------------------------------

void NovaFoliageDispatcher::_make_sampler_bindings(const Dictionary &defs_by_match,
                                                   opennova::foliage::PlacementSamplers &out_samplers) const {
	NovaTerrainData *td = terrain_data_.ptr();

	// sub_606490 (path/ambient-source spacing) remains deferred until the
	// ambient-source registry exists - same as the quad tiers.
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
	// foliage-map index -> def-slot mask chain (the same seam the quad host
	// paths use). Dictionary captured by value (COW ref) so the samplers
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

// The engine->Godot instance mapping, DERIVED from the libs corner outputs
// (pinned by the ctest/GUT parity tests - never guessed). Both tiers share
// it: the model tier through the transform below, the far ground patch
// through _append_render_instance.
//
//   MODEL TRANSFORM = translate(center, hbase) * rotY(yaw + pi)
//                   * scale(0.75, 0.5, 0.75) * translate(-(cx, 0, cz))
//   PATCH TRANSFORM = translate(center, hbase + surface_offset)
//                   * rotY(yaw + pi)            (mesh already at world size)
//
// Derivation. Both libs generators emit engine-space corners k = 0..3 at
// local coords (A, B) = (k&1 ? +F : -F, k&2 ? +F : -F) rotated by yaw, with
// world deltas dX = A cos - B sin, dZ = -(A sin + B cos) (the B axis runs
// NEGATIVE world Z - a handedness flip, det -1 on (A, B) -> (X, Z))
// [orig: Foliage_GenerateModelTileInstances @ 0x600980 (model);
// Foliage_BuildPatchData @ 0x5C0240 (quad)]. Both render meshes live in an
// X-mirrored space relative to that layout - the 3DI -> Godot conversion
// negates X (nova_object_data.cpp godot_position), and the far patch mesh
// is BUILT with mesh (x, z) = (-A, +B) to match - which flips handedness
// AGAIN, so the composite Godot-mesh -> world XZ map is a proper rotation:
// for a mesh-space offset (gx, gz), (dX, dZ) = s * (gx cos t + gz sin t,
// -gx sin t + gz cos t) with t = yaw + pi - exactly Godot's rotY(t).
// Mesh corner (sx, sz) therefore lands on engine corner (A, B) =
// (-F sx, +F sz):
//   mesh (-1,-1) -> c1, (+1,-1) -> c0, (-1,+1) -> c3, (+1,+1) -> c2
// which is why the packed COLOR corner order is (h1, h0, h3, h2) and the
// engine u axis maps to -u_mesh (the CUSTOM_DATA T_A sign flip); v is
// unchanged. Model scale: the VB normalization x_n = (x - cx)/(2R) + 0.5
// spanning footprint corners at +-0.75R gives the witnessed effective
// render scale 0.75 on XZ [orig: Foliage_FillInstancedModelBuffers
// @ 0x5ffa20 (the normalize)], and pos.y = y * 0.5 - the model height is
// HALVED in the VB. hbase = the fit at the instance center (corner average
// + the sag-fold center E_A + E_B); the vertex shader adds only the
// per-vertex DELTA, so the transform carries the base.
Basis NovaFoliageDispatcher::_engine_yaw_basis(float yaw_radians) const {
	return Basis(Vector3(0, 1, 0), yaw_radians + static_cast<float>(Math_PI));
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
	_make_sampler_bindings(defs_by_match, samplers);

	++model_frame_counter_;
	// Host cadence mapping: retail bumps the wind phase once per TILE DRAW
	// [orig: Foliage_ModelWindPhaseCounter @ 0x3162170 in
	// Foliage_UploadModelTileVSConstants @ 0x600f00]; the host advances once
	// per dispatch and shares the phase across tiles via the shader uniform.
	++model_wind_counter_;

	const bool has_view = !(view_xform == Transform3D());
	const Transform3D view_inv = has_view ? view_xform.affine_inverse() : Transform3D();

	// Per-frame (slot, tile) dedup - host mapping: retail re-draws a tile
	// shared by two entities once PER ENTITY (identical geometry, overdraw);
	// a MultiMesh would visibly double-stamp, so shared tiles emit once.
	std::unordered_set<uint64_t> seen_tiles;
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
				const uint64_t dedup_key =
				    (static_cast<uint64_t>(s) << 32) | draw.tile_key;
				if (!seen_tiles.insert(dedup_key).second) {
					continue;
				}
				++dispatch_stats_.model_tiles_emitted;
				auto &slot_instances = model_instances_[s];
				for (int i = 0; i < draw.result.count; ++i) {
					slot_instances.push_back(draw.result.instances[i]);
				}
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
			continue;  // degenerate model; far tier keeps the fallback box
		}
		bounds.valid = true;
		slot_bounds_[s] = bounds;
	}
}

Ref<Mesh> NovaFoliageDispatcher::_build_far_patch_mesh() const {
	// FAR-tier GROUND patch: an XZ-plane quad the ground-fit shader bends
	// onto the placement's witnessed corner/midpoint fold. The quad tier's
	// own placement data (corner_y/midpoint_y/patch_control) describes a
	// ground-conforming patch, not a billboard - the earlier upright-quad
	// host mapping is retracted (D-FOLIAGE-4 note). Mesh axes are chosen as
	// mesh (x, z) = (-A, +B) so the SAME engine yaw mapping as the model
	// tier applies; UV = the normalized weight space ((0,0) at mesh
	// (-w, -w)). The retail emitter's exact quad SIZE remains the ungrilled
	// D-FOLIAGE-1/-3 leg, so the width stays the quad_half_width knob.
	const float w = quad_half_width_;

	PackedVector3Array positions;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedInt32Array indices;
	positions.push_back(Vector3(-w, 0.0f, -w));
	positions.push_back(Vector3(w, 0.0f, -w));
	positions.push_back(Vector3(-w, 0.0f, w));
	positions.push_back(Vector3(w, 0.0f, w));
	uvs.push_back(Vector2(0.0f, 0.0f));
	uvs.push_back(Vector2(1.0f, 0.0f));
	uvs.push_back(Vector2(0.0f, 1.0f));
	uvs.push_back(Vector2(1.0f, 1.0f));
	for (int i = 0; i < 4; ++i) {
		normals.push_back(Vector3(0.0f, 1.0f, 0.0f));
	}
	indices.push_back(0);
	indices.push_back(1);
	indices.push_back(3);
	indices.push_back(0);
	indices.push_back(3);
	indices.push_back(2);

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = positions;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;

	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

Ref<Texture2D> NovaFoliageDispatcher::_slot_fd_texture(int slot_index) const {
	if (slot_index < 0 || slot_index >= slot_fd_textures_.size()) {
		return Ref<Texture2D>();
	}
	return slot_fd_textures_[slot_index];
}

Ref<Texture2D> NovaFoliageDispatcher::_slot_fd_or_albedo(int slot_index) const {
	// The ":fd" bake - both tiers bind it [orig: Foliage_DrawModelTileSlot
	// @ 0x601d90]; fall back to the MODEL mesh's own albedo when no bake
	// was supplied.
	Ref<Texture2D> fd_tex = _slot_fd_texture(slot_index);
	if (fd_tex.is_valid()) {
		return fd_tex;
	}
	Ref<Mesh> model_mesh;
	if (slot_index >= 0 && slot_index < slot_meshes_.size()) {
		model_mesh = slot_meshes_[slot_index];
	}
	if (model_mesh.is_valid() && model_mesh->get_surface_count() > 0) {
		Ref<Material> source_material = model_mesh->surface_get_material(0);
		if (source_material.is_valid()) {
			if (BaseMaterial3D *base_material = Object::cast_to<BaseMaterial3D>(source_material.ptr())) {
				return base_material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
			}
		}
	}
	return Ref<Texture2D>();
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

	// NEAR tier: weights re-normalized from the pre-scale model vertex.
	material->set_shader_parameter("u_weights_from_uv", false);

	Ref<Texture2D> fd_tex = _slot_fd_or_albedo(slot_index);
	material->set_shader_parameter("u_fd_texture", fd_tex);
	material->set_shader_parameter("u_has_fd_texture", fd_tex.is_valid());

	// Colormap from the same source chain as the quad tier (runtime terrain
	// first, editor colormap-only source second).
	Ref<NovaTerrainData> cm_src = terrain_data_.is_valid() ? terrain_data_ : colormap_source_;
	const bool has_cm = cm_src.is_valid() && cm_src->get_colormap().is_valid();
	if (has_cm) {
		material->set_shader_parameter("u_colormap", cm_src->get_colormap());
	}
	material->set_shader_parameter("u_has_colormap", has_cm);

	// The env terrain tint feeding the in-shader emitter-form v0 derivation.
	material->set_shader_parameter("u_terrain_tint", terrain_tint_);

	// The bound square the vertex shader re-normalizes against (pre-scale
	// Godot mesh space) [orig: Foliage_FillInstancedModelBuffers @ 0x5ffa20].
	const SlotModelBounds &bounds = slot_bounds_[slot_index];
	material->set_shader_parameter("u_bound_center", Vector2(bounds.center_x, bounds.center_z));
	material->set_shader_parameter("u_bound_radius", bounds.radius);

	if (mm_model_by_slot_[slot_index] != nullptr) {
		mm_model_by_slot_[slot_index]->set_material_override(material);
	}
}

void NovaFoliageDispatcher::_rebuild_model_multimeshes() {
	using opennova::foliage::model_instance_hbase;

	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		const auto &instances = model_instances_[s];
		const int count = static_cast<int>(instances.size());
		if (count == 0) {
			if (mm_model_by_slot_[s] != nullptr) {
				mm_model_by_slot_[s]->queue_free();
				mm_model_by_slot_[s] = nullptr;
			}
			continue;
		}

		Ref<Mesh> slot_mesh;
		if (s < slot_meshes_.size()) {
			slot_mesh = slot_meshes_[s];
		}
		if (slot_mesh.is_null()) {
			continue;  // no model, no near tier (slot_active already gates this)
		}

		if (mm_model_by_slot_[s] == nullptr) {
			MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
			mmi->set_name(String("FoliageModelSlot") + String::num_int64(s));
			Ref<MultiMesh> mm;
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_use_colors(true);
			mm->set_use_custom_data(true);
			mm->set_mesh(slot_mesh);
			mmi->set_multimesh(mm);
			// The model pass has no witnessed shadow path (fixed-function
			// alpha-tested draw); the far tier's SHADOW attrib stays a
			// quad-tier host feature.
			mmi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			// The shader displaces vertices (ground fit + wind) after the CPU
			// AABB is computed; margin keeps the culler honest.
			mmi->set_extra_cull_margin(8.0f);
			add_child(mmi);
			mm_model_by_slot_[s] = mmi;
		}

		Ref<MultiMesh> mm = mm_model_by_slot_[s]->get_multimesh();
		if (mm.is_null()) {
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_use_colors(true);
			mm->set_use_custom_data(true);
			mm->set_mesh(slot_mesh);
			mm_model_by_slot_[s]->set_multimesh(mm);
		} else if (mm->get_mesh() != slot_mesh) {
			mm->set_mesh(slot_mesh);
		}

		_update_model_slot_material(s);
		Ref<ShaderMaterial> material = foliage_model_materials_[s];
		if (material.is_valid()) {
			// The wind phase [orig: c9 = (sin(counter*0.001)*0.08, ...) in
			// Foliage_UploadModelTileVSConstants @ 0x600f00]; per-frame host
			// cadence (see the counter's note).
			material->set_shader_parameter(
			    "u_model_wind_phase", static_cast<float>(static_cast<double>(model_wind_counter_) * 0.001));
		}

		mm->set_instance_count(count);
		const SlotModelBounds &bounds = slot_bounds_[s];
		for (int i = 0; i < count; ++i) {
			const auto &inst = instances[i];
			const float hbase = model_instance_hbase(inst);
			mm->set_instance_transform(i, _model_instance_transform(inst, hbase, bounds));
			// COLOR = per-corner height deltas in the SHADER corner order
			// (h1, h0, h3, h2) - the mesh-X negation permutation derived at
			// _model_instance_transform.
			mm->set_instance_color(i, Color(inst.corner_height[1] - hbase,
			                                inst.corner_height[0] - hbase,
			                                inst.corner_height[3] - hbase,
			                                inst.corner_height[2] - hbase));
			// CUSTOM = (E_A, -T_A, E_B, T_B): the engine u axis is the mesh
			// -u axis, so T_A flips sign; v is unchanged.
			mm->set_instance_custom_data(i, Color(inst.fold_e_a, -inst.fold_t_a,
			                                      inst.fold_e_b, inst.fold_t_b));
		}
	}
}

bool NovaFoliageDispatcher::_scatter_cell(int slot_index,
                                          int cell_x_int,
                                          int cell_z_int,
                                          const Ref<NovaTerrainFoliageDef> &def,
                                          const Dictionary &defs_by_match,
                                          std::vector<Transform3D> &out_transforms,
                                          std::vector<Color> &out_colors,
                                          std::vector<Color> &out_customs) {
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

	samplers.slot_mask_at = [this, td, &defs_by_match](Fixed16_16 wx, Fixed16_16 wz) -> uint32_t {
		const float wx_f = static_cast<float>(wx) * opennova::foliage::FIXED_TO_FLOAT;
		const float wz_f = static_cast<float>(wz) * opennova::foliage::FIXED_TO_FLOAT;
		int painted;
		if (td != nullptr) {
			painted = td->get_foliage_index_world(wx_f, wz_f);
		} else if (foliage_sampler_.is_valid()) {
			Array args;
			args.push_back(wx_f);
			args.push_back(wz_f);
			painted = static_cast<int>(foliage_sampler_.callv(args));
		} else {
			return 0u;
		}
		if (painted == 0 || !defs_by_match.has(painted)) {
			return 0u;
		}

		uint32_t mask = 0u;
		Array def_list = defs_by_match[painted];
		for (int i = 0; i < def_list.size(); ++i) {
			const int s = static_cast<int>(def_list[i]);
			if (s >= 0 && s < 32) {
				mask |= (1u << s);
			}
		}
		return mask;
	};

	opennova::foliage::PlacementConfig config;
	config.attrib_flags[slot_index] = static_cast<uint8_t>(def.is_valid() ? def->get_attrib_flags() : 0);
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		config.quad_half_width[s] = quad_half_width_;
	}

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

	for (int i = 0; i < result.count; ++i) {
		_append_render_instance(result.instances[i], out_transforms, out_colors, out_customs);
	}

	(void)def;
	return !out_transforms.empty();
}

void NovaFoliageDispatcher::_clear_children() {
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		if (mm_by_slot_[s] != nullptr && mm_by_slot_[s]->get_parent() == this) {
			mm_by_slot_[s]->queue_free();
		}
		mm_by_slot_[s] = nullptr;
		if (mm_model_by_slot_[s] != nullptr && mm_model_by_slot_[s]->get_parent() == this) {
			mm_model_by_slot_[s]->queue_free();
		}
		mm_model_by_slot_[s] = nullptr;
	}
}

Ref<Mesh> NovaFoliageDispatcher::_fallback_mesh() const {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(2.0, 6.0, 2.0));
	return box;
}

void NovaFoliageDispatcher::_update_slot_material(int slot_index) {
	if (slot_index < 0 || slot_index >= opennova::FOLIAGE_MAX_DEFS) {
		return;
	}

	// The FAR ground patches run the SAME witnessed ground-fit shader as the
	// model tier (one shader, one copy of the fit/combine math); the patch
	// spans the corner rectangle directly, so weights come from UV.
	if (foliage_model_shader_.is_null()) {
		foliage_model_shader_ =
		    ResourceLoader::get_singleton()->load("res://shaders/foliage_model.gdshader", "Shader");
	}
	if (foliage_model_shader_.is_null()) {
		return;
	}

	Ref<ShaderMaterial> material = foliage_materials_[slot_index];
	if (material.is_null()) {
		material.instantiate();
		material->set_shader(foliage_model_shader_);
		foliage_materials_[slot_index] = material;
	} else if (material->get_shader() != foliage_model_shader_) {
		material->set_shader(foliage_model_shader_);
	}

	material->set_shader_parameter("u_weights_from_uv", true);

	// The far patches bind the ":fd" bake [orig: Foliage_LoadDefAssets
	// @ 0x601260 tail - the quad tier textures with the same "%s:fd"].
	Ref<Texture2D> fd_tex = _slot_fd_or_albedo(slot_index);
	material->set_shader_parameter("u_fd_texture", fd_tex);
	material->set_shader_parameter("u_has_fd_texture", fd_tex.is_valid());

	// Colormap from the runtime terrain first, editor colormap-only source
	// second, so the witnessed blend PS sees the CPU sampler's texels.
	Ref<NovaTerrainData> cm_src = terrain_data_.is_valid() ? terrain_data_ : colormap_source_;
	const bool has_cm = cm_src.is_valid() && cm_src->get_colormap().is_valid();
	if (has_cm) {
		material->set_shader_parameter("u_colormap", cm_src->get_colormap());
	}
	material->set_shader_parameter("u_has_colormap", has_cm);

	// The env terrain tint feeding the in-shader emitter-form v0 derivation.
	material->set_shader_parameter("u_terrain_tint", terrain_tint_);

	if (mm_by_slot_[slot_index] != nullptr) {
		mm_by_slot_[slot_index]->set_material_override(material);
	}
}

void NovaFoliageDispatcher::_rebuild_multimeshes() {
	std::vector<Transform3D> per_slot_t[opennova::FOLIAGE_MAX_DEFS];
	std::vector<Color> per_slot_c[opennova::FOLIAGE_MAX_DEFS];
	std::vector<Color> per_slot_cd[opennova::FOLIAGE_MAX_DEFS];

	if (render_algorithm_ == DISPATCH_ALGORITHM_ENGINE_CENTERS) {
		for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
			per_slot_t[s] = engine_transforms_[s];
			per_slot_c[s] = engine_colors_[s];
			per_slot_cd[s] = engine_customs_[s];
		}
	} else {
		for (const auto &kv : lru_) {
			const int s = kv.first.slot;
			if (s < 0 || s >= opennova::FOLIAGE_MAX_DEFS) {
				continue;
			}
			const auto &entry = kv.second;
			per_slot_t[s].insert(per_slot_t[s].end(), entry.transforms.begin(), entry.transforms.end());
			per_slot_c[s].insert(per_slot_c[s].end(), entry.colors.begin(), entry.colors.end());
			per_slot_cd[s].insert(per_slot_cd[s].end(), entry.customs.begin(), entry.customs.end());
		}
	}

	// FAR tier renders the shared ground patch, never the full model mesh -
	// the full 3DI stamps only in the NEAR/MODEL tier clusters
	// (docs/foliage/foliage-re.md §The model tier; D-FOLIAGE-4). Slots
	// without a usable model keep the visible fallback box.
	if (far_patch_mesh_.is_null()) {
		far_patch_mesh_ = _build_far_patch_mesh();
	}
	auto mesh_for_slot = [this](int s) -> Ref<Mesh> {
		if (s >= 0 && s < opennova::FOLIAGE_MAX_DEFS && slot_bounds_[s].valid) {
			return far_patch_mesh_;
		}
		return _fallback_mesh();
	};

	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		const int count = static_cast<int>(per_slot_t[s].size());
		if (count == 0) {
			if (mm_by_slot_[s] != nullptr) {
				mm_by_slot_[s]->queue_free();
				mm_by_slot_[s] = nullptr;
			}
			continue;
		}
		++dispatch_stats_.rebuilt_slots;
		dispatch_stats_.instance_uploads += count;

		Ref<Mesh> slot_mesh = mesh_for_slot(s);
		if (mm_by_slot_[s] == nullptr) {
			MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
			mmi->set_name(String("FoliageSlot") + String::num_int64(s));
			Ref<MultiMesh> mm;
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_use_colors(true);
			mm->set_use_custom_data(true);
			mm->set_mesh(slot_mesh);
			mmi->set_multimesh(mm);
			// The ground-fit shader bends the flat patch after the CPU AABB
			// is computed; margin keeps the culler honest on relief.
			mmi->set_extra_cull_margin(8.0f);
			add_child(mmi);
			mm_by_slot_[s] = mmi;
		}

		Ref<MultiMesh> mm = mm_by_slot_[s]->get_multimesh();
		if (mm.is_null()) {
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_use_colors(true);
			mm->set_use_custom_data(true);
			mm->set_mesh(slot_mesh);
			mm_by_slot_[s]->set_multimesh(mm);
		} else if (mm->get_mesh() != slot_mesh) {
			mm->set_mesh(slot_mesh);
		}

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

		mm->set_instance_count(count);
		const bool has_customs = per_slot_cd[s].size() == per_slot_t[s].size();
		for (int i = 0; i < count; ++i) {
			mm->set_instance_transform(i, per_slot_t[s][i]);
			mm->set_instance_color(i, per_slot_c[s][i]);
			if (has_customs) {
				mm->set_instance_custom_data(i, per_slot_cd[s][i]);
			}
		}
	}
}

} // namespace godot
