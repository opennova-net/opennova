@tool
class_name NovaWater
extends Node3D

# Water plane preview/runtime adapter.
# Engine equivalents: water color/height/murk consume globals parsed by
# [orig: TimeOfDay_ParseProperty @ 0x57c590] and fog colors interpolated by
# [orig: Environment_ComputeTimeOfDayColors @ 0x57de40] (docs/env/env-tod-re.md).

# Visual-layer allocation for the reflection contract (env #30). The mirror
# re-renders the WORLD scene - which contains the local player's body - but
# never the water surface itself [orig: Water_ReflectionPrerender @ 0x5c2780
# -> render_main_scene @ 0x5c1240 (mirrored sky/terrain/world/celestial/glare;
# no water draw)] and never the first-person arms/weapon, which retail renders
# as its own separate near-Z viewport pass over the finished frame
# [orig: Player_RenderFirstPersonViewModel @ 0x4ded60]. LocalPlayerPresenter stamps
# the two player layers each frame and masks BODY_REFLECTION_ONLY off the
# player camera; the mirror camera below is the only view that includes it.
const VISUAL_LAYER_WORLD := 1 << 0  # Godot's default layer: every normal world instance
const VISUAL_LAYER_WATER := 1 << 10  # the water surface itself; mirror-excluded
const VISUAL_LAYER_VIEWMODEL := 1 << 11  # the FP arms/weapon overlay; mirror-excluded, main-visible
const VISUAL_LAYER_BODY_REFLECTION_ONLY := 1 << 12  # the FP-mode local body; mirror-visible, main-excluded
# Shadow participation is orthogonal to camera visibility. Retail renders live
# entity silhouettes and static terrain-tile silhouettes through separate
# projection lists, so the reimpl lights select these marker layers with
# shadow_caster_mask without re-lighting the hand-lit base materials.
const VISUAL_LAYER_STATIC_SHADOW_CASTER := 1 << 13
const VISUAL_LAYER_DYNAMIC_SHADOW_CASTER := 1 << 14
const VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER := 1 << 15
const VISUAL_LAYER_SHADOW_CASTER_MASK := \
		VISUAL_LAYER_STATIC_SHADOW_CASTER \
		| VISUAL_LAYER_DYNAMIC_SHADOW_CASTER
# Retail allocates a square 256 RTT at water detail 2; only detail >= 3 or the
# capture override selects 512 [orig: sub_5C08B0 @ 0x5c08d1..0x5c0937].
# The reimpl has no higher-detail/capture selector, so its witnessed mapping is 256.
const REFLECTION_RTT_SIZE := Vector2i(256, 256)

@export var environment_path: NodePath
# The weather node owning the cloud-scroll core: the water scroll speed rides
# the same RAMPING rate the sky layers consume (duck-typed, lazy resolve).
@export var weather_path: NodePath
@export var terrain_data: NovaTerrainData:
	set(value):
		terrain_data = value
		# Terrain carries the map's water height. It's assigned after the .trn loads
		# — long after _ready — so recompute here too, or the plane stays at the
		# scene default instead of dropping to the map's level. A present terrain
		# height beats the .env one (witnessed precedence, env #28).
		_recompute_terrain_water_fallback()
		_apply_environment_water_height()
@export_range(-100, 200, 0.1) var water_height: float = 0.0:
	set(value):
		water_height = value
		# The strip vertices carry the plane height themselves (the mesh node
		# stays pinned at the world origin) — no node repositioning here.
		_sync_render_activity()
@export_range(0, 1, 0.01) var water_alpha: float = 0.6

# When set (not NaN), the world drives water height directly and the env/terrain
# fallback is ignored — the terrain editor authors height through its document.
var _height_override: float = NAN
# The mission header's attrib-gated water value is a distinct rung from the
# live EnvFile view. Keeping it here preserves authoring > BMS > TRN > ENV,
# including an explicit BMS zero (which disables water instead of falling
# through to terrain).
var _mission_water_height_override: float = NAN
# GameWorld retains this node across unload/reload; unhosted authoring previews
# do not need to opt in, so a standalone water node starts enabled.
var _world_rendering_enabled := true


