extends GutTest

# The methods of tests/slot_shadow_test.gd that need a RenderingDevice, split out of it so
# the windowed run selects them (G-100).

const CRATE_3DI := "res://../fixtures/threedi/synth/crate.3di"


func _environment() -> MissionEnvironment:
	var data := EnvFile.new()
	data.reset_to_default()
	data.set_curtime(1200)
	var environment := MissionEnvironment.new()
	environment.name = "SlotEnv"
	environment.environment_data = data
	add_child_autofree(environment)
	return environment


func _scope_with_world_environment() -> Node3D:
	var scope := Node3D.new()
	add_child_autofree(scope)
	var world_environment := WorldEnvironment.new()
	world_environment.name = "ClearColor"
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color.BLACK
	world_environment.environment = environment
	scope.add_child(world_environment)
	return scope


func test_fresh_capture_target_reads_the_retail_white_before_any_draw() -> void:
	# A target no capture has landed on yet samples as the retail cleared RT
	# (0x00FFFFFF, renderer::kSlotCaptureClearArgb): here the scope has no
	# WorldEnvironment, so the effect never installs and nothing draws, yet the
	# armed order's target exists and the drape would sample it.
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var scope := Node3D.new()
	add_child_autofree(scope)
	var environment := _environment()
	var camera := Camera3D.new()
	camera.current = true
	scope.add_child(camera)
	camera.look_at_from_position(Vector3(0, 2, 10), Vector3.ZERO, Vector3.UP)
	var shadow := SlotShadow.new()
	scope.add_child(shadow)
	shadow.set_environment_node(environment)
	shadow.set_shadow_detail(3)
	var crate := _crate_caster(scope, Vector3.ZERO)
	crate.advance_runtime_frame(1.0 / 62.0)
	shadow.advance_frame()
	assert_false(shadow.is_capture_effect_installed(),
			"no WorldEnvironment in scope: the capture effect has nowhere to install")
	var order := shadow.get_capture_order_of(crate)
	assert_true(order >= 0, "the crate still takes a slot and arms a target")
	assert_eq(shadow.get_capture_target_size(order), 512)
	for i in range(2):
		await get_tree().process_frame
	RenderingServer.force_sync()
	var image := shadow.get_capture_image(order)
	assert_not_null(image, "the never-drawn target reads back")
	if image != null:
		var stats := _corner_and_center(image)
		assert_gt(float(stats["corner_min"]), 0.99, "the corners are the white clear")
		assert_gt(float(stats["center_max"]), 0.99,
				"the center is the white clear too: no capture landed, no shadow")
	crate.set_shadow_caster_enabled(false)
	shadow.advance_frame()


func _crate_caster(scope: Node3D, at: Vector3) -> ObjectModel:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(CRATE_3DI)), OK)
	var model := ObjectModel.new()
	scope.add_child(model)
	model.set_process(false)
	model.set_object_data(data)
	model.position = at
	model.set_shadow_caster_enabled(true)
	return model


func _corner_and_center(image: Image) -> Dictionary:
	var w := image.get_width()
	var h := image.get_height()
	var corners := [image.get_pixel(1, 1), image.get_pixel(w - 2, 1),
			image.get_pixel(1, h - 2), image.get_pixel(w - 2, h - 2)]
	var brightest_corner := 0.0
	for corner in corners:
		brightest_corner = maxf(brightest_corner,
				minf(corner.r, minf(corner.g, corner.b)))
	var darkest := 1.0
	for y in range(h / 2 - 2, h / 2 + 3):
		for x in range(w / 2 - 2, w / 2 + 3):
			var p := image.get_pixel(x, y)
			darkest = minf(darkest, maxf(p.r, maxf(p.g, p.b)))
	return {"corner_min": brightest_corner, "center_max": darkest}


const AB_LUM_3DI := "res://../fixtures/threedi/synth/mount_mtrl2_ab_lum.3di"


