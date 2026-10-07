// Terrain — Godot Node3D that builds and renders terrain meshes from CPT data.

#include "terrain/terrain.h"
#include "terrain/terrain_surface_inputs.h"
#include "terrain/terrain_tile_info.h"
#include "env/water.h"
#include "render/visual_layers.h"
#include "mission/mission_object_placer.h"


// Retail: PolyTrn_RenderTile @0x60da70 (docs/terrain/terrain-re.md, docs/tiles/til-re.md);
// the sector traversal names below are jodemo-era (Terrain_RenderSectorTile @0x5CDAA0,
// Terrain_TraverseQuadTreeNode @0x5C89C0, Terrain_CollectVisibleSectors @0x5C9120).

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include "object/object_model.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <cmath>
#include <cstring>
#include <utility>

using namespace godot;

void Terrain::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_light_context", "scene", "time_ms"),
		&Terrain::set_light_context);
	ClassDB::bind_method(D_METHOD("get_light_patches_lit"),
		&Terrain::get_light_patches_lit);
	ClassDB::bind_method(D_METHOD("get_light_rows_total"),
		&Terrain::get_light_rows_total);
	ClassDB::bind_method(D_METHOD("set_terrain_data", "data"), &Terrain::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"), &Terrain::get_terrain_data);
	ClassDB::bind_method(D_METHOD("get_surface_inputs"), &Terrain::get_surface_inputs);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data", PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"),
		"set_terrain_data", "get_terrain_data");

	ClassDB::bind_method(D_METHOD("set_lod_quality", "quality"), &Terrain::set_lod_quality);
	ClassDB::bind_method(D_METHOD("get_lod_quality"), &Terrain::get_lod_quality);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_quality", PROPERTY_HINT_RANGE, "0.1,4.0,0.1"),
		"set_lod_quality", "get_lod_quality");

	ClassDB::bind_method(D_METHOD("set_tile_info_override", "tile_info"), &Terrain::set_tile_info_override);
	ClassDB::bind_method(D_METHOD("get_tile_info_override"), &Terrain::get_tile_info_override);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "tile_info_override", PROPERTY_HINT_RESOURCE_TYPE, "TerrainTileInfo"),
		"set_tile_info_override", "get_tile_info_override");

	ClassDB::bind_method(D_METHOD("set_environment_path", "path"), &Terrain::set_environment_path);
	ClassDB::bind_method(D_METHOD("get_environment_path"), &Terrain::get_environment_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "environment_path"),
		"set_environment_path", "get_environment_path");

	ClassDB::bind_method(D_METHOD("set_weather_path", "path"), &Terrain::set_weather_path);
	ClassDB::bind_method(D_METHOD("get_weather_path"), &Terrain::get_weather_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_path"),
		"set_weather_path", "get_weather_path");
	ClassDB::bind_method(D_METHOD("set_water_path", "path"), &Terrain::set_water_path);
	ClassDB::bind_method(D_METHOD("get_water_path"), &Terrain::get_water_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "water_path"),
		"set_water_path", "get_water_path");
	ClassDB::bind_method(D_METHOD("get_terrain_material"),
		&Terrain::get_terrain_material);
	ClassDB::bind_method(D_METHOD("get_tile_cache_texture"),
		&Terrain::get_tile_cache_texture);
	ClassDB::bind_method(D_METHOD("get_tile_cache_diagnostics"),
		&Terrain::get_tile_cache_diagnostics);
	ClassDB::bind_method(D_METHOD("invalidate_tile_cache_region", "minimum_x_q16",
			"minimum_z_q16", "maximum_x_q16", "maximum_z_q16"),
			&Terrain::invalidate_tile_cache_region);
	ClassDB::bind_method(D_METHOD("append_terrain_scorch", "texture_index",
			"minimum_x_q16", "minimum_z_q16", "maximum_x_q16",
			"maximum_z_q16"), &Terrain::append_terrain_scorch);
	ClassDB::bind_method(D_METHOD("clear_terrain_scorches"),
			&Terrain::clear_terrain_scorches);
	ClassDB::bind_method(
		D_METHOD("set_tile_cache_capture_diagnostics", "enabled"),
		&Terrain::set_tile_cache_capture_diagnostics);
	ClassDB::bind_method(D_METHOD("set_static_shadow_placer", "placer"),
		&Terrain::set_static_shadow_placer);
	ClassDB::bind_method(D_METHOD("set_static_terrain_shadow_enabled", "enabled"),
		&Terrain::set_static_terrain_shadow_enabled);
	ClassDB::bind_method(D_METHOD("is_static_terrain_shadow_enabled"),
		&Terrain::is_static_terrain_shadow_enabled);
	ClassDB::bind_method(D_METHOD("set_suppressed_static_shadow_bms_ids", "bms_ids"),
		&Terrain::set_suppressed_static_shadow_bms_ids);
	ClassDB::bind_method(D_METHOD("get_suppressed_static_shadow_bms_ids"),
		&Terrain::get_suppressed_static_shadow_bms_ids);

	ClassDB::bind_method(D_METHOD("build"), &Terrain::build);
	ClassDB::bind_method(D_METHOD("build_begin"), &Terrain::build_begin);
	ClassDB::bind_method(D_METHOD("build_step"), &Terrain::build_step);
	ClassDB::bind_method(D_METHOD("get_build_step_count"), &Terrain::get_build_step_count);
	ClassDB::bind_method(D_METHOD("get_build_steps_done"), &Terrain::get_build_steps_done);
	ClassDB::bind_method(D_METHOD("get_build_step_label"), &Terrain::get_build_step_label);
	ClassDB::bind_method(D_METHOD("is_built"), &Terrain::is_built);
	ClassDB::bind_method(D_METHOD("render_frame"), &Terrain::render_frame);
	ClassDB::bind_method(D_METHOD("render_inset_frame", "camera"), &Terrain::render_inset_frame);
	ClassDB::bind_method(D_METHOD("release_inset_frame"), &Terrain::release_inset_frame);
	ClassDB::bind_method(D_METHOD("is_inset_frame_live"), &Terrain::is_inset_frame_live);
	ClassDB::bind_method(D_METHOD("get_visible_patch_layers", "inset"),
		&Terrain::get_visible_patch_layers);

	// Debug API
	ClassDB::bind_method(D_METHOD("get_visible_patch_count"), &Terrain::get_visible_patch_count);
	ClassDB::bind_method(D_METHOD("get_patches_active"), &Terrain::get_patches_active);
	ClassDB::bind_method(D_METHOD("has_visible_terrain_bounds"),
			&Terrain::has_visible_terrain_bounds);

	ClassDB::bind_method(D_METHOD("set_debug_no_frustum", "enabled"), &Terrain::set_debug_no_frustum);
	ClassDB::bind_method(D_METHOD("get_debug_no_frustum"), &Terrain::get_debug_no_frustum);
	ClassDB::bind_method(D_METHOD("set_debug_no_nearfar", "enabled"), &Terrain::set_debug_no_nearfar);
	ClassDB::bind_method(D_METHOD("get_debug_no_nearfar"), &Terrain::get_debug_no_nearfar);
	ClassDB::bind_method(D_METHOD("set_debug_no_sideplanes", "enabled"), &Terrain::set_debug_no_sideplanes);
	ClassDB::bind_method(D_METHOD("get_debug_no_sideplanes"), &Terrain::get_debug_no_sideplanes);
	ClassDB::bind_method(D_METHOD("set_debug_no_partial_subdiv", "enabled"), &Terrain::set_debug_no_partial_subdiv);
	ClassDB::bind_method(D_METHOD("get_debug_no_partial_subdiv"), &Terrain::get_debug_no_partial_subdiv);
	ClassDB::bind_method(D_METHOD("set_debug_force_leaves", "enabled"), &Terrain::set_debug_force_leaves);
	ClassDB::bind_method(D_METHOD("get_debug_force_leaves"), &Terrain::get_debug_force_leaves);
	ClassDB::bind_method(D_METHOD("set_debug_force_lod0", "enabled"), &Terrain::set_debug_force_lod0);
	ClassDB::bind_method(D_METHOD("get_debug_force_lod0"), &Terrain::get_debug_force_lod0);

	ClassDB::bind_method(D_METHOD("set_debug_mode", "mode"), &Terrain::set_debug_mode);
	ClassDB::bind_method(D_METHOD("get_debug_mode"), &Terrain::get_debug_mode);
	ClassDB::bind_method(D_METHOD("set_ground_overlay", "image", "rect", "outside", "outside_on"),
			&Terrain::set_ground_overlay);
	ClassDB::bind_method(D_METHOD("clear_ground_overlay"), &Terrain::clear_ground_overlay);
	ClassDB::bind_method(D_METHOD("has_ground_overlay"), &Terrain::has_ground_overlay);
	BIND_ENUM_CONSTANT(DEBUG_MODE_NORMAL);
	BIND_ENUM_CONSTANT(DEBUG_MODE_LOD_COLORS);
	BIND_ENUM_CONSTANT(DEBUG_MODE_SECTOR_COLORS);
	BIND_ENUM_CONSTANT(DEBUG_MODE_NORMALS);
	BIND_ENUM_CONSTANT(DEBUG_MODE_HEIGHTMAP);

	BIND_ENUM_CONSTANT(BUILD_STEP_MORE);
	BIND_ENUM_CONSTANT(BUILD_STEP_DONE);
	BIND_ENUM_CONSTANT(BUILD_STEP_FAILED);

	ADD_GROUP("Debug", "debug_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_mode", PROPERTY_HINT_ENUM,
		"Normal,LOD Colors,Sector Colors,Normals,Heightmap"),
		"set_debug_mode", "get_debug_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_no_frustum"), "set_debug_no_frustum", "get_debug_no_frustum");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_no_nearfar"), "set_debug_no_nearfar", "get_debug_no_nearfar");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_no_sideplanes"), "set_debug_no_sideplanes", "get_debug_no_sideplanes");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_no_partial_subdiv"), "set_debug_no_partial_subdiv", "get_debug_no_partial_subdiv");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_force_leaves"), "set_debug_force_leaves", "get_debug_force_leaves");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_force_lod0"), "set_debug_force_lod0", "get_debug_force_lod0");
}

