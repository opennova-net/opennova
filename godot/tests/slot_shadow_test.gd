extends GutTest

## The render-slot entity ground-shadow device (SlotShadow): the 24-patch /
## 12-capture admission, the per-armed-slot capture requests the
## RenderingDevice pass draws, the capture-with child walk, the retail
## refresh cadence, the PROJSHAD coverage table, the compositor lifecycle and
## the local player's first-person drape gates. The planning laws themselves
## are pinned portable (tests/renderer/render_slot_shadow_test.cpp); this
## suite pins the device wiring (docs/render/render-lighting-re.md, the
## render-slot side).

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


func _camera() -> Camera3D:
	var camera := Camera3D.new()
	camera.current = true
	add_child_autofree(camera)
	camera.global_transform = Transform3D(Basis(), Vector3.ZERO)
	return camera


func _caster_at(z: float) -> ObjectModel:
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.position = Vector3(0.0, 0.0, -z)
	var mesh := MeshInstance3D.new()
	mesh.mesh = BoxMesh.new()
	model.add_child(mesh)
	model.set_shadow_caster_enabled(true)
	return model


## A fresh device planned once against an empty group: the caster group is
## scene-tree wide, so every absolute count in this suite guards against a
## caster another test left enabled.
func _fresh_shadow(environment: MissionEnvironment) -> SlotShadow:
	var shadow := SlotShadow.new()
	add_child_autofree(shadow)
	shadow.set_environment_node(environment)
	shadow.advance_frame()
	assert_eq(int(shadow.get_report()["registered"]), 0,
			"no caster from another test is still in the slot group")
	return shadow


func test_caster_flag_joins_and_leaves_the_slot_group() -> void:
	var model := _caster_at(5.0)
	assert_true(model.is_in_group("slot_shadow_casters"),
			"an enabled dynamic caster registers for slot planning")
	model.set_shadow_caster_enabled(false)
	assert_false(model.is_in_group("slot_shadow_casters"),
			"disabling the caster releases the slot registration")


func test_admission_caps_follow_the_retail_patch_and_capture_budgets() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	for i in range(30):
		var _model := _caster_at(4.0 + 2.0 * float(i))
	shadow.advance_frame()
	var report: Dictionary = shadow.get_report()
	assert_eq(int(report["registered"]), 30, "every group caster registers")
	assert_eq(int(report["bound"]), 24,
			"the drape patch budget binds the nearest 24 (retail: 24 slots)")
	assert_eq(int(report["captures"]), 12,
			"the silhouette RT budget captures the nearest 12 (retail chain)")
	# Every fresh capture order is dirty on its first assignment, so all
	# twelve arm a request this frame (RenderSlot_SortAndAssign dword3).
	assert_eq(int(report["armed"]), 12, "every newly ordered slot arms a capture")
	assert_eq(shadow.get_armed_capture_mask(), 0xFFF)


func test_drape_binds_the_twelve_capture_textures_once() -> void:
	# The retail RT chain: twelve textures the drape samples through
	# u_slot_tex_i, each a Texture2DRD over the live device's resolve target
	# (RenderSlot_InitTextureChain @0x5d5320).
	var drape := SlotShadow.get_drape_material()
	assert_eq(SlotShadow.get_capture_count(), 12)
	for order in range(12):
		var bound: Texture2D = drape.get_shader_parameter("u_slot_tex_%d" % order)
		assert_not_null(bound, "u_slot_tex_%d is bound" % order)
		assert_true(bound is Texture2DRD, "the drape samples a Texture2DRD")
		assert_eq(bound, SlotShadow.get_capture_texture(order),
				"the bound texture is the order's capture texture")
	assert_null(SlotShadow.get_capture_texture(12), "no texture past the chain")