## Hides every drawn surface of the model whose wrapper path lacks the
## fragment, so the capture walks only the surfaces under test.
func _keep_only_shader_surfaces(root: Node, shader_fragment: String) -> void:
	if root is MeshInstance3D:
		var mesh_instance := root as MeshInstance3D
		var material := mesh_instance.get_active_material(0) as ShaderMaterial
		var shader_path := material.shader.resource_path \
				if material != null and material.shader != null else ""
		mesh_instance.visible = shader_fragment in shader_path
	for child in root.get_children():
		_keep_only_shader_surfaces(child, shader_fragment)


func _darkest_max_channel(image: Image) -> float:
	var darkest := 1.0
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var p := image.get_pixel(x, y)
			darkest = minf(darkest, maxf(p.r, maxf(p.g, p.b)))
	return darkest


func test_windowed_alpha_blend_ffp_caster_blends_a_partial_silhouette() -> void:
	# The _FFP alpha-blend PROJSHAD variant blends black by Diffuse1.a x
	# AlphaGenValue (SRCALPHA/INVSRCALPHA over the white clear) instead of
	# replacing at alpha 1, so a translucent FF_ST_AB strip casts a partial
	# (gray) silhouette: the mount fixture's FF_ST_AB_LUM heat slab alone, its
	# AlphaGen at 0.5 over the no-texture white default, reads mid-gray.
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var scope := _scope_with_world_environment()
	var environment := _environment()
	var camera := Camera3D.new()
	camera.current = true
	scope.add_child(camera)
	camera.look_at_from_position(Vector3(0, 2, 10), Vector3.ZERO, Vector3.UP)
	var shadow := SlotShadow.new()
	scope.add_child(shadow)
	shadow.set_environment_node(environment)
	shadow.set_shadow_detail(3)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(AB_LUM_3DI)), OK)
	var model := ObjectModel.new()
	scope.add_child(model)
	model.set_process(false)
	model.set_object_data(data)
	model.set_shadow_caster_enabled(true)
	_keep_only_shader_surfaces(model, "/self_lit/")
	model.advance_runtime_frame(1.0 / 62.0)
	var slab_materials := 0
	for row in model.get_surface_materials():
		var material := row as ShaderMaterial
		if material != null and material.shader != null \
				and "/self_lit/" in material.shader.resource_path:
			material.set_shader_parameter("u_alpha_mod", 0.5)
			slab_materials += 1
	assert_gt(slab_materials, 0, "the fixture carries the FF_ST_AB_LUM heat slab")
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(model)
	assert_true(order >= 0, "the slab caster takes a slot")
	var report: Dictionary = shadow.get_report()
	assert_gt(int(report["slot_blended_commands"]), 0,
			"the alpha-blend FFP strip compiles onto the blended pipeline")
	assert_eq(int(report["slot_surfaces_compiled"]), int(report["slot_blended_commands"]),
			"only the slab draws: every hidden opaque surface stays out of the walk")
	for i in range(3):
		await get_tree().process_frame
	RenderingServer.force_sync()
	report = shadow.get_report()
	assert_eq(String(report["slot_status"]), "drawn", String(report["slot_failure"]))
	var image := shadow.get_capture_image(order)
	assert_not_null(image, "the resolve target reads back")
	if image == null:
		return
	var stats := _corner_and_center(image)
	assert_gt(float(stats["corner_min"]), 0.99, "the corners keep the white clear")
	var darkest := _darkest_max_channel(image)
	assert_gt(darkest, 0.3, "no fragment of the translucent slab replaces to black")
	assert_lt(darkest, 0.7, "the slab's silhouette is blended by its 0.5 coverage")
	model.set_shadow_caster_enabled(false)
	shadow.advance_frame()


const ANIM_FIXTURES := "res://../fixtures/anim"


