@tool
class_name NovaCelestial
extends Node3D

# Renders the sun/moon/star 3DI bodies named in the .env, attached to the sky.
# Engine equivalents (docs/env/env-tod-re.md "Celestial bodies"):
# - [orig: EffectWorld_LoadCelestialModels @ 0x5adc50] resolves sun_3di/moon_3di/
#   star_3di/glare_3di to models (glare/star under additive mode 0x300000).
# - [orig: render_celestial_bodies @ 0x5acaa0] places sun/moon at
#   camera + direction * 64 (full camera height, identity rotation) with the
#   witnessed overcast/SunDim (sun) and fog-distance (moon) alphas; the world
#   overdraws them, so depth-tested materials are the structural equivalent.
# - [orig: render_skybox_sun_glow @ 0x5acd00] drives the glare: two jittered
#   terrain rays per frame into an 8-sample window + hysteresis brightness,
#   glow alpha = dot_view^4/2 x brightness x folds (env #14, closed).
# The 3DI diffuse stays; bodies are tinted and dimmed by the TOD sun/moon color.

const NovaObjectModelScript = preload("res://engine/object/nova_object_model.gd")
const CelestialShader = preload("res://shaders/celestial.gdshader")
const CelestialAdditiveShader = preload("res://shaders/celestial_additive.gdshader")

# Render-priority rungs from the witnessed frame ladder, single-sourced from
# libs/renderer/render_order via NovaObjectShaderCache (maturity REN-3,
# docs/render/render-order-re.md): the sky pass draws star field then bodies
# BEFORE all world alpha [orig: Terrain_RenderSkyboxPass @ 0x610ac0 before
# Terrain_RenderSceneWithReflection @ 0x5c93a0]; the glare is the frame's
# final draw [orig: render_skybox_sun_glow @ 0x5c9714].
const PRIORITY_SUN := NovaObjectShaderCache.RENDER_RUNG_SKY_BODY
const PRIORITY_MOON := NovaObjectShaderCache.RENDER_RUNG_SKY_BODY
const PRIORITY_STAR := NovaObjectShaderCache.RENDER_RUNG_SKY_STARS
const PRIORITY_GLARE := NovaObjectShaderCache.RENDER_RUNG_SUN_GLOW

@export var environment_path: NodePath
# The loaded terrain for the glare occlusion rays; no terrain = unobstructed.
var terrain_data: NovaTerrainData = null

var _glare_occlusion := NovaGlareOcclusion.new()
var _resource_root: NovaResourceRoot
var _cached_env: Node = null
var _cached_cam: Camera3D = null
var _bodies: Dictionary = {} # name -> { model, materials, tint }
var _loaded_names: Dictionary = {}
# env #33: the 256-instance star field (generation + twinkle in libs/env via
# NovaStarField; regenerated per celestial load like retail
# [orig: Star_GenerateInstanceTable @ 0x5ac850 <- EffectWorld_LoadCelestialModels]).
var _star_core := NovaStarField.new()
var _star_mmi: MultiMeshInstance3D = null


func _ready() -> void:
	_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null
	_rebuild_if_needed()


func set_resource_root(value: NovaResourceRoot) -> void:
	_resource_root = value
	_loaded_names.clear()
	_rebuild_if_needed()


func _env_data() -> EnvFile:
	if _cached_env and _cached_env.has_method("get_environment_data"):
		return _cached_env.get_environment_data()
	return null


func _rebuild_if_needed() -> void:
	var env_data := _env_data()
	if env_data == null or _resource_root == null:
		return
	var wanted := {
		"sun": { "name": env_data.get_sun_3di(), "additive": false, "priority": PRIORITY_SUN, "tint": "sun" },
		"moon": { "name": env_data.get_moon_3di(), "additive": false, "priority": PRIORITY_MOON, "tint": "moon" },
		"glare": { "name": env_data.get_glare_3di(), "additive": true, "priority": PRIORITY_GLARE, "tint": "sun" },
	}
	# Rebuild only when the set of names actually changed (undo/scrub safe).
	var signature := {}
	for key in wanted:
		signature[key] = wanted[key]["name"]
	signature["star"] = env_data.get_star_3di()
	if signature == _loaded_names:
		return
	_loaded_names = signature

	for child in get_children():
		remove_child(child)
		child.queue_free()
	_bodies.clear()
	_star_mmi = null
	_build_star_field(env_data.get_star_3di())

	for key in wanted:
		var spec: Dictionary = wanted[key]
		var model_name: String = spec["name"]
		if model_name.strip_edges().is_empty():
			continue
		var data := _load_object_data(model_name)
		if data == null:
			continue
		var model: Node3D = NovaObjectModelScript.new()
		model.name = "Celestial_" + key
		add_child(model)
		model.set_object_data(data)
		var material := _make_celestial_material(spec["additive"], spec["priority"])
		var materials := _apply_material_override(model, material)
		if key == "glare":
			for surface_material in materials:
				surface_material.set_shader_parameter("u_glare_view_fade", true)
		_bodies[key] = {
			"model": model,
			"materials": materials,
			"tint": spec["tint"],
		}