func set_height_override(value: float) -> void:
	_height_override = value
	_apply_environment_water_height()


## Set the mission/BMS rung in world units; NAN means absent. Zero is
## meaningful and still beats TRN and ENV.
func set_mission_water_height_override(value: float) -> void:
	_mission_water_height_override = value
	_apply_environment_water_height()


## Disable retained runtime rendering while no world is loaded.
func set_world_rendering_enabled(value: bool) -> void:
	_world_rendering_enabled = value
	_sync_render_activity()


## Env_WaterHeightFixed == 0 is retail's no-water sentinel. Signed nonzero
## heights remain valid for terrain below the world origin.
func is_water_active() -> bool:
	return water_height != 0.0


## The authored height can remain valid while GameWorld retains this node
## between loads. Consumers that decide whether water participates in a render
## frame must use this predicate rather than the height sentinel alone.
func is_water_render_active() -> bool:
	return _world_rendering_enabled and is_water_active()


# Publish this water plane's height as the session's transparent water-split
# (the g_WaterSplitHeightFloat equivalent [orig: @ 0x5c93e2..0x5c93f0]) so
# blended world materials can take their far/camera-side rung; cleared when
# the water node leaves the tree.
func _push_water_split_height() -> void:
	if not built or not is_inside_tree():
		return
	if is_water_render_active() and is_visible_in_tree():
		NovaObjectShaderCache.get_singleton().set_water_split_height(water_height)
	else:
		NovaObjectShaderCache.get_singleton().clear_water_split_height()


func _sync_render_activity() -> void:
	if not built:
		return
	var world_active := (is_water_render_active()
			and is_inside_tree() and is_visible_in_tree())
	if mesh_instance:
		mesh_instance.visible = world_active
	# Do not spend a permanent UPDATE_ALWAYS pass on an absent, hidden, or
	# off-screen surface. The strip march re-arms this after it produces rows.
	var reflection_active := world_active and _has_drawable_surface and _cached_cam != null
	if reflection_viewport:
		reflection_viewport.render_target_update_mode = (
				SubViewport.UPDATE_ALWAYS if reflection_active else SubViewport.UPDATE_DISABLED)
	if water_material:
		water_material.set_shader_parameter("u_has_reflection", reflection_active)
	_push_water_split_height()
	if not world_active:
		_clear_strip_surfaces()


func _notification(what: int) -> void:
	if what == NOTIFICATION_VISIBILITY_CHANGED and built:
		_sync_render_activity()


func _exit_tree() -> void:
	# Only clear a split that could have been pushed (_push_water_split_height
	# requires `built`): an unconditional call here CREATED the shader-cache
	# singleton during scene teardown on every quit — the never-freed extension
	# object behind the packaging boot-smoke teardown AV.
	if built:
		NovaObjectShaderCache.get_singleton().clear_water_split_height()
	# Reflection teardown: stop the offscreen renders and disarm the shader's
	# reflection branch — the u_water_color fallback takes over if the
	# material outlives the node. Re-armed by the next build().
	if reflection_viewport:
		reflection_viewport.render_target_update_mode = SubViewport.UPDATE_DISABLED
	if water_material:
		water_material.set_shader_parameter("u_has_reflection", false)
		water_material.set_shader_parameter("u_reflection", null)