func _black_centroid(image: Image) -> Dictionary:
	var sum := Vector2.ZERO
	var count := 0
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var p := image.get_pixel(x, y)
			if maxf(p.r, maxf(p.g, p.b)) < 0.5:
				sum += Vector2(x, y)
				count += 1
	return {"count": count, "centroid": sum / maxf(float(count), 1.0)}


func _capture_after_frames(shadow: SlotShadow, order: int, frames: int) -> Image:
	for i in range(frames):
		await get_tree().process_frame
	RenderingServer.force_sync()
	return shadow.get_capture_image(order)


func test_windowed_skinned_caster_silhouette_follows_the_posed_bone() -> void:
	# The capture skins on the GPU from the frame's bone palette (skeleton
	# global pose x skin bind pose over the packed bind-space positions): a
	# rigid crate fake-skinned onto the soldier skeleton (every vertex on bone
	# 0) casts at its bind position first, and after bone 0 is translated the
	# silhouette moves with it - black at the posed position, the bind
	# position reading the white clear again.
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var scope := _scope_with_world_environment()
	var environment := _environment()
	var camera := Camera3D.new()
	camera.current = true
	scope.add_child(camera)
	camera.look_at_from_position(Vector3(0, 2, 10), Vector3.ZERO, Vector3.UP)
	var shadow := SlotShadow.new()
	scope.add_child(shadow)
	shadow.set_environment_node(environment)
	shadow.set_shadow_detail(4)  # mask 0: every slot re-captures every frame
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(ANIM_FIXTURES)), OK)
	var skeletal := SkeletalAnim.new()
	assert_true(skeletal.load_from_resource_root(root, "soldier.adm"),
			"soldier.adm loads: %s" % skeletal.get_last_error())
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(CRATE_3DI)), OK)
	var model := ObjectModel.new()
	scope.add_child(model)
	model.set_process(false)
	model.set_skeletal_anim(skeletal)
	model.set_object_data(data)
	assert_true(model.has_skeleton(), "the rigid crate fake-skins onto the soldier skeleton")
	model.set_shadow_caster_enabled(true)
	# A wide capture so the posed crate stays inside the extent: half extent
	# = min(1.25 x 4, 4 + 0.75) = 4.75 u over the 1024 px detail-4 base.
	model.set_shadow_bound_radii(4.0, 4.0)
	model.advance_runtime_frame(1.0 / 62.0)
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(model)
	assert_true(order >= 0, "the skinned crate takes a slot")
	var report: Dictionary = shadow.get_report()
	assert_gt(int(report["slot_skinned_commands"]), 0,
			"the crate's surface compiles as a GPU-skinned command")
	var bind_image: Image = await _capture_after_frames(shadow, order, 3)
	assert_not_null(bind_image)
	if bind_image == null:
		return
	var size := bind_image.get_width()
	var bind := _black_centroid(bind_image)
	assert_gt(int(bind["count"]), 100, "the bind-pose crate draws a silhouette")

	var skeleton: Skeleton3D = model.get_skeleton()
	assert_not_null(skeleton)
	if skeleton == null:
		return
	var rest_origin: Vector3 = skeleton.get_bone_rest(0).origin
	# A 2 u world-x translation of bone 0: the capture looks along the slot
	# direction, so its image shift is the offset's component across that
	# direction, 2 u x sqrt(1 - fwd.x^2) over the 9.5 u extent (fwd is the
	# clamped noon sun: fwd.x^2 stays well below 0.7).
	skeleton.set_bone_pose_position(0, rest_origin + Vector3(2.0, 0.0, 0.0))
	shadow.advance_frame()
	var posed_image: Image = await _capture_after_frames(shadow, order, 3)
	assert_not_null(posed_image)
	if posed_image == null:
		return
	var posed := _black_centroid(posed_image)
	assert_gt(int(posed["count"]), 100, "the posed crate still draws a silhouette")
	var shift: float = (Vector2(posed["centroid"]) - Vector2(bind["centroid"])).length()
	var full_shift := 2.0 / 9.5 * float(size)
	assert_gt(shift, full_shift * 0.55,
			"the silhouette follows the posed bone (shift %s px of %s)" % [shift, full_shift])
	assert_lt(shift, full_shift * 1.05,
			"the silhouette moves no further than the bone offset projects")
	var bind_center := Vector2i(Vector2(bind["centroid"]))
	var at_bind := posed_image.get_pixel(bind_center.x, bind_center.y)
	assert_gt(minf(at_bind.r, minf(at_bind.g, at_bind.b)), 0.99,
			"the bind position reads the white clear once the bone has moved away")
	var posed_center := Vector2i(Vector2(posed["centroid"]))
	var at_posed := posed_image.get_pixel(posed_center.x, posed_center.y)
	assert_lt(maxf(at_posed.r, maxf(at_posed.g, at_posed.b)), 0.05,
			"the posed position is black")
	model.set_shadow_caster_enabled(false)
	shadow.advance_frame()


