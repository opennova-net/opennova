#include "nova_foliage_dispatcher.h"

#include "nova_terrain_data.h"

#include <terrain/lighting.h>

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <algorithm>
#include <cmath>

namespace godot {

namespace {

constexpr float INVALID_HEIGHT_THRESHOLD = -1.0e6f;
constexpr int32_t RUNTIME_VIEW_RADIUS_FIXED = 0x40000;

// Engine foliage lighting is not normal/slope lighting. Foliage_BuildGeometry
// @0x005BF5F0 samples Terrain_GetModulatedColorAtPos@0x005C5FE0; sub_5C0240
// only prepares height/patch-control data for the later render emitter.

uint32_t color_to_argb(const Color &color) {
	auto to_byte = [](float value) -> uint32_t {
		return static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255));
	};
	return (to_byte(color.a) << 24) | (to_byte(color.r) << 16) | (to_byte(color.g) << 8) | to_byte(color.b);
}

Color argb_to_color(uint32_t argb) {
	constexpr float INV_255 = 1.0f / 255.0f;
	return Color(static_cast<float>((argb >> 16) & 0xFFu) * INV_255,
	             static_cast<float>((argb >> 8) & 0xFFu) * INV_255,
	             static_cast<float>(argb & 0xFFu) * INV_255,
	             static_cast<float>((argb >> 24) & 0xFFu) * INV_255);
}

} // namespace

NovaFoliageDispatcher::NovaFoliageDispatcher() = default;
NovaFoliageDispatcher::~NovaFoliageDispatcher() = default;

void NovaFoliageDispatcher::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_foliage_defs", "defs"), &NovaFoliageDispatcher::set_foliage_defs);
	ClassDB::bind_method(D_METHOD("get_foliage_defs"), &NovaFoliageDispatcher::get_foliage_defs);
	ClassDB::bind_method(D_METHOD("set_slot_meshes", "meshes"), &NovaFoliageDispatcher::set_slot_meshes);
	ClassDB::bind_method(D_METHOD("get_slot_meshes"), &NovaFoliageDispatcher::get_slot_meshes);
	ClassDB::bind_method(D_METHOD("set_height_sampler", "sampler"), &NovaFoliageDispatcher::set_height_sampler);
	ClassDB::bind_method(D_METHOD("get_height_sampler"), &NovaFoliageDispatcher::get_height_sampler);
	ClassDB::bind_method(D_METHOD("set_foliage_sampler", "sampler"), &NovaFoliageDispatcher::set_foliage_sampler);
	ClassDB::bind_method(D_METHOD("get_foliage_sampler"), &NovaFoliageDispatcher::get_foliage_sampler);
	ClassDB::bind_method(D_METHOD("set_terrain_data", "data"), &NovaFoliageDispatcher::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"), &NovaFoliageDispatcher::get_terrain_data);
	ClassDB::bind_method(D_METHOD("set_preview_cell_radius", "radius"), &NovaFoliageDispatcher::set_preview_cell_radius);
	ClassDB::bind_method(D_METHOD("get_preview_cell_radius"), &NovaFoliageDispatcher::get_preview_cell_radius);
	ClassDB::bind_method(D_METHOD("set_lru_capacity", "capacity"), &NovaFoliageDispatcher::set_lru_capacity);
	ClassDB::bind_method(D_METHOD("get_lru_capacity"), &NovaFoliageDispatcher::get_lru_capacity);
	ClassDB::bind_method(D_METHOD("set_quad_half_width", "width"), &NovaFoliageDispatcher::set_quad_half_width);
	ClassDB::bind_method(D_METHOD("get_quad_half_width"), &NovaFoliageDispatcher::get_quad_half_width);
	ClassDB::bind_method(D_METHOD("set_surface_offset", "offset"), &NovaFoliageDispatcher::set_surface_offset);
	ClassDB::bind_method(D_METHOD("get_surface_offset"), &NovaFoliageDispatcher::get_surface_offset);
	ClassDB::bind_method(D_METHOD("dispatch", "centre", "view_xform"),
	                     &NovaFoliageDispatcher::dispatch, DEFVAL(Transform3D()));
	ClassDB::bind_method(D_METHOD("reset"), &NovaFoliageDispatcher::reset);
	ClassDB::bind_method(D_METHOD("get_total_instances"), &NovaFoliageDispatcher::get_total_instances);
	ClassDB::bind_method(D_METHOD("get_cached_cells"), &NovaFoliageDispatcher::get_cached_cells);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "foliage_defs"), "set_foliage_defs", "get_foliage_defs");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "slot_meshes", PROPERTY_HINT_ARRAY_TYPE, "Mesh"),
	             "set_slot_meshes", "get_slot_meshes");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "height_sampler"), "set_height_sampler", "get_height_sampler");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "foliage_sampler"), "set_foliage_sampler", "get_foliage_sampler");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data", PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainData"),
	             "set_terrain_data", "get_terrain_data");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "preview_cell_radius"), "set_preview_cell_radius", "get_preview_cell_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lru_capacity"), "set_lru_capacity", "get_lru_capacity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "quad_half_width"), "set_quad_half_width", "get_quad_half_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "surface_offset"), "set_surface_offset", "get_surface_offset");
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
	mm_dirty_ = true;
}

