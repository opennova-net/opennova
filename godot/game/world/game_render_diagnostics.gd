class_name GameRenderDiagnostics
extends RefCounted

## Typed, immutable-at-the-seam snapshot of the renderer state used by a
## GameWorld frame. The world exposes this one record; MCP, probes, and future
## comparison tooling consume to_json_value() instead of reaching into scene
## internals independently.

const SCHEMA := "OpenNovaRenderDiagnosticsV1"
const LIGHT_LIMIT := 128
const ANISOTROPY_SETTING := \
		"rendering/textures/default_filters/anisotropic_filtering_level"

var _value: Dictionary = {}


static func from_world(
		world: GameWorld,
		camera: Camera3D,
		viewport: Viewport) -> GameRenderDiagnostics:
	var snapshot := GameRenderDiagnostics.new()
	snapshot._sample(world, camera, viewport)
	return snapshot


func to_json_value() -> Dictionary:
	return _value.duplicate(true)


func _sample(world: GameWorld, camera: Camera3D, viewport: Viewport) -> void:
	var env: MissionEnvironment = world.get_environment_node()
	var weather: Weather = world.get_weather_node()
	var water: Water = world.get_water_node()
	var sky := world.get_node_or_null("SkyDome") as SkyDome
	var celestial: Celestial = world.get_celestial_node()
	var dynamic_shadow := world.get_node_or_null("SunShadow") as SunShadow
	var terrain: Terrain = world.get_terrain_node()
	var clear := world.get_node_or_null("ClearColor") as WorldEnvironment
	_value = {
		"schema": SCHEMA,
		"frame": {
			"process": Engine.get_process_frames(),
			"physics": Engine.get_physics_frames(),
			"ticks_usec": Time.get_ticks_usec(),
		},
		"world": {
			"loaded": bool(world.is_loaded()),
			"mission_file": String(world.get_loaded_mission_file()),
			"visible": bool(world.is_visible_in_tree()),
			"clear_color": (
					clear.environment.background_color
					if clear != null and clear.environment != null
					else null),
		},
		"camera": _camera_state(camera, viewport),
		"environment": _environment_state(env),
		"weather": _weather_state(weather, camera),
		"sky": _sky_state(sky),
		"celestial": _celestial_state(celestial),
		"water": _water_state(water, camera),
		"terrain": _terrain_state(world),
		"shadows": {
			"dynamic": _shadow_state(dynamic_shadow),
			"static_terrain": _terrain_page_shadow_state(terrain),
		},
		"lights": _lights_state(world),
		"passes": {
			# Godot exposes counts for the last completed draw. A capture bundle
			# samples this immediately after frame_post_draw, so these counters and
			# the readback image share the frame serial above.
			"relation": "last_completed_draw",
			"root": _viewport_pass_state(viewport),
			"water_reflection": _viewport_pass_state(
					water.get_reflection_viewport() if water != null else null),
		},
		"renderer": _renderer_state(viewport),
		"runtime": {
			"performance": world.get_runtime_perf_counters(),
		},
	}


static func _terrain_state(world: GameWorld) -> Dictionary:
	if world == null:
		return {"available": false, "surface_inputs": {}}
	var terrain: Terrain = world.get_terrain_node()
	if terrain == null:
		return {"available": false, "surface_inputs": {}}
	var surface_inputs: TerrainSurfaceInputs = terrain.get_surface_inputs()
	return {
		"available": true,
		"visible": terrain.visible,
		"visible_in_tree": terrain.is_visible_in_tree(),
		"tile_cache": terrain.get_tile_cache_diagnostics(),
		"surface_inputs": (
				surface_inputs.get_diagnostics()
				if surface_inputs != null else {}),
	}


static func _camera_state(camera: Camera3D, viewport: Viewport) -> Dictionary:
	if camera == null:
		return {"available": false}
	var camera_transform := camera.get_camera_transform()
	var projection := camera.get_camera_projection()
	var viewport_size := viewport.get_visible_rect().size \
			if viewport != null else Vector2.ZERO
	return {
		"available": true,
		"current": camera.is_current(),
		"global_transform": camera_transform,
		"view_transform": camera_transform.affine_inverse(),
		"projection_columns": _projection_columns(projection),
		"projection_mode": camera.get_projection(),
		"projection_label": _projection_label(camera.get_projection()),
		"fov_deg": camera.get_fov(),
		"size": camera.get_size(),
		"near": camera.get_near(),
		"far": camera.get_far(),
		"frustum_offset": camera.get_frustum_offset(),
		"h_offset": camera.get_h_offset(),
		"v_offset": camera.get_v_offset(),
		"keep_aspect": camera.get_keep_aspect_mode(),
		"cull_mask": camera.get_cull_mask(),
		"viewport_size": viewport_size,
		"aspect": (
				viewport_size.x / viewport_size.y
				if viewport_size.y > 0.0 else 0.0),
	}