func test_windowed_capture_draws_the_caster_black_over_the_white_clear() -> void:
	# The retail slot RT: cleared 0x00FFFFFF, the PROJSHAD pass draws the
	# caster black (Render_ShadowPass @0x5d7b70; vscPostBlackT1). Needs a live
	# RenderingDevice (a windowed forward_plus run); headless stays pending.
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var scope := _scope_with_world_environment()
	var environment := _environment()
	var camera := Camera3D.new()
	camera.current = true
	scope.add_child(camera)
	camera.look_at_from_position(Vector3(0, 2, 10), Vector3.ZERO, Vector3.UP)
	var shadow := SlotShadow.new()
	scope.add_child(shadow)
	shadow.set_environment_node(environment)
	shadow.set_shadow_detail(3)
	var crate := _crate_caster(scope, Vector3.ZERO)
	crate.advance_runtime_frame(1.0 / 62.0)
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(crate)
	assert_true(order >= 0, "the crate takes a slot")
	assert_ne(shadow.get_armed_capture_mask() & (1 << order), 0, "its fresh order is armed")
	assert_eq(shadow.get_capture_target_size(order), 512)
	var report: Dictionary = shadow.get_report()
	assert_gt(int(report["slot_surfaces_compiled"]), 0,
			"the crate's FF_ST_OP strip compiles a black-pass command")
	assert_eq(int(report["slot_unclassified_surfaces"]), 0)
	# Let the beauty view render (the PRE_OPAQUE effect draws the capture).
	for i in range(3):
		await get_tree().process_frame
	RenderingServer.force_sync()
	report = shadow.get_report()
	assert_eq(String(report["slot_status"]), "drawn", String(report["slot_failure"]))
	assert_gte(int(report["slot_captures_drawn"]), 1, "the armed capture drew")
	assert_gt(int(report["slot_draw_calls"]), 0)
	var image := shadow.get_capture_image(order)
	assert_not_null(image, "the resolve target reads back")
	if image == null:
		return
	assert_eq(image.get_width(), 512)
	var stats := _corner_and_center(image)
	assert_gt(float(stats["corner_min"]), 0.99,
			"the corners keep the white clear (no caster there)")
	assert_lt(float(stats["center_max"]), 0.05,
			"the caster's silhouette is black at the capture center")
	crate.set_shadow_caster_enabled(false)
	shadow.advance_frame()



## A 1024x1024 white texture whose level 0 is opaque and every smaller level
## fully transparent.
func _opaque_level0_texture() -> ImageTexture:
	var data := PackedByteArray()
	var side := 1024
	var colour := Color(1, 1, 1, 1)
	while side >= 1:
		var level := Image.create(side, side, false, Image.FORMAT_RGBA8)
		level.fill(colour)
		data.append_array(level.get_data())
		colour = Color(1, 1, 1, 0)
		side /= 2
	return ImageTexture.create_from_image(
			Image.create_from_data(1024, 1024, true, Image.FORMAT_RGBA8, data))