var mesh_instance: MeshInstance3D
var water_material: ShaderMaterial
# The reflection RTT rig (env #30): retail prerenders the mirrored scene
# into Water_ReflectionTexture BEFORE the main frame [orig: Render_TerrainScene
# @ 0x610c80 -> Water_ReflectionPrerender @ 0x5c2780 -> render_main_scene
# @ 0x5c1240]. The reimpl form is a SubViewport on the SAME World3D with a
# mirrored camera; Godot renders SubViewports ahead of the viewport that
# samples them, preserving the witnessed prerender order.
var reflection_viewport: SubViewport = null
var reflection_camera: Camera3D = null
var built: bool = false
var _cached_env: Node = null
var _cached_weather: Node = null
var _cached_cam: Camera3D = null
var _terrain_water_height: float = 0.0
# The witnessed per-frame water core: the noise texture pair
# [orig: render_water_surface @ 0x5c32c0 -> Water_GenerateNoiseTextures
# @ 0x5c0360] plus the screen-marched strip tessellation (env #29)
# [orig: render_water_strip_detailed @ 0x5c27d0]; math in libs/env.
var _water_core := NovaWaterCore.new()
var _noise_color_img: Image = null
var _noise_normal_img: Image = null
var _noise_color_tex: ImageTexture = null
var _noise_normal_tex: ImageTexture = null
var _frame_counter: int = 0
var _has_drawable_surface := false
# Standalone fallback when no weather node is wired (the UV offsets ride the
# weather core's cloud-scroll accumulators).
var _fallback_scroll: NovaWeatherCore = null
var _fallback_tick_credit := 0.0


func _ready() -> void:
	_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null
	_recompute_terrain_water_fallback()
	_apply_environment_water_height()
	build()


# The map's water height (engine half-world units) from the loaded terrain.
# Retail stores the terrain value AFTER the .env parse when its bit-31 "has
# water" flag is set [orig: Terrain_Init @ 0x60fcb1..0x60fcba], so a present
# terrain height BEATS the .env one (env #28). Our .trn text config proxies
# "flagged" as a nonzero value. Zero when there's no terrain or no water.
func _recompute_terrain_water_fallback() -> void:
	_terrain_water_height = 0.0
	if terrain_data and terrain_data.is_loaded():
		var raw := terrain_data.get_water_height()
		if raw != 0:
			_terrain_water_height = float(raw) * 0.5