static func _projection_columns(projection: Projection) -> Array:
	return [
		[projection.x.x, projection.x.y, projection.x.z, projection.x.w],
		[projection.y.x, projection.y.y, projection.y.z, projection.y.w],
		[projection.z.x, projection.z.y, projection.z.z, projection.z.w],
		[projection.w.x, projection.w.y, projection.w.z, projection.w.w],
	]


static func _projection_label(mode: int) -> String:
	match mode:
		Camera3D.PROJECTION_PERSPECTIVE:
			return "perspective"
		Camera3D.PROJECTION_ORTHOGONAL:
			return "orthogonal"
		Camera3D.PROJECTION_FRUSTUM:
			return "frustum"
	return "unknown"


static func _environment_state(env: MissionEnvironment) -> Dictionary:
	if env == null:
		return {"available": false, "loaded": false}
	var light_state: EnvLightState = env.get_light_state()
	var values: EnvLightValues = light_state.get_values() \
			if light_state != null else null
	return {
		"available": true,
		"loaded": env.is_loaded(),
		"time_hhmm": env.get_time_of_day(),
		"mission_minute_of_day": env.get_mission_minute_of_day(),
		"mission_time_fixed24": env.get_mission_time_fixed24(),
		"generation": env.get_env_generation(),
		"light_state_generation": (
				light_state.get_generation() if light_state != null else -1),
		"weather_driven": env.is_weather_driven(),
		"night_phase": env.is_night_phase(),
		"day_phase_blend": env.get_day_phase_blend(),
		"sun_direction": env.get_sun_direction(),
		"moon_direction": env.get_moon_direction(),
		"active_light_direction": env.get_light_direction(),
		"sun_color": env.get_sun_color(),
		"moon_color": env.get_moon_color(),
		"fill_light": env.get_fill_light(),
		"sun_light": env.get_sun_light(),
		"sky_ambient": env.get_sky_ambient(),
		"fog_color": env.get_fog_color(),
		"skyfog_color": env.get_skyfog_color(),
		"fog_start": env.get_fog_start(),
		"fog_end": env.get_fog_end_distance(),
		"fog_type": env.get_fog_type(),
		"fog_distance": env.get_fog_distance(),
		"sky_height": env.get_sky_height(),
		"sky_speed": env.get_sky_speed(),
		"water_color": env.get_water_color(),
		"source_gain": env.get_color_src_gain(),
		"published_lighting": _light_values_state(values),
	}


static func _light_values_state(values: EnvLightValues) -> Dictionary:
	if values == null:
		return {"available": false}
	return {
		"available": true,
		"hemi_sky": values.get_hemi_sky(),
		"direction": values.get_dir(),
		"directional_color": values.get_dir_color(),
		"hemi_ground": values.get_hemi_ground(),
		"ceiling": values.get_ceiling(),
		"floor": values.get_floor_color(),
		"gain": values.get_gain(),
		"fog_enabled": values.get_fog_enabled(),
		"fog_color": values.get_fog_color(),
		"fog_start": values.get_fog_start(),
		"fog_end": values.get_fog_end(),
		"fog_type": values.get_fog_type(),
	}


static func _weather_state(weather: Weather, camera: Camera3D) -> Dictionary:
	if weather == null:
		return {"available": false}
	var position := camera.get_camera_transform().origin \
			if camera != null else Vector3.ZERO
	return {
		"available": true,
		"wind_strength": weather.get_wind_strength(),
		"wind_duration": weather.get_wind_duration(),
		"sway_amount": weather.get_sway_amount(),
		"sway_phase": weather.get_sway_phase(),
		"lightning_intensity": weather.get_lightning_intensity(),
		"cloud_uv_rate_per_second": weather.get_cloud_uv_rate_per_second(),
		"cloud_uv_offset1": weather.get_cloud_uv_offset1(position.x, position.z),
		"cloud_uv_offset2": weather.get_cloud_uv_offset2(position.x, position.z),
		"smooth_fill": weather.get_smooth_fill(),
		"smooth_sun": weather.get_smooth_sun(),
		"smooth_fog": weather.get_smooth_fog(),
		"smooth_sky": weather.get_smooth_sky(),
		"smooth_skyfog": weather.get_smooth_skyfog(),
		"smooth_ceiling": weather.get_smooth_ceiling(),
		"smooth_cloud": weather.get_smooth_cloud(),
		"smooth_floor": weather.get_smooth_floor(),
		# The marched iris-exposure feed (D-RLIT-2): the stamped per-sample
		# classification codes distinguish a starved capture (empty) from a
		# settled one.
		"iris_samples": Array(weather.get_iris_samples()),
	}


