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


func test_the_mission_start_makes_the_whole_chain_ahead_of_the_first_capture() -> void:
	# Retail makes the twelve RTs with the mission start's render subsystems (engine
	# renderer/render_slot_shadow.h creates_slot_texture_chain, D-NET-NEW-LF-24): every
	# order's target stands at the chain's size before any capture arms; without a
	# RenderingDevice none is made.
	var shadow := SlotShadow.new()
	add_child_autofree(shadow)
	shadow.set_shadow_detail(3)
	shadow.create_capture_targets()
	var sizes := [512, 512, 256, 256, 128, 128, 64, 64, 32, 32, 32, 32]
	var device := RenderingServer.get_rendering_device() != null
	for order in range(12):
		assert_eq(shadow.get_capture_target_size(order), int(sizes[order]) if device else 0,
				"order %d's target (device: %s)" % [order, device])
	assert_eq(shadow.get_armed_capture_mask(), 0, "no capture armed by the making")


func test_the_load_packs_every_casters_geometry_ahead_of_its_first_capture() -> void:
	# Retail's capture draws each caster through the model the mission start loaded (engine
	# renderer/render_slot_shadow.h slot_casters_load_with_models, D-NET-NEW-LF-27): the load's
	# warm packs the crate's admitted surfaces, and its first capture packs nothing more. With
	# the chain off nothing is packed.
	var scope := _scope_with_world_environment()
	var environment := _environment()
	var camera := Camera3D.new()
	camera.current = true
	scope.add_child(camera)
	camera.look_at_from_position(Vector3(0, 2, 10), Vector3.ZERO, Vector3.UP)
	var shadow := SlotShadow.new()
	scope.add_child(shadow)
	shadow.set_environment_node(environment)
	var crate := _crate_caster(scope, Vector3.ZERO)
	crate.advance_runtime_frame(1.0 / 62.0)
	shadow.set_shadow_detail(0)
	assert_eq(shadow.warm_caster_streams(), 0, "the chain off: no caster packs")
	shadow.set_shadow_detail(3)
	var warmed: int = shadow.warm_caster_streams()
	shadow.advance_frame()
	var report: Dictionary = shadow.get_report()
	assert_gte(warmed, int(report["slot_surfaces_compiled"]),
			"the warm packed every surface the first capture draws")
	assert_eq(int(report["slot_packed_vertices"]), 0,
			"the first capture packs nothing the load packed")
	crate.set_shadow_caster_enabled(false)
	shadow.advance_frame()


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