Terrain::Terrain() {
	surface_inputs.instantiate();
	// Authored terrain rides the ordinary world layer; the flat fallback its
	// own bit alone, like the foliage blanket: the beauty camera admits it and
	// the water mirror excludes it, because the retail mirror prerender view
	// skips empty sectors whenever the mission has water while the live view
	// draws them (docs/terrain/terrain-re.md, "Empty-sector flat fallback").
	// The Inset's own pool rides INSET_VIEW whole; the Inset camera excludes
	// the flat-fallback bit, and the main pool swaps WORLD for MAIN_VIEW while
	// the Inset renders (render_inset_frame).
	main_pool.world_layer = visual_layers::WORLD;
	main_pool.flat_layer = visual_layers::TERRAIN_FLAT_FALLBACK;
	inset_pool.world_layer = visual_layers::INSET_VIEW;
	inset_pool.flat_layer = visual_layers::INSET_VIEW;
}

Terrain::~Terrain() {
	tile_cache_device.set_static_shadow_rasterizer(nullptr);
	static_shadow_rasterizer.set_mission_object_placer({});
	if (tile_info_override.is_valid()) {
		const Callable changed = callable_mp(this, &Terrain::_on_tile_info_changed);
		if (tile_info_override->is_connected("changed", changed)) {
			tile_info_override->disconnect("changed", changed);
		}
	}
	_clear_patch_pool();
}

void Terrain::set_terrain_data(const Ref<TerrainData> &p_data) {
	if (terrain_data.is_valid() && terrain_data->is_connected("terrain_changed", callable_mp(this, &Terrain::_on_terrain_changed))) {
		terrain_data->disconnect("terrain_changed", callable_mp(this, &Terrain::_on_terrain_changed));
	}
	// A stepped build in flight is of the old data: dropped with what it made so far (its next
	// build_step fails; a build of the new data begins again).
	if (building_) {
		_clear_terrain();
		building_ = false;
		build_done_ = 0;
		build_total_ = 0;
	}
	terrain_data = p_data;
	static_shadow_rasterizer.set_terrain_data(p_data);
	surface_inputs->set_terrain_data(p_data);
	if (terrain_data.is_valid()) {
		terrain_data->connect("terrain_changed", callable_mp(this, &Terrain::_on_terrain_changed));
	}
}

void Terrain::set_static_shadow_placer(
		const Ref<MissionObjectPlacer> &p_placer) {
	static_shadow_rasterizer.set_mission_object_placer(p_placer);
	tile_cache_device.set_static_shadow_rasterizer(
			p_placer.is_valid() ? &static_shadow_rasterizer : nullptr);
	tile_cache_device.invalidate_static_shadow_pages();
}

void Terrain::set_static_terrain_shadow_enabled(bool p_enabled) {
	if (static_shadow_rasterizer.is_enabled() == p_enabled) return;
	static_shadow_rasterizer.set_enabled(p_enabled);
	tile_cache_device.invalidate_static_shadow_pages();
}

bool Terrain::is_static_terrain_shadow_enabled() const {
	return static_shadow_rasterizer.is_enabled();
}

void Terrain::set_suppressed_static_shadow_bms_ids(
		const PackedInt32Array &p_bms_ids) {
	PackedInt32Array normalized = p_bms_ids;
	normalized.sort();
	PackedInt32Array unique;
	for (int index = 0; index < normalized.size(); ++index) {
		if (index == 0 || normalized[index] != normalized[index - 1]) {
			unique.push_back(normalized[index]);
		}
	}
	if (static_shadow_rasterizer.get_suppressed_bms_ids() == unique) return;
	static_shadow_rasterizer.set_suppressed_bms_ids(unique);
	tile_cache_device.invalidate_static_shadow_pages();
}

PackedInt32Array Terrain::get_suppressed_static_shadow_bms_ids() const {
	return static_shadow_rasterizer.get_suppressed_bms_ids();
}

Ref<TerrainData> Terrain::get_terrain_data() const {
	return terrain_data;
}

Ref<TerrainSurfaceInputs> Terrain::get_surface_inputs() const {
	return surface_inputs;
}

Ref<Texture2D> Terrain::get_heightfield_normal_texture() const {
	return surface_inputs->get_heightfield_normal_texture();
}

Ref<Texture2DArray> Terrain::get_tile_cache_texture() const {
	return tile_cache_device.get_texture();
}

void Terrain::set_tile_cache_capture_diagnostics(bool p_enabled) {
	tile_cache_device.set_capture_diagnostics(p_enabled);
}

Dictionary Terrain::get_tile_cache_diagnostics() const {
	Dictionary diagnostics = tile_cache_device.get_diagnostics();
	diagnostics["capture_diagnostics"] =
			tile_cache_device.is_capture_diagnostics_enabled();
	const Dictionary provider = static_shadow_rasterizer.get_diagnostics();
	const Array keys = provider.keys();
	for (int index = 0; index < keys.size(); ++index) {
		const Variant key = keys[index];
		diagnostics[String("shadow_provider_") + String(key)] = provider[key];
	}
	return diagnostics;
}

bool Terrain::append_terrain_scorch(int64_t p_texture_index,
		int64_t p_minimum_x_q16, int64_t p_minimum_z_q16,
		int64_t p_maximum_x_q16, int64_t p_maximum_z_q16) {
	if (p_texture_index < 0 || p_texture_index > UINT8_MAX ||
			p_minimum_x_q16 < INT32_MIN || p_minimum_x_q16 > INT32_MAX ||
			p_minimum_z_q16 < INT32_MIN || p_minimum_z_q16 > INT32_MAX ||
			p_maximum_x_q16 < INT32_MIN || p_maximum_x_q16 > INT32_MAX ||
			p_maximum_z_q16 < INT32_MIN || p_maximum_z_q16 > INT32_MAX) {
		return false;
	}
	return tile_cache_device.append_terrain_scorch({
			static_cast<uint8_t>(p_texture_index),
			static_cast<int32_t>(p_minimum_x_q16),
			static_cast<int32_t>(p_minimum_z_q16),
			static_cast<int32_t>(p_maximum_x_q16),
			static_cast<int32_t>(p_maximum_z_q16)});
}

void Terrain::clear_terrain_scorches() {
	tile_cache_device.clear_terrain_scorches();
}

int64_t Terrain::invalidate_tile_cache_region(int64_t p_minimum_x_q16,
		int64_t p_minimum_z_q16, int64_t p_maximum_x_q16,
		int64_t p_maximum_z_q16) {
	if (p_minimum_x_q16 < INT32_MIN || p_minimum_x_q16 > INT32_MAX ||
			p_minimum_z_q16 < INT32_MIN || p_minimum_z_q16 > INT32_MAX ||
			p_maximum_x_q16 < INT32_MIN || p_maximum_x_q16 > INT32_MAX ||
			p_maximum_z_q16 < INT32_MIN || p_maximum_z_q16 > INT32_MAX) {
		return 0;
	}
	return static_cast<int64_t>(tile_cache_device.invalidate_region(
			static_cast<int32_t>(p_minimum_x_q16),
			static_cast<int32_t>(p_minimum_z_q16),
			static_cast<int32_t>(p_maximum_x_q16),
			static_cast<int32_t>(p_maximum_z_q16)));
}