func _load_object_data(graphic: String) -> NovaObjectData:
	var model_name := graphic
	if model_name.get_extension().is_empty():
		model_name += ".3di"
	var data := NovaObjectData.new()
	if data.open_from_resource_root(_resource_root, model_name) == OK:
		return data
	return null


func _make_celestial_material(additive: bool, priority: int) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	material.shader = CelestialAdditiveShader if additive else CelestialShader
	material.render_priority = priority
	if additive:
		# Star default opacity - a stand-in until the witnessed per-star
		# twinkle lands (env #33; the 0x2000/0x10000 value stems from the dead
		# variant @ 0x5ac230). The glare overwrites its opacity per frame.
		material.set_shader_parameter("u_opacity", float(0x2000) / 65536.0)
	return material


# Walk the model's MeshInstance3D parts, reuse each surface's diffuse texture in
# our celestial material, and return the ACTUAL installed materials. NovaObjectModel
# puts its generated material in GeometryInstance3D.material_override; clear that
# whole-mesh override after harvesting its texture so these surface overrides own
# the draw and remain the objects updated by the TOD pass.
func _apply_material_override(model: Node3D, base_material: ShaderMaterial) -> Array[ShaderMaterial]:
	var installed: Array[ShaderMaterial] = []
	var meshes: Array[MeshInstance3D] = []
	_collect_meshes(model, meshes)
	for mesh_instance in meshes:
		# The vertex shader relocates celestials for the active render-pass camera.
		# Keep the source-camera AABB/occlusion result from rejecting the mirror
		# pass before that relocation reaches the GPU.
		mesh_instance.extra_cull_margin = 1.0e6
		mesh_instance.ignore_occlusion_culling = true
		mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		var surface_count := mesh_instance.mesh.get_surface_count() if mesh_instance.mesh else 0
		var surface_materials: Array[ShaderMaterial] = []
		for surface in surface_count:
			var src := mesh_instance.get_active_material(surface)
			var material := base_material.duplicate() as ShaderMaterial
			# The stock sun/moon models author FF_ST_AD_LUM. Their textures use
			# opaque black as the additive zero, so forcing those surfaces
			# through blend_mix exposes a dark quad at the horizon. Preserve
			# the source material's blend classification while replacing only
			# the celestial placement/tint shader logic.
			if source_material_uses_additive(src):
				material.shader = CelestialAdditiveShader
			if src is ShaderMaterial:
				var diffuse = (src as ShaderMaterial).get_shader_parameter("u_diffuse")
				if diffuse:
					material.set_shader_parameter("u_diffuse", diffuse)
			surface_materials.append(material)
		# NovaObjectModel's whole-mesh override otherwise wins over the celestial
		# surface materials on the render path.
		mesh_instance.material_override = null
		for surface in surface_count:
			var material := surface_materials[surface]
			mesh_instance.set_surface_override_material(surface, material)
			installed.append(material)
	return installed


## Whether a source surface uses additive blending that the celestial
## replacement material must preserve.
static func source_material_uses_additive(source: Material) -> bool:
	if not (source is ShaderMaterial):
		return false
	var shader := (source as ShaderMaterial).shader
	return shader != null and shader.code.contains("blend_add")


static func _collect_meshes(node: Node, out: Array[MeshInstance3D]) -> void:
	if node is MeshInstance3D:
		out.append(node)
	for child in node.get_children():
		_collect_meshes(child, out)