## A tree may hold several scenes, each its own World3D (the OpenNova Editor's mission devices under their
## SubViewports): a device plans the casters of its own world alone, the group being the tree's.
func test_a_device_plans_its_own_worlds_casters() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var near_model := _caster_at(5.0)
	# A second scene: its own viewport and world, a caster of its own in it.
	var other_scene := SubViewport.new()
	other_scene.own_world_3d = true
	add_child_autofree(other_scene)
	var elsewhere: ObjectModel = autofree(ObjectModel.new())
	other_scene.add_child(elsewhere)
	elsewhere.set_process(false)
	elsewhere.position = Vector3(0.0, 0.0, -6.0)
	var mesh := MeshInstance3D.new()
	mesh.mesh = BoxMesh.new()
	elsewhere.add_child(mesh)
	elsewhere.set_shadow_caster_enabled(true)
	assert_true(elsewhere.is_in_group("slot_shadow_casters"), "the other scene's caster joins the tree's group")
	assert_ne(elsewhere.get_world_3d(), shadow.get_world_3d(), "a world of its own")
	shadow.advance_frame()
	assert_eq(int(shadow.get_report()["registered"]), 1, "the device's own world's caster alone registers")
	assert_true(shadow.get_capture_order_of(near_model) >= 0, "its own caster takes a slot")
	assert_eq(shadow.get_capture_order_of(elsewhere), -1, "the other world's caster takes none")
	# Moved into the device's world, it plans there.
	elsewhere.reparent(self)
	shadow.advance_frame()
	assert_eq(int(shadow.get_report()["registered"]), 2, "moved into the device's world, it registers")


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
##  @0x5d5130; RenderSlot_BuildShadowCascadeUVMatrices @0x58cf10;
##  Shadow_SystemInitResources @0x5d6260 — renderer::slot_patch_bounds,
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
	var order := shadow.get_capture_order_of(caster)
	var bounds := _patch_bounds(shadow.get_patch_vertices(order))
	var side_x := bounds.z - bounds.x
	var side_z := bounds.w - bounds.y
	assert_almost_eq(side_x, side_z, 0.001, "the patch is a square")
	assert_true(side_x >= 6.0 and side_x <= 20.0,
			"the patch side is the clamped lod (6..20 u): %s" % str(side_x))
	assert_true(bounds.x <= caster.global_position.x and caster.global_position.x <= bounds.z,
			"the patch straddles the caster east-west")
	assert_true(bounds.y <= caster.global_position.z and caster.global_position.z <= bounds.w,
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


## The maintained caster registry caches per-caster facts (radii, the person
## flag, the decal); every mutation site bumps the registry revision, so a
## fact changed AFTER a planned frame must land on the very next frame — the
## stale-cache guard for the registry rework.
func test_cached_caster_facts_refresh_on_their_setters() -> void:
	var environment := _environment()
	_camera()
	var shadow := _fresh_shadow(environment)
	var caster := _caster_at(4.0)
	caster.set_shadow_bound_radii(2.0, 2.0625)
	shadow.advance_frame()
	var drape := SlotShadow.get_drape_material()
	var patch_before := _patch_bounds(shadow.get_patch_vertices(0))
	var clip_v_before: Vector4 = drape.get_shader_parameter("u_slot_clip_v")[0]
	# A radius stamped after the plan (the husk-swap shape) resizes the patch
	# and re-derives the clip rows on the very next frame.
	caster.set_shadow_bound_radii(4.0, 9.0)
	shadow.advance_frame()
	var patch_after := _patch_bounds(shadow.get_patch_vertices(0))
	assert_gt(patch_after.z - patch_after.x, patch_before.z - patch_before.x,
			"a bigger entity bound stamped after a plan grows the next patch")
	var clip_v_after: Vector4 = drape.get_shader_parameter("u_slot_clip_v")[0]
	assert_ne(clip_v_after, clip_v_before,
			"the bigger capture sphere re-derives the clip rows next frame")
	caster.set_shadow_caster_enabled(false)
	shadow.advance_frame()


## A bound slot past the 12-capture budget drapes nothing: retail's
## authored-blob leg (RenderSlot_DrawAuthoredBlobDecal @0x5d59d0) gates on
## ItemDef+0x114, which JO never assigns, so the drape is the silhouette pass
## alone.
func test_terrain_drape_is_the_silhouette_pass_alone() -> void:
	var drape: ShaderMaterial = SlotShadow.get_drape_material()
	assert_not_null(drape, "the shared drape material exists")
	assert_null(drape.next_pass, "no authored-blob pass chains behind the silhouette pass")
	assert_true(drape.shader.resource_path.ends_with(
			"slot_shadow_drape.gdshader"))


func test_slots_past_the_capture_budget_publish_no_drape() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	for i in range(20):
		var _model := _caster_at(4.0 + 2.0 * float(i))
	shadow.advance_frame()
	var report: Dictionary = shadow.get_report()
	assert_eq(int(report["bound"]), 20, "all twenty casters bind a drape patch")
	assert_eq(int(report["captures"]), 12, "only the nearest twelve own an RT")
	assert_false(report.has("blobs"), "no authored-blob leg is reported")
	var terms: PackedVector4Array = SlotShadow.get_drape_material().get_shader_parameter(
			"u_slot_term")
	var drawn := 0
	for term in terms:
		if term.w > 0.5:
			drawn += 1
	assert_eq(drawn, 12, "only the twelve RT slots publish a drape term")


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


const PUMP_3DI := "res://../fixtures/threedi/synth/pump.3di"


func test_capture_follows_the_casters_authored_rlod_switch() -> void:
	# An authored RLOD switch keeps the caster's surface slots (the same
	# MeshInstance3D nodes) and swaps the level's ArrayMesh onto them
	# (ObjectModel.apply_level_surfaces). The capture pass keys its packed
	# geometry on the mesh as well as the node, so a crossing packs the new
	# level once (the pump's LOD1 drops the base slab's post: fewer packed
	# vertices), a return to a level seen before re-packs nothing, and the
	# silhouette never stays the first-seen level's. The compile runs with or
	# without a RenderingDevice; a windowed run draws what it compiles.
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
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(PUMP_3DI)), OK)
	var model := ObjectModel.new()
	scope.add_child(model)
	model.set_process(false)
	model.set_authored_lod_enabled(true)
	model.set_object_data(data)
	assert_gt(model.get_level_surface_count(1), 0,
			"the pump retains its coarser authored level")
	model.set_shadow_caster_enabled(true)
	model.advance_runtime_frame(1.0 / 62.0)
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(model)
	assert_true(order >= 0, "the pump takes a slot")
	var report: Dictionary = shadow.get_report()
	var level0_vertices := int(report["slot_packed_vertices"])
	assert_gt(level0_vertices, 0, "LOD0's surfaces pack at first sight")
	shadow.advance_frame()
	assert_eq(int(shadow.get_report()["slot_packed_vertices"]), 0,
			"a stable frame re-packs nothing")

	model.set_active_lod(1)
	shadow.advance_frame()
	report = shadow.get_report()
	var level1_vertices := int(report["slot_packed_vertices"])
	assert_gt(level1_vertices, 0,
			"the level swapped onto the retained slots packs once")
	assert_lt(level1_vertices, level0_vertices,
			"LOD1 packs the coarser geometry (the post is gone), not LOD0's again")
	assert_gt(int(report["slot_surfaces_compiled"]), 0,
			"the switched caster still compiles its black pass")
	shadow.advance_frame()
	assert_eq(int(shadow.get_report()["slot_packed_vertices"]), 0,
			"the switched level is stable on the next frame")

	model.set_active_lod(0)
	shadow.advance_frame()
	assert_eq(int(shadow.get_report()["slot_packed_vertices"]), 0,
			"LOD0's entry was retained through the crossing and needs no re-pack")
	model.set_shadow_caster_enabled(false)
	shadow.advance_frame()