# Celestial bodies + the glare occlusion accumulator (docs/env/env-tod-re.md
# "Celestial bodies"): per-body opacity/visibility and the glare
# brightness/window make "occluded", "starved", and "model absent"
# distinguishable in a captured fixture state.
static func _celestial_state(celestial: Celestial) -> Dictionary:
	if celestial == null:
		return {"available": false}
	var state: Dictionary = celestial.get_diagnostics()
	state["available"] = true
	state["visible"] = celestial.is_visible_in_tree()
	return state


static func _sky_state(sky: SkyDome) -> Dictionary:
	if sky == null:
		return {"available": false, "built": false}
	var material: ShaderMaterial = sky.get_sky_material()
	var mesh: MeshInstance3D = sky.get_mesh_instance()
	return {
		"available": true,
		"built": sky.is_built(),
		"visible": sky.is_visible_in_tree(),
		"mesh_visible": mesh.is_visible_in_tree() if mesh != null else false,
		"shader": _shader_parameters(material, [
			"u_flat_pass", "u_flat_color", "u_sky_base", "u_sky_bright",
			"u_sky_highlight", "u_cloud_base", "u_cloud_highlight",
			"u_cloud_edge", "u_sun_dir", "u_light_dir", "u_fog_color",
			"u_fog_end", "u_sky_height", "u_has_clouds",
			"u_scroll_offset1", "u_scroll_offset2", "u_cloud_tex1",
			"u_cloud_tex2",
		]),
	}


static func _water_state(water: Water, camera: Camera3D) -> Dictionary:
	if water == null:
		return {"available": false, "render_active": false}
	var material: ShaderMaterial = water.get_water_material()
	var mesh: MeshInstance3D = water.get_mesh_instance()
	var reflection_viewport: SubViewport = water.get_reflection_viewport()
	var camera_height := camera.get_camera_transform().origin.y \
			if camera != null else NAN
	return {
		"available": true,
		"built": water.is_built(),
		"active": water.is_water_active(),
		"render_active": water.is_water_render_active(),
		"height": water.get_water_height(),
		"alpha": water.get_water_alpha(),
		"camera_above": (
				camera_height > water.get_water_height()
				if not is_nan(camera_height) else null),
		"mesh_visible": mesh.is_visible_in_tree() if mesh != null else false,
		"surface_count": (
				mesh.mesh.get_surface_count()
				if mesh != null and mesh.mesh != null else 0),
		"shader": _shader_parameters(material, [
			"u_water_color", "u_has_reflection", "u_reflection_uv_scale",
			"u_water_uv", "u_fog_color", "u_water_murk",
			"u_underwater_view", "u_reflection", "u_noise_color",
			"u_noise_normal",
		]),
		"reflection": {
			"available": reflection_viewport != null,
			"size": reflection_viewport.size \
					if reflection_viewport != null else Vector2i.ZERO,
			"camera": _camera_state(
					water.get_reflection_camera(), reflection_viewport),
		},
	}


static func _shader_parameters(
		material: ShaderMaterial,
		names: Array) -> Dictionary:
	if material == null:
		return {"available": false}
	var parameters := {"available": true}
	for raw_name in names:
		var name := StringName(String(raw_name))
		parameters[String(name)] = _resource_value(
				material.get_shader_parameter(name))
	return parameters


static func _resource_value(value: Variant) -> Variant:
	if value is Texture2D:
		var texture := value as Texture2D
		return {
			"available": true,
			"class": texture.get_class(),
			"path": texture.resource_path,
			"size": texture.get_size(),
			"instance_id": texture.get_instance_id(),
		}
	return value