func _process(_delta: float) -> void:
	if _bodies.is_empty():
		_rebuild_if_needed()
		if _bodies.is_empty():
			return
	var env := _cached_env
	if env == null or not env.has_method("is_loaded") or not env.is_loaded():
		return

	if not _cached_cam or not _cached_cam.is_inside_tree() or not _cached_cam.current:
		_cached_cam = EnvRenderCamera.find(self)
	var cam_pos := _cached_cam.global_position if _cached_cam else Vector3.ZERO

	var sun_dir: Vector3 = env.get_sun_direction()
	var moon_dir: Vector3 = env.get_moon_direction()

	var star_tint: Vector3 = env.get_sky_ambient()
	if _star_mmi != null and _star_mmi.material_override is ShaderMaterial:
		var star_material := _star_mmi.material_override as ShaderMaterial
		star_material.set_shader_parameter("u_tint", star_tint)
		star_material.set_shader_parameter("u_anchor_camera_world", cam_pos)
		# Keep every instance local to a camera-anchored MMI. Absolute per-star
		# transforms leave the MultiMesh AABB at the world origin and disappear
		# once a mission camera travels far enough from it.
		_star_mmi.global_position = cam_pos
	# The active light (sun by day, moon at night) drives the near-light cull
	# [orig: Environment_GetLightDirectionFixed @ 0x57d8e0 at the field loop].
	var light_dir: Vector3 = env.get_light_direction() if env.has_method("get_light_direction") else sun_dir
	_update_star_field(light_dir)

	for key in _bodies:
		var body: Dictionary = _bodies[key]
		var model: Node3D = body["model"]
		var dir := moon_dir if key == "moon" else sun_dir
		# camera + direction * 64, FULL camera height, identity rotation
		# [orig: render_celestial_bodies @ 0x5acaa0]. Below the horizon the
		# terrain depth-occludes the body, like retail's draw order.
		model.global_position = cam_pos + dir * EnvFile.celestial_body_distance()
		var tint: Vector3 = _tint_for(env, body["tint"])
		_set_body_shader_parameter(body, "u_anchor_camera_world", cam_pos)
		_set_body_shader_parameter(body, "u_tint", tint)
		if key == "sun":
			# Overcast (#16) stays 0 until the overcast systems land; the
			# SunDim channel is live end-to-end (env #27 — spring-smoothed in
			# the weather core; target 0 in stock data).
			var sun_dim: float = env.get_sun_dim_pct() if env.has_method("get_sun_dim_pct") else 0.0
			_set_body_shader_parameter(body, "u_opacity", EnvFile.celestial_sun_alpha(0.0, sun_dim))
		elif key == "moon":
			# The moon fades with the fog distance [orig: @ 0x5acc40].
			_set_body_shader_parameter(body, "u_opacity",
					EnvFile.celestial_moon_alpha(env.get_fog_level(), 0.0))
		elif key == "glare":
			# env #14 (closed): two jittered terrain rays per frame feed the
			# witnessed 8-sample window + dead-band hysteresis; glow alpha =
			# dot_view^4/2 x brightness x folds
			# [orig: render_skybox_sun_glow @ 0x5acd00].
			var ray_length: float = _glare_occlusion.get_ray_length()
			var visible_a := _glare_ray_clear(cam_pos, sun_dir, ray_length, _glare_occlusion.get_ray_jitter_a())
			var visible_b := _glare_ray_clear(cam_pos, sun_dir, ray_length, _glare_occlusion.get_ray_jitter_b())
			_glare_occlusion.tick(visible_a, visible_b, env.get_fog_level())
			var sun_dim_glow: float = env.get_sun_dim_pct() if env.has_method("get_sun_dim_pct") else 0.0
			# Preserve the recovered fixed-point occlusion/brightness/dimming fold
			# at dot=1. The shader applies the remaining positive dot^4 factor from
			# EACH pass camera, so the mirror view no longer inherits the main
			# camera's glare angle (or its CPU visibility rejection).
			var peak_glow := EnvFile.glare_glow_alpha(
					1.0, _glare_occlusion.get_brightness(), 0.0, sun_dim_glow)
			model.visible = peak_glow > 0.0
			_set_body_shader_parameter(body, "u_glare_direction", sun_dir)
			_set_body_shader_parameter(body, "u_opacity", peak_glow)