func build() -> void:
	if mesh_instance:
		mesh_instance.queue_free()
		mesh_instance = null
	built = false
	water_material = ShaderMaterial.new()
	water_material.shader = load("res://shaders/water.gdshader") as Shader
	# The water surface draws between the two water-side transparent brackets
	# [orig: Terrain_RenderWaterPass @ 0x610640 between the SortAndFlush pair
	# @ 0x5c9596 / @ 0x5c967a; ladder in libs/renderer/render_order, REN-3].
	water_material.render_priority = NovaObjectShaderCache.RENDER_RUNG_WATER

	# The witnessed screen-marched strip mesh is LIVE (env #29): _process
	# rebuilds the surface every frame from NovaWaterCore.strip_build
	# [orig: render_water_strip_detailed @ 0x5c27d0], so the mesh starts
	# empty. The reflection RTT consumer is LIVE too (env #30, rig below).
	# Remaining variants: the LOW tier (render_water_strip @ 0x5c1d60,
	# water detail <= 1 — sin-table Y displacement) and the nightvision
	# redraw.
	var mesh := ArrayMesh.new()
	mesh_instance = MeshInstance3D.new()
	mesh_instance.mesh = mesh
	# Strip vertices are ABSOLUTE world positions (the plane height rides the
	# rows, not the node): pin the mesh at the world origin; top_level guards
	# against a transformed parent node.
	mesh_instance.top_level = true
	mesh_instance.position = Vector3.ZERO
	mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mesh_instance.gi_mode = GeometryInstance3D.GI_MODE_DISABLED
	# The water surface rides its OWN visual layer (bit 10) instead of the
	# default bit 0: a fresh Camera3D cull_mask has all 20 layer bits set, so
	# every normal view still renders the water, while the mirror camera
	# below masks this one bit out — the witnessed offscreen prerender draws
	# sky/terrain/world/celestials but never the water surface itself
	# [orig: the render_main_scene @ 0x5c1240 pass list has no water draw].
	mesh_instance.layers = VISUAL_LAYER_WATER
	add_child(mesh_instance)
	built = true

	# The witnessed per-frame noise texture pair (created once, updated per
	# frame) [orig: Water_GenerateNoiseTextures @ 0x5c0360].
	var size := _water_core.get_texture_size()
	_water_core.update(0)
	_noise_color_img = Image.create_from_data(size, size, false, Image.FORMAT_RGBA8, _water_core.get_color_rgba8())
	_noise_normal_img = Image.create_from_data(size, size, false, Image.FORMAT_RGBA8, _water_core.get_normal_rgba8())
	_noise_color_tex = ImageTexture.create_from_image(_noise_color_img)
	_noise_normal_tex = ImageTexture.create_from_image(_noise_normal_img)
	water_material.set_shader_parameter("u_noise_color", _noise_color_tex)
	water_material.set_shader_parameter("u_noise_normal", _noise_normal_tex)

	# The reflection RTT (env #30): the mirrored scene renders offscreen and
	# the strip shader samples it as t2. Retail clears the RTT to the SKYFOG
	# color at depth 1 - 2^-15 and re-renders sky dome, lo-res terrain, the
	# reflected world, celestial bodies + sun glare [orig:
	# Water_ReflectionPrerender @ 0x5c2780 -> render_main_scene @ 0x5c1240];
	# world-driven, the shared World3D's sky/environment covers the skyfog clear
	# and the mirror camera sees the same live scene.
	if reflection_viewport == null:
		reflection_viewport = SubViewport.new()
		reflection_viewport.name = "WaterReflectionViewport"
		# The mirror renders the LIVE world, not a copy.
		reflection_viewport.own_world_3d = false
		reflection_viewport.handle_input_locally = false
		# Retail renders through this square RTT, independent of display size.
		reflection_viewport.size = REFLECTION_RTT_SIZE
		add_child(reflection_viewport)
		reflection_camera = Camera3D.new()
		reflection_camera.name = "WaterReflectionCamera"
		# Hide the water-only visual layer (set on mesh_instance above) from
		# the mirror pass: the witnessed offscreen scene never draws the water
		# surface [orig: render_main_scene @ 0x5c1240]. The first-person
		# viewmodel layer is masked out with it - retail's FP arms/weapon are
		# a separate near-Z overlay pass that never enters the mirrored scene
		# [orig: Player_RenderFirstPersonViewModel @ 0x4ded60]. Everything
		# else stays in, INCLUDING the reflection-only body layer: the
		# witnessed mirrored scene is a re-render of the world, local player's
		# body and all [orig: Water_ReflectionPrerender @ 0x5c2780 ->
		# render_main_scene @ 0x5c1240].
		reflection_camera.cull_mask = 0xFFFFF & ~(
				VISUAL_LAYER_WATER
				| VISUAL_LAYER_VIEWMODEL
				| VISUAL_LAYER_SHADOW_CASTER_MASK)
		reflection_viewport.add_child(reflection_camera)
		reflection_camera.current = true
	reflection_viewport.render_target_update_mode = SubViewport.UPDATE_DISABLED
	water_material.set_shader_parameter("u_reflection", reflection_viewport.get_texture())
	water_material.set_shader_parameter("u_has_reflection", false)
	_sync_render_activity()