static func _shadow_state(shadow: SunShadow) -> Dictionary:
	if shadow == null:
		return {"available": false}
	return {
		"available": true,
		"projection_mode": shadow.get_projection_mode(),
		"visible": shadow.visible,
		"visible_in_tree": shadow.is_visible_in_tree(),
		"processing": shadow.is_processing(),
		"shadow_enabled": shadow.has_shadow(),
		"global_transform": shadow.global_transform,
		"emission_direction": -shadow.global_transform.basis.z,
		"receiver_cull_mask": shadow.get_cull_mask(),
		"caster_cull_mask": shadow.get_shadow_caster_mask(),
		"shadow_mode": shadow.get_shadow_mode(),
		"max_distance": shadow.get_param(Light3D.PARAM_SHADOW_MAX_DISTANCE),
		"bias": shadow.get_param(Light3D.PARAM_SHADOW_BIAS),
		"normal_bias": shadow.get_param(Light3D.PARAM_SHADOW_NORMAL_BIAS),
		"fade_start": shadow.get_param(Light3D.PARAM_SHADOW_FADE_START),
		"size": shadow.get_param(Light3D.PARAM_SIZE),
		"energy": shadow.get_param(Light3D.PARAM_ENERGY),
	}


static func _terrain_page_shadow_state(terrain: Terrain) -> Dictionary:
	if terrain == null:
		return {
			"available": false,
			"implementation": "terrain_page_alpha",
			"enabled": false,
			"active": false,
			"suppressed_bms_ids": [],
			"raster_jobs": 0,
			"raster_failures": 0,
			"alpha_changed_bytes": 0,
			"rgb_changed_bytes": 0,
			"frame_alpha_changed_bytes": 0,
			"frame_rgb_changed_bytes": 0,
			"frame_pages_with_draws": 0,
			"frame_triangles": 0,
		}
	var cache := terrain.get_tile_cache_diagnostics()
	var available := bool(cache.get("shadow_raster_available", false))
	var enabled := terrain.is_static_terrain_shadow_enabled()
	return {
		"available": available,
		"implementation": "terrain_page_alpha",
		"enabled": enabled,
		"active": available and enabled,
		"suppressed_bms_ids": Array(
				terrain.get_suppressed_static_shadow_bms_ids()),
		"raster_jobs": int(cache.get("shadow_raster_jobs", 0)),
		"raster_failures": int(cache.get("shadow_raster_failures", 0)),
		"alpha_changed_bytes": int(cache.get(
				"shadow_alpha_changed_bytes", 0)),
		"rgb_changed_bytes": int(cache.get("shadow_rgb_changed_bytes", 0)),
		"frame_alpha_changed_bytes": int(cache.get(
				"frame_shadow_alpha_changed_bytes", 0)),
		"frame_rgb_changed_bytes": int(cache.get(
				"frame_shadow_rgb_changed_bytes", 0)),
		"frame_pages_with_draws": int(cache.get(
				"shadow_provider_frame_pages_with_draws", 0)),
		"frame_triangles": int(cache.get(
				"shadow_provider_frame_triangles", 0)),
	}


static func _lights_state(root: Node) -> Dictionary:
	var all: Array[Light3D] = []
	_collect_lights(root, all)
	var active_lights: Array[Light3D] = []
	for light in all:
		if light.visible and light.is_visible_in_tree():
			active_lights.append(light)
	var rows: Array = []
	for index in range(mini(active_lights.size(), LIGHT_LIMIT)):
		rows.append(_light_state(active_lights[index]))
	var shadowed := 0
	var omni := 0
	var spot := 0
	var directional := 0
	for light in active_lights:
		if light.has_shadow():
			shadowed += 1
		if light is OmniLight3D:
			omni += 1
		elif light is SpotLight3D:
			spot += 1
		elif light is DirectionalLight3D:
			directional += 1
	# The EffectWorld point lights are shader-fed pool instances, not Light3D
	# nodes — the omni node census above must stay 0 while this sibling block
	# reports the hosted table (effect_light_director.gd).
	var effectworld: Dictionary = {}
	var world := root as GameWorld
	if world != null:
		var effect_report := world.get_effect_light_report()
		if effect_report != null:
			effectworld = effect_report.to_json_value() as Dictionary
	return {
		"total_nodes": all.size(),
		"active": active_lights.size(),
		"inactive": all.size() - active_lights.size(),
		"shadowed": shadowed,
		"omni": omni,
		"spot": spot,
		"directional": directional,
		"returned": rows.size(),
		"truncated": rows.size() < active_lights.size(),
		"rows": rows,
		"effectworld": effectworld,
	}