func _set_body_shader_parameter(body: Dictionary, parameter: StringName, value) -> void:
	var materials: Array = body.get("materials", [])
	for material in materials:
		if material is ShaderMaterial:
			(material as ShaderMaterial).set_shader_parameter(parameter, value)


# env #33: the star field owner — one MultiMesh of camera-facing quads under
# the additive celestial shader, textured with the star 3DI's diffuse. The
# witnessed placement is camera + offset per star with per-star twinkle
# brightness [orig: render_star_field @ 0x5ad9c0]; the near-light cull and
# the twinkle accumulator run in libs/env.
func _build_star_field(star_name: String) -> void:
	if star_name.strip_edges().is_empty() or _resource_root == null:
		return
	var data := _load_object_data(star_name)
	if data == null:
		return
	var diffuse: Texture2D = data.load_material_texture(0, 0)
	var material := _make_celestial_material(true, PRIORITY_STAR)
	if diffuse:
		material.set_shader_parameter("u_diffuse", diffuse)
	material.set_shader_parameter("u_opacity", 1.0)
	material.set_shader_parameter("u_billboard", true)
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	var quad := QuadMesh.new()
	quad.size = Vector2.ONE
	mm.mesh = quad
	mm.instance_count = _star_core.get_count()
	_star_mmi = MultiMeshInstance3D.new()
	_star_mmi.name = "StarField"
	_star_mmi.multimesh = mm
	_star_mmi.material_override = material
	# Instances stay local to this camera-anchored node. The field spans the
	# whole sky around that local origin; cull as one conservative unit and
	# bypass main-view occlusion because the reflection pass reanchors it.
	_star_mmi.custom_aabb = AABB(Vector3(-600, -600, -600), Vector3(1200, 1200, 1200))
	_star_mmi.extra_cull_margin = 1.0e6
	_star_mmi.ignore_occlusion_culling = true
	_star_mmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_star_mmi)
	_star_core.regenerate(1)


func _update_star_field(light_dir: Vector3) -> void:
	if _star_mmi == null:
		return
	var mm := _star_mmi.multimesh
	var buf := _star_core.tick_frame(light_dir)
	if buf.is_empty():
		return
	var count := _star_core.get_count()
	for i in count:
		var o := i * 6
		var visible := buf[o + 5] > 0.5
		if not visible:
			mm.set_instance_transform(i, Transform3D(Basis().scaled(Vector3.ZERO), Vector3.ZERO))
			continue
		var offset := Vector3(buf[o], buf[o + 1], buf[o + 2])
		var scale := buf[o + 3]
		var brightness := buf[o + 4]
		# Orientation is deliberately identity here. The additive vertex shader
		# rebuilds the quad from the active pass camera's right/up basis, which is
		# the only way one MultiMesh can billboard correctly in both viewports.
		var xform := Transform3D(Basis().scaled(Vector3(scale, scale, scale)), offset)
		mm.set_instance_transform(i, xform)
		mm.set_instance_color(i, Color(brightness, brightness, brightness))


func _tint_for(env: Node, tint_key: String) -> Vector3:
	match tint_key:
		"sun":
			return env.get_sun_color()
		"moon":
			return env.get_moon_color()
		_:
			return env.get_sky_ambient()




# Terrain line-of-sight for the glare: the ported boolean raycast form
# [orig: Terrain_RaycastLoResNoNormal @ 0x610860 -> Terrain_RaycastHeightmapLoRes
# @ 0x60cb80] over the jittered segment (camera -> camera + sun_dir * ray_length
# + jitter). Retail's wrapper passes hit = NULL (a pure clear test); our
# NovaTerrainData.raycast_terrain reports the miss as all-NAN, so clear = the
# hit is NAN (the refine only sharpens the hit point — the clear/blocked answer
# is the march's). No terrain loaded = clear (nothing occludes) — the
# editor-guard divergence from retail's null-atlas return-HIT, kept
# deliberately: an unloaded world has nothing to block the sun
# (docs/terrain/terrain-re.md §Runtime terrain queries).
func _glare_ray_clear(from_pos: Vector3, sun_dir: Vector3, ray_length: float, jitter: Vector3) -> bool:
	if terrain_data == null or not terrain_data.is_loaded():
		return true
	var hit: Vector3 = terrain_data.raycast_terrain(from_pos, from_pos + sun_dir * ray_length + jitter)
	return is_nan(hit.x)