func test_admitted_caster_publishes_one_capture_request() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var near_model := _caster_at(5.0)
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(near_model)
	assert_true(order >= 0 and order < 12, "an admitted caster takes a slot order")
	assert_eq(shadow.get_capture_caster_count(order), 1,
			"the request carries the caster alone")
	assert_ne(shadow.get_armed_capture_mask() & (1 << order), 0,
			"the fresh order is armed (dirty)")
	assert_eq(int(shadow.get_report()["armed"]), 1)
	# The target side follows the retail chain for the detail level
	# (render_slot_shadow.h slot_texture_size) once a device exists; without
	# one the request still publishes and the size stays 0.
	var size := shadow.get_capture_target_size(order)
	if RenderingServer.get_rendering_device() != null:
		assert_eq(size, 512, "detail 3 order 0 = the 512 px chain base")
	else:
		assert_eq(size, 0, "no device target headless")
	# Releasing the caster drops its order on the next plan.
	near_model.set_shadow_caster_enabled(false)
	shadow.advance_frame()
	assert_eq(shadow.get_capture_order_of(near_model), -1,
			"a released caster leaves its slot")
	assert_eq(int(shadow.get_report()["armed"]), 0)


func _child_shares_parent_slot(parent_first: bool) -> void:
	# The retail child walk renders a capture-with child (held weapon, mounted
	# child) into its PARENT's slot RT (RenderSlot_RenderEntityAndChildren
	# @0x5d78ef..0x5d79d6); the child's own slot is excluded. Registration
	# order must not matter.
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var parent: ObjectModel
	var child: ObjectModel
	if parent_first:
		parent = _caster_at(5.0)
		child = _caster_at(5.5)
	else:
		child = _caster_at(5.5)
		parent = _caster_at(5.0)
	child.set_slot_shadow_capture_with(parent)
	shadow.advance_frame()
	var order_label := "parent first" if parent_first else "child first"
	var parent_order := shadow.get_capture_order_of(parent)
	assert_true(parent_order >= 0, "the parent captures (%s)" % order_label)
	assert_eq(shadow.get_capture_order_of(child), parent_order,
			"the linked child renders into its parent's slot (%s)" % order_label)
	assert_eq(shadow.get_capture_caster_count(parent_order), 2,
			"the request carries the parent and its claimed child (%s)" % order_label)
	assert_eq(int(shadow.get_report()["captures"]), 1,
			"the child's own slot is excluded (%s)" % order_label)


func test_capture_with_child_rides_its_parent_slot_parent_registered_first() -> void:
	_child_shares_parent_slot(true)


func test_capture_with_child_rides_its_parent_slot_child_registered_first() -> void:
	_child_shares_parent_slot(false)


func test_capture_with_child_refused_by_the_full_table_still_rides_its_parent() -> void:
	# The fixed 256-record table refuses the 257th registration
	# (RenderSlot_AllocSlot @0x5d5690), so a linked child past the cap has no
	# assignment row of its own; the child walk follows the entity hierarchy,
	# not the table, so it still captures into its parent's slot — and leaves
	# once the link drops, even while it stays row-less.
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var parent := _caster_at(5.0)
	for i in range(255):
		var _filler := _caster_at(6.0 + 0.5 * float(i))
	var child := _caster_at(5.5)
	child.set_slot_shadow_capture_with(parent)
	shadow.advance_frame()
	assert_eq(int(shadow.get_report()["registered"]), 256,
			"the fixed table holds 256 records, so the linked child is refused")
	var parent_order := shadow.get_capture_order_of(parent)
	assert_true(parent_order >= 0, "the nearest caster captures")
	assert_eq(shadow.get_capture_order_of(child), parent_order,
			"a row-less linked child still renders into its parent's slot")
	assert_eq(shadow.get_capture_caster_count(parent_order), 2)
	child.set_slot_shadow_capture_with(null)
	shadow.advance_frame()
	assert_eq(shadow.get_capture_order_of(child), -1,
			"dropping the link leaves the parent's slot on the next plan")


