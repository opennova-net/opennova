// Terrain — Godot Node3D that builds and renders terrain meshes from CPT data.

#include "terrain/terrain.h"
#include "terrain/terrain_surface_inputs.h"
#include "terrain/terrain_tile_info.h"
#include "env/slot_shadow.h"
#include "env/water.h"
#include "mission/mission_object_placer.h"

// Retail: PolyTrn_RenderTile @0x60da70 (docs/terrain/terrain-re.md, docs/tiles/til-re.md);
// the sector traversal names below are jodemo-era (Terrain_RenderSectorTile @0x5CDAA0,
// Terrain_TraverseQuadTreeNode @0x5C89C0, Terrain_CollectVisibleSectors @0x5C9120).
// docs/engine_spec_terrain.md 7.1-7.2

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

	ClassDB::bind_method(D_METHOD("set_tile_overlay_enabled", "enabled"), &Terrain::set_tile_overlay_enabled);
	ClassDB::bind_method(D_METHOD("get_tile_overlay_enabled"), &Terrain::get_tile_overlay_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tile_overlay_enabled"),
		"set_tile_overlay_enabled", "get_tile_overlay_enabled");

	ClassDB::bind_method(D_METHOD("set_tile_info_override", "tile_info"), &Terrain::set_tile_info_override);
	ClassDB::bind_method(D_METHOD("get_tile_info_override"), &Terrain::get_tile_info_override);
	ClassDB::bind_method(D_METHOD("rebuild_tile_overlay"), &Terrain::rebuild_tile_overlay);
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
	ClassDB::bind_method(D_METHOD("render_frame"), &Terrain::render_frame);

	// Debug API
	ClassDB::bind_method(D_METHOD("has_visible_terrain_bounds"),
			&Terrain::has_visible_terrain_bounds);
	ClassDB::bind_method(D_METHOD("get_visible_terrain_min_height"),
			&Terrain::get_visible_terrain_min_height);
	ClassDB::bind_method(D_METHOD("get_visible_terrain_max_height"),
			&Terrain::get_visible_terrain_max_height);
	ClassDB::bind_method(D_METHOD("get_visible_patch_count"), &Terrain::get_visible_patch_count);
	ClassDB::bind_method(D_METHOD("get_patches_active"), &Terrain::get_patches_active);

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

Ref<Texture2D> Terrain::get_tile_overlay_texture() const {
	return surface_inputs->get_tile_overlay_texture();
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

std::optional<opennova::TerrainTilePageBinding>
Terrain::get_tile_cache_binding_for_world_point_native(
		float p_world_x, float p_world_z) {
	if (!std::isfinite(p_world_x) || !std::isfinite(p_world_z)) {
		return std::nullopt;
	}
	constexpr float SECTOR_SIZE = 512.0f;
	opennova::TerrainTileResidentPoint point;
	point.sector_origin_x = static_cast<int32_t>(
			std::floor(p_world_x / SECTOR_SIZE)) * 512;
	point.sector_origin_z = static_cast<int32_t>(
			std::floor(p_world_z / SECTOR_SIZE)) * 512;
	point.world_x = p_world_x;
	point.world_z = p_world_z;
	return tile_cache_device.best_ready(point);
}

Vector3 Terrain::get_tile_overlay_tint() const {
	return tile_overlay_tint;
}

void Terrain::set_lod_quality(float p_quality) {
	lod_quality = p_quality;
}

float Terrain::get_lod_quality() const {
	return lod_quality;
}

void Terrain::set_tile_overlay_enabled(bool p_enabled) {
	if (tile_overlay_enabled == p_enabled) {
		return;
	}
	tile_overlay_enabled = p_enabled;
	surface_inputs->set_tile_overlay_enabled(p_enabled);
	if (built) {
		_rebuild_tile_overlay_texture();
	}
}

bool Terrain::get_tile_overlay_enabled() const {
	return tile_overlay_enabled;
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
		_rebuild_tile_overlay_texture();
	}
}

Ref<TerrainTileInfo> Terrain::get_tile_info_override() const {
	return tile_info_override;
}