func _process(delta: float) -> void:
	if not built or water_material == null:
		return
	if not _cached_env or not _cached_env.is_inside_tree():
		_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null
	# Live environment edits can change the fallback height. A standalone
	# water node with no authoritative env/terrain keeps its direct property.
	if _cached_env and _cached_env.has_method("is_loaded") and _cached_env.is_loaded():
		_apply_environment_water_height()
	# The murk uniform feed stays for world/probe compatibility even though the
	# shader's murk role moved to the per-vertex COLOR.a (env #29).
	water_material.set_shader_parameter("u_water_murk", water_alpha)

	if (not _cached_cam or not _cached_cam.is_inside_tree()
			or not _cached_cam.current):
		_cached_cam = EnvRenderCamera.find(self)
	if (not _world_rendering_enabled or not is_water_active()
			or not is_visible_in_tree() or _cached_cam == null):
		_clear_strip_surfaces()
		_sync_render_activity()
		return
	var cam_pos := Vector3.ZERO
	if _cached_cam:
		# Camera3D's public render transform includes h_offset/v_offset;
		# global_position does not. Classify and march from the same effective
		# eye the main viewport actually renders.
		cam_pos = _cached_cam.get_camera_transform().origin

	# env #30: refresh the mirror camera before this frame's strip rebuild —
	# the SubViewport renders ahead of the main view, like the witnessed
	# prerender [orig: Water_ReflectionPrerender @ 0x5c2780 runs BEFORE the
	# main frame in Render_TerrainScene @ 0x610c80].
	_update_reflection_camera()

	# Regenerate the animated noise pair once per rendered water frame; unlike
	# the fixed-62 Hz weather clock, this is explicitly render-driven
	# [orig: render_water_surface @ 0x5c3326 regenerates per frame].
	_frame_counter += 1
	_water_core.update(_frame_counter)
	_noise_color_img.set_data(_water_core.get_texture_size(), _water_core.get_texture_size(), false, Image.FORMAT_RGBA8, _water_core.get_color_rgba8())
	_noise_normal_img.set_data(_water_core.get_texture_size(), _water_core.get_texture_size(), false, Image.FORMAT_RGBA8, _water_core.get_normal_rgba8())
	_noise_color_tex.update(_noise_color_img)
	_noise_normal_tex.update(_noise_normal_img)

	# Strip inputs default to the shader-uniform stand-in values so owners
	# without a loaded env still march strips.
	var murk := water_alpha
	var fog_end := 1000.0
	var uv_state := Vector4(1.0, 0.2, 0.0, 0.0)
	var lit := Color(0.408, 0.314, 0.224)
	var env_data: EnvFile = null

	var env := _cached_env
	if env and env.has_method("is_loaded") and env.is_loaded():
		# Water renders lit: water_rgb x (light*0.707 + sky) x 2, saturating
		# [orig: Environment_UpdateWeatherTick @ 0x57f16b].
		var water: Vector3 = env.get_water_color()
		var light: Vector3 = env.get_sun_light()
		var sky: Vector3 = env.get_sky_ambient()
		var combined := EnvFile.combine_terrain_light(
				Color(light.x, light.y, light.z), Color(sky.x, sky.y, sky.z))
		lit = EnvFile.lit_water_color(Color(water.x, water.y, water.z), combined)
		water_material.set_shader_parameter("u_water_color", Vector3(lit.r, lit.g, lit.b))
		fog_end = env.get_fog_level()
		# The witnessed UV transform (scale/bias from the fog-distance INT
		# part, offsets from the layer-1 cloud accumulators + 32x camera)
		# [orig: render_water_surface @ 0x5c3348..0x5c33db]. The weather node
		# owns the shared accumulators; standalone owners tick a private core.
		if not _cached_weather or not _cached_weather.is_inside_tree():
			_cached_weather = get_node_or_null(weather_path) if not weather_path.is_empty() else null
		if _cached_weather and _cached_weather.has_method("get_water_uv_state"):
			uv_state = _cached_weather.get_water_uv_state(cam_pos.x, cam_pos.z, fog_end)
		else:
			if _fallback_scroll == null:
				_fallback_scroll = NovaWeatherCore.new()
			_fallback_tick_credit += maxf(delta, 0.0) * NovaWeather.WEATHER_TICK_HZ
			var tick_count := int(floor(_fallback_tick_credit + 1.0e-9))
			if tick_count > 0:
				_fallback_tick_credit = maxf(
						0.0, _fallback_tick_credit - float(tick_count))
				if tick_count > NovaWeather.MAX_CATCHUP_TICKS:
					tick_count = NovaWeather.MAX_CATCHUP_TICKS
					_fallback_tick_credit = 0.0
				for _tick in range(tick_count):
					_fallback_scroll.tick_cloud_scroll(env.get_sky_speed())
			uv_state = _fallback_scroll.get_water_uv_state(cam_pos.x, cam_pos.z, fog_end)
		water_material.set_shader_parameter("u_water_uv", uv_state)
		water_material.set_shader_parameter("u_fog_color", env.get_fog_color())
		env_data = env.get_environment_data()
		if env_data:
			murk = env_data.get_water_murk()
			water_material.set_shader_parameter("u_water_murk", murk)

	_rebuild_strip_mesh(cam_pos, murk, fog_end, uv_state, lit, env_data)
	_sync_render_activity()