func _patch_bounds(vertices: PackedVector3Array) -> Vector4:
	if vertices.is_empty():
		return Vector4()
	var bounds := Vector4(INF, INF, -INF, -INF)
	for vertex in vertices:
		bounds.x = minf(bounds.x, vertex.x)
		bounds.y = minf(bounds.y, vertex.z)
		bounds.z = maxf(bounds.z, vertex.x)
		bounds.w = maxf(bounds.w, vertex.z)
	return bounds


func _single_caster_term(shadow: SlotShadow, model: ObjectModel) -> Vector4:
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(model)
	assert_true(order >= 0, "the caster owns a capture order")
	var terms: PackedVector4Array = SlotShadow.get_drape_material().get_shader_parameter(
			"u_slot_term")
	return terms[order] if order >= 0 else Vector4()


## The drape distance fade is ONE value per slot, from the entity-to-camera 3D
## distance (retail: RenderSlot_DrawSilhouetteDrape @0x5d5cc4..0x5d5d59), folded
## into the slot's material ambient 1 - (1 - fade) * q: a caster 60 u out
## (fade 0.5) publishes half the darkening of the same caster 10 u out.
func test_drape_fade_is_one_value_per_slot_from_the_entity_distance() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var caster := _caster_at(10.0)
	var near := _single_caster_term(shadow, caster)
	assert_eq(near.w, 1.0, "a near slot drapes")
	assert_lt(near.x, 1.0, "the near ambient darkens")
	caster.position = Vector3(0.0, 0.0, -60.0)
	var far := _single_caster_term(shadow, caster)
	assert_eq(far.w, 1.0, "a 60 u slot still drapes")
	for channel in 3:
		assert_almost_eq(far[channel], 1.0 - 0.5 * (1.0 - near[channel]), 0.0005,
				"channel %d: the 60 u slot carries the 0.5 fade" % channel)
	caster.set_shadow_caster_enabled(false)
	shadow.advance_frame()