func test_refresh_cadence_arms_by_record_index_and_the_local_player_every_frame() -> void:
	# Detail 3: mask 1, a slot re-captures when (frame & 1) == (index & 1) or
	# its order just changed; the local player refreshes every frame from
	# detail 3 up [orig: RenderSlot_RenderEntityAndChildren @0x5d76d9..0x5d7748].
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)  # frame 1
	var model := _caster_at(5.0)
	shadow.advance_frame()  # frame 2: the fresh order is dirty -> armed
	var order := shadow.get_capture_order_of(model)
	assert_true(order >= 0)
	assert_ne(shadow.get_armed_capture_mask() & (1 << order), 0, "dirty order arms")
	var armed_history := []
	for i in range(4):
		shadow.advance_frame()  # frames 3..6
		armed_history.append((shadow.get_armed_capture_mask() & (1 << order)) != 0)
		assert_eq(shadow.get_capture_order_of(model), order,
				"the capture order stays sticky across the cadence")
	assert_eq(armed_history, [false, true, false, true],
			"record 0 re-captures on even frames only at detail 3 (mask 1)")
	assert_eq(shadow.get_capture_caster_count(order), 1,
			"an armed frame publishes the caster's request")
	shadow.advance_frame()  # frame 7: unarmed
	assert_eq(shadow.get_capture_caster_count(order), 0,
			"an unarmed frame publishes no request; the target keeps its capture")
	shadow.set_local_player_model(model)
	for i in range(3):
		shadow.advance_frame()
		assert_ne(shadow.get_armed_capture_mask() & (1 << order), 0,
				"the local player's slot re-captures every frame from detail 3")


func test_projshad_coverage_follows_the_engine_table_per_technique() -> void:
	# The capture samples the coverage the technique's PROJSHAD pass samples
	# (engine object_projected_shadow_coverage = the pipeline manifest's
	# projected_shadow_contracts): _FFP keeps Diffuse1.a x AlphaGenValue, its
	# _MT blocks x Diffuse2.a, the file effects Diffuse1.a alone, glass none.
	var cache := ObjectShaderCache.get_singleton()
	assert_not_null(cache)
	if cache == null:
		return
	var expectations := {
		"FF_ST_OP": "diffuse_alpha_ffp",
		"FF_ST_AB": "diffuse_alpha_ffp",
		"FF_MT_OP": "diffuse_detail_alpha_ffp",
		"FF_ST_OP_LUM": "diffuse_alpha_ffp",
		"FF_MT_OP_LUM": "diffuse_detail_alpha_ffp",
		"VS_PHONGT": "diffuse_alpha",
		"VS_DOT3DIFF": "diffuse_alpha",
		"VS_DOT3DIFFOBJ": "diffuse_alpha",
		"VS_BMTXMIRRT": "diffuse_alpha",
		"VS_SKBASIC": "diffuse_alpha",
		"FFP_GLASS": "no_pass",
		"VS_SKGLASS": "no_pass",
	}
	var manifest_text := FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json")
	var manifest: Dictionary = JSON.parse_string(manifest_text)
	var contracts: Dictionary = manifest["projected_shadow_contracts"]
	var known: Array = Array(contracts.values())
	for tag in expectations:
		var emissive := 2 if String(tag).ends_with("_LUM") else 0
		var key := cache.classify(tag, 0, emissive, 0, 128)
		var coverage := cache.projected_shadow_coverage_for_key(key)
		assert_eq(coverage, expectations[tag], "%s PROJSHAD coverage" % tag)
		assert_true(known.has(coverage),
				"%s names a manifest projected_shadow_contracts token" % tag)


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


func _effect_present(world_environment: WorldEnvironment) -> bool:
	var compositor := world_environment.compositor
	if compositor == null:
		return false
	for effect in compositor.compositor_effects:
		if effect is SlotCaptureCompositorEffect:
			return true
	return false