static func _collect_lights(node: Node, out: Array[Light3D]) -> void:
	if node is Light3D:
		out.append(node as Light3D)
	for child in node.get_children():
		_collect_lights(child, out)


static func _light_state(light: Light3D) -> Dictionary:
	var kind := "directional" if light is DirectionalLight3D else (
			"omni" if light is OmniLight3D else (
			"spot" if light is SpotLight3D else "light"))
	return {
		"path": String(light.get_path()),
		"class": light.get_class(),
		"kind": kind,
		"visible": light.visible,
		"visible_in_tree": light.is_visible_in_tree(),
		"global_transform": light.global_transform,
		"color": light.get_color(),
		"energy": light.get_param(Light3D.PARAM_ENERGY),
		"range": light.get_param(Light3D.PARAM_RANGE),
		"attenuation": light.get_param(Light3D.PARAM_ATTENUATION),
		"spot_angle": light.get_param(Light3D.PARAM_SPOT_ANGLE),
		"spot_attenuation": light.get_param(Light3D.PARAM_SPOT_ATTENUATION),
		"negative": light.is_negative(),
		"shadow_enabled": light.has_shadow(),
		"receiver_cull_mask": light.get_cull_mask(),
		"caster_cull_mask": light.get_shadow_caster_mask(),
	}


static func _viewport_pass_state(viewport: Viewport) -> Dictionary:
	if viewport == null:
		return {"available": false}
	return {
		"available": true,
		"visible_objects": viewport.get_render_info(
				Viewport.RENDER_INFO_TYPE_VISIBLE,
				Viewport.RENDER_INFO_OBJECTS_IN_FRAME),
		"visible_primitives": viewport.get_render_info(
				Viewport.RENDER_INFO_TYPE_VISIBLE,
				Viewport.RENDER_INFO_PRIMITIVES_IN_FRAME),
		"visible_draw_calls": viewport.get_render_info(
				Viewport.RENDER_INFO_TYPE_VISIBLE,
				Viewport.RENDER_INFO_DRAW_CALLS_IN_FRAME),
		"shadow_objects": viewport.get_render_info(
				Viewport.RENDER_INFO_TYPE_SHADOW,
				Viewport.RENDER_INFO_OBJECTS_IN_FRAME),
		"shadow_primitives": viewport.get_render_info(
				Viewport.RENDER_INFO_TYPE_SHADOW,
				Viewport.RENDER_INFO_PRIMITIVES_IN_FRAME),
		"shadow_draw_calls": viewport.get_render_info(
				Viewport.RENDER_INFO_TYPE_SHADOW,
				Viewport.RENDER_INFO_DRAW_CALLS_IN_FRAME),
	}


static func _renderer_state(viewport: Viewport) -> Dictionary:
	var anisotropy_level := int(ProjectSettings.get_setting(
			ANISOTROPY_SETTING, 0))
	var anisotropy_samples := _anisotropy_samples(anisotropy_level)
	var state := {
		"display_driver": DisplayServer.get_name(),
		"method": String(ProjectSettings.get_setting(
				"rendering/renderer/rendering_method", "unknown")),
		"adapter": RenderingServer.get_video_adapter_name(),
		"vendor": RenderingServer.get_video_adapter_vendor(),
		"api_version": RenderingServer.get_video_adapter_api_version(),
		"default_anisotropy": {
			"setting": ANISOTROPY_SETTING,
			"level": anisotropy_level,
			"samples": anisotropy_samples,
			"active": anisotropy_samples > 1,
		},
	}
	if viewport != null:
		state.merge({
			"viewport_size": viewport.get_visible_rect().size,
			"scaling_3d_scale": viewport.get_scaling_3d_scale(),
			"scaling_3d_mode": viewport.get_scaling_3d_mode(),
			"msaa_3d": viewport.get_msaa_3d(),
			"screen_space_aa": viewport.get_screen_space_aa(),
			"taa": viewport.is_using_taa(),
			"debanding": viewport.is_using_debanding(),
			"occlusion_culling": viewport.is_using_occlusion_culling(),
			"debug_draw": viewport.get_debug_draw(),
		})
	return state


static func _anisotropy_samples(level: int) -> int:
	match level:
		0:
			return 1
		1:
			return 2
		2:
			return 4
		3:
			return 8
		4:
			return 16
	return 0