## At >= 80 u retail skips the drape outright (0x500000 @0x5d5d3b) while the
## silhouette capture itself still runs.
func test_drape_is_skipped_past_80_units_while_the_capture_still_arms() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var caster := _caster_at(85.0)
	var term := _single_caster_term(shadow, caster)
	assert_eq(term.w, 0.0, "the 85 u slot publishes no drape")
	var order := shadow.get_capture_order_of(caster)
	assert_ne(shadow.get_armed_capture_mask() & (1 << order), 0,
			"its fresh capture order is still armed")
	caster.set_shadow_caster_enabled(false)
	shadow.advance_frame()


## The sun leg reads the STORED slot vertical — the raw clamped-negated tuple
## (retail: fabs of slot+0x6C @0x5d5f63) — so a dawn sun below the 0.25 clamp
## darkens by sun*0.25 / (sun*0.25 + sky), not by the normalized vertical.
func test_drape_reads_the_raw_clamped_sun_vertical() -> void:
	var data := EnvFile.new()
	data.reset_to_default()
	data.set_curtime(630)
	var environment := MissionEnvironment.new()
	environment.environment_data = data
	add_child_autofree(environment)
	var tuple: Vector3 = environment.get_light_direction()
	if tuple.y >= 0.25 or tuple.y <= 0.0:
		pending("the 06:30 default sun is not below the 0.25 clamp")
		return
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var caster := _caster_at(10.0)
	var term := _single_caster_term(shadow, caster)
	var sun: Vector3 = environment.get_sun_light()
	var sky: Vector3 = environment.get_sky_ambient()
	for channel in 3:
		var lit := sun[channel] * 0.25
		var q := lit / (lit + sky[channel]) if lit + sky[channel] > 0.0 else 0.0
		assert_almost_eq(term[channel], 1.0 - q, 0.0005,
				"channel %d uses |y| = 0.25, the stored clamp" % channel)
	caster.set_shadow_caster_enabled(false)
	shadow.advance_frame()


## The drape fogs with the terrain's primary fog config (toward white): it
## carries exactly the fog uniforms MissionEnvironment hands the terrain
## (retail: RenderSlot_DrawAllDrapes @0x5d6ea1, CD3DDevice_SetFogAndBlendMode
## mode 3; FOGENABLE in the technique's intrinsic flags @0x5d62f7).
func test_drape_carries_the_terrain_fog_uniforms() -> void:
	var environment := _environment()
	_camera()
	var shadow := _fresh_shadow(environment)
	shadow.advance_frame()
	var reference := ShaderMaterial.new()
	reference.shader = SlotShadow.get_drape_material().shader
	environment.apply_terrain_uniforms(reference)
	var drape := SlotShadow.get_drape_material()
	for name in ["u_fog_color", "u_fog_start", "u_fog_end", "u_fog_type"]:
		assert_eq(drape.get_shader_parameter(name), reference.get_shader_parameter(name),
				"%s matches the terrain's fog" % name)
	# The drape rung sits right after the viewmodel and before every world
	# rung (renderer::kRungSlotDrape: the terrain sector pass follows the
	# viewmodel, and its drapes precede every model and transparent submit).
	assert_eq(drape.render_priority, ObjectShaderCache.RENDER_RUNG_SLOT_DRAPE,
			"the drape draws on its own rung")
	assert_eq(ObjectShaderCache.RENDER_RUNG_SLOT_DRAPE, ObjectShaderCache.RENDER_RUNG_VIEWMODEL + 1,
			"right after the viewmodel")
	assert_gt(drape.render_priority, ObjectShaderCache.RENDER_RUNG_SKY_CLOUDS,
			"the sky pass draws before the drape")
	assert_lt(drape.render_priority, ObjectShaderCache.RENDER_RUNG_OBJECT_POST_MULTIPLY,
			"the drape precedes every world rung")