# Mirrors the live camera about the water plane y = water_height into the
# reflection SubViewport [orig: Water_ReflectionPrerender @ 0x5c2780 packs the
# live camera block {x, y, z, yaw, pitch, roll}; the mirrored view builds
# inside render_main_scene @ 0x5c1240's view-matrix section]. Hex-Rays elides
# retail's exact mirror transform, but the witnessed texm3x2 rows PIN its
# form: they sample the RTT at u = screen U (no horizontal flip) and
# v ~ 1 - screen V (libs/env WaterStripRows), which only holds when the
# offscreen camera is the UP-PRESERVED proper mirror — reflect the basis
# about the plane, then negate the reflected up column. The raw reflection
# alone is IMPROPER (det -1: every triangle's winding flips, so faces cull
# backwards); negating the up column restores det +1 (the conjugated rotation
# = yaw kept, pitch/roll negated) and renders the vertical mirror the rows'
# vbase - screenV coordinate expects, so the fragment lookup stays the
# witnessed row math before a reimpl projection-scale correction. (Negating any
# other column would instead need matching flips in the shader.)
func _update_reflection_camera() -> void:
	if reflection_viewport == null or reflection_camera == null:
		return
	if _cached_cam == null or not _cached_cam.is_inside_tree():
		# No live view: the strip build clears too — the stale mirror image
		# is never sampled.
		return
	var viewport := _cached_cam.get_viewport()
	if viewport == null:
		return
	var source_size := viewport.get_visible_rect().size
	# A one-pixel viewport is a real transient state while ONED swaps or lays
	# out workspaces. The strip builder below already treats either dimension
	# <= 1 as non-drawable; stop the mirror projection here too, before an
	# extreme aspect asks Camera3D for an out-of-range FOV.
	if source_size.x <= 1.0 or source_size.y <= 1.0:
		return
	var source_aspect := source_size.x / source_size.y

	# position' = (x, 2*wh - y, z); each basis column reflects about the
	# plane normal n = (0, 1, 0) as c' = c - 2*n*dot(c, n) (flip the Y
	# component), then the reflected up column negates — see above.
	var xform := _cached_cam.global_transform
	var bx := xform.basis.x
	var by := xform.basis.y
	var bz := xform.basis.z
	var mirrored := Basis(
			Vector3(bx.x, -bx.y, bx.z),
			Vector3(-by.x, by.y, -by.z),
			Vector3(bz.x, -bz.y, bz.z))
	var origin := xform.origin
	origin.y = 2.0 * water_height - origin.y
	reflection_camera.global_transform = Transform3D(mirrored, origin)
	# Retail rebuilds projection for the square RTT while preserving the
	# source's horizontal field [orig: Viewport_BuildProjectionMatrix @
	# 0x410fb0; bounds @ 0x5c1476]. Camera3D's authored `fov`/`size` axis is
	# selected by keep_aspect, so normalize every source projection to its
	# horizontal extent before installing it on the square pass.
	reflection_camera.keep_aspect = Camera3D.KEEP_WIDTH
	match _cached_cam.projection:
		Camera3D.PROJECTION_ORTHOGONAL:
			var horizontal_size := _cached_cam.size
			if _cached_cam.keep_aspect == Camera3D.KEEP_HEIGHT:
				horizontal_size *= source_aspect
			reflection_camera.set_orthogonal(
					horizontal_size, _cached_cam.near, _cached_cam.far)
		Camera3D.PROJECTION_FRUSTUM:
			# Godot's frustum size is always the vertical span; unlike the
			# orthogonal/perspective builders it does not reinterpret the axis
			# through keep_aspect. The square pass therefore always receives the
			# source vertical span multiplied by its aspect.
			var horizontal_size := _cached_cam.size * source_aspect
			var mirrored_offset := Vector2(
					_cached_cam.frustum_offset.x,
					-_cached_cam.frustum_offset.y)
			reflection_camera.set_frustum(horizontal_size,
					mirrored_offset, _cached_cam.near, _cached_cam.far)
		_:
			var source_horizontal_fov := _cached_cam.fov
			if _cached_cam.keep_aspect == Camera3D.KEEP_HEIGHT:
				source_horizontal_fov = rad_to_deg(2.0 * atan(
						tan(deg_to_rad(_cached_cam.fov) * 0.5) * source_aspect))
			# Camera3D accepts [1, 179] degrees. Very thin but still
			# drawable Godot viewports asymptotically approach 180 degrees.
			reflection_camera.set_perspective(
					clampf(source_horizontal_fov, 1.0, 179.0),
					_cached_cam.near, _cached_cam.far)
	# These offsets are independent of the projection mode and are otherwise
	# lost when the reflection camera is rebuilt from the source transform.
	reflection_camera.h_offset = _cached_cam.h_offset
	# The proper mirror camera deliberately negates the reflected UP column.
	# Negate its local vertical offset too so the effective camera origin is
	# the geometric reflection of the source rather than shifted oppositely.
	reflection_camera.v_offset = -_cached_cam.v_offset
	# The strip rows encode normalized coordinates from the source viewport.
	# Preserving horizontal FOV makes the square mirror's X focal scale match,
	# but its Y focal scale is source_height/source_width of the main camera's.
	# Convert the complete witnessed texm3x2 result at the sim/present boundary so a
	# fixed reflected world point remains registered while the view rotates.
	water_material.set_shader_parameter("u_reflection_uv_scale",
			Vector2(1.0, 1.0 / source_aspect))
	# NEAR-PLANE NOTE (TRACKED approximation, env #30 ledger): retail clips
	# the mirrored scene against the water surface — the PolyTrn context arms
	# a below-plane clip at waterHeight - 0.1 [orig: plane block wh - 0.1,
	# render_main_scene @ 0x5c1240]. Godot exposes no oblique clip plane, so
	# the reimpl does NOT clip: the mirrored camera predominantly sees
	# above-water geometry anyway.