std::optional<opennova::TerrainTilePageBinding>
Terrain::get_tile_cache_binding_for_world_point_native(
		float p_world_x, float p_world_z) {
	if (!std::isfinite(p_world_x) || !std::isfinite(p_world_z)) {
		return std::nullopt;
	}
	return tile_cache_device.lookup(
			opennova::TerrainTileResidentPoint{p_world_x, p_world_z});
}

void Terrain::set_lod_quality(float p_quality) {
	lod_quality = p_quality;
}

float Terrain::get_lod_quality() const {
	return lod_quality;
}

void Terrain::set_tile_info_override(const Ref<TerrainTileInfo> &p_info) {
	const Callable changed = callable_mp(this, &Terrain::_on_tile_info_changed);
	if (tile_info_override.is_valid() &&
			tile_info_override->is_connected("changed", changed)) {
		tile_info_override->disconnect("changed", changed);
	}
	tile_info_override = p_info;
	if (tile_info_override.is_valid()) {
		tile_info_override->connect("changed", changed);
	}
	surface_inputs->set_tile_info_override(p_info);
	if (built) {
		_rebuild_tile_overlay_pages();
	}
}

Ref<TerrainTileInfo> Terrain::get_tile_info_override() const {
	return tile_info_override;
}

void Terrain::set_environment_path(const NodePath& p_path) {
	environment_path = p_path;
	terrain_node_cache_valid = false;
}

NodePath Terrain::get_environment_path() const {
	return environment_path;
}

void Terrain::set_weather_path(const NodePath& p_path) {
	weather_path = p_path;
	terrain_node_cache_valid = false;
}

NodePath Terrain::get_weather_path() const {
	return weather_path;
}

void Terrain::set_water_path(const NodePath& p_path) {
	water_path = p_path;
	terrain_node_cache_valid = false;
}

NodePath Terrain::get_water_path() const {
	return water_path;
}


// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

void Terrain::_notification(int p_what) {
	if (p_what == NOTIFICATION_VISIBILITY_CHANGED) {
		if (!is_visible_in_tree()) {
			_hide_visible_patches();
		}
	} else if (p_what == NOTIFICATION_READY) {
		// Auto-load and auto-build if terrain_data is configured
		if (terrain_data.is_valid() && !terrain_data->is_loaded()) {
			Error err = terrain_data->load();
			if (err != OK) {
				UtilityFunctions::push_warning("Terrain: Failed to auto-load terrain data");
				return;
			}
		}
		if (terrain_data.is_valid() && terrain_data->is_loaded() && !built) {
			build();
		}
		terrain_node_cache_valid = false;
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		_clear_patch_pool();
		cached_env_node = nullptr;
		cached_weather_node = nullptr;
		cached_water_node = nullptr;
		terrain_node_cache_valid = false;
	}
}

// ---------------------------------------------------------------------------
// The terrain frame leg (ADR 0033 R2): the engine compiles the draw list, this
// node applies it. Every per-frame decision — sector window, traversal,
// foliage handoff, order, budget, family resolve — is TerrainFrameCompiler's;
// what remains here is device work (camera sampling, instance-pool writes,
// material pushes).
// ---------------------------------------------------------------------------

void Terrain::render_frame() {
	// Invalidate up front: a frame with no usable camera must report no
	// cells rather than leaving a prior camera's draw list live for the
	// foliage dispatcher.
	frame_draw_list_live = false;
	frame_bounds_live = false;
	const bool hidden = !is_visible_in_tree();
	if (hidden) {
		_hide_visible_patches();
	}
	if (!built) {
		return;
	}
	// Sample the scene camera — the one device input the compiler needs.
	Camera3D* cam = nullptr;
	Viewport* vp = get_viewport();
	if (vp) cam = vp->get_camera_3d();
	if (!cam || !cam->is_inside_tree()) {
		return;
	}
	const opennova::TerrainViewInput view_input = _view_input_for(cam);
	if (hidden) {
		// A hidden terrain draws nothing (the indoors letter skips the
		// traversal and the sector pass), but the frame's bounds walk still
		// feeds the water leg's water-active test
		// (opennova::track_terrain_visible_bounds).
		frame_bounds = opennova::track_terrain_visible_bounds(scene_snapshot, view_input);
		frame_bounds_live = true;
		return;
	}
	const opennova::TerrainDrawList &draw_list =
			frame_compiler.compile(scene_snapshot, view_input);
	frame_draw_list_live = true;
	frame_bounds = draw_list.visible_bounds;
	frame_bounds_live = true;
	_apply_frame_draw_list(draw_list);
}

// The camera sample the compiler needs, for the main frame and the weapon
// Inset pass's own traversal alike.
opennova::TerrainViewInput Terrain::_view_input_for(Camera3D *p_camera) {
	// The RENDER eye (get_camera_transform includes h/v offsets), so the
	// below-water classification stays coherent with Water's surface flip and
	// the frame clear — the same eye those classifiers sample. Offsets are
	// zero for common cameras, so traversal is unchanged in practice.
	const Transform3D cam_xform = p_camera->get_camera_transform();
	const Vector3 cam_pos = cam_xform.origin;
	const Transform3D view = cam_xform.affine_inverse();
	const Projection proj = p_camera->get_camera_projection();

	if (!terrain_node_cache_valid)
		_cache_env_weather_nodes();

	opennova::TerrainViewInput view_input;
	view_input.cam_x = static_cast<float>(cam_pos.x);
	view_input.cam_y = static_cast<float>(cam_pos.y);
	view_input.cam_z = static_cast<float>(cam_pos.z);
	// The map's live water height, unconditionally: retail's compare reads
	// g_EnvWaterHeightFixed with no render gate, so the engine side gets the
	// authored value whenever a Water node exists (a missing node passes 0,
	// and non-negative terrain keeps a dry map's compare inert).
	// [orig: cameraY < g_EnvWaterHeightFixed @0x60fea5, no zero guard —
	//  see docs/terrain/terrain-re.md, the underwater selector section]
	view_input.water_height = cached_water_node != nullptr
			? cached_water_node->get_water_height()
			: 0.0f;

	// Column-major view matrix from the camera's inverse transform.
	const Basis& b = view.basis;
	const Vector3& o = view.origin;
	view_input.view[0]  = b[0][0]; view_input.view[1]  = b[1][0]; view_input.view[2]  = b[2][0]; view_input.view[3]  = 0;
	view_input.view[4]  = b[0][1]; view_input.view[5]  = b[1][1]; view_input.view[6]  = b[2][1]; view_input.view[7]  = 0;
	view_input.view[8]  = b[0][2]; view_input.view[9]  = b[1][2]; view_input.view[10] = b[2][2]; view_input.view[11] = 0;
	view_input.view[12] = o.x;     view_input.view[13] = o.y;     view_input.view[14] = o.z;     view_input.view[15] = 1;

	// The terrain walk never reads the projection: it rebuilds its clip cone
	// from the horizontal FOV, which the symmetric projection's first column
	// carries as 1/tan(fov/2) whatever the camera's keep-aspect mode.
	const double p00 = static_cast<double>(proj.columns[0][0]);
	if (p00 > 0.0)
		view_input.fov_deg = static_cast<float>(
				2.0 * std::atan(1.0 / p00) * 180.0 / 3.14159265358979323846);
	// The frame's view distance is the integer part of the live fog distance,
	// the same env scalar the occlusion frame and the priority score read.
	if (cached_env_node != nullptr)
		view_input.far_distance = std::floor(cached_env_node->get_fog_distance());
	view_input.polygon_detail = polygon_detail;

	traversal_config.quality = lod_quality;
	view_input.config = traversal_config;
	return view_input;
}