func test_windowed_capture_stops_at_the_stage_textures_last_retail_mip_level() -> void:
	# TBoringFFPProjShad samples Diffuse1 through sampLinearWrap2D under the
	# effects' filter mode, clamped at the stage's last retail mip level
	# (u_diffuse_max_lod; GTexture_CreateFromPixelData_0, retail). The capture
	# minifies the 1024 texture past level 0, whose alpha alone is opaque:
	# unbounded the slab casts nothing, with a ceiling of 0 it casts black.
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var scope := _scope_with_world_environment()
	var environment := _environment()
	var camera := Camera3D.new()
	camera.current = true
	scope.add_child(camera)
	camera.look_at_from_position(Vector3(0, 2, 10), Vector3.ZERO, Vector3.UP)
	var shadow := SlotShadow.new()
	scope.add_child(shadow)
	shadow.set_environment_node(environment)
	shadow.set_shadow_detail(3)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(AB_LUM_3DI)), OK)
	var model := ObjectModel.new()
	scope.add_child(model)
	model.set_process(false)
	model.set_object_data(data)
	model.set_shadow_caster_enabled(true)
	_keep_only_shader_surfaces(model, "/self_lit/")
	model.advance_runtime_frame(1.0 / 62.0)
	var slab: Array[ShaderMaterial] = []
	var texture := _opaque_level0_texture()
	for row in model.get_surface_materials():
		var material := row as ShaderMaterial
		if material != null and material.shader != null 				and "/self_lit/" in material.shader.resource_path:
			material.set_shader_parameter("u_diffuse", texture)
			material.set_shader_parameter("u_diffuse_max_lod", 1000.0)
			material.set_shader_parameter("u_alpha_mod", 1.0)
			slab.append(material)
	assert_gt(slab.size(), 0, "the fixture carries the FF_ST_AB_LUM heat slab")
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(model)
	assert_true(order >= 0, "the slab caster takes a slot")
	var unbounded := await _capture_after_frames(shadow, order, 3)
	for material in slab:
		material.set_shader_parameter("u_diffuse_max_lod", 0.0)
	shadow.advance_frame()
	var clamped := await _capture_after_frames(shadow, order, 3)
	var report: Dictionary = shadow.get_report()
	assert_eq(String(report["slot_status"]), "drawn", String(report["slot_failure"]))
	assert_not_null(unbounded)
	assert_not_null(clamped)
	if unbounded != null and clamped != null:
		assert_gt(_darkest_max_channel(unbounded), 0.9,
				"unbounded, the minified slab samples its transparent small levels")
		assert_lt(_darkest_max_channel(clamped), 0.1,
				"the ceiling keeps level 0's opaque alpha: a black silhouette")
	model.set_shadow_caster_enabled(false)
	shadow.advance_frame()


## Retail draws the drapes right after the terrain batch with Z-write off, so
## every later draw in front of the terrain overwrites them and an overhanging
## patch edge multiplies the sky drawn before it (renderer::kRungSlotDrape).
## The terrain and the sky dome write a stencil mark in their colour pass; the
## drape (the shipped shaders, slot 0 armed with a uniform silhouette) reads it.
## A 64 x 64 view straight down from 6 u (fov 60: 3.46 u to the edge on the
## ground) over a 4 x 4 terrain card, an opaque red slab 1 x 0.02 x 1 at x = 1
## whose top lies inside the drape lift, a 6 x 6 drape at 0.05 u and the flat
## yellow dome around the eye.
const DRAPE_TERRAIN_PX := Vector2i(22, 32)  # the terrain beside the slab (x = -1)
const DRAPE_SLAB_PX := Vector2i(41, 32)     # the slab top (x = 1)
const DRAPE_SKY_PX := Vector2i(55, 32)      # the drape edge past the terrain (x = 2.5)
const DRAPE_BARE_SKY_PX := Vector2i(62, 32) # the dome past the drape (x = 3.3)