# Rebuilds the surface from the witnessed screen march (env #29)
# [orig: render_water_strip_detailed @ 0x5c27d0; per-side callers
# render_water_surface @ 0x5c3492 (camera above) / @ 0x5c3542 (underwater)].
# Vertices are absolute world positions; COLOR carries the row diffuse,
# CUSTOM1 the row specular (the ps.1.1 v1 register [orig: add r0.rgb, r0, v1
# — Water_InitSurfaceShaders @ 0x5c19b0]), CUSTOM0 = (depth, rhw, screen U,
# screen V), CUSTOM2 the texm3x2 perturbation basis (t1.xy, t2.xy — with
# CUSTOM0.zw it reassembles the witnessed t1/t2 rows the env #30 reflection
# lookup dots against the DuDv sample), TEX_UV the witnessed world/32 pair
# (carried for parity/debug — the shader keeps its camera-relative UV model).
func _rebuild_strip_mesh(cam_pos: Vector3, murk: float, fog_end: float,
		uv_state: Vector4, lit: Color, env_data: EnvFile) -> void:
	if mesh_instance == null or not (mesh_instance.mesh is ArrayMesh):
		return
	if _cached_cam == null or not _cached_cam.is_inside_tree():
		_clear_strip_surfaces()
		return
	var viewport := _cached_cam.get_viewport()
	if viewport == null:
		_clear_strip_surfaces()
		return
	var vp_size := Vector2i(viewport.get_visible_rect().size)
	if vp_size.x <= 1 or vp_size.y <= 1:
		_clear_strip_surfaces()
		return
	# The camera-side gate and the pass fog end both ride the underwater flag
	# [orig: side gate against Env_WaterHeightFixed in render_water_surface;
	#  Environment_GetFogEndDistance(underwater) @ 0x5c28a2 — below the
	#  surface the murk visibility curve replaces the weather fog distance].
	var underwater := cam_pos.y < water_height
	# Above water the pass fog end is the smoothed fog distance attenuated by the
	# overcast blend: fogDist * (1 - overcast/2) [orig: Environment_GetFogEndDistance
	# @ 0x57e435 — (0x10000 - (Env_OvercastBlend >> 1)) * Env_FogDistCurrent >> 16].
	# The overcast channel is state-live but unconsumed (env #27 residual) — 0 until
	# the overcast systems land, like every other overcast feed.
	var overcast := 0.0
	var pass_fog_end := fog_end * (1.0 - overcast * 0.5)
	if underwater and env_data:
		pass_fog_end = env_data.get_fog_end_underwater()
	# The adjusted camera transform includes Camera3D h/v offsets, keeping the
	# screen-marched row coordinates registered to the actual main view.
	_water_core.strip_set_view(_cached_cam.get_camera_transform(),
			_cached_cam.get_camera_projection(), vp_size, pass_fog_end)
	# The nightvision redraw variant is a FrameFX pass, not ported yet.
	var rows: int = _water_core.strip_build(water_height, murk, lit,
			uv_state.x, uv_state.y, underwater, false)
	if rows < 2:
		# Plane off-screen or a sub-2-row march — nothing submits
		# [orig: windows under 2 rows draw nothing @ 0x5c3195].
		_clear_strip_surfaces()
		return
	_has_drawable_surface = true
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = _water_core.strip_positions()
	arrays[Mesh.ARRAY_COLOR] = _water_core.strip_colors()
	arrays[Mesh.ARRAY_TEX_UV] = _water_core.strip_uv0()
	arrays[Mesh.ARRAY_CUSTOM0] = _water_core.strip_custom0()
	arrays[Mesh.ARRAY_CUSTOM1] = _water_core.strip_custom1()
	arrays[Mesh.ARRAY_CUSTOM2] = _water_core.strip_custom2()
	arrays[Mesh.ARRAY_INDEX] = _water_core.strip_indices()
	var mesh := mesh_instance.mesh as ArrayMesh
	mesh.clear_surfaces()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {},
			(Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT)
			| (Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM1_SHIFT)
			| (Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM2_SHIFT))
	mesh.surface_set_material(0, water_material)
	# The witnessed per-side material swap: camera-above -> the blend material,
	# underwater -> the opaque one (blend off, flags 0x20000) — ported as the
	# shader's u_underwater_view branch [orig: selection @ 0x5c33e6..0x5c34ea;
	# Water_ShaderOpaque @ 0x28ee8c8].
	water_material.set_shader_parameter("u_underwater_view", underwater)