// The weapon Inset pass's own terrain frame (the engine compiler carries the
// witness order): its traversal on the second compiler, its page sweep as its
// own frame of the shared cache, its own pool and light rows on INSET_VIEW,
// and the main pool moved to MAIN_VIEW for as long as the Inset renders. The
// traversal takes the main frame's indoors gate, as the Inset's does: a hidden
// terrain draws no Inset patches.
void Terrain::render_inset_frame(Camera3D *p_camera) {
	if (!is_visible_in_tree() || !built || terrain_material.is_null() ||
			p_camera == nullptr || !p_camera->is_inside_tree()) {
		release_inset_frame();
		return;
	}
	const opennova::TerrainDrawList &draw_list =
			inset_frame_compiler.compile(scene_snapshot, _view_input_for(p_camera));
	inset_draw_list_live = true;
	_sync_inset_material();
	if (!inset_pool.instances[0].is_valid()) {
		_create_pool_instances(inset_pool, inset_material->get_rid());
	}
	_set_pool_world_layer(main_pool, visual_layers::MAIN_VIEW);
	_apply_pool(inset_pool, draw_list, _compose_pages(draw_list, false));
	int rows_total = 0;
	_render_light_rows(draw_list, inset_pool.rows, inset_material, rows_total);
	// The Inset eye's own below-water side (its traversal's flag).
	inset_material->set_shader_parameter("u_below_water", draw_list.below_water);
}

void Terrain::release_inset_frame() {
	inset_draw_list_live = false;
	_set_pool_world_layer(main_pool, visual_layers::WORLD);
	if (inset_pool.instances[0].is_valid()) {
		_free_pool_instances(inset_pool);
	}
}

// The main pool's authored patches swap layers in place (the flat fallback
// keeps its own bit); a slot reused later takes the pool's layer on its next
// stamp.
void Terrain::_set_pool_world_layer(PatchPool &r_pool, uint32_t p_layer) {
	if (r_pool.world_layer == p_layer) {
		return;
	}
	r_pool.world_layer = p_layer;
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	for (int i = 0; i < r_pool.active; ++i) {
		if (!r_pool.instances[i].is_valid() || !r_pool.uniforms_stamped[i] ||
				r_pool.last_zero_height[i]) {
			continue;
		}
		rs->instance_set_layer_mask(r_pool.instances[i], p_layer);
		r_pool.last_layer[i] = p_layer;
	}
}

// The Inset pool's material follows the main one: every parameter the main
// frame wrote (surface inputs, pages, light textures, fog/env, debug mode,
// water noise) syncs on change, except the view's own light rows and
// below-water flag.
void Terrain::_sync_inset_material() {
	const ObjectID source(terrain_material->get_instance_id());
	if (inset_material.is_null() || inset_material_source != source) {
		inset_material = terrain_material->duplicate();
		inset_material_source = source;
		inset_synced_parameters.clear();
		if (terrain_shader.is_valid()) {
			const Array uniforms = terrain_shader->get_shader_uniform_list();
			for (int64_t index = 0; index < uniforms.size(); ++index) {
				const Dictionary uniform = uniforms[index];
				const StringName name = uniform.get("name", StringName());
				if (name == StringName("u_terrain_light_rows") ||
						name == StringName("u_terrain_light_enabled") ||
						name == StringName("u_below_water")) {
					continue;
				}
				inset_synced_parameters.push_back(name);
			}
		}
		inset_pool.rows.enabled_written = -1;
		_ensure_light_rows(inset_pool.rows);
		inset_material->set_shader_parameter("u_terrain_light_rows", inset_pool.rows.texture);
		inset_material->set_shader_parameter("u_terrain_light_enabled", false);
		inset_pool.rows.enabled_written = 0;
		if (RenderingServer *rs = RenderingServer::get_singleton()) {
			for (int i = 0; i < PATCH_POOL_SIZE; ++i) {
				if (inset_pool.instances[i].is_valid()) {
					rs->instance_geometry_set_material_override(inset_pool.instances[i],
							inset_material->get_rid());
				}
			}
		}
	}
	for (const StringName &name : inset_synced_parameters) {
		const Variant value = terrain_material->get_shader_parameter(name);
		if (inset_material->get_shader_parameter(name) != value) {
			inset_material->set_shader_parameter(name, value);
		}
	}
}

const std::vector<FoliageDetailPatch> &Terrain::get_inset_foliage_detail_patches_native() const {
	static const std::vector<FoliageDetailPatch> empty;
	return inset_draw_list_live ? inset_frame_compiler.last_draw_list().detail_cells : empty;
}

PackedInt32Array Terrain::get_visible_patch_layers(bool p_inset) const {
	const PatchPool &pool = p_inset ? inset_pool : main_pool;
	PackedInt32Array layers;
	for (int i = 0; i < PATCH_POOL_SIZE; ++i) {
		if (pool.visible[i]) {
			layers.push_back(static_cast<int32_t>(pool.last_layer[i]));
		}
	}
	return layers;
}

// The device half of the main terrain frame: the composed pages, the patch
// pool, the light rows and the shared material's per-frame inputs.
void Terrain::_apply_frame_draw_list(const opennova::TerrainDrawList &draw_list) {
	_apply_pool(main_pool, draw_list, _compose_pages(draw_list, true));
	// The light-pool re-draw rows for exactly this draw list's patches.
	light_patches_lit = _render_light_rows(draw_list, main_pool.rows, terrain_material,
			light_rows_total);

	// Update shader parameters on the single shared material
	if (terrain_material.is_valid()) {
		terrain_material->set_shader_parameter("u_debug_mode", static_cast<int>(debug_mode));
	}

	// The below-water modulation inputs (D-TERRAIN-8,
	// docs/terrain/terrain-re.md underwater section): the engine-compiled
	// flag plus the water module's live noise texture. The shared ImageTexture
	// updates in place per frame, so the bind sticks until the node changes.
	if (terrain_material.is_valid()) {
		terrain_material->set_shader_parameter(
				"u_below_water", draw_list.below_water);
		Ref<Texture2D> noise;
		if (cached_water_node != nullptr &&
				cached_water_node->is_water_render_active()) {
			noise = cached_water_node->get_noise_color_texture();
		}
		if (noise != bound_water_noise) {
			terrain_material->set_shader_parameter("u_water_noise", noise);
			bound_water_noise = noise;
		}
	}

	// Update lighting from MissionEnvironment — native, direct typed calls
	// (ADR 0034 d6).
	if (terrain_material.is_valid()) {
		if (cached_env_node && cached_env_node->is_loaded()) {
			// The env -> terrain-uniform push, shared with the editor preview
			// (MissionEnvironment.apply_terrain_uniforms drives both shaders'
			// uniforms). With a weather node the env already holds the tick's
			// written-back smoothed currents, and its per-pass builder adds
			// what the raw smoother lacks: the NVG sky blend, the thermal
			// ramps, the underwater g_EnvWaterColorLit fog. The terrain surface
			// consumes only c1 = light + c0 = sky [orig: @ 0x604420, see
			// docs/terrain/terrain-re.md].
			cached_env_node->apply_terrain_uniforms(terrain_material);
		}
	}
}

// One traversal's page sweep over the shared cache: each is its own PolyTrn
// frame (the engine cache's begin_frame carries the witness).
const std::vector<opennova::TerrainTilePageBinding> &Terrain::_compose_pages(
		const opennova::TerrainDrawList &draw_list, bool p_display_frame_start) {
	// The portable cache owns page identity/composition decisions; the device
	// supplies the environment bytes that are actually baked into each page.
	// Quantization inside the adapter prevents sub-byte weather drift from
	// invalidating the whole working set. The page path consumes the RAW
	// getter tuple: the static projector's (g2,g1,g0) reduction and the DOT3
	// (g2,g0,g1) byte pack are both ports of retail's packing of that tuple
	// [orig: Environment_GetLightDirectionFloat @0x57D870 read by the
	// collector @0x60D2F5/0x60D2FF and the tile DOT3 pack @0x60E231..0x60E331;
	// see docs/terrain/terrain-re.md] — never the Godot-axes vector
	// get_light_direction() serves, which would swap x/z a second time.
	Vector3 page_tile_tint(1.0f, 1.0f, 1.0f);
	// The unseeded 45-degree default in GODOT axes; EnvironmentState's
	// kDefaultSunDirRender is the same numbers in the render basis, which after
	// the x/z swap points up+east while this points up+south (open grill item).
	Vector3 page_light_direction(0.0f, 0.70710678f, 0.70710678f);
	if (cached_env_node && cached_env_node->is_loaded()) {
		page_tile_tint = cached_env_node->get_tile_overlay_tint();
		page_light_direction =
				cached_env_node->get_light_direction_render_tuple();
	}
	if (p_display_frame_start) {
		static_shadow_rasterizer.begin_frame(page_light_direction,
				light_time_ms < 0 ? 0u : static_cast<uint32_t>(light_time_ms));
		// The pages a changed caster's shadow touched or touches, composed
		// again below (only while the casters are followed: the editor's
		// mission device). Mission (x, y) is Godot (x, -z).
		for (const opennova::terrain::TerrainStaticShadowReach &reach :
				static_shadow_rasterizer.take_changed_reaches()) {
			tile_cache_device.invalidate_region(reach.min_x, -reach.max_y,
					reach.max_x, -reach.min_y);
		}
	}
	// The page claims stamp the weather clock's TOD epoch (g_EnvTodEpoch,
	// one step per 311 logic ticks); a page whose stamp falls behind is
	// refreshed on an all-hit frame.
	const uint32_t tod_epoch = cached_env_node != nullptr &&
					cached_env_node->state().weather() != nullptr
			? cached_env_node->state().weather()->tod_epoch
			: 0u;
	tile_cache_device.begin_frame(++page_sweep_id, tod_epoch);
	// Every missing visible page composes before the patches draw.
	return tile_cache_device.compose_frame(draw_list, page_tile_tint, page_light_direction);
}