Array NovaFoliageDispatcher::get_slot_meshes() const { return slot_meshes_; }

void NovaFoliageDispatcher::set_height_sampler(const Callable &p_sampler) { height_sampler_ = p_sampler; }
Callable NovaFoliageDispatcher::get_height_sampler() const { return height_sampler_; }

void NovaFoliageDispatcher::set_foliage_sampler(const Callable &p_sampler) {
	foliage_sampler_ = p_sampler;
	if (!_is_runtime_dispatch_mode()) {
		reset();
	}
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

void NovaFoliageDispatcher::set_preview_cell_radius(int p_radius) {
	preview_cell_radius_ = p_radius < 0 ? 0 : p_radius;
}

int NovaFoliageDispatcher::get_preview_cell_radius() const { return preview_cell_radius_; }

void NovaFoliageDispatcher::set_lru_capacity(int p_capacity) {
	lru_capacity_ = p_capacity < 1 ? 1 : p_capacity;
}

int NovaFoliageDispatcher::get_lru_capacity() const { return lru_capacity_; }

void NovaFoliageDispatcher::set_quad_half_width(float p_width) {
	quad_half_width_ = p_width;
	reset();
}

float NovaFoliageDispatcher::get_quad_half_width() const { return quad_half_width_; }

void NovaFoliageDispatcher::set_surface_offset(float p_offset) {
	surface_offset_ = p_offset;
	reset();
}

float NovaFoliageDispatcher::get_surface_offset() const { return surface_offset_; }

bool NovaFoliageDispatcher::_is_runtime_dispatch_mode() const {
	return terrain_data_.is_valid();
}

void NovaFoliageDispatcher::reset() {
	lru_.clear();
	touch_counter_ = 0;
	runtime_frame_counter_ = 0;
	for (auto &dispatcher : runtime_dispatchers_) {
		dispatcher.reset();
	}
	for (auto &slot_transforms : runtime_transforms_) {
		slot_transforms.clear();
	}
	for (auto &slot_colors : runtime_colors_) {
		slot_colors.clear();
	}
	mm_dirty_ = true;
	_clear_children();
}

int NovaFoliageDispatcher::get_cached_cells() const {
	if (_is_runtime_dispatch_mode()) {
		int total = 0;
		for (const auto &dispatcher : runtime_dispatchers_) {
			total += dispatcher.lru_occupancy();
		}
		return total;
	}
	return static_cast<int>(lru_.size());
}

int NovaFoliageDispatcher::get_total_instances() const {
	if (_is_runtime_dispatch_mode()) {
		int total = 0;
		for (const auto &slot_instances : runtime_transforms_) {
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

void NovaFoliageDispatcher::dispatch(Vector3 centre, Transform3D view_xform) {
	if (foliage_defs_.is_empty()) {
		return;
	}

	Dictionary defs_by_match = _build_defs_by_match();
	if (defs_by_match.is_empty()) {
		return;
	}

	if (_is_runtime_dispatch_mode()) {
		_dispatch_runtime(centre, view_xform, defs_by_match);
		return;
	}

	if (!height_sampler_.is_valid() || !foliage_sampler_.is_valid()) {
		return;
	}

	++touch_counter_;

	// Editor preview extension: scan a wider cell grid around the centre.
	const float base_x = std::floor(centre.x / 16.0f) * 16.0f;
	const float base_z = std::floor(centre.z / 16.0f) * 16.0f + 16.0f;

	const int num_slots = foliage_defs_.size();
	for (int slot_index = 0; slot_index < num_slots && slot_index < opennova::FOLIAGE_MAX_DEFS; ++slot_index) {
		Ref<NovaTerrainFoliageDef> def = foliage_defs_[slot_index];
		if (def.is_null()) {
			continue;
		}
		for (int dz = -preview_cell_radius_; dz <= preview_cell_radius_; ++dz) {
			for (int dx = -preview_cell_radius_; dx <= preview_cell_radius_; ++dx) {
				const int cell_x = static_cast<int>(base_x) + dx * 16;
				const int cell_z = static_cast<int>(base_z) + dz * 16;

				CellKey key{cell_x, cell_z, slot_index};
				auto it = lru_.find(key);
				if (it != lru_.end()) {
					it->second.touch = touch_counter_;
					continue;
				}

				std::vector<Transform3D> transforms;
				std::vector<Color> colors;
				const bool any = _scatter_cell(slot_index, cell_x, cell_z, def, defs_by_match, transforms, colors);
				if (!any) {
					continue;
				}
				LRUEntry entry;
				entry.transforms = std::move(transforms);
				entry.colors = std::move(colors);
				entry.touch = touch_counter_;
				lru_.emplace(key, std::move(entry));
				mm_dirty_ = true;
			}
		}
	}

	const int num_slots_used = foliage_defs_.size();
	const int grid_side = 2 * preview_cell_radius_ + 1;
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

Color NovaFoliageDispatcher::_sample_ground_color(const opennova::foliage::PlacementInstance &inst,
                                                  float quad_half_width) const {
	using opennova::foliage::FIXED_TO_FLOAT;

	(void)quad_half_width;
	NovaTerrainData *td = terrain_data_.ptr();
	if (td == nullptr) {
		return Color(1.0f, 1.0f, 1.0f, 1.0f);
	}

	const float wx = static_cast<float>(inst.world_x_fixed) * FIXED_TO_FLOAT;
	const float wz = static_cast<float>(inst.world_z_fixed) * FIXED_TO_FLOAT;

	// Jointops.exe Foliage_BuildGeometry@0x005BF5F0 averages four 0x8000
	// fixed-point offsets around each source vertex. MultiMesh has one color per
	// instance, so use the instance center as the proxy source vertex.
	const uint32_t c0 = color_to_argb(td->get_colormap_color_world(wx - 0.5f, wz - 0.5f));
	const uint32_t c1 = color_to_argb(td->get_colormap_color_world(wx + 0.5f, wz - 0.5f));
	const uint32_t c2 = color_to_argb(td->get_colormap_color_world(wx - 0.5f, wz + 0.5f));
	const uint32_t c3 = color_to_argb(td->get_colormap_color_world(wx + 0.5f, wz + 0.5f));
	const uint32_t packed = opennova::terrain::terrain_average_four_argb(c0, c1, c2, c3);
	return argb_to_color(packed);
}

void NovaFoliageDispatcher::_append_render_instance(const opennova::foliage::PlacementInstance &inst,
                                                    float quad_half_width,
                                                    std::vector<Transform3D> &out_transforms,
                                                    std::vector<Color> &out_colors) const {
	using opennova::foliage::FIXED_TO_FLOAT;

	const float wx = static_cast<float>(inst.world_x_fixed) * FIXED_TO_FLOAT;
	const float wz = static_cast<float>(inst.world_z_fixed) * FIXED_TO_FLOAT;
	const float wy = static_cast<float>(inst.world_y_fixed) * FIXED_TO_FLOAT;
	const float yaw = inst.rotation_radians;

	const float cos_y = std::cos(yaw);
	const float sin_y = std::sin(yaw);
	auto corner_xz = [cos_y, sin_y, quad_half_width](int c) -> std::pair<float, float> {
		const float qx = ((c & 1) ? 1.0f : -1.0f) * quad_half_width;
		const float qy = ((c & 2) ? 1.0f : -1.0f) * quad_half_width;
		const float dx = qx * cos_y - qy * sin_y;
		const float dz_local = qy * cos_y + qx * sin_y;
		return {dx, -dz_local};
	};

	Basis basis(Quaternion(Vector3(0, 1, 0), yaw));
	float corner_heights[4];
	bool valid_corners = true;
	for (int c = 0; c < 4; ++c) {
		corner_heights[c] = static_cast<float>(inst.corner_y_fixed[c]) * FIXED_TO_FLOAT;
		if (corner_heights[c] <= INVALID_HEIGHT_THRESHOLD) {
			valid_corners = false;
			break;
		}
	}
	if (valid_corners) {
		const auto [c0x, c0z] = corner_xz(0);
		const auto [c1x, c1z] = corner_xz(1);
		const auto [c2x, c2z] = corner_xz(2);
		const Vector3 p0(c0x, corner_heights[0] - wy, c0z);
		const Vector3 p1(c1x, corner_heights[1] - wy, c1z);
		const Vector3 p2(c2x, corner_heights[2] - wy, c2z);
		Vector3 normal = (p1 - p0).cross(p2 - p0).normalized();
		if (normal.y < 0.0f) {
			normal = -normal;
		}
		if (normal.length_squared() > 0.0f) {
			const Quaternion slope_rot(Vector3(0, 1, 0), normal);
			const Quaternion yaw_rot(Vector3(0, 1, 0), yaw);
			basis = Basis(slope_rot * yaw_rot);
		}
	}

	out_transforms.emplace_back(basis, Vector3(wx, wy + surface_offset_, wz));
	out_colors.emplace_back(_sample_ground_color(inst, quad_half_width));
}

void NovaFoliageDispatcher::_dispatch_runtime(Vector3 centre, const Transform3D &view_xform, const Dictionary &defs_by_match) {
	NovaTerrainData *td = terrain_data_.ptr();
	if (td == nullptr) {
		return;
	}

	// Snapshot per-slot instance counts so we can skip the GPU rebuild when
	// nothing changed this frame (the common case once the LRU is warm).
	std::array<size_t, opennova::FOLIAGE_MAX_DEFS> prev_slot_counts{};
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		prev_slot_counts[s] = runtime_transforms_[s].size();
	}

	for (auto &slot_transforms : runtime_transforms_) {
		slot_transforms.clear();
	}
	for (auto &slot_colors : runtime_colors_) {
		slot_colors.clear();
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
	samplers.height_at = [td](opennova::foliage::Fixed16_16 wx,
	                          opennova::foliage::Fixed16_16 wz) -> opennova::foliage::Fixed16_16 {
		const float wx_f = static_cast<float>(wx) * opennova::foliage::FIXED_TO_FLOAT;
		const float wz_f = static_cast<float>(wz) * opennova::foliage::FIXED_TO_FLOAT;
		const float y = td->get_height_world_bilinear(Vector3(wx_f, 0.0f, wz_f));
		return static_cast<opennova::foliage::Fixed16_16>(y * 65536.0f);
	};
	samplers.slot_mask_at = [td, &defs_by_match](opennova::foliage::Fixed16_16 wx,
	                                             opennova::foliage::Fixed16_16 wz) -> uint32_t {
		const float wx_f = static_cast<float>(wx) * opennova::foliage::FIXED_TO_FLOAT;
		const float wz_f = static_cast<float>(wz) * opennova::foliage::FIXED_TO_FLOAT;
		const int painted = td->get_foliage_index_world(wx_f, wz_f);
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

	const auto centre_x_fixed =
	    static_cast<opennova::foliage::Fixed16_16>(std::lround(static_cast<double>(centre.x) * 65536.0));
	const auto centre_z_fixed =
	    static_cast<opennova::foliage::Fixed16_16>(std::lround(static_cast<double>(centre.z) * 65536.0));

	++runtime_frame_counter_;

	// Engine near-plane reject. sub_5C1940 @ 0x5c19a3 calls
	// Math_TransformPoint(view_matrix, center, ...) and rejects the whole
	// dispatch when the transformed Z >= 38.0. Godot cameras look down -Z, so
	// the world->view transform yields a negative forward Z for points in
	// front of the camera; negate into the engine's +Z-forward convention
	// before the compare. Identity transform (editor / no camera) keeps
	// runtime_view_camera_z == DISPATCHER_NEAR_Z so the reject is a no-op.
	const Vector3 view_local = view_xform.affine_inverse().xform(centre);
	const float runtime_view_camera_z =
	    view_xform == Transform3D() ? opennova::foliage::DISPATCHER_NEAR_Z : -view_local.z;

	for (int slot_index = 0; slot_index < foliage_defs_.size() && slot_index < opennova::FOLIAGE_MAX_DEFS; ++slot_index) {
		Ref<NovaTerrainFoliageDef> def = foliage_defs_[slot_index];
		if (def.is_null()) {
			continue;
		}

		std::vector<opennova::foliage::ZSortInstance> zsort;
		runtime_dispatchers_[slot_index].dispatch(slot_index,
		                                          centre_x_fixed,
		                                          centre_z_fixed,
		                                          runtime_view_camera_z,
		                                          RUNTIME_VIEW_RADIUS_FIXED,
		                                          runtime_frame_counter_,
		                                          config,
		                                          samplers,
		                                          zsort);
		auto &slot_transforms = runtime_transforms_[slot_index];
		auto &slot_colors = runtime_colors_[slot_index];
		slot_transforms.reserve(zsort.size());
		slot_colors.reserve(zsort.size());
		for (const auto &z_instance : zsort) {
			_append_render_instance(z_instance.instance, quad_half_width_, slot_transforms, slot_colors);
		}
	}

	// Only push to GPU when the per-slot instance set actually changed. For a
	// stationary camera once the LRU is warm, the stagger gate produces cached
	// re-bakes that are byte-identical to the previous frame — size matches,
	// content matches, the rebuild is a wasted RenderingServer round-trip.
	bool any_count_changed = false;
	for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
		if (runtime_transforms_[s].size() != prev_slot_counts[s]) {
			any_count_changed = true;
			break;
		}
	}
	if (any_count_changed || mm_dirty_) {
		_rebuild_multimeshes();
		mm_dirty_ = false;
	}
}

bool NovaFoliageDispatcher::_scatter_cell(int slot_index,
                                          int cell_x_int,
                                          int cell_z_int,
                                          const Ref<NovaTerrainFoliageDef> &def,
                                          const Dictionary &defs_by_match,
                                          std::vector<Transform3D> &out_transforms,
                                          std::vector<Color> &out_colors) {
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
		_append_render_instance(result.instances[i], quad_half_width_, out_transforms, out_colors);
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
	}
}

Ref<Mesh> NovaFoliageDispatcher::_fallback_mesh() const {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(2.0, 6.0, 2.0));
	return box;
}

void NovaFoliageDispatcher::_update_slot_material(int slot_index, const Ref<Mesh> &slot_mesh) {
	if (slot_index < 0 || slot_index >= opennova::FOLIAGE_MAX_DEFS) {
		return;
	}

	if (foliage_shader_.is_null()) {
		foliage_shader_ = ResourceLoader::get_singleton()->load("res://shaders/foliage.gdshader", "Shader");
	}
	if (foliage_shader_.is_null()) {
		return;
	}

	Ref<ShaderMaterial> material = foliage_materials_[slot_index];
	if (material.is_null()) {
		material.instantiate();
		material->set_shader(foliage_shader_);
		foliage_materials_[slot_index] = material;
	} else if (material->get_shader() != foliage_shader_) {
		material->set_shader(foliage_shader_);
	}

	Ref<Texture2D> albedo_tex;
	if (slot_mesh.is_valid() && slot_mesh->get_surface_count() > 0) {
		Ref<Material> source_material = slot_mesh->surface_get_material(0);
		if (source_material.is_valid()) {
			if (BaseMaterial3D *base_material = Object::cast_to<BaseMaterial3D>(source_material.ptr())) {
				albedo_tex = base_material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
			}
		}
	}

	material->set_shader_parameter("u_albedo_texture", albedo_tex);
	material->set_shader_parameter("u_has_albedo_texture", albedo_tex.is_valid());
	if (terrain_data_.is_valid()) {
		material->set_shader_parameter("u_colormap", terrain_data_->get_colormap());
	}

	if (mm_by_slot_[slot_index] != nullptr) {
		mm_by_slot_[slot_index]->set_material_override(material);
	}
}

void NovaFoliageDispatcher::_rebuild_multimeshes() {
	std::vector<Transform3D> per_slot_t[opennova::FOLIAGE_MAX_DEFS];
	std::vector<Color> per_slot_c[opennova::FOLIAGE_MAX_DEFS];

	if (_is_runtime_dispatch_mode()) {
		for (int s = 0; s < opennova::FOLIAGE_MAX_DEFS; ++s) {
			per_slot_t[s] = runtime_transforms_[s];
			per_slot_c[s] = runtime_colors_[s];
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
		}
	}

	auto mesh_for_slot = [this](int s) -> Ref<Mesh> {
		if (s >= 0 && s < slot_meshes_.size()) {
			Ref<Mesh> mesh = slot_meshes_[s];
			if (mesh.is_valid()) {
				return mesh;
			}
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

		Ref<Mesh> slot_mesh = mesh_for_slot(s);
		if (mm_by_slot_[s] == nullptr) {
			MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
			mmi->set_name(String("FoliageSlot") + String::num_int64(s));
			Ref<MultiMesh> mm;
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_use_colors(true);
			mm->set_mesh(slot_mesh);
			mmi->set_multimesh(mm);
			add_child(mmi);
			mm_by_slot_[s] = mmi;
		}

		Ref<MultiMesh> mm = mm_by_slot_[s]->get_multimesh();
		if (mm.is_null()) {
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_use_colors(true);
			mm->set_mesh(slot_mesh);
			mm_by_slot_[s]->set_multimesh(mm);
		} else if (mm->get_mesh() != slot_mesh) {
			mm->set_mesh(slot_mesh);
		}

		// SHADOW attribute bit (Jointops.exe FoliageDef +532 bit 1, engine_spec_foliage §3.1).
		// The bit is a shadow-render opt-in; when set the slot's instances cast shadows,
		// otherwise we disable the pass entirely. Applied every rebuild so def edits
		// propagate without forcing a full scene reload.
		_update_slot_material(s, slot_mesh);

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
		for (int i = 0; i < count; ++i) {
			mm->set_instance_transform(i, per_slot_t[s][i]);
			mm->set_instance_color(i, per_slot_c[s][i]);
		}
	}
}

} // namespace godot