func _clear_strip_surfaces() -> void:
	_has_drawable_surface = false
	if mesh_instance == null:
		return
	var mesh := mesh_instance.mesh as ArrayMesh
	if mesh and mesh.get_surface_count() > 0:
		mesh.clear_surfaces()


func _apply_environment_water_height() -> void:
	# The witnessed precedence is BMS > TRN(flagged) > ENV [orig: env parse
	# @ 0x52073b, then Terrain_Init @ 0x60fcba overrides when flagged, then
	# the BMS override @ 0x525371] — the reimpl override rung is the authoring
	# seam on top (env #28).
	if not is_nan(_height_override):
		water_height = _height_override
		return
	if not is_nan(_mission_water_height_override):
		water_height = _mission_water_height_override
		return
	if _terrain_water_height != 0.0:
		water_height = _terrain_water_height
		return
	var env := _cached_env
	if env and env.has_method("has_water_height") and env.has_water_height():
		# .env water_height is stored <<15 by the engine — half world units,
		# same convention as the terrain value above
		# [orig: TimeOfDay_ParseProperty @ 0x57cb4e].
		water_height = float(env.get_water_height()) * 0.5
		return
	# A loaded terrain or environment is authoritative even when its encoded
	# height is zero. Clear stale editor/previous-map state instead of drawing
	# phantom water; standalone nodes with no source retain direct properties.
	var has_loaded_terrain: bool = (
			terrain_data != null and bool(terrain_data.is_loaded()))
	var has_loaded_env: bool = bool(
			env != null and env.has_method("is_loaded") and env.is_loaded())
	if has_loaded_terrain or has_loaded_env:
		water_height = 0.0
