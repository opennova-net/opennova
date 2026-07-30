// NovaTerrain — Godot Node3D that builds and renders terrain meshes from CPT data.

#include "nova_terrain.h"
#include "nova_terrain_surface_inputs.h"
#include "nova_terrain_tile_info.h"

// Engine: Jointops.exe Terrain_RenderSectorTile@0x5CDAA0,
// Terrain_TraverseQuadTreeNode@0x5C89C0, Terrain_CollectVisibleSectors@0x5C9120
// docs/engine_spec_terrain.md 7.1-7.2

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/projection.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

using namespace godot;

void NovaTerrain::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_terrain_data", "data"), &NovaTerrain::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"), &NovaTerrain::get_terrain_data);
	ClassDB::bind_method(D_METHOD("get_surface_inputs"), &NovaTerrain::get_surface_inputs);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data", PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainData"),
		"set_terrain_data", "get_terrain_data");

	ClassDB::bind_method(D_METHOD("set_lod_quality", "quality"), &NovaTerrain::set_lod_quality);
	ClassDB::bind_method(D_METHOD("get_lod_quality"), &NovaTerrain::get_lod_quality);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_quality", PROPERTY_HINT_RANGE, "0.1,4.0,0.1"),
		"set_lod_quality", "get_lod_quality");

	ClassDB::bind_method(D_METHOD("set_tile_overlay_enabled", "enabled"), &NovaTerrain::set_tile_overlay_enabled);
	ClassDB::bind_method(D_METHOD("get_tile_overlay_enabled"), &NovaTerrain::get_tile_overlay_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tile_overlay_enabled"),
		"set_tile_overlay_enabled", "get_tile_overlay_enabled");

	ClassDB::bind_method(D_METHOD("set_tile_info_override", "tile_info"), &NovaTerrain::set_tile_info_override);
	ClassDB::bind_method(D_METHOD("get_tile_info_override"), &NovaTerrain::get_tile_info_override);
	ClassDB::bind_method(D_METHOD("rebuild_tile_overlay"), &NovaTerrain::rebuild_tile_overlay);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "tile_info_override", PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainTileInfo"),
		"set_tile_info_override", "get_tile_info_override");

	ClassDB::bind_method(D_METHOD("set_environment_path", "path"), &NovaTerrain::set_environment_path);
	ClassDB::bind_method(D_METHOD("get_environment_path"), &NovaTerrain::get_environment_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "environment_path"),
		"set_environment_path", "get_environment_path");

	ClassDB::bind_method(D_METHOD("set_weather_path", "path"), &NovaTerrain::set_weather_path);
	ClassDB::bind_method(D_METHOD("get_weather_path"), &NovaTerrain::get_weather_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_path"),
		"set_weather_path", "get_weather_path");

	ClassDB::bind_method(D_METHOD("build"), &NovaTerrain::build);

	// Debug API
	ClassDB::bind_method(D_METHOD("get_traversal_stats"), &NovaTerrain::get_traversal_stats);
	ClassDB::bind_method(D_METHOD("get_visible_patch_count"), &NovaTerrain::get_visible_patch_count);
	ClassDB::bind_method(D_METHOD("get_lod_distribution"), &NovaTerrain::get_lod_distribution);
	ClassDB::bind_method(D_METHOD("get_patches_active"), &NovaTerrain::get_patches_active);

	ClassDB::bind_method(D_METHOD("set_debug_no_frustum", "enabled"), &NovaTerrain::set_debug_no_frustum);
	ClassDB::bind_method(D_METHOD("get_debug_no_frustum"), &NovaTerrain::get_debug_no_frustum);
	ClassDB::bind_method(D_METHOD("set_debug_no_nearfar", "enabled"), &NovaTerrain::set_debug_no_nearfar);
	ClassDB::bind_method(D_METHOD("get_debug_no_nearfar"), &NovaTerrain::get_debug_no_nearfar);
	ClassDB::bind_method(D_METHOD("set_debug_no_sideplanes", "enabled"), &NovaTerrain::set_debug_no_sideplanes);
	ClassDB::bind_method(D_METHOD("get_debug_no_sideplanes"), &NovaTerrain::get_debug_no_sideplanes);
	ClassDB::bind_method(D_METHOD("set_debug_no_partial_subdiv", "enabled"), &NovaTerrain::set_debug_no_partial_subdiv);
	ClassDB::bind_method(D_METHOD("get_debug_no_partial_subdiv"), &NovaTerrain::get_debug_no_partial_subdiv);
	ClassDB::bind_method(D_METHOD("set_debug_force_leaves", "enabled"), &NovaTerrain::set_debug_force_leaves);
	ClassDB::bind_method(D_METHOD("get_debug_force_leaves"), &NovaTerrain::get_debug_force_leaves);
	ClassDB::bind_method(D_METHOD("set_debug_force_lod0", "enabled"), &NovaTerrain::set_debug_force_lod0);
	ClassDB::bind_method(D_METHOD("get_debug_force_lod0"), &NovaTerrain::get_debug_force_lod0);

	ClassDB::bind_method(D_METHOD("set_debug_mode", "mode"), &NovaTerrain::set_debug_mode);
	ClassDB::bind_method(D_METHOD("get_debug_mode"), &NovaTerrain::get_debug_mode);

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