func _crate_slot_matrix(shadow: SlotShadow, crate: ObjectModel) -> Projection:
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(crate)
	assert_true(order >= 0, "the crate owns a capture order")
	return SlotShadow.get_drape_material().get_shader_parameter("u_slot_mat_%d" % maxi(order, 0))


## The capture camera is retail's look-at mapped into presentation axes
## (renderer::slot_capture_view_axes): a right-handed frame, so the device's
## back-face cull keeps the light-facing faces like retail's CULLMODE CCW over
## its view (RenderSlot_SetupShadowCascadeMatrices @0x58d31e). The drape samples through
## the same pose: its u row is the camera x axis and its v row the negated y, so
## u x v points along the camera forward, the downward slot direction. The
## mirrored (det -1) frame points it up.
func test_capture_view_is_right_handed_along_the_slot_direction() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3(0, 2, 10), Vector3.ZERO, Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var scope := Node3D.new()
	add_child_autofree(scope)
	var crate := _crate_caster(scope, Vector3.ZERO)
	var slot := _crate_slot_matrix(shadow, crate)
	var u_row := Vector3(slot.x.x, slot.y.x, slot.z.x)
	var v_row := Vector3(slot.x.y, slot.y.y, slot.z.y)
	assert_lt(u_row.cross(v_row).y, 0.0,
			"u x v looks down the slot direction (a right-handed capture view)")
	assert_almost_eq(u_row.y, 0.0, 0.0001, "the camera right row stays horizontal")
	crate.set_shadow_caster_enabled(false)
	shadow.advance_frame()


## Every slot quantity keys on the entity origin, never the render bounds
## (retail: Entity_RenderWithLODCallback @0x5d6fc4..0x5d6fd9 renders the entity at
## the view origin; the light query box @0x5d6af1..0x5d6b39): the crate's box
## rises 1 u above its origin, yet the origin projects to the capture centre.
func test_slot_view_centres_on_the_entity_origin() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3(0, 2, 10), Vector3.ZERO, Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var scope := Node3D.new()
	add_child_autofree(scope)
	var crate := _crate_caster(scope, Vector3(3.0, 0.0, -2.0))
	crate.advance_runtime_frame(1.0 / 62.0)
	assert_gt(crate.get_model_bounds().get_center().y, 0.25,
			"the crate's render bounds sit above its origin")
	var slot := _crate_slot_matrix(shadow, crate)
	var projected: Vector4 = slot * Vector4(3.0, 0.0, -2.0, 1.0)
	assert_almost_eq(projected.x, 0.5, 0.0001, "the origin lands on the capture centre (u)")
	assert_almost_eq(projected.y, 0.5, 0.0001, "the origin lands on the capture centre (v)")
	crate.set_shadow_caster_enabled(false)
	shadow.advance_frame()


## Retail's slot pass reads no render gate: a caster the blink or outdoors
## latch hides (the occlusion claim) keeps its slot, its capture and its drape,
## and its geometry still compiles into the black pass (retail:
## RenderSlot_SortAndAssign excludes only Flags & 1 @0x5d657d;
## RenderSlot_RenderEntityAndChildren @0x5d7690 has no visibility test). A node
## hidden for any other reason still leaves.
func test_occlusion_hidden_caster_keeps_its_slot() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3(0, 2, 10), Vector3.ZERO, Vector3.UP)
	var shadow := _fresh_shadow(environment)
	shadow.set_shadow_detail(4)  # mask 0: every slot compiles every frame
	var scope := Node3D.new()
	add_child_autofree(scope)
	var crate := _crate_caster(scope, Vector3.ZERO)
	crate.advance_runtime_frame(1.0 / 62.0)
	crate.set_occlusion_hidden(true)
	assert_false(crate.visible, "the occlusion claim hides the node")
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(crate)
	assert_true(order >= 0, "the occluded caster keeps its capture order")
	assert_gt(int(shadow.get_report()["slot_surfaces_compiled"]), 0,
			"the occluded caster's geometry still compiles into the black pass")
	var terms: PackedVector4Array = SlotShadow.get_drape_material().get_shader_parameter(
			"u_slot_term")
	assert_eq(terms[order].w, 1.0, "and its drape still publishes")
	crate.set_occlusion_hidden(false)
	crate.visible = false
	shadow.advance_frame()
	assert_eq(shadow.get_capture_order_of(crate), -1,
			"a node hidden outside the occlusion claim leaves its slot")
	crate.set_shadow_caster_enabled(false)
	shadow.advance_frame()