func _flat_texture(colour: Color) -> ImageTexture:
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	image.fill(colour)
	return ImageTexture.create_from_image(image)


func _drape_stencil_view(sky_drawn: bool, with_drape: bool) -> Image:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var clear := WorldEnvironment.new()
	clear.environment = Environment.new()
	clear.environment.background_mode = Environment.BG_COLOR
	clear.environment.background_color = Color(0.0, 0.0, 1.0)
	clear.environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	viewport.add_child(clear)
	var camera := Camera3D.new()
	camera.fov = 60.0
	camera.far = 500.0
	viewport.add_child(camera)
	camera.look_at_from_position(Vector3(0.0, 6.0, 0.0), Vector3.ZERO, Vector3.FORWARD)
	camera.make_current()
	# The dome pass: flat, unfogged, on its rung; a closed bracket keeps only
	# the stencil mark over the clear colour.
	var dome_material := ShaderMaterial.new()
	dome_material.shader = load("res://shaders/sky.gdshader") as Shader
	dome_material.render_priority = ObjectShaderCache.RENDER_RUNG_SKY_DOME
	dome_material.set_shader_parameter("u_flat_pass", true)
	dome_material.set_shader_parameter("u_flat_color", Vector3(1.0, 1.0, 0.0))
	dome_material.set_shader_parameter("u_fog_end", 0.0)
	dome_material.set_shader_parameter("u_beauty_pass_drawn", sky_drawn)
	var dome := MeshInstance3D.new()
	var sphere := SphereMesh.new()
	sphere.radius = 100.0
	sphere.height = 200.0
	# The real dome faces its inside to the eye (sky.gdshader culls back faces).
	sphere.flip_faces = true
	dome.mesh = sphere
	dome.material_override = dome_material
	viewport.add_child(dome)
	# The terrain card, drawn in the normals debug colour (0.5, 1, 0.5).
	var terrain_material := ShaderMaterial.new()
	terrain_material.shader = load("res://shaders/terrain.gdshader") as Shader
	terrain_material.set_shader_parameter("u_debug_mode", 3)
	var terrain := MeshInstance3D.new()
	var card := PlaneMesh.new()
	card.size = Vector2(4.0, 4.0)
	terrain.mesh = card
	terrain.material_override = terrain_material
	viewport.add_child(terrain)
	# An opaque model in front of the terrain but inside the drape lift.
	var slab_material := StandardMaterial3D.new()
	slab_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	slab_material.albedo_color = Color(1.0, 0.0, 0.0)
	var slab := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(1.0, 0.02, 1.0)
	slab.mesh = box
	slab.material_override = slab_material
	viewport.add_child(slab)
	slab.position = Vector3(1.0, 0.01, 0.0)
	if with_drape:
		# Slot 0 armed with a black silhouette and a 0.25 material term: the
		# drape multiplies whatever it may touch by 0.25 (unfogged).
		var drape_material := ShaderMaterial.new()
		drape_material.shader = SlotShadow.get_drape_material().shader
		drape_material.render_priority = SlotShadow.get_drape_material().render_priority
		var terms := PackedVector4Array()
		terms.resize(12)
		terms[0] = Vector4(0.25, 0.25, 0.25, 1.0)
		var zeros := PackedVector4Array()
		zeros.resize(12)
		drape_material.set_shader_parameter("u_slot_term", terms)
		drape_material.set_shader_parameter("u_slot_clip_u", zeros)
		drape_material.set_shader_parameter("u_slot_clip_v", zeros)
		drape_material.set_shader_parameter("u_slot_light_pos", zeros)
		drape_material.set_shader_parameter("u_slot_light_diffuse", zeros)
		drape_material.set_shader_parameter("u_slot_tex_0", _flat_texture(Color.BLACK))
		drape_material.set_shader_parameter("u_shadowztex", _flat_texture(Color.BLACK))
		drape_material.set_shader_parameter("u_slot_mat_0", Projection.IDENTITY)
		drape_material.set_shader_parameter("u_fog_type", 1)
		drape_material.set_shader_parameter("u_fog_start", 0.0)
		drape_material.set_shader_parameter("u_fog_end", 1.0e6)
		var drape := MeshInstance3D.new()
		var patch := PlaneMesh.new()
		patch.size = Vector2(6.0, 6.0)
		drape.mesh = patch
		drape.material_override = drape_material
		viewport.add_child(drape)
		drape.position = Vector3(0.0, 0.05, 0.0)
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	return viewport.get_texture().get_image()