// Apply one view's draw list onto its instance pool: draw-list index == pool
// slot.
void Terrain::_apply_pool(PatchPool &r_pool, const opennova::TerrainDrawList &draw_list,
		const std::vector<opennova::TerrainTilePageBinding> &pages) {
	RenderingServer* rs = RenderingServer::get_singleton();
	const int count = static_cast<int>(draw_list.patches.size());

	for (int i = 0; i < count; i++) {
		const opennova::TerrainPatchDraw &draw = draw_list.patches[i];
		const auto& ti = tile_infos[draw.tile_index];
		const Ref<ArrayMesh> &mesh = draw.zero_height
				? ti.flat_lod_meshes[draw.lod_family] : ti.lod_meshes[draw.lod_family];
		const RID instance = r_pool.instances[i];
		if (mesh.is_null()) {
			// The compiler resolved the family against the same index counts
			// the mesh build used; a null here means the two went out of sync.
			if (r_pool.visible[i]) {
				rs->instance_set_visible(instance, false);
				r_pool.visible[i] = false;
			}
			continue;
		}

		// Only update mesh if changed
		RID mesh_rid = mesh->get_rid();
		if (mesh_rid != r_pool.last_mesh_rid[i]) {
			rs->instance_set_base(instance, mesh_rid);
			r_pool.last_mesh_rid[i] = mesh_rid;
		}

		// Only update transform if changed
		Transform3D xform(Basis(), Vector3(draw.sector_ox, 0.0f, draw.sector_oz));
		if (xform != r_pool.last_transform[i]) {
			rs->instance_set_transform(instance, xform);
			r_pool.last_transform[i] = xform;
		}

		// Per-instance uniforms: written on change only (the mesh/transform
		// gates above already work that way).
		const bool fresh = !r_pool.uniforms_stamped[i];
		if (fresh || draw.zero_height != r_pool.last_zero_height[i]) {
			rs->instance_geometry_set_shader_parameter(
					instance, "u_instance_zero_height", draw.zero_height);
			r_pool.last_zero_height[i] = draw.zero_height;
		}
		// The pool's layer for this patch (the constructor documents them).
		const uint32_t layer_mask = draw.zero_height ? r_pool.flat_layer : r_pool.world_layer;
		if (fresh || layer_mask != r_pool.last_layer[i]) {
			rs->instance_set_layer_mask(instance, layer_mask);
			r_pool.last_layer[i] = layer_mask;
		}
		const Vector2 quadrant(static_cast<float>(draw.quadrant_x),
				static_cast<float>(draw.quadrant_z));
		if (fresh || quadrant != r_pool.last_quadrant[i]) {
			rs->instance_geometry_set_shader_parameter(
				instance, "u_instance_source_quadrant", quadrant);
			r_pool.last_quadrant[i] = quadrant;
		}

		const opennova::TerrainTilePageBinding &page = pages[i];
		const std::optional<opennova::TerrainTilePageProjection> projection =
				page.ready
						? opennova::TerrainTileCompositionCache::page_projection(
								page.page, draw.zero_height)
						: std::nullopt;
		const bool ready = projection.has_value();
		if (fresh || ready != r_pool.last_page_ready[i]) {
			rs->instance_geometry_set_shader_parameter(
					instance, "u_instance_tile_cache_ready", ready);
			r_pool.last_page_ready[i] = ready;
		}
		if (ready) {
			const float layer = static_cast<float>(page.layer);
			const Vector4 projection_row(projection->world_origin_x,
					projection->world_origin_z,
					projection->inverse_world_span,
					projection->world_span);
			if (fresh || layer != r_pool.last_page_layer[i]) {
				rs->instance_geometry_set_shader_parameter(
						instance, "u_instance_tile_cache_layer", layer);
				r_pool.last_page_layer[i] = layer;
			}
			if (fresh || projection_row != r_pool.last_page_projection[i]) {
				rs->instance_geometry_set_shader_parameter(
						instance, "u_instance_tile_cache_projection",
						projection_row);
				r_pool.last_page_projection[i] = projection_row;
			}
		}
		r_pool.uniforms_stamped[i] = true;

		// Per-instance debug data (only set when a debug mode is active)
		if (debug_mode != DEBUG_MODE_NORMAL) {
			rs->instance_geometry_set_shader_parameter(instance,
				"u_instance_lod", static_cast<float>(draw.lod_family));
			rs->instance_geometry_set_shader_parameter(instance,
				"u_instance_sector_x", draw.sector_ox / 512.0f);
			rs->instance_geometry_set_shader_parameter(instance,
				"u_instance_sector_z", draw.sector_oz / 512.0f);
		}

		if (!r_pool.visible[i]) {
			rs->instance_set_visible(instance, true);
			r_pool.visible[i] = true;
		}
	}

	// Hide unused pool entries
	for (int i = count; i < r_pool.active; i++) {
		if (r_pool.visible[i]) {
			rs->instance_set_visible(r_pool.instances[i], false);
			r_pool.visible[i] = false;
		}
	}
	r_pool.active = count;
}

void Terrain::set_light_context(const Ref<LightScene> &p_scene, int p_time_ms) {
	light_scene = p_scene;
	light_time_ms = p_time_ms;
	if (light_scene.is_valid()) {
		_bind_light_textures();
	} else if (terrain_material.is_valid()) {
		terrain_material->set_shader_parameter("u_terrain_light_enabled", false);
		// The direct write IS the latched value: a later re-arm must push the
		// enable again, not compare against the stale 1.
		main_pool.rows.enabled_written = 0;
		light_patches_lit = 0;
		light_rows_total = 0;
		if (inset_material.is_valid()) {
			inset_material->set_shader_parameter("u_terrain_light_enabled", false);
			inset_pool.rows.enabled_written = 0;
		}
	}
}

void Terrain::_ensure_light_rows(LightRows &r_rows) {
	if (r_rows.texture.is_valid()) {
		return;
	}
	r_rows.bytes.resize(static_cast<int64_t>(LIGHT_ROWS_TEXELS) * PATCH_POOL_SIZE * 16);
	r_rows.bytes.fill(0);
	r_rows.image = Image::create_from_data(LIGHT_ROWS_TEXELS,
			PATCH_POOL_SIZE, false, Image::FORMAT_RGBAF, r_rows.bytes);
	r_rows.texture = ImageTexture::create_from_image(r_rows.image);
	r_rows.uploaded = r_rows.bytes.duplicate();
	r_rows.enabled_written = -1;
}

void Terrain::_bind_light_textures() {
	if (light_textures_bound || terrain_material.is_null()) {
		return;
	}
	// The procedural textures of the ps.1.1 terrain light pass, built once per
	// node: "texlight2d" 64x64 (both falloff stages sample it; clamped,
	// bilinear, no mips) and the cube-normalize map stage 0 samples
	// (renderer/light_terrain_pass.h carries the witness and the stage map).
	const int size = LightScene::terrain_light_texture_size();
	if (light_disc_texture.is_null()) {
		light_disc_texture = ImageTexture::create_from_image(
				Image::create_from_data(size, size, false, Image::FORMAT_RGBA8,
						LightScene::terrain_light_disc_rgba8()));
	}
	if (light_cube_texture.is_null()) {
		const int cube_size = LightScene::terrain_light_cube_size();
		TypedArray<Ref<Image>> faces;
		for (int face = 0; face < 6; ++face) {
			faces.push_back(Image::create_from_data(cube_size, cube_size, false,
					Image::FORMAT_RGBA8, LightScene::terrain_light_cube_face_rgba8(face)));
		}
		light_cube_texture.instantiate();
		light_cube_texture->create_from_images(faces);
	}
	// The main view's rows texture, fresh for this material (the Inset
	// material carries its own, render_inset_frame).
	main_pool.rows = LightRows();
	_ensure_light_rows(main_pool.rows);
	terrain_material->set_shader_parameter("u_terrain_light_disc",
			light_disc_texture);
	terrain_material->set_shader_parameter("u_terrain_light_cube",
			light_cube_texture);
	terrain_material->set_shader_parameter("u_terrain_light_rows",
			main_pool.rows.texture);
	light_textures_bound = true;
}