## The vehicle the local player rides shares the local player's halved
## priority (retail: RenderSlot_SortAndAssign @0x5d669c..0x5d66a6, >> 1
## @0x5d6864) and its every-frame refresh from detail 3 (retail:
## RenderSlot_RenderEntityAndChildren @0x5d7707..0x5d7734).
func test_local_vehicle_rides_the_local_player_priority_and_refresh() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	shadow.set_shadow_detail(3)
	var near := _caster_at(6.0)
	var vehicle := _caster_at(10.0)
	shadow.advance_frame()
	assert_eq(shadow.get_capture_order_of(near), 0, "the nearer caster ranks first on foot")
	shadow.set_local_player_parent_model(vehicle)
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(vehicle)
	assert_eq(order, 0, "the ridden vehicle's halved score ranks it first")
	for i in range(3):
		shadow.advance_frame()
		assert_ne(shadow.get_armed_capture_mask() & (1 << order), 0,
				"the ridden vehicle re-captures every frame at detail 3")
	shadow.set_local_player_parent_model(null)
	near.set_shadow_caster_enabled(false)
	vehicle.set_shadow_caster_enabled(false)
	shadow.advance_frame()


## The drape is its own patch mesh (retail: RenderSlot_RebuildPatchVertexBuffer
## @0x5d5130): (lod + 1)^2 vertices at 1 u spacing over the point-sampled
## terrain height (0 without a terrain here) plus the (lod + 1) * 0.004 u lift,
## rebuilt only when the patch origin or lod moves; a slot that stops draping
## hides its mesh.
func test_drape_patch_is_a_lifted_one_unit_mesh() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var caster := _caster_at(4.0)
	caster.set_shadow_bound_radii(2.0, 2.0625)
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(caster)
	assert_true(order >= 0, "the caster owns a capture order")
	var vertices := shadow.get_patch_vertices(order)
	var bounds := _patch_bounds(vertices)
	var lod := int(round(bounds.z - bounds.x))
	assert_eq(vertices.size(), (lod + 1) * (lod + 1), "(lod + 1)^2 vertices")
	if vertices.size() > lod + 1:
		assert_almost_eq(vertices[1].z - vertices[0].z, 1.0, 0.0001,
				"consecutive vertices step 1 u south")
		assert_almost_eq(vertices[lod + 1].x - vertices[0].x, 1.0, 0.0001,
				"each row steps 1 u east")
		for vertex in vertices:
			assert_almost_eq(vertex.y, (lod + 1) * 0.004, 0.00001,
					"every vertex rides the (lod + 1) * 0.004 lift over the terrain")
	# Past 80 u the slot drapes nothing: its mesh hides.
	caster.position = Vector3(0.0, 0.0, -85.0)
	shadow.advance_frame()
	assert_eq(shadow.get_patch_vertices(order).size(), 0,
			"a culled slot draws no patch")
	caster.set_shadow_caster_enabled(false)
	shadow.advance_frame()