void Terrain::rebuild_tile_overlay() {
	_rebuild_tile_overlay_texture();
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
	if (!is_visible_in_tree()) {
		_hide_visible_patches();
		return;
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
	// The RENDER eye (get_camera_transform includes h/v offsets), so the
	// below-water classification stays coherent with Water's surface flip and
	// the frame clear — the same eye those classifiers sample. Offsets are
	// zero for common cameras, so traversal is unchanged in practice.
	const Transform3D cam_xform = cam->get_camera_transform();
	const Vector3 cam_pos = cam_xform.origin;
	const Transform3D view = cam_xform.affine_inverse();
	const Projection proj = cam->get_camera_projection();

	if (!terrain_node_cache_valid)
		_cache_env_weather_nodes();

	opennova::TerrainViewInput view_input;
	view_input.cam_x = static_cast<float>(cam_pos.x);
	view_input.cam_y = static_cast<float>(cam_pos.y);
	view_input.cam_z = static_cast<float>(cam_pos.z);
	// The map's live water height, unconditionally: retail's compare reads
	// Env_WaterHeightFixed with no render gate, so the engine side gets the
	// authored value whenever a Water node exists (a missing node passes 0,
	// and non-negative terrain keeps a dry map's compare inert).
	// [orig: cameraY < Env_WaterHeightFixed @0x60fea5, no zero guard —
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

	// Godot Projection stores columns; the compiler wants column-major floats.
	for (int col = 0; col < 4; col++) {
		view_input.proj[col * 4 + 0] = proj.columns[col][0];
		view_input.proj[col * 4 + 1] = proj.columns[col][1];
		view_input.proj[col * 4 + 2] = proj.columns[col][2];
		view_input.proj[col * 4 + 3] = proj.columns[col][3];
	}

	traversal_config.quality = lod_quality;
	view_input.config = traversal_config;

	const opennova::TerrainDrawList &draw_list =
			frame_compiler.compile(scene_snapshot, view_input);
	frame_draw_list_live = true;

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
	Vector3 page_light_direction(0.0f, 0.70710678f, 0.70710678f);
	if (cached_env_node && cached_env_node->is_loaded()) {
		page_tile_tint = cached_env_node->get_tile_overlay_tint();
		page_light_direction =
				cached_env_node->get_light_direction_render_tuple();
	}
	static_shadow_rasterizer.begin_frame(page_light_direction,
			light_time_ms < 0 ? 0u : static_cast<uint32_t>(light_time_ms));
	tile_cache_device.begin_frame(draw_list.frame_id);

	// Apply the draw list onto the instance pool: draw-list index == pool slot.
	RenderingServer* rs = RenderingServer::get_singleton();
	const int count = static_cast<int>(draw_list.patches.size());

	for (int i = 0; i < count; i++) {
		const opennova::TerrainPatchDraw &draw = draw_list.patches[i];
		const auto& ti = tile_infos[draw.tile_index];
		const Ref<ArrayMesh> &mesh = ti.lod_meshes[draw.lod_family];
		if (mesh.is_null()) {
			// The compiler resolved the family against the same index counts
			// the mesh build used; a null here means the two went out of sync.
			if (patch_visible[i]) {
				rs->instance_set_visible(patch_instances[i], false);
				patch_visible[i] = false;
			}
			continue;
		}

		// Only update mesh if changed
		RID mesh_rid = mesh->get_rid();
		if (mesh_rid != last_mesh_rid[i]) {
			rs->instance_set_base(patch_instances[i], mesh_rid);
			last_mesh_rid[i] = mesh_rid;
		}

		// Only update transform if changed
		Transform3D xform(Basis(), Vector3(draw.sector_ox, 0.0f, draw.sector_oz));
		if (xform != last_transform[i]) {
			rs->instance_set_transform(patch_instances[i], xform);
			last_transform[i] = xform;
		}

		// Per-instance uniforms: written on change only (the mesh/transform
		// gates above already work that way).
		const bool fresh = !patch_uniforms_stamped[i];
		const Vector2 quadrant(static_cast<float>(draw.quadrant_x),
				static_cast<float>(draw.quadrant_z));
		if (fresh || quadrant != last_quadrant[i]) {
			rs->instance_geometry_set_shader_parameter(
				patch_instances[i], "u_instance_source_quadrant", quadrant);
			last_quadrant[i] = quadrant;
		}

		const opennova::TerrainTilePageBinding page =
				tile_cache_device.request(
					draw, page_tile_tint, page_light_direction);
		const std::optional<opennova::TerrainTilePageProjection> projection =
				page.ready
						? opennova::TerrainTileCompositionCache::page_projection(page.page)
						: std::nullopt;
		const bool ready = projection.has_value();
		if (fresh || ready != last_page_ready[i]) {
			rs->instance_geometry_set_shader_parameter(
					patch_instances[i], "u_instance_tile_cache_ready", ready);
			last_page_ready[i] = ready;
		}
		if (ready) {
			const float layer = static_cast<float>(page.layer);
			const Vector4 projection_row(projection->world_origin_x,
					projection->world_origin_z,
					projection->inverse_world_span,
					projection->world_span);
			if (fresh || layer != last_page_layer[i]) {
				rs->instance_geometry_set_shader_parameter(
						patch_instances[i], "u_instance_tile_cache_layer", layer);
				last_page_layer[i] = layer;
			}
			if (fresh || projection_row != last_page_projection[i]) {
				rs->instance_geometry_set_shader_parameter(
						patch_instances[i], "u_instance_tile_cache_projection",
						projection_row);
				last_page_projection[i] = projection_row;
			}
		}
		patch_uniforms_stamped[i] = true;

		// Per-instance debug data (only set when a debug mode is active)
		if (debug_mode > 0) {
			rs->instance_geometry_set_shader_parameter(patch_instances[i],
				"u_instance_lod", static_cast<float>(draw.lod_family));
			rs->instance_geometry_set_shader_parameter(patch_instances[i],
				"u_instance_sector_x", draw.sector_ox / 512.0f);
			rs->instance_geometry_set_shader_parameter(patch_instances[i],
				"u_instance_sector_z", draw.sector_oz / 512.0f);
		}

		if (!patch_visible[i]) {
			rs->instance_set_visible(patch_instances[i], true);
			patch_visible[i] = true;
		}
	}

	// Hide unused pool entries
	for (int i = count; i < patches_active; i++) {
		if (patch_visible[i]) {
			rs->instance_set_visible(patch_instances[i], false);
			patch_visible[i] = false;
		}
	}
	patches_active = count;

	// The light-pool re-draw rows for exactly this draw list's patches.
	_render_light_rows(draw_list);

	// Update shader parameters on the single shared material
	if (terrain_material.is_valid()) {
		terrain_material->set_shader_parameter("u_debug_mode", debug_mode);
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

	// Update lighting from MissionEnvironment, prefer smoothed colors from
	// Weather — both native now, direct typed calls (ADR 0034 d6).
	if (terrain_material.is_valid()) {
		if (cached_env_node && cached_env_node->is_loaded()) {
			// Base env -> terrain-uniform push, shared with the editor preview
			// (MissionEnvironment.apply_terrain_uniforms drives both shaders' uniforms).
			cached_env_node->apply_terrain_uniforms(terrain_material);
			// Runtime-only: prefer Weather-smoothed colors when a weather node
			// is present (overriding the ones it smooths). The terrain surface
			// consumes only c1 = light + c0 = sky [orig: @ 0x604420, see docs/terrain/terrain-re.md].
			if (cached_weather_node) {
				terrain_material->set_shader_parameter("u_sun_light", cached_weather_node->get_smooth_sun());
				terrain_material->set_shader_parameter("u_sky_ambient", cached_weather_node->get_smooth_sky());
				// The underwater pass replaces the weather fog block with
				// Env_WaterColorLit. Above water, retain Weather's direct smoothed
				// color override exactly as before.
				if (!cached_env_node->is_underwater_view()) {
					terrain_material->set_shader_parameter("u_fog_color", cached_weather_node->get_smooth_fog());
				}
			}
			// Tile overlay tint: HALF(terrain_rgb) under MODULATE2X folded to
			// one multiply; the shared runtime/ONED tile path consumes this uniform.
			// [orig: PolyTrn_RenderTile @ 0x60df0d, see docs/terrain/terrain-re.md].
			tile_overlay_tint = cached_env_node->get_tile_overlay_tint();
			terrain_material->set_shader_parameter(
				"u_tile_overlay_tint", tile_overlay_tint);
		}
	}
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
		light_rows_enabled_written = 0;
		light_patches_lit = 0;
		light_rows_total = 0;
	}
}

void Terrain::_bind_light_textures() {
	if (light_textures_bound || terrain_material.is_null()) {
		return;
	}
	// The two procedural textures, built once per process like the corona
	// texture [orig: Lighting_InitTextures @0x5a94f0 creates "texlight2d"
	// 64x64 and "texlightspot1d" 64x8, both without mips, and the 0x600 shader
	// they bind addresses CLAMP — CGfxTexture_SetSamplerAddressing (ex sub_680720)(this, clamp=1, 0, 0, 0)
	// @0x5a98eb..0x5a98f4; the shader samplers carry the matching
	// filter_linear, repeat_disable hints].
	const int size = LightScene::terrain_light_texture_size();
	const int rows = LightScene::terrain_light_strip_rows();
	if (light_disc_texture.is_null()) {
		light_disc_texture = ImageTexture::create_from_image(
				Image::create_from_data(size, size, false, Image::FORMAT_RGBA8,
						LightScene::terrain_light_disc_rgba8()));
	}
	if (light_strip_texture.is_null()) {
		light_strip_texture = ImageTexture::create_from_image(
				Image::create_from_data(size, rows, false, Image::FORMAT_RGBA8,
						LightScene::terrain_light_strip_rgba8()));
	}
	// The rows texture: one row per pool slot, two RGBAF texels per light —
	// (position.xyz Godot world, inv_scale) then (c4..c6, the patch's count).
	light_rows_bytes.resize(
			static_cast<int64_t>(LIGHT_ROWS_TEXELS) * PATCH_POOL_SIZE * 16);
	light_rows_bytes.fill(0);
	light_rows_image = Image::create_from_data(LIGHT_ROWS_TEXELS,
			PATCH_POOL_SIZE, false, Image::FORMAT_RGBAF, light_rows_bytes);
	light_rows_texture = ImageTexture::create_from_image(light_rows_image);
	light_rows_uploaded = light_rows_bytes.duplicate();
	light_rows_enabled_written = -1;
	terrain_material->set_shader_parameter("u_terrain_light_disc",
			light_disc_texture);
	terrain_material->set_shader_parameter("u_terrain_light_strip",
			light_strip_texture);
	terrain_material->set_shader_parameter("u_terrain_light_rows",
			light_rows_texture);
	light_textures_bound = true;
}

void Terrain::_render_light_rows(const opennova::TerrainDrawList &draw_list) {
	light_patches_lit = 0;
	light_rows_total = 0;
	if (light_scene.is_null() || terrain_material.is_null()) {
		return;
	}
	_bind_light_textures();
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
	// Env_TerrainColorRecip [orig: @0x5aa1ef..0x5aa23f].
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
	light_rows_total = static_cast<int>(total);
	// Rewrite the whole rows image: a slot that lost its lights reads count 0.
	light_rows_bytes.fill(0);
	float *texels = reinterpret_cast<float *>(light_rows_bytes.ptrw());
	for (int i = 0; i < count; i++) {
		const opennova::renderer::TerrainLightPatchRows &rows = light_patch_rows[i];
		if (rows.count > 0) {
			++light_patches_lit;
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
	const int64_t byte_count = light_rows_bytes.size();
	if (light_rows_uploaded.size() != byte_count ||
			std::memcmp(light_rows_uploaded.ptr(), light_rows_bytes.ptr(),
					static_cast<size_t>(byte_count)) != 0) {
		light_rows_image->set_data(LIGHT_ROWS_TEXELS, PATCH_POOL_SIZE, false,
				Image::FORMAT_RGBAF, light_rows_bytes);
		light_rows_texture->update(light_rows_image);
		light_rows_uploaded.resize(byte_count);
		std::memcpy(light_rows_uploaded.ptrw(), light_rows_bytes.ptr(),
				static_cast<size_t>(byte_count));
	}
	const int enabled = light_rows_total > 0 ? 1 : 0;
	if (enabled != light_rows_enabled_written) {
		terrain_material->set_shader_parameter("u_terrain_light_enabled",
				enabled != 0);
		light_rows_enabled_written = enabled;
	}
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
// Normal from heightmap gradient
// ---------------------------------------------------------------------------

Vector3 Terrain::_heightmap_normal(const std::vector<uint16_t>& depth, int gx, int gz,
                                       const opennova::terrain::CoordsTaps& taps) const {
	const int size = 1024;
	const float scale = 1.0f / 256.0f;
	// Same lock policy as the vertex heights: a gradient tap at the quadrant edge
	// wraps inside it rather than reading the neighbouring quadrant's shoreline,
	// which would tilt the seam row's normals into a false cliff face.
	int x0 = taps.x(gx - 1);
	int x1 = taps.x(gx + 1);
	int z0 = taps.z(gz - 1);
	int z1 = taps.z(gz + 1);
	float hL = depth[gz * size + x0] * scale;
	float hR = depth[gz * size + x1] * scale;
	float hD = depth[z0 * size + gx] * scale;
	float hU = depth[z1 * size + gx] * scale;
	Vector3 n(hL - hR, 2.0f, hD - hU);
	return n.normalized();
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
	surface_inputs->rebuild(
		terrain_data, tile_info_override, tile_overlay_enabled);
	surface_inputs->apply_to_material(terrain_material);
	tile_cache_device.rebuild(terrain_data, surface_inputs,
			tile_info_override, tile_overlay_enabled);
	terrain_material->set_shader_parameter(
			"u_tile_cache", tile_cache_device.get_texture());
	terrain_material->set_shader_parameter(
			"u_has_tile_cache", tile_cache_device.is_ready());
}

void Terrain::_clear_tile_overlay_texture() {
	surface_inputs->clear_tile_overlay();
	if (terrain_material.is_valid()) {
		surface_inputs->apply_to_material(terrain_material);
	}
}

void Terrain::_rebuild_tile_overlay_texture() {
	if (terrain_material.is_null()) {
		return;
	}
	surface_inputs->set_terrain_data(terrain_data);
	surface_inputs->set_tile_info_override(tile_info_override);
	surface_inputs->set_tile_overlay_enabled(tile_overlay_enabled);
	surface_inputs->rebuild_tile_overlay();
	surface_inputs->apply_to_material(terrain_material);
	tile_cache_device.rebuild(terrain_data, surface_inputs,
			tile_info_override, tile_overlay_enabled);
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
	_rebuild_tile_overlay_texture();
}

void Terrain::_hide_visible_patches() {
	RenderingServer* rs = RenderingServer::get_singleton();
	for (int i = 0; i < PATCH_POOL_SIZE; i++) {
		if (!patch_visible[i]) {
			continue;
		}
		if (rs && patch_instances[i].is_valid()) {
			rs->instance_set_visible(patch_instances[i], false);
		}
		patch_visible[i] = false;
	}
}

void Terrain::_clear_patch_pool() {
	frame_draw_list_live = false;
	RenderingServer* rs = RenderingServer::get_singleton();
	if (!rs) {
		return;
	}
	for (int i = 0; i < PATCH_POOL_SIZE; i++) {
		if (patch_instances[i].is_valid()) {
			rs->free_rid(patch_instances[i]);
			patch_instances[i] = RID();
		}
		last_mesh_rid[i] = RID();
		last_transform[i] = Transform3D();
		patch_visible[i] = false;
		// A fresh instance carries no instance uniforms: every per-slot latch
		// forgets this build's values, or a page that comes ready later on the
		// next build with the same layer/projection would never be written.
		patch_uniforms_stamped[i] = false;
		last_page_ready[i] = false;
		last_page_layer[i] = -1.0f;
		last_page_projection[i] = Vector4();
	}
	patches_active = 0;
}

void Terrain::_clear_terrain() {
	_clear_patch_pool();
	tile_cache_device.clear();
	if (terrain_material.is_valid()) {
		terrain_material->set_shader_parameter("u_tile_cache", Variant());
		terrain_material->set_shader_parameter("u_has_tile_cache", false);
		terrain_material->set_shader_parameter("u_terrain_light_enabled", false);
	}
	light_patches_lit = 0;
	light_rows_total = 0;
	_clear_tile_overlay_texture();
	_clear_derived_textures();

	tile_infos.clear();
	scene_snapshot = opennova::TerrainSceneSnapshot();
	built = false;
}

void Terrain::build() {
	_clear_terrain();

	if (terrain_data.is_null() || !terrain_data->is_loaded()) {
		UtilityFunctions::push_warning("Terrain::build() — terrain_data not loaded");
		return;
	}

	// The engine owns the scene: quadtree, tile metadata, mipchain, sector
	// routing (ADR 0033 R2). The mesh build below uploads the same CPT tiles
	// the snapshot's index counts describe.
	scene_snapshot = opennova::build_terrain_scene_snapshot(
			terrain_data->get_cpt(), terrain_data->get_trn());
	if (!scene_snapshot.valid()) {
		UtilityFunctions::push_warning("Terrain: no valid baked CPT data; skipping native mesh build");
		return;
	}
	if (!_build_terrain()) {
		UtilityFunctions::push_warning("Terrain: no valid baked CPT data; skipping native mesh build");
		return;
	}

	// Create lightweight RenderingServer instances for the patch pool
	RenderingServer* rs = RenderingServer::get_singleton();
	RID scenario = get_world_3d()->get_scenario();
	RID mat_rid = terrain_material->get_rid();

	for (int i = 0; i < PATCH_POOL_SIZE; i++) {
		RID inst = rs->instance_create();
		rs->instance_set_scenario(inst, scenario);
		rs->instance_geometry_set_material_override(inst, mat_rid);
		// Static terrain silhouettes are already carried in the composed page A;
		// the terrain participates only in the ordinary world-visible layer.
		rs->instance_set_layer_mask(inst, 1u << 0);
		rs->instance_set_visible(inst, false);
		// Draw-list index == pool slot == the light rows texture row this
		// instance reads; fixed for the instance's lifetime.
		rs->instance_geometry_set_shader_parameter(inst,
				"u_instance_light_slot", static_cast<float>(i));
		patch_instances[i] = inst;
		patch_visible[i] = false;
		patch_uniforms_stamped[i] = false;
	}

	_load_textures();
	light_textures_bound = false;
	if (light_scene.is_valid()) {
		_bind_light_textures();
	}

	built = true;

	UtilityFunctions::print_verbose("Terrain: Built ", static_cast<int>(tile_infos.size()),
		" tiles, ", static_cast<int>(scene_snapshot.quad_nodes.size()),
		" quad nodes, pool=", PATCH_POOL_SIZE);
}

bool Terrain::_build_terrain() {
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
	const float height_scale = 1.0f / 256.0f;
	const opennova::terrain::CoordsQuadrantLocks quadrant_locks =
		coords_locks_from(terrain_data->get_trn());

	terrain_shader = _load_terrain_shader();
	terrain_material.instantiate();
	terrain_material->set_shader(terrain_shader);
	// Terrain receives the LIVE entity ground shadows: the render-slot drape
	// next pass multiplies each bound slot's silhouette capture (or authored
	// blob decal) into the terrain along the slot projection direction with
	// the per-channel ambient law and the 40..80 u fade. Retail drapes over
	// 21x21 terrain-following patches; the terrain surface itself stands in
	// for the patch mesh and the projection is evaluated per pixel [orig:
	// RenderSlot_DrawAllDrapes @0x5d6e20, render_sector_model @0x5d5ca0 —
	// engine/runtime/renderer/render_slot_shadow.h carries the witness map;
	// SlotShadow is the capture device]. Entity shadows land on TERRAIN ONLY,
	// like retail's terrain-following patches; static building silhouettes
	// stay page-alpha in the tile composer.
	terrain_material->set_next_pass(SlotShadow::get_drape_material());

	tile_infos.resize(cpt.tiles.size());

	int total_verts = 0;
	int total_indices = 0;

	for (size_t ti = 0; ti < cpt.tiles.size(); ti++) {
		const auto& tile = cpt.tiles[ti];
		auto& info = tile_infos[ti];

		// Build vertex data
		PackedVector3Array positions;
		PackedVector3Array normals;
		PackedVector2Array uvs;
		positions.resize(tile.vertex_count);
		normals.resize(tile.vertex_count);
		uvs.resize(tile.vertex_count);

		int local_base_x = tile.tile_x & 0x1FF;
		int local_base_z = tile.tile_y & 0x1FF;

		// The tile's own quadrant decides the lock policy for every one of its
		// vertices; a tile whose last row/column lands on the quadrant boundary is
		// exactly the case the .trn locks exist for.
		// [orig: sub_402D20 @0x402D20 (jodemo.exe) — quadrant = (tile_x >= 0x200) + 2 * (tile_y >= 0x200)., see docs/terrain/terrain-re.md]
		const opennova::terrain::CoordsTaps taps =
			opennova::terrain::coords_taps_for_quadrant(
				quadrant_locks, tile.tile_x & 0x200, tile.tile_y & 0x200, hm_size);

		for (int vi = 0; vi < tile.vertex_count; vi++) {
			uint16_t rel_x = tile.vertex_indices[vi * 2 + 0];
			uint16_t rel_y = tile.vertex_indices[vi * 2 + 1];

			int wx = tile.tile_x + rel_x;
			int wz = tile.tile_y + rel_y;
			int hx = taps.x(wx);
			int hz = taps.z(wz);
			float hy = cpt.depth_buffer[hz * hm_size + hx] * height_scale;

			float lx = static_cast<float>(local_base_x + rel_x);
			float lz = static_cast<float>(local_base_z + rel_y);

			positions.set(vi, Vector3(lx, hy, lz));
			normals.set(vi, _heightmap_normal(cpt.depth_buffer, hx, hz, taps));
			uvs.set(vi, Vector2(static_cast<float>(wx) / 1024.0f,
			                    static_cast<float>(wz) / 1024.0f));
		}

		total_verts += tile.vertex_count;

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

			total_indices += indices.size();

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
			info.lod_meshes[lod] = lod_mesh;
		}
	}

	UtilityFunctions::print_verbose("Terrain: ", total_verts, " verts, ", total_indices, " indices across ",
		static_cast<int>(cpt.tiles.size()), " tiles");
	return true;
}

// ---------------------------------------------------------------------------
// Debug API — cold reads over the compiler's last draw list
// ---------------------------------------------------------------------------

bool Terrain::has_visible_terrain_bounds() const {
	return frame_draw_list_live &&
			frame_compiler.last_draw_list().visible_bounds.valid;
}

float Terrain::get_visible_terrain_min_height() const {
	return has_visible_terrain_bounds()
			? frame_compiler.last_draw_list().visible_bounds.min[1]
			: 0.0f;
}

float Terrain::get_visible_terrain_max_height() const {
	return has_visible_terrain_bounds()
			? frame_compiler.last_draw_list().visible_bounds.max[1]
			: 0.0f;
}

int Terrain::get_patches_active() const {
	return patches_active;
}

int Terrain::get_visible_patch_count() const {
	int visible_count = 0;
	for (int i = 0; i < PATCH_POOL_SIZE; i++) {
		if (patch_visible[i]) {
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

void Terrain::set_debug_mode(int mode) { debug_mode = mode; }
int Terrain::get_debug_mode() const { return debug_mode; }