func test_capture_effect_installs_on_the_scope_world_environment_and_re_enters() -> void:
	# The captures draw at PRE_OPAQUE on the beauty view's compositor so the
	# drape samples finished targets in the same frame; exit-tree releases
	# the effect and its device resources, re-entry rebuilds them (the
	# FrameFx contract).
	var scope := _scope_with_world_environment()
	var world_environment := scope.get_node("ClearColor") as WorldEnvironment
	var shadow := SlotShadow.new()
	scope.add_child(shadow)
	assert_true(shadow.is_capture_effect_installed(),
			"ready installs the capture effect on the scope's WorldEnvironment")
	assert_true(_effect_present(world_environment))
	var report: Dictionary = shadow.get_report()
	assert_true(bool(report["capture_effect_installed"]))
	assert_eq(int(report["slot_callback_type"]),
			CompositorEffect.EFFECT_CALLBACK_TYPE_PRE_OPAQUE,
			"the captures run before the opaque pass")
	scope.remove_child(shadow)
	assert_false(shadow.is_capture_effect_installed(), "exit-tree uninstalls")
	assert_false(_effect_present(world_environment),
			"the compositor no longer carries the effect")
	# Shutdown is idempotent: a second exit-tree path finds nothing to free.
	scope.add_child(shadow)
	assert_true(shadow.is_capture_effect_installed(), "re-entry reinstalls")
	assert_true(_effect_present(world_environment))
	scope.remove_child(shadow)
	assert_false(shadow.is_capture_effect_installed())
	shadow.free()


func _effect_count(world_environment: WorldEnvironment) -> int:
	var count := 0
	if world_environment.compositor == null:
		return 0
	for effect in world_environment.compositor.compositor_effects:
		if effect is SlotCaptureCompositorEffect:
			count += 1
	return count


func test_capture_effect_follows_a_replaced_scope_compositor() -> void:
	# FrameFx::install_compositor and DisplayDecode::install replace the
	# WorldEnvironment's compositor with a fresh one carrying the previous
	# effects; a scope may also hand it an empty one. The install must follow
	# the live compositor on the next frame so the report, the draw and the
	# uninstall all name the compositor the view renders with.
	var scope := _scope_with_world_environment()
	var world_environment := scope.get_node("ClearColor") as WorldEnvironment
	var shadow := SlotShadow.new()
	scope.add_child(shadow)
	assert_true(shadow.is_capture_effect_installed())
	var first := world_environment.compositor
	# The FrameFx shape: a new Compositor that copies the previous effects.
	var copied := Compositor.new()
	copied.compositor_effects = first.compositor_effects.duplicate()
	world_environment.compositor = copied
	assert_false(shadow.is_capture_effect_installed(),
			"the install is keyed on the compositor object, so the copy reads stale")
	shadow.advance_frame()
	assert_true(shadow.is_capture_effect_installed(),
			"advance_frame re-installs into the live compositor")
	assert_eq(_effect_count(world_environment), 1,
			"the carried-over effect is not added a second time")
	# An empty replacement: the effect must be re-added.
	world_environment.compositor = Compositor.new()
	shadow.advance_frame()
	assert_true(shadow.is_capture_effect_installed())
	assert_eq(_effect_count(world_environment), 1,
			"a compositor that dropped the effect gets it back")
	assert_true(bool(shadow.get_report()["capture_effect_installed"]))
	scope.remove_child(shadow)
	assert_false(_effect_present(world_environment),
			"exit-tree uninstalls from the live compositor, not the stale one")
	shadow.free()


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


func test_local_player_first_person_drape_gates() -> void:
	# Retail skips the local player's own drape in first person while
	# prone-latched or below shadow detail 2
	# (RenderSlot_DrawAllDrapes @0x5d6e70..0x5d6e90).
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var body := _caster_at(2.0)
	shadow.set_local_player_model(body)
	shadow.set_local_player_first_person(true)
	shadow.set_local_player_prone(true)
	shadow.advance_frame()
	assert_eq(int(shadow.get_report()["captures"]), 0,
			"a prone first-person local player draws no own drape")
	shadow.set_local_player_prone(false)
	shadow.advance_frame()
	assert_eq(int(shadow.get_report()["captures"]), 1,
			"standing first person keeps the local drape at detail >= 2")
	shadow.set_shadow_detail(1)
	shadow.advance_frame()
	assert_eq(int(shadow.get_report()["captures"]), 0,
			"below shadow detail 2 the first-person local drape is skipped")