NovaTerrain::NovaTerrain() {
	surface_inputs.instantiate();
}

NovaTerrain::~NovaTerrain() {
	_clear_patch_pool();
}

void NovaTerrain::set_terrain_data(const Ref<NovaTerrainData> &p_data) {
	if (terrain_data.is_valid() && terrain_data->is_connected("terrain_changed", callable_mp(this, &NovaTerrain::_on_terrain_changed))) {
		terrain_data->disconnect("terrain_changed", callable_mp(this, &NovaTerrain::_on_terrain_changed));
	}
	terrain_data = p_data;
	surface_inputs->set_terrain_data(p_data);
	if (terrain_data.is_valid()) {
		terrain_data->connect("terrain_changed", callable_mp(this, &NovaTerrain::_on_terrain_changed));
	}
}

Ref<NovaTerrainData> NovaTerrain::get_terrain_data() const {
	return terrain_data;
}

Ref<NovaTerrainSurfaceInputs> NovaTerrain::get_surface_inputs() const {
	return surface_inputs;
}

Ref<Texture2D> NovaTerrain::get_heightfield_normal_texture() const {
	return surface_inputs->get_heightfield_normal_texture();
}

Ref<Texture2D> NovaTerrain::get_tile_overlay_texture() const {
	return surface_inputs->get_tile_overlay_texture();
}

Vector3 NovaTerrain::get_tile_overlay_tint() const {
	return tile_overlay_tint;
}

void NovaTerrain::set_lod_quality(float p_quality) {
	lod_quality = p_quality;
}

float NovaTerrain::get_lod_quality() const {
	return lod_quality;
}

void NovaTerrain::set_tile_overlay_enabled(bool p_enabled) {
	if (tile_overlay_enabled == p_enabled) {
		return;
	}
	tile_overlay_enabled = p_enabled;
	surface_inputs->set_tile_overlay_enabled(p_enabled);
	if (built) {
		_rebuild_tile_overlay_texture();
	}
}

bool NovaTerrain::get_tile_overlay_enabled() const {
	return tile_overlay_enabled;
}

void NovaTerrain::set_tile_info_override(const Ref<NovaTerrainTileInfo> &p_info) {
	tile_info_override = p_info;
	surface_inputs->set_tile_info_override(p_info);
	if (built) {
		_rebuild_tile_overlay_texture();
	}
}

Ref<NovaTerrainTileInfo> NovaTerrain::get_tile_info_override() const {
	return tile_info_override;
}

void NovaTerrain::rebuild_tile_overlay() {
	_rebuild_tile_overlay_texture();
}

void NovaTerrain::set_environment_path(const NodePath& p_path) {
	environment_path = p_path;
	terrain_node_cache_valid = false;
}

NodePath NovaTerrain::get_environment_path() const {
	return environment_path;
}

void NovaTerrain::set_weather_path(const NodePath& p_path) {
	weather_path = p_path;
	terrain_node_cache_valid = false;
}

NodePath NovaTerrain::get_weather_path() const {
	return weather_path;
}


// ---------------------------------------------------------------------------
// Notifications (_process)
// ---------------------------------------------------------------------------