int Terrain::_render_light_rows(const opennova::TerrainDrawList &draw_list, LightRows &r_rows,
		const Ref<ShaderMaterial> &p_material, int &r_total) {
	r_total = 0;
	if (light_scene.is_null() || terrain_material.is_null() || p_material.is_null()) {
		return 0;
	}
	_bind_light_textures();
	_ensure_light_rows(r_rows);
	int patches_lit = 0;
	const int count = static_cast<int>(draw_list.patches.size());
	light_patch_bounds.resize(static_cast<size_t>(count));
	light_patch_rows.resize(static_cast<size_t>(count));
	for (int i = 0; i < count; i++) {
		const opennova::TerrainPatchDraw &draw = draw_list.patches[i];
		// The patch mesh's sector-local AABB at the family the draw resolved,
		// offset by its sector origin — the traverse_quadtree world AABB the
		// object pass's LightDrawContext shape takes.
		AABB local;
		if (draw.tile_index >= 0 &&
				static_cast<size_t>(draw.tile_index) < tile_infos.size()) {
			const Ref<ArrayMesh> &mesh =
					tile_infos[draw.tile_index].lod_meshes[draw.lod_family];
			if (mesh.is_valid()) {
				local = mesh->get_aabb();
			}
		}
		const Vector3 lo = local.position;
		const Vector3 hi = local.position + local.size;
		const float aabb_min[3] = {
			static_cast<float>(lo.x), static_cast<float>(lo.y),
			static_cast<float>(lo.z)
		};
		const float aabb_max[3] = {
			static_cast<float>(hi.x), static_cast<float>(hi.y),
			static_cast<float>(hi.z)
		};
		light_patch_bounds[i] = opennova::renderer::terrain_patch_light_bounds(
				aabb_min, aabb_max, draw.sector_ox, draw.sector_oz);
	}
	// EffectWorld_AmbientScale = the env light-state gain (the modulator
	// unpack the object pass feeds too); the recip factor unpacks the loaded
	// g_EnvTerrainColorRecip [orig: @0x5aa1ef..0x5aa23f].
	Vector3 gain(1.0f, 1.0f, 1.0f);
	uint32_t recip_packed = opennova::renderer::kTerrainFactorDefaultPacked;
	if (cached_env_node != nullptr) {
		const Ref<EnvLightState> light_state = cached_env_node->get_light_state();
		if (light_state.is_valid() && light_state->get_values().is_valid()) {
			gain = light_state->get_values()->get_gain();
		}
		if (cached_env_node->is_loaded()) {
			recip_packed = cached_env_node->state().terrain_color_recip_packed();
		}
	}
	const size_t total = light_scene->collect_terrain_light_rows(
			light_patch_bounds.data(), light_patch_bounds.size(), gain,
			light_time_ms, cached_weather_node, recip_packed,
			light_patch_rows.data());
	r_total = static_cast<int>(total);
	// Rewrite the whole rows image: a slot that lost its lights reads count 0.
	r_rows.bytes.fill(0);
	float *texels = reinterpret_cast<float *>(r_rows.bytes.ptrw());
	for (int i = 0; i < count; i++) {
		const opennova::renderer::TerrainLightPatchRows &rows = light_patch_rows[i];
		if (rows.count > 0) {
			++patches_lit;
		}
		float *row = texels + static_cast<size_t>(i) * LIGHT_ROWS_TEXELS * 4;
		for (size_t k = 0; k < rows.count; ++k) {
			const opennova::renderer::TerrainLightRow &light = rows.rows[k];
			float *posr = row + k * 8;
			float *color = posr + 4;
			// mission (x, y, z) -> Godot (x, z, -y): the same fold the pool's
			// object leg applies to its selected positions.
			posr[0] = light.position[0];
			posr[1] = light.position[2];
			posr[2] = -light.position[1];
			posr[3] = light.inv_scale;
			color[0] = light.pixel_rgb[0];
			color[1] = light.pixel_rgb[1];
			color[2] = light.pixel_rgb[2];
			color[3] = static_cast<float>(rows.count);
		}
	}
	// Upload only when the rows moved: a still camera under steady lights
	// rebuilds identical bytes every frame.
	const int64_t byte_count = r_rows.bytes.size();
	if (r_rows.uploaded.size() != byte_count ||
			std::memcmp(r_rows.uploaded.ptr(), r_rows.bytes.ptr(),
					static_cast<size_t>(byte_count)) != 0) {
		r_rows.image->set_data(LIGHT_ROWS_TEXELS, PATCH_POOL_SIZE, false,
				Image::FORMAT_RGBAF, r_rows.bytes);
		r_rows.texture->update(r_rows.image);
		r_rows.uploaded.resize(byte_count);
		std::memcpy(r_rows.uploaded.ptrw(), r_rows.bytes.ptr(),
				static_cast<size_t>(byte_count));
	}
	const int enabled = r_total > 0 ? 1 : 0;
	if (enabled != r_rows.enabled_written) {
		p_material->set_shader_parameter("u_terrain_light_enabled", enabled != 0);
		r_rows.enabled_written = enabled;
	}
	return patches_lit;
}

void Terrain::_cache_env_weather_nodes() {
	terrain_node_cache_valid = true;
	cached_env_node = nullptr;
	cached_weather_node = nullptr;
	cached_water_node = nullptr;

	if (!environment_path.is_empty())
		cached_env_node = Object::cast_to<MissionEnvironment>(
				get_node_or_null(environment_path));
	if (cached_env_node && !weather_path.is_empty())
		cached_weather_node =
				Object::cast_to<Weather>(get_node_or_null(weather_path));
	if (!water_path.is_empty())
		cached_water_node =
				Object::cast_to<Water>(get_node_or_null(water_path));
}

// ---------------------------------------------------------------------------
// Strip-to-list conversion
// ---------------------------------------------------------------------------

void Terrain::_strip_to_list(const std::vector<uint16_t>& strip,
                                  PackedInt32Array& out) {
	if (strip.size() < 3) return;
	for (size_t i = 2; i < strip.size(); i++) {
		uint16_t a = strip[i - 2], b = strip[i - 1], c = strip[i];
		if (a == b || b == c || a == c) continue;
		// Reference uses CW winding (OpenGL default with glFrontFace unset renders both).
		// Godot culls CW faces (back = CW), so swap to CCW.
		if (i & 1) { out.push_back(a); out.push_back(b); out.push_back(c); }
		else       { out.push_back(b); out.push_back(a); out.push_back(c); }
	}
}

// ---------------------------------------------------------------------------
// Terrain shader — loaded from res://shaders/terrain.gdshader
// ---------------------------------------------------------------------------

Ref<Shader> Terrain::_load_terrain_shader() {
	return ResourceLoader::get_singleton()->load("res://shaders/terrain.gdshader", "Shader");
}

// ---------------------------------------------------------------------------
// Texture loading — uses Texture2D resources from TerrainData
// ---------------------------------------------------------------------------

void Terrain::_clear_derived_textures() {
	surface_inputs->clear_derived_textures();
	if (terrain_material.is_valid()) {
		surface_inputs->apply_to_material(terrain_material);
	}
}

void Terrain::_load_textures() {
	if (terrain_material.is_null() || terrain_data.is_null()) {
		return;
	}
	surface_inputs->rebuild(terrain_data, tile_info_override);
	surface_inputs->apply_to_material(terrain_material);
	tile_cache_device.rebuild(terrain_data, surface_inputs, tile_info_override);
	terrain_material->set_shader_parameter(
			"u_tile_cache", tile_cache_device.get_texture());
	terrain_material->set_shader_parameter(
			"u_has_tile_cache", tile_cache_device.is_ready());
}