func _pixel(image: Image, at: Vector2i) -> Color:
	return image.get_pixel(at.x, at.y)


func test_windowed_drape_stops_at_models_in_front_of_the_terrain() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var bare: Image = await _drape_stencil_view(true, false)
	var draped: Image = await _drape_stencil_view(true, true)
	var terrain_bare := _pixel(bare, DRAPE_TERRAIN_PX)
	var terrain_draped := _pixel(draped, DRAPE_TERRAIN_PX)
	assert_gt(terrain_bare.g, 0.9, "the terrain card draws its debug green (%s)" % terrain_bare)
	assert_lt(terrain_draped.g, terrain_bare.g * 0.8,
			"the drape darkens the terrain where it is the front-most surface (%s -> %s)"
			% [terrain_bare, terrain_draped])
	var slab_bare := _pixel(bare, DRAPE_SLAB_PX)
	var slab_draped := _pixel(draped, DRAPE_SLAB_PX)
	assert_gt(slab_bare.r, 0.9, "the slab draws red over the terrain (%s)" % slab_bare)
	assert_almost_eq(slab_draped.r, slab_bare.r, 0.02,
			"a model in front of the terrain keeps its colour inside the lift (%s)"
			% slab_draped)
	assert_almost_eq(slab_draped.g, slab_bare.g, 0.02)
	assert_almost_eq(slab_draped.b, slab_bare.b, 0.02)


func test_windowed_drape_edge_multiplies_the_sky_drawn_before_it() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var bare: Image = await _drape_stencil_view(true, false)
	var draped: Image = await _drape_stencil_view(true, true)
	var sky_bare := _pixel(bare, DRAPE_SKY_PX)
	var sky_draped := _pixel(draped, DRAPE_SKY_PX)
	assert_gt(sky_bare.r, 0.9, "the flat yellow dome fills the view past the card (%s)" % sky_bare)
	assert_lt(sky_draped.r, sky_bare.r * 0.8,
			"an overhanging drape edge multiplies the dome (%s -> %s)" % [sky_bare, sky_draped])
	var beyond := _pixel(draped, DRAPE_BARE_SKY_PX)
	assert_almost_eq(beyond.r, _pixel(bare, DRAPE_BARE_SKY_PX).r, 0.02,
			"the dome past the patch keeps its colour (%s)" % beyond)


func test_windowed_closed_sky_bracket_leaves_the_clear_to_the_drape() -> void:
	# A skipped sky bracket leaves the clear colour, which retail's drape then
	# multiplies like any other pixel the frame holds there.
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var bare: Image = await _drape_stencil_view(false, false)
	var draped: Image = await _drape_stencil_view(false, true)
	var clear_bare := _pixel(bare, DRAPE_SKY_PX)
	assert_gt(clear_bare.b, 0.9, "a closed bracket shows the blue clear (%s)" % clear_bare)
	assert_lt(clear_bare.r, 0.05, "no dome colour under a closed bracket (%s)" % clear_bare)
	var clear_draped := _pixel(draped, DRAPE_SKY_PX)
	assert_lt(clear_draped.b, clear_bare.b * 0.8,
			"the drape edge multiplies the clear colour (%s -> %s)" % [clear_bare, clear_draped])
	assert_almost_eq(_pixel(draped, DRAPE_BARE_SKY_PX).b, 1.0, 0.02,
			"the clear past the patch is untouched")