void NovaTerrain::_notification(int p_what) {
	if (p_what == NOTIFICATION_VISIBILITY_CHANGED) {
		if (!is_visible_in_tree()) {
			_hide_visible_patches();
		}
	} else if (p_what == NOTIFICATION_PROCESS) {
		if (!is_visible_in_tree()) {
			_hide_visible_patches();
			return;
		}
		if (!built) return;
		foliage_detail_patches.clear();

		const auto& trn = terrain_data->get_trn();

		// Find the active camera — editor viewport or scene camera
		Camera3D* cam = nullptr;
		if (Engine::get_singleton()->is_editor_hint()) {
			EditorInterface* ei = EditorInterface::get_singleton();
			if (ei) {
				SubViewport* svp = ei->get_editor_viewport_3d(0);
				if (svp) cam = svp->get_camera_3d();
			}
		} else {
			Viewport* vp = get_viewport();
			if (vp) cam = vp->get_camera_3d();
		}
		if (!cam) {
			foliage_detail_patches.clear();
			return;
		}

		if (!cam->is_inside_tree()) {
			foliage_detail_patches.clear();
			return;
		}

		Vector3 cam_pos = cam->get_global_position();

		// Build MVP from camera for frustum extraction
		Transform3D cam_xform = cam->get_global_transform();
		Projection proj = cam->get_camera_projection();

		// Godot Projection is row-major internally. We need column-major for
		// Gribb/Hartmann extraction. Projection::columns[] stores columns.
		Transform3D view = cam_xform.affine_inverse();

		// Build view matrix as float[16] column-major
		float view_m[16];
		const Basis& b = view.basis;
		const Vector3& o = view.origin;
		view_m[0]  = b[0][0]; view_m[1]  = b[1][0]; view_m[2]  = b[2][0]; view_m[3]  = 0;
		view_m[4]  = b[0][1]; view_m[5]  = b[1][1]; view_m[6]  = b[2][1]; view_m[7]  = 0;
		view_m[8]  = b[0][2]; view_m[9]  = b[1][2]; view_m[10] = b[2][2]; view_m[11] = 0;
		view_m[12] = o.x;     view_m[13] = o.y;     view_m[14] = o.z;     view_m[15] = 1;

		// Projection columns to float[16] column-major
		float proj_m[16];
		for (int col = 0; col < 4; col++) {
			proj_m[col * 4 + 0] = proj.columns[col][0];
			proj_m[col * 4 + 1] = proj.columns[col][1];
			proj_m[col * 4 + 2] = proj.columns[col][2];
			proj_m[col * 4 + 3] = proj.columns[col][3];
		}

		// MVP = proj * view (column-major multiply)
		float mvp[16] = {};
		for (int row = 0; row < 4; row++) {
			for (int col = 0; col < 4; col++) {
				for (int k = 0; k < 4; k++) {
					mvp[col * 4 + row] += proj_m[k * 4 + row] * view_m[col * 4 + k];
				}
			}
		}

		opennova::Frustum frustum = opennova::extract_frustum(mvp);
		traversal_config.quality = lod_quality;

		// Collect visible patches across sectors
		std::vector<opennova::VisiblePatch> visible;
		visible.reserve(256);
		opennova::TraversalStats tstats;

		// Engine: sector iteration uses 512-world-unit sectors before the shared
		// quadtree walk (Terrain_CollectVisibleSectors@0x5C9120).
		int cam_sx = (int)cam_pos.x >> 9;
		int cam_sz = (int)cam_pos.z >> 9;
		int mask_x = trn.wrap_x ? 0 : -16;
		int mask_z = trn.wrap_y ? 0 : -16;

		for (int dz = -5; dz <= 5; dz++) {
			for (int dx = 0; dx < 11; dx++) {
				int sx = (cam_sx - 5) + dx;
				int sz = dz + cam_sz;
				int gx = sx - trn.origin_x;
				int gz = sz - trn.origin_y;

				if ((gx & mask_x) != 0) gx = (gx < 0) ? 0 : 0xFF;
				if ((gz & mask_z) != 0) gz = (gz < 0) ? 0 : 0xFF;

				int sector_id = trn.sector_grid[gz & 0xF][gx & 0xF];
				if (sector_id <= 0) continue;

				int child = -1;
				if (sector_id == 1) child = 0;
				else if (sector_id == 3) child = 1;
				else if (sector_id == 2) child = 2;
				else if (sector_id == 4) child = 3;
				if (child < 0 || child >= 4 || l1_children[child] < 0) continue;

				float sector_ox = static_cast<float>(sx * 512);
				float sector_oz = static_cast<float>(sz * 512);

				const size_t sector_patch_begin = visible.size();
				opennova::traverse_quadtree(
					quad_nodes, tile_mesh_meta,
					l1_children[child],
					frustum, cam_pos.x, cam_pos.y, cam_pos.z,
					sector_ox, sector_oz,
					traversal_config, visible, tstats);

				// Detail foliage collection is NOT a radial walk: retail's
				// frustum-culled traversal hands each frustum-surviving emitted
				// node of LOD level >= 3 to the 16u cell collector, so cells
				// behind the camera never enter the visible-key list and the
				// far-slot pool's working set stays below its 16-bit-index
				// capacity (over-collection thrashed the witnessed LRU into
				// per-frame cell blink; D-FOLIAGE-13)
				// [orig: Terrain_TraverseQuadtreeNode handoff
				// @ 0x60905c..0x60907c -> Terrain_CollectNearFoliagePatches
				// @ 0x603e60]. The collector re-tests every cell's clamped
				// AABB against the 42u limit, so the handoff is a broad phase.
				for (size_t pi = sector_patch_begin; pi < visible.size(); ++pi) {
					const opennova::VisiblePatch &vp = visible[pi];
					if (vp.lod_level < 3 || vp.tile_index < 0 ||
							vp.tile_index >= (int)tile_mesh_meta.size()) {
						continue;
					}
					const int node_size = 1024 >> vp.lod_level;
					const auto &tm = tile_mesh_meta[vp.tile_index];
					const int local_x = (int)std::lround(
						tm.center[0] - node_size * 0.5f);
					const int local_z = (int)std::lround(
						tm.center[2] - node_size * 0.5f);
					opennova::collect_foliage_detail_patches(
						mipchain, sector_id, sx * 512, sz * 512,
						local_x, local_z, node_size,
						static_cast<float>(cam_pos.x),
						static_cast<float>(cam_pos.y),
						static_cast<float>(cam_pos.z),
						foliage_detail_patches);
				}
			}
		}

		// Sort front-to-back
		std::sort(visible.begin(), visible.end(),
			[](const opennova::VisiblePatch& a, const opennova::VisiblePatch& b) {
				return a.distance < b.distance;
			});

		// Assign visible patches to pool via RenderingServer
		RenderingServer* rs = RenderingServer::get_singleton();
		int count = std::min(static_cast<int>(visible.size()), PATCH_POOL_SIZE);

		for (int i = 0; i < count; i++) {
			const auto& vp = visible[i];

			if (vp.tile_index < 0 || vp.tile_index >= static_cast<int>(tile_infos.size())) {
				if (patch_visible[i]) {
					rs->instance_set_visible(patch_instances[i], false);
					patch_visible[i] = false;
				}
				continue;
			}

			const auto& ti = tile_infos[vp.tile_index];

			// Select LOD level
			int lod = opennova::terrain_lod_family(vp.lod_sub);
			if (ti.lod_meshes[lod].is_null() && lod != 0) lod = 0;
			if (ti.lod_meshes[lod].is_null()) {
				if (patch_visible[i]) {
					rs->instance_set_visible(patch_instances[i], false);
					patch_visible[i] = false;
				}
				continue;
			}

			const auto& source_tile =
				terrain_data->get_cpt().tiles[vp.tile_index];

			// Only update mesh if changed
			RID mesh_rid = ti.lod_meshes[lod]->get_rid();
			if (mesh_rid != last_mesh_rid[i]) {
				rs->instance_set_base(patch_instances[i], mesh_rid);
				last_mesh_rid[i] = mesh_rid;
			}

			// Only update transform if changed
			Transform3D xform(Basis(), Vector3(vp.sector_ox, 0.0f, vp.sector_oz));
			if (xform != last_transform[i]) {
				rs->instance_set_transform(patch_instances[i], xform);
				last_transform[i] = xform;
			}

			rs->instance_geometry_set_shader_parameter(
				patch_instances[i], "u_instance_source_quadrant",
				Vector2(static_cast<float>((source_tile.tile_x >> 9) & 1),
					static_cast<float>((source_tile.tile_y >> 9) & 1)));

			// Per-instance debug data (only set when a debug mode is active)
			if (debug_mode > 0) {
				rs->instance_geometry_set_shader_parameter(patch_instances[i],
					"u_instance_lod", static_cast<float>(lod));
				rs->instance_geometry_set_shader_parameter(patch_instances[i],
					"u_instance_sector_x", vp.sector_ox / 512.0f);
				rs->instance_geometry_set_shader_parameter(patch_instances[i],
					"u_instance_sector_z", vp.sector_oz / 512.0f);
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

		// Capture debug stats
		last_stats = tstats;
		std::memset(lod_distribution, 0, sizeof(lod_distribution));
		for (int i = 0; i < count; i++) {
			int lod = opennova::terrain_lod_family(visible[i].lod_sub);
			lod_distribution[lod]++;
		}

		// Update shader parameters on the single shared material
		if (terrain_material.is_valid()) {
			terrain_material->set_shader_parameter("u_debug_mode", debug_mode);
		}

		// Update lighting from NovaEnvironment, prefer smoothed colors from NovaWeather
		// Uses dynamic call() so environment/weather can be GDScript or C++.
		if (terrain_material.is_valid()) {
			if (!terrain_node_cache_valid)
				_cache_env_weather_nodes();

			if (cached_env_node && (bool)cached_env_node->call("is_loaded")) {
				// Base env -> terrain-uniform push, shared with the editor preview
				// (NovaEnvironment.apply_terrain_uniforms drives both shaders' uniforms).
				cached_env_node->call("apply_terrain_uniforms", terrain_material);
				// Runtime-only: prefer NovaWeather-smoothed colors when a weather node
				// is present (overriding the ones it smooths). The terrain surface
				// consumes only c1 = light + c0 = sky [orig: @ 0x604420].
				if (cached_weather_node) {
					terrain_material->set_shader_parameter("u_sun_light", cached_weather_node->call("get_smooth_sun"));
					terrain_material->set_shader_parameter("u_sky_ambient", cached_weather_node->call("get_smooth_sky"));
					terrain_material->set_shader_parameter("u_fog_color", cached_weather_node->call("get_smooth_fog"));
				}
				// Tile overlay tint: HALF(terrain_rgb) under MODULATE2X folded to
				// one multiply; the shared runtime/ONED tile path consumes this uniform.
				// [orig: PolyTrn_RenderTile @ 0x60df0d].
				tile_overlay_tint =
					cached_env_node->call("get_tile_overlay_tint");
				terrain_material->set_shader_parameter(
					"u_tile_overlay_tint", tile_overlay_tint);
			}
		}

	} else if (p_what == NOTIFICATION_READY) {
		// Auto-load and auto-build if terrain_data is configured
		if (terrain_data.is_valid() && !terrain_data->is_loaded()) {
			Error err = terrain_data->load();
			if (err != OK) {
				UtilityFunctions::push_warning("NovaTerrain: Failed to auto-load terrain data");
				return;
			}
		}
		if (terrain_data.is_valid() && terrain_data->is_loaded() && !built) {
			build();
		}
		terrain_node_cache_valid = false;
		set_process(true);
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		_clear_patch_pool();
		cached_env_node = nullptr;
		cached_weather_node = nullptr;
		terrain_node_cache_valid = false;
	}
}

void NovaTerrain::_cache_env_weather_nodes() {
	terrain_node_cache_valid = true;
	cached_env_node = nullptr;
	cached_weather_node = nullptr;

	if (!environment_path.is_empty())
		cached_env_node = get_node_or_null(environment_path);
	if (cached_env_node && cached_env_node->has_method("is_loaded")) {
		if (!weather_path.is_empty())
			cached_weather_node = get_node_or_null(weather_path);
		if (cached_weather_node && !cached_weather_node->has_method("get_smooth_sun"))
			cached_weather_node = nullptr;
	} else {
		cached_env_node = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Strip-to-list conversion
// ---------------------------------------------------------------------------

void NovaTerrain::_strip_to_list(const std::vector<uint16_t>& strip,
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

Vector3 NovaTerrain::_heightmap_normal(const std::vector<uint16_t>& depth, int gx, int gz,
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

Ref<Shader> NovaTerrain::_load_terrain_shader() {
	return ResourceLoader::get_singleton()->load("res://shaders/terrain.gdshader", "Shader");
}

// ---------------------------------------------------------------------------
// Texture loading — uses Texture2D resources from NovaTerrainData
// ---------------------------------------------------------------------------

void NovaTerrain::_clear_derived_textures() {
	surface_inputs->clear_derived_textures();
	if (terrain_material.is_valid()) {
		surface_inputs->apply_to_material(terrain_material);
	}
}

void NovaTerrain::_load_textures() {
	if (terrain_material.is_null() || terrain_data.is_null()) {
		return;
	}
	surface_inputs->rebuild(
		terrain_data, tile_info_override, tile_overlay_enabled);
	surface_inputs->apply_to_material(terrain_material);
}

void NovaTerrain::_clear_tile_overlay_texture() {
	surface_inputs->clear_tile_overlay();
	if (terrain_material.is_valid()) {
		surface_inputs->apply_to_material(terrain_material);
	}
}

void NovaTerrain::_rebuild_tile_overlay_texture() {
	if (terrain_material.is_null()) {
		return;
	}
	surface_inputs->set_terrain_data(terrain_data);
	surface_inputs->set_tile_info_override(tile_info_override);
	surface_inputs->set_tile_overlay_enabled(tile_overlay_enabled);
	surface_inputs->rebuild_tile_overlay();
	surface_inputs->apply_to_material(terrain_material);
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void NovaTerrain::_on_terrain_changed() {
	if (!built || terrain_data.is_null()) return;
	// Rebuild the derived terrain inputs and update shader parameters.
	_load_textures();
}

void NovaTerrain::_hide_visible_patches() {
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

void NovaTerrain::_clear_patch_pool() {
	foliage_detail_patches.clear();
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
	}
	patches_active = 0;
}

void NovaTerrain::_clear_terrain() {
	_clear_patch_pool();
	_clear_tile_overlay_texture();
	_clear_derived_textures();
	foliage_detail_patches.clear();

	tile_infos.clear();
	quad_nodes.clear();
	tile_mesh_meta.clear();
	mipchain = {};
	root_node = -1;
	for (int i = 0; i < 4; i++) l1_children[i] = -1;
	built = false;
}

void NovaTerrain::build() {
	_clear_terrain();

	if (terrain_data.is_null() || !terrain_data->is_loaded()) {
		UtilityFunctions::push_warning("NovaTerrain::build() — terrain_data not loaded");
		return;
	}

	if (!_build_terrain()) {
		UtilityFunctions::push_warning("NovaTerrain: no valid baked CPT data; skipping native mesh build");
		return;
	}
	_build_quadtree();

	// Create lightweight RenderingServer instances for the patch pool
	RenderingServer* rs = RenderingServer::get_singleton();
	RID scenario = get_world_3d()->get_scenario();
	RID mat_rid = terrain_material->get_rid();

	for (int i = 0; i < PATCH_POOL_SIZE; i++) {
		RID inst = rs->instance_create();
		rs->instance_set_scenario(inst, scenario);
		rs->instance_geometry_set_material_override(inst, mat_rid);
		// World-visible plus the terrain-only static-shadow receiver marker.
		// Keep bit assignments mirrored in nova_water.gd.
		rs->instance_set_layer_mask(inst, (1u << 0) | (1u << 15));
		rs->instance_set_visible(inst, false);
		patch_instances[i] = inst;
		patch_visible[i] = false;
	}

	_load_textures();

	built = true;

	UtilityFunctions::print_verbose("NovaTerrain: Built ", static_cast<int>(tile_infos.size()),
		" tiles, ", static_cast<int>(quad_nodes.size()), " quad nodes, pool=", PATCH_POOL_SIZE);
}

bool NovaTerrain::_build_terrain() {
	const auto& cpt = terrain_data->get_cpt();

	if (cpt.tiles.empty() || cpt.depth_buffer.empty()) {
		return false;
	}

	constexpr int hm_size = 1024;
	constexpr size_t expected_depth_samples = static_cast<size_t>(hm_size) * static_cast<size_t>(hm_size);
	if (cpt.depth_buffer.size() != expected_depth_samples) {
		UtilityFunctions::push_warning(
			"NovaTerrain: CPT depth buffer has ",
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
	Ref<Shader> shadow_shader = ResourceLoader::get_singleton()->load(
			"res://shaders/sun_shadow_catcher.gdshader", "Shader");
	if (shadow_shader.is_valid()) {
		shadow_receiver_material.instantiate();
		shadow_receiver_material->set_shader(shadow_shader);
		terrain_material->set_next_pass(shadow_receiver_material);
	}

	tile_infos.resize(cpt.tiles.size());
	tile_mesh_meta.resize(cpt.tiles.size());

	int total_verts = 0;
	int total_indices = 0;

	for (size_t ti = 0; ti < cpt.tiles.size(); ti++) {
		const auto& tile = cpt.tiles[ti];
		auto& info = tile_infos[ti];
		auto& meta = tile_mesh_meta[ti];

		meta.vertex_count = tile.vertex_count;
		meta.tile_size = tile.tile_size;
		meta.aabb_min[0] = meta.aabb_min[1] = meta.aabb_min[2] = 1e9f;
		meta.aabb_max[0] = meta.aabb_max[1] = meta.aabb_max[2] = -1e9f;

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
		// [orig: sub_402D20 — quadrant = (tile_x >= 0x200) + 2 * (tile_y >= 0x200).]
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

			// Update AABB
			if (lx < meta.aabb_min[0]) meta.aabb_min[0] = lx;
			if (hy < meta.aabb_min[1]) meta.aabb_min[1] = hy;
			if (lz < meta.aabb_min[2]) meta.aabb_min[2] = lz;
			if (lx > meta.aabb_max[0]) meta.aabb_max[0] = lx;
			if (hy > meta.aabb_max[1]) meta.aabb_max[1] = hy;
			if (lz > meta.aabb_max[2]) meta.aabb_max[2] = lz;
		}

		// Compute center + radius
		for (int i = 0; i < 3; i++)
			meta.center[i] = (meta.aabb_min[i] + meta.aabb_max[i]) * 0.5f;
		float dx = meta.aabb_max[0] - meta.center[0];
		float dy = meta.aabb_max[1] - meta.center[1];
		float dz = meta.aabb_max[2] - meta.center[2];
		meta.radius = std::sqrt(dx * dx + dy * dy + dz * dz);

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

			info.lod_index_counts[lod] = indices.size();
			meta.lods[lod].index_count = indices.size();

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

	UtilityFunctions::print_verbose("NovaTerrain: ", total_verts, " verts, ", total_indices, " indices across ",
		static_cast<int>(cpt.tiles.size()), " tiles");
	return true;
}

void NovaTerrain::_build_quadtree() {
	const auto& cpt = terrain_data->get_cpt();

	if (cpt.tiles.empty()) return;

	// Build tile lookup by (x, z, size)
	struct TileKey {
		uint16_t x, z, size;
		bool operator<(const TileKey& o) const {
			if (size != o.size) return size < o.size;
			if (x != o.x) return x < o.x;
			return z < o.z;
		}
	};
	std::map<TileKey, int> tile_lookup;
	for (size_t i = 0; i < cpt.tiles.size(); i++) {
		const auto& t = cpt.tiles[i];
		tile_lookup[{t.tile_x, t.tile_y, t.tile_size}] = static_cast<int>(i);
	}

	// Determine leaf size
	uint16_t leaf_size = 1024;
	for (const auto& t : cpt.tiles)
		if (t.tile_size < leaf_size) leaf_size = t.tile_size;

	// Build mipchain
	mipchain = opennova::build_mipchain(cpt.depth_buffer, 1024);

	// Shared quadtree builder feeding the same 1024 -> leaf subdivision the
	// engine traverses in Terrain_TraverseQuadTreeNode@0x5C89C0.
	struct Builder {
		std::vector<opennova::QuadNode>& nodes;
		const std::map<TileKey, int>& lut;
		opennova::Mipchain& mc;
		uint16_t leaf_sz;
		int atlas_dim;

		int build(int size, int lx, int lz, int lod_level) {
			opennova::QuadNode node;
			node.lod_level = lod_level;
			node.size = size;

			node.aabb_min[0] = static_cast<float>(lx & 0x1FF);
			node.aabb_min[2] = static_cast<float>(lz & 0x1FF);
			node.aabb_max[0] = static_cast<float>((lx & 0x1FF) + size);
			node.aabb_max[2] = static_cast<float>((lz & 0x1FF) + size);

			// Height bounds from mipchain
			if (lod_level < mc.level_count) {
				int cells_per_side = atlas_dim / size;
				int mip_idx = (lx / size) + (lz / size) * cells_per_side;
				uint8_t* entry = mc.levels[lod_level] + 2 * mip_idx;
				node.aabb_min[1] = static_cast<float>(entry[0]) * 0.5f;
				node.aabb_max[1] = static_cast<float>(entry[1]) * 0.5f;
			} else {
				node.aabb_min[1] = 0;
				node.aabb_max[1] = 128.0f;
			}

			for (int i = 0; i < 3; i++)
				node.center[i] = (node.aabb_min[i] + node.aabb_max[i]) * 0.5f;
			float dx = node.aabb_max[0] - node.center[0];
			float dy = node.aabb_max[1] - node.center[1];
			float dz = node.aabb_max[2] - node.center[2];
			node.radius = std::sqrt(dx * dx + dy * dy + dz * dz);

			auto it = lut.find({static_cast<uint16_t>(lx), static_cast<uint16_t>(lz),
			                    static_cast<uint16_t>(size)});
			node.tile_index = (it != lut.end()) ? it->second : -1;
			node.is_leaf = (size <= static_cast<int>(leaf_sz));

			int this_idx = static_cast<int>(nodes.size());
			nodes.push_back(node);

			if (size > static_cast<int>(leaf_sz)) {
				int half = size / 2;
				nodes[this_idx].children[0] = build(half, lx,        lz,        lod_level + 1);
				nodes[this_idx].children[1] = build(half, lx + half, lz,        lod_level + 1);
				nodes[this_idx].children[2] = build(half, lx,        lz + half, lod_level + 1);
				nodes[this_idx].children[3] = build(half, lx + half, lz + half, lod_level + 1);
			}
			return this_idx;
		}
	};

	quad_nodes.reserve(512);
	Builder builder{quad_nodes, tile_lookup, mipchain, leaf_size, 1024};
	root_node = builder.build(1024, 0, 0, 0);

	if (root_node >= 0) {
		for (int i = 0; i < 4; i++)
			l1_children[i] = quad_nodes[root_node].children[i];
	}

	UtilityFunctions::print_verbose("NovaTerrain: Quadtree built — ", static_cast<int>(quad_nodes.size()),
		" nodes, leaf_size=", leaf_size, ", mipchain levels=", mipchain.level_count);
}

// ---------------------------------------------------------------------------
// Debug API
// ---------------------------------------------------------------------------

Dictionary NovaTerrain::get_traversal_stats() const {
	Dictionary d;
	d["nodes_visited"] = last_stats.nodes_visited;
	d["rej_nearfar"] = last_stats.rej_nearfar;
	d["rej_left"] = last_stats.rej_left;
	d["rej_right"] = last_stats.rej_right;
	d["rej_bottom"] = last_stats.rej_bottom;
	d["rej_top"] = last_stats.rej_top;
	d["partial_subdiv"] = last_stats.partial_subdiv_count;
	d["budget_drops"] = last_stats.budget_drops;
	d["leaf_emits"] = last_stats.leaf_emits;
	d["nonleaf_emits"] = last_stats.nonleaf_emits;
	d["dist_min"] = last_stats.dist_min;
	d["dist_max"] = last_stats.dist_max;
	d["lod_fallbacks"] = last_stats.lod_fallbacks;
	return d;
}

PackedInt32Array NovaTerrain::get_lod_distribution() const {
	PackedInt32Array arr;
	arr.resize(8);
	for (int i = 0; i < 8; i++)
		arr.set(i, lod_distribution[i]);
	return arr;
}

int NovaTerrain::get_patches_active() const {
	return patches_active;
}

int NovaTerrain::get_visible_patch_count() const {
	int visible_count = 0;
	for (int i = 0; i < PATCH_POOL_SIZE; i++) {
		if (patch_visible[i]) {
			visible_count++;
		}
	}
	return visible_count;
}

const std::vector<FoliageDetailPatch> &NovaTerrain::get_foliage_detail_patches_native() const {
	return foliage_detail_patches;
}

void NovaTerrain::set_debug_no_frustum(bool v) { traversal_config.no_frustum = v; }
bool NovaTerrain::get_debug_no_frustum() const { return traversal_config.no_frustum; }
void NovaTerrain::set_debug_no_nearfar(bool v) { traversal_config.no_nearfar = v; }
bool NovaTerrain::get_debug_no_nearfar() const { return traversal_config.no_nearfar; }
void NovaTerrain::set_debug_no_sideplanes(bool v) { traversal_config.no_sideplanes = v; }
bool NovaTerrain::get_debug_no_sideplanes() const { return traversal_config.no_sideplanes; }
void NovaTerrain::set_debug_no_partial_subdiv(bool v) { traversal_config.no_partial_subdiv = v; }
bool NovaTerrain::get_debug_no_partial_subdiv() const { return traversal_config.no_partial_subdiv; }
void NovaTerrain::set_debug_force_leaves(bool v) { traversal_config.force_leaves = v; }
bool NovaTerrain::get_debug_force_leaves() const { return traversal_config.force_leaves; }
void NovaTerrain::set_debug_force_lod0(bool v) { traversal_config.force_lod0 = v; }
bool NovaTerrain::get_debug_force_lod0() const { return traversal_config.force_lod0; }

void NovaTerrain::set_debug_mode(int mode) { debug_mode = mode; }
int NovaTerrain::get_debug_mode() const { return debug_mode; }