void Terrain::_rebuild_tile_overlay_pages() {
	if (terrain_material.is_null()) {
		return;
	}
	surface_inputs->set_terrain_data(terrain_data);
	surface_inputs->set_tile_info_override(tile_info_override);
	tile_cache_device.rebuild(terrain_data, surface_inputs, tile_info_override);
	terrain_material->set_shader_parameter(
			"u_tile_cache", tile_cache_device.get_texture());
	terrain_material->set_shader_parameter(
			"u_has_tile_cache", tile_cache_device.is_ready());
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void Terrain::_on_terrain_changed() {
	if (!built || terrain_data.is_null()) return;
	// Rebuild the derived terrain inputs and update shader parameters.
	_load_textures();
}

void Terrain::_on_tile_info_changed() {
	if (!built || terrain_data.is_null()) return;
	_rebuild_tile_overlay_pages();
}

void Terrain::_hide_visible_patches() {
	_hide_pool(main_pool);
	_hide_pool(inset_pool);
}

void Terrain::_hide_pool(PatchPool &r_pool) {
	RenderingServer* rs = RenderingServer::get_singleton();
	for (int i = 0; i < PATCH_POOL_SIZE; i++) {
		if (!r_pool.visible[i]) {
			continue;
		}
		if (rs && r_pool.instances[i].is_valid()) {
			rs->instance_set_visible(r_pool.instances[i], false);
		}
		r_pool.visible[i] = false;
	}
}

void Terrain::_create_pool_instances(PatchPool &r_pool, const RID &p_material) {
	RenderingServer* rs = RenderingServer::get_singleton();
	if (rs == nullptr || get_world_3d().is_null()) {
		return;
	}
	const RID scenario = get_world_3d()->get_scenario();
	for (int i = 0; i < PATCH_POOL_SIZE; i++) {
		RID inst = rs->instance_create();
		rs->instance_set_scenario(inst, scenario);
		rs->instance_geometry_set_material_override(inst, p_material);
		// Static terrain silhouettes are already carried in the composed page A;
		// authored terrain participates only in its view's layer (_apply_pool
		// stamps each patch's).
		rs->instance_set_layer_mask(inst, r_pool.world_layer);
		rs->instance_set_visible(inst, false);
		// Draw-list index == pool slot == the light rows texture row this
		// instance reads; fixed for the instance's lifetime.
		rs->instance_geometry_set_shader_parameter(inst,
				"u_instance_light_slot", static_cast<float>(i));
		r_pool.instances[i] = inst;
		r_pool.visible[i] = false;
		r_pool.uniforms_stamped[i] = false;
		r_pool.last_layer[i] = r_pool.world_layer;
	}
}

void Terrain::_free_pool_instances(PatchPool &r_pool) {
	RenderingServer* rs = RenderingServer::get_singleton();
	for (int i = 0; i < PATCH_POOL_SIZE; i++) {
		if (rs && r_pool.instances[i].is_valid()) {
			rs->free_rid(r_pool.instances[i]);
		}
		r_pool.instances[i] = RID();
		r_pool.last_mesh_rid[i] = RID();
		r_pool.last_transform[i] = Transform3D();
		r_pool.visible[i] = false;
		// A fresh instance carries no instance uniforms: every per-slot latch
		// forgets this build's values, or a page that comes ready later on the
		// next build with the same layer/projection would never be written.
		r_pool.uniforms_stamped[i] = false;
		r_pool.last_page_ready[i] = false;
		r_pool.last_page_layer[i] = -1.0f;
		r_pool.last_page_projection[i] = Vector4();
	}
	r_pool.active = 0;
}

void Terrain::_clear_patch_pool() {
	frame_draw_list_live = false;
	frame_bounds_live = false;
	inset_draw_list_live = false;
	if (!RenderingServer::get_singleton()) {
		return;
	}
	_free_pool_instances(main_pool);
	_free_pool_instances(inset_pool);
	main_pool.world_layer = visual_layers::WORLD;
}

void Terrain::_clear_terrain() {
	_clear_patch_pool();
	inset_material.unref();
	inset_material_source = ObjectID();
	inset_synced_parameters.clear();
	inset_pool.rows = LightRows();
	tile_cache_device.clear();
	if (terrain_material.is_valid()) {
		terrain_material->set_shader_parameter("u_tile_cache", Variant());
		terrain_material->set_shader_parameter("u_has_tile_cache", false);
		terrain_material->set_shader_parameter("u_terrain_light_enabled", false);
	}
	light_patches_lit = 0;
	light_rows_total = 0;
	_clear_derived_textures();

	tile_infos.clear();
	scene_snapshot = opennova::TerrainSceneSnapshot();
	built = false;
	// A build in flight goes with what it built.
	building_ = false;
}

void Terrain::build() {
	if (!build_begin()) {
		return;
	}
	while (build_step() == BUILD_STEP_MORE) {
	}
}

bool Terrain::build_begin() {
	_clear_terrain();
	build_done_ = 0;
	build_total_ = 0;
	if (terrain_data.is_null() || !terrain_data->is_loaded()) {
		UtilityFunctions::push_warning("Terrain::build() — terrain_data not loaded");
		return false;
	}
	// The prepare unit, a unit per tile, then the pool, the surface inputs' three
	// texture sets, the tile cache and the end.
	build_total_ = static_cast<int>(terrain_data->get_cpt().tiles.size()) + kBuildTailUnits + 1;
	building_ = true;
	return true;
}

Terrain::BuildStep Terrain::build_step() {
	if (!building_) {
		return built ? BUILD_STEP_DONE : BUILD_STEP_FAILED;
	}
	const auto failed = [this]() {
		UtilityFunctions::push_warning("Terrain: no valid baked CPT data; skipping native mesh build");
		building_ = false;
		return BUILD_STEP_FAILED;
	};
	const int tiles = static_cast<int>(tile_infos.size());
	if (build_done_ == 0) {
		// The engine owns the scene: quadtree, tile metadata, mipchain, sector
		// routing (ADR 0033 R2). The mesh build uploads the same CPT tiles
		// the snapshot's index counts describe.
		scene_snapshot = opennova::build_terrain_scene_snapshot(
				terrain_data->get_cpt(), terrain_data->get_trn());
		if (!scene_snapshot.valid() || !_build_terrain_begin()) {
			return failed();
		}
		// A terrain with no tile is refused by the prepare; the count stands.
		build_total_ = static_cast<int>(tile_infos.size()) + kBuildTailUnits + 1;
	} else if (build_done_ <= tiles) {
		if (!_build_terrain_tile(static_cast<size_t>(build_done_ - 1))) {
			return failed();
		}
	} else {
		switch (build_done_ - tiles) {
			case 1:
				UtilityFunctions::print_verbose("Terrain: ", build_total_verts_, " verts, ",
						build_total_indices_, " indices across ", tiles, " tiles");
				// Create lightweight RenderingServer instances for the main view's pool
				// (the Inset's is created when it first renders).
				_create_pool_instances(main_pool, terrain_material->get_rid());
				break;
			case 2:
				if (terrain_material.is_valid()) {
					surface_inputs->set_terrain_data(terrain_data);
					surface_inputs->set_tile_info_override(tile_info_override);
					surface_inputs->rebuild_heightfield();
				}
				break;
			case 3:
				if (terrain_material.is_valid()) {
					surface_inputs->rebuild_blend();
				}
				break;
			case 4:
				if (terrain_material.is_valid()) {
					surface_inputs->rebuild_detail_textures();
					surface_inputs->apply_to_material(terrain_material);
				}
				break;
			case 5:
				if (terrain_material.is_valid()) {
					tile_cache_device.rebuild(terrain_data, surface_inputs, tile_info_override);
					terrain_material->set_shader_parameter(
							"u_tile_cache", tile_cache_device.get_texture());
					terrain_material->set_shader_parameter(
							"u_has_tile_cache", tile_cache_device.is_ready());
				}
				break;
			default:
				light_textures_bound = false;
				if (light_scene.is_valid()) {
					_bind_light_textures();
				}
				built = true;
				building_ = false;
				UtilityFunctions::print_verbose("Terrain: Built ", static_cast<int>(tile_infos.size()),
					" tiles, ", static_cast<int>(scene_snapshot.quad_nodes.size()),
					" quad nodes, pool=", PATCH_POOL_SIZE);
				++build_done_;
				return BUILD_STEP_DONE;
		}
	}
	++build_done_;
	return BUILD_STEP_MORE;
}

String Terrain::get_build_step_label() const {
	if (!building_) {
		return String();
	}
	const int tiles = static_cast<int>(tile_infos.size());
	if (build_done_ == 0) {
		return "scene";
	}
	if (build_done_ <= tiles) {
		return "tiles";
	}
	switch (build_done_ - tiles) {
		case 1: return "patches";
		case 2: return "heightfield";
		case 3: return "blend";
		case 4: return "detail";
		case 5: return "pages";
		default: return "lights";
	}
}

bool Terrain::_build_terrain_begin() {
	const auto& cpt = terrain_data->get_cpt();

	if (cpt.tiles.empty() || cpt.depth_buffer.empty()) {
		return false;
	}

	constexpr int hm_size = 1024;
	constexpr size_t expected_depth_samples = static_cast<size_t>(hm_size) * static_cast<size_t>(hm_size);
	if (cpt.depth_buffer.size() != expected_depth_samples) {
		UtilityFunctions::push_warning(
			"Terrain: CPT depth buffer has ",
			static_cast<int64_t>(cpt.depth_buffer.size()),
			" samples; expected ",
			static_cast<int64_t>(expected_depth_samples)
		);
		return false;
	}
	terrain_shader = _load_terrain_shader();
	terrain_material.instantiate();
	terrain_material->set_shader(terrain_shader);
	_apply_ground_overlay();

	tile_infos.resize(cpt.tiles.size());
	build_total_verts_ = 0;
	build_total_indices_ = 0;
	return true;
}

bool Terrain::_build_terrain_tile(size_t ti) {
	const auto& cpt = terrain_data->get_cpt();
	if (ti >= cpt.tiles.size() || ti >= tile_infos.size()) {
		return false;
	}
	const auto& tile = cpt.tiles[ti];
	auto& info = tile_infos[ti];

	// The engine produces both vertex variants; this only uploads
	// the selected positions, normals and unchanged detail atlas UVs.
	// Only quadrant 1 can be the empty-sector fallback topology.
	const int variants = tile.tile_x < 512 && tile.tile_y < 512 ? 2 : 1;
	for (int variant = 0; variant < variants; ++variant) {
		const std::vector<opennova::TerrainTileVertex> vertices =
				opennova::build_terrain_tile_vertices(cpt, terrain_data->get_trn(),
						static_cast<int>(ti), variant != 0);
		if (vertices.size() != tile.vertex_count) return false;
		PackedVector3Array positions;
		PackedVector3Array normals;
		PackedVector2Array uvs;
		positions.resize(tile.vertex_count);
		normals.resize(tile.vertex_count);
		uvs.resize(tile.vertex_count);
		for (int vi = 0; vi < tile.vertex_count; ++vi) {
			const opennova::TerrainTileVertex &vertex = vertices[vi];
			positions.set(vi, Vector3(
					vertex.position[0], vertex.position[1], vertex.position[2]));
			normals.set(vi, Vector3(
					vertex.normal[0], vertex.normal[1], vertex.normal[2]));
			uvs.set(vi, Vector2(vertex.atlas_uv[0], vertex.atlas_uv[1]));
		}

		// Both variants share one vertex count; tally it once per tile.
		if (variant == 0) build_total_verts_ += tile.vertex_count;

		// Create one ArrayMesh per LOD level (single surface each)
		for (int lod = 0; lod < 8; lod++) {
			const auto& src_lod = tile.lods[lod];

			PackedInt32Array indices;
			if (src_lod.is_strip) {
				_strip_to_list(src_lod.indices, indices);
			} else {
				// Swap first two indices per triangle (CW -> CCW for Godot)
				for (size_t i = 0; i + 2 < src_lod.indices.size(); i += 3) {
					indices.push_back(src_lod.indices[i + 1]);
					indices.push_back(src_lod.indices[i]);
					indices.push_back(src_lod.indices[i + 2]);
				}
			}

			// The <3 gate matches the snapshot's per-LOD index counts, so the
			// compiler's family fallback and this mesh set agree by
			// construction (both derive from the same converted counts).
			if (indices.size() < 3) continue;

			build_total_indices_ += indices.size();

			Array arrays;
			arrays.resize(Mesh::ARRAY_MAX);
			arrays[Mesh::ARRAY_VERTEX] = positions;
			arrays[Mesh::ARRAY_NORMAL] = normals;
			arrays[Mesh::ARRAY_TEX_UV] = uvs;
			arrays[Mesh::ARRAY_INDEX] = indices;

			Ref<ArrayMesh> lod_mesh;
			lod_mesh.instantiate();
			lod_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
			lod_mesh->surface_set_material(0, terrain_material);
			if (variant == 0) info.lod_meshes[lod] = lod_mesh;
			else info.flat_lod_meshes[lod] = lod_mesh;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Debug API — cold reads over the compiler's last draw list
// ---------------------------------------------------------------------------

bool Terrain::has_visible_terrain_bounds() const {
	return frame_bounds_live && frame_bounds.valid;
}

float Terrain::get_visible_terrain_min_height() const {
	return has_visible_terrain_bounds() ? frame_bounds.min[1] : 0.0f;
}

float Terrain::get_visible_terrain_max_height() const {
	return has_visible_terrain_bounds() ? frame_bounds.max[1] : 0.0f;
}

int Terrain::get_patches_active() const {
	return main_pool.active;
}

int Terrain::get_visible_patch_count() const {
	int visible_count = 0;
	for (int i = 0; i < PATCH_POOL_SIZE; i++) {
		if (main_pool.visible[i]) {
			visible_count++;
		}
	}
	return visible_count;
}

const std::vector<FoliageDetailPatch> &Terrain::get_foliage_detail_patches_native() const {
	// No compile ran for the current frame (torn down, hidden, no camera):
	// report no cells rather than a stale draw list's.
	static const std::vector<FoliageDetailPatch> empty;
	if (!frame_draw_list_live) {
		return empty;
	}
	return frame_compiler.last_draw_list().detail_cells;
}

void Terrain::set_debug_no_frustum(bool v) { traversal_config.no_frustum = v; }
bool Terrain::get_debug_no_frustum() const { return traversal_config.no_frustum; }
void Terrain::set_debug_no_nearfar(bool v) { traversal_config.no_nearfar = v; }
bool Terrain::get_debug_no_nearfar() const { return traversal_config.no_nearfar; }
void Terrain::set_debug_no_sideplanes(bool v) { traversal_config.no_sideplanes = v; }
bool Terrain::get_debug_no_sideplanes() const { return traversal_config.no_sideplanes; }
void Terrain::set_debug_no_partial_subdiv(bool v) { traversal_config.no_partial_subdiv = v; }
bool Terrain::get_debug_no_partial_subdiv() const { return traversal_config.no_partial_subdiv; }
void Terrain::set_debug_force_leaves(bool v) { traversal_config.force_leaves = v; }
bool Terrain::get_debug_force_leaves() const { return traversal_config.force_leaves; }
void Terrain::set_debug_force_lod0(bool v) { traversal_config.force_lod0 = v; }
bool Terrain::get_debug_force_lod0() const { return traversal_config.force_lod0; }

void Terrain::set_debug_mode(DebugMode mode) { debug_mode = mode; }
Terrain::DebugMode Terrain::get_debug_mode() const { return debug_mode; }

void Terrain::set_ground_overlay(const Ref<Image> &p_image, const Rect2 &p_rect, const Color &p_outside,
		bool p_outside_on) {
	ground_overlay_texture_.unref();
	if (p_image.is_valid() && !p_image->is_empty()) ground_overlay_texture_ = ImageTexture::create_from_image(p_image);
	const float span_x = p_rect.size.x > 0.0f ? p_rect.size.x : 1.0f;
	const float span_z = p_rect.size.y > 0.0f ? p_rect.size.y : 1.0f;
	// No picture: every point is past it.
	ground_overlay_rect_ = ground_overlay_texture_.is_valid()
			? Vector4(p_rect.position.x, p_rect.position.y, 1.0f / span_x, 1.0f / span_z)
			: Vector4(0.0f, 0.0f, 0.0f, 0.0f);
	ground_overlay_outside_ = p_outside_on ? p_outside : Color(0.0f, 0.0f, 0.0f, 0.0f);
	ground_overlay_on_ = true;
	_apply_ground_overlay();
}

void Terrain::clear_ground_overlay() {
	ground_overlay_texture_.unref();
	ground_overlay_on_ = false;
	_apply_ground_overlay();
}

void Terrain::_apply_ground_overlay() {
	if (terrain_material.is_null()) return;
	terrain_material->set_shader_parameter("u_overlay_on", ground_overlay_on_);
	terrain_material->set_shader_parameter("u_overlay", ground_overlay_texture_);
	terrain_material->set_shader_parameter("u_overlay_rect", ground_overlay_rect_);
	terrain_material->set_shader_parameter("u_overlay_outside", ground_overlay_outside_);
}