## The drape is bounded by the retail lod x lod patch and clipped by the
## shadowztex depth stage: a bound silhouette slot publishes a non-degenerate
## world patch of side 6..20 u around the caster, its depth-clip texgen rows,
## and the shared 32x4 step texture [orig: RenderSlot_RebuildPatchVertexBuffer
##  @0x5d5130; build_shadow_cascade_uv_matrices @0x58cf10;
##  shadow_system_init_resources @0x5d6260 — renderer::slot_patch_bounds,
##  slot_depth_clip, shadowztex_pixels].
func test_bound_slot_publishes_its_patch_and_depth_clip() -> void:
	var environment := _environment()
	_camera()
	var shadow := _fresh_shadow(environment)
	var caster := _caster_at(4.0)
	caster.set_shadow_bound_radii(2.0, 2.0625)  # lod 5 -> floor 6 at noon, <= 20 grazing
	shadow.advance_frame()
	var drape := SlotShadow.get_drape_material()
	var ztex: Texture2D = drape.get_shader_parameter("u_shadowztex")
	assert_not_null(ztex, "the depth-clip texture is bound once on the drape")
	if ztex != null:
		assert_eq(ztex.get_width(), 32)
		assert_eq(ztex.get_height(), 4)
	var patches: PackedVector4Array = drape.get_shader_parameter("u_slot_patch")
	assert_eq(patches.size(), 12)
	var patch := patches[0]
	var side_x := patch.z - patch.x
	var side_z := patch.w - patch.y
	assert_almost_eq(side_x, side_z, 0.001, "the patch is a square")
	assert_true(side_x >= 6.0 and side_x <= 20.0,
			"the patch side is the clamped lod (6..20 u): %s" % str(side_x))
	assert_true(patch.x <= caster.global_position.x and caster.global_position.x <= patch.z,
			"the patch straddles the caster east-west")
	assert_true(patch.y <= caster.global_position.z and caster.global_position.z <= patch.w,
			"the patch straddles the caster north-south")
	var clip_u: PackedVector4Array = drape.get_shader_parameter("u_slot_clip_u")
	var clip_v: PackedVector4Array = drape.get_shader_parameter("u_slot_clip_v")
	assert_eq(clip_u.size(), 12)
	assert_eq(clip_v.size(), 12)
	assert_gt(Vector3(clip_u[0].x, clip_u[0].y, clip_u[0].z).length(), 0.0,
			"the clip u row carries the steepened slot direction")
	assert_gt(Vector3(clip_v[0].x, clip_v[0].y, clip_v[0].z).length(), 0.0,
			"the clip v row carries the far-fade direction")
	# The caster's own position sits beyond the plane through lp = pos - dir
	# (u2 > 0.5): the black half keeps the shadow under the caster.
	var u2 := Vector3(clip_u[0].x, clip_u[0].y, clip_u[0].z).dot(caster.global_position) + clip_u[0].w
	assert_gt(u2, 0.5, "the ground under the caster lies in the drawn half")
	caster.set_shadow_caster_enabled(false)
	shadow.advance_frame()


func test_terrain_material_chains_the_shared_drape_passes() -> void:
	var drape: ShaderMaterial = SlotShadow.get_drape_material()
	assert_not_null(drape, "the shared drape material exists")
	assert_not_null(drape.next_pass,
			"the authored-blob pass chains behind the silhouette pass")
	assert_true(drape.shader.resource_path.ends_with(
			"slot_shadow_drape.gdshader"))


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


func test_windowed_capture_draws_the_caster_black_over_the_white_clear() -> void:
	# The retail slot RT: cleared 0x00FFFFFF, the PROJSHAD pass draws the
	# caster black (render_shadow_pass @0x5d7b70; vscPostBlackT1). Needs a live
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