## Retail draws every drape inside the terrain sector pass, which the indoors
## letter skips with the terrain, while the slot captures run earlier in the
## frame (renderer::kRungSlotDrape). OcclusionFrame closes this gate with the
## terrain's: the patches hide, the slot keeps its order and keeps capturing.
func test_a_closed_terrain_pass_draws_no_drape_but_keeps_capturing() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	shadow.set_shadow_detail(4)  # mask 0: every slot re-captures every frame
	var caster := _caster_at(4.0)
	caster.set_shadow_bound_radii(2.0, 2.0625)
	shadow.advance_frame()
	var order := shadow.get_capture_order_of(caster)
	assert_true(order >= 0, "the caster owns a capture order")
	assert_true(shadow.is_terrain_pass_drawn(), "the terrain pass starts open")
	assert_gt(shadow.get_patch_vertices(order).size(), 0, "the open pass drapes the slot")
	shadow.set_terrain_pass_drawn(false)
	assert_eq(shadow.get_patch_vertices(order).size(), 0,
			"closing the pass hides the drape at once")
	shadow.advance_frame()
	assert_eq(shadow.get_patch_vertices(order).size(), 0, "a closed pass draws no drape")
	assert_eq(shadow.get_capture_order_of(caster), order, "the slot keeps its order")
	assert_ne(shadow.get_armed_capture_mask() & (1 << order), 0,
			"the capture keeps running while the drapes are skipped")
	shadow.set_terrain_pass_drawn(true)
	shadow.advance_frame()
	assert_gt(shadow.get_patch_vertices(order).size(), 0, "reopening drapes the slot again")
	caster.set_shadow_caster_enabled(false)
	shadow.advance_frame()

## An attached-light slot lights its patch with D3D light 4 (retail:
## RenderSlot_DrawSilhouetteDrape @0x5d5e50..0x5d5f4c): D3DRS_AMBIENT white,
## a white material, and the Light_FillD3DPointLight fill (@0x5aa450: the
## params colour x 1.5, range = radius x 1.25, atten2 = 15 / range^2) with
## its diffuse rescaled to -(c + lum)(1 - fade). The drape publishes that
## light per slot for the shader's per-point N.L and attenuation, and leaves
## the slot's material term white; a sun slot publishes no light.
func test_attached_light_slot_publishes_the_d3d_point_light() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var director := EffectLightDirector.new()
	var light_position := Vector3(0.0, 2.0, -4.0)
	assert_gt(director.scene().spawn_model_light(ModelLightSpawn.make(light_position, 8.0)), 0)
	shadow.set_light_director(director)
	shadow.set_light_context(Vector3.ONE, 0, null)
	var caster := _caster_at(4.0)
	var term := _single_caster_term(shadow, caster)
	var order := shadow.get_capture_order_of(caster)
	var drape := SlotShadow.get_drape_material()
	var positions = drape.get_shader_parameter("u_slot_light_pos")
	var diffuses = drape.get_shader_parameter("u_slot_light_diffuse")
	assert_true(positions is PackedVector4Array and diffuses is PackedVector4Array,
			"the drape carries the per-slot attached light")
	if order >= 0 and positions is PackedVector4Array and diffuses is PackedVector4Array:
		var pos: Vector4 = positions[order]
		assert_almost_eq(Vector3(pos.x, pos.y, pos.z), light_position,
				Vector3(0.001, 0.001, 0.001), "the light's world position")
		assert_almost_eq(pos.w, 10.0, 0.001, "range = radius x 1.25")
		# White record bytes /256, x 1.5 on the D3D path; lum of a grey equals
		# the grey, so the rescaled diffuse is -2 x 1.5 x 255/256 at fade 0.
		var expected := -2.0 * 1.5 * 255.0 / 256.0
		var diffuse: Vector4 = diffuses[order]
		for channel in 3:
			assert_almost_eq(diffuse[channel], expected, 0.001,
					"channel %d: the rescaled D3D diffuse" % channel)
		assert_almost_eq(diffuse.w, 15.0 / 100.0, 0.0001, "atten2 = 15 / range^2")
		for channel in 3:
			assert_eq(term[channel], 1.0, "the attached slot's material term is white")
	# Without the light the same caster is a sun slot: no light published.
	shadow.set_light_director(null)
	term = _single_caster_term(shadow, caster)
	positions = drape.get_shader_parameter("u_slot_light_pos")
	if order >= 0 and positions is PackedVector4Array:
		assert_eq((positions[order] as Vector4).w, 0.0, "a sun slot publishes no light")
	assert_lt(term.x, 1.0, "the sun slot darkens through its material term")
	caster.set_shadow_caster_enabled(false)
	shadow.advance_frame()

