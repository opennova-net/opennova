extends GutTest

## The render-slot entity ground-shadow device (SlotShadow): the 24-patch /
## 12-capture admission, the per-slot capture layer channels, and the local
## player's first-person drape gates. The planning laws themselves are pinned
## portable (tests/renderer/render_slot_shadow_test.cpp); this suite pins the
## device wiring (docs/render/render-lighting-re.md, the render-slot side).


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
	assert_true(model.is_in_group("nova_slot_shadow_casters"),
			"an enabled dynamic caster registers for slot planning")
	model.set_shadow_caster_enabled(false)
	assert_false(model.is_in_group("nova_slot_shadow_casters"),
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


func test_admitted_casters_carry_exactly_one_capture_channel() -> void:
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := _fresh_shadow(environment)
	var near_model := _caster_at(5.0)
	var mesh := near_model.get_child(0) as VisualInstance3D
	var before := mesh.layers
	shadow.advance_frame()
	var capture_bits: int = mesh.layers & Water.VISUAL_LAYER_SLOT_CAPTURE_MASK
	assert_ne(capture_bits, 0, "an admitted caster gains its capture channel")
	assert_eq(capture_bits & (capture_bits - 1), 0,
			"exactly one per-slot channel bit is assigned")
	assert_eq(mesh.layers & ~Water.VISUAL_LAYER_SLOT_CAPTURE_MASK,
			before & ~Water.VISUAL_LAYER_SLOT_CAPTURE_MASK,
			"the stamp never disturbs the base/caster layers")
	# Releasing the caster clears the channel on the next plan.
	near_model.set_shadow_caster_enabled(false)
	shadow.advance_frame()
	assert_eq(mesh.layers & Water.VISUAL_LAYER_SLOT_CAPTURE_MASK, 0,
			"a released caster loses its capture channel")


func _child_shares_parent_channel(parent_first: bool) -> void:
	# The retail child walk renders a capture-with child (held weapon, mounted
	# child) into its PARENT's slot RT (RenderSlot_RenderEntityAndChildren
	# @0x5d78ef..0x5d79d6); the child's own slot is excluded. Registration
	# order must not matter: the child's own (excluded) row once cleared the
	# bit the parent's row had just stamped.
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
	var parent_mesh := parent.get_child(0) as VisualInstance3D
	var child_mesh := child.get_child(0) as VisualInstance3D
	var parent_bits: int = parent_mesh.layers & Water.VISUAL_LAYER_SLOT_CAPTURE_MASK
	var order := "parent first" if parent_first else "child first"
	assert_ne(parent_bits, 0, "the parent captures (%s)" % order)
	assert_eq(child_mesh.layers & Water.VISUAL_LAYER_SLOT_CAPTURE_MASK, parent_bits,
			"the linked child renders into its parent's slot RT (%s)" % order)
	assert_eq(int(shadow.get_report()["captures"]), 1,
			"the child's own slot is excluded (%s)" % order)


func test_capture_with_child_rides_its_parent_slot_parent_registered_first() -> void:
	_child_shares_parent_channel(true)


func test_capture_with_child_rides_its_parent_slot_child_registered_first() -> void:
	_child_shares_parent_channel(false)


func test_capture_with_child_refused_by_the_full_table_still_rides_its_parent() -> void:
	# The fixed 256-record table refuses the 257th registration
	# (RenderSlot_AllocSlot @0x5d5690), so a linked child past the cap has no
	# assignment row of its own; the child walk follows the entity hierarchy,
	# not the table, so it still captures into its parent's RT — and gives
	# the channel back once the link drops, even while it stays row-less.
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
	var parent_mesh := parent.get_child(0) as VisualInstance3D
	var child_mesh := child.get_child(0) as VisualInstance3D
	var parent_bits: int = parent_mesh.layers & Water.VISUAL_LAYER_SLOT_CAPTURE_MASK
	assert_ne(parent_bits, 0, "the nearest caster captures")
	assert_eq(child_mesh.layers & Water.VISUAL_LAYER_SLOT_CAPTURE_MASK, parent_bits,
			"a row-less linked child still renders into its parent's slot RT")
	child.set_slot_shadow_capture_with(null)
	shadow.advance_frame()
	assert_eq(child_mesh.layers & Water.VISUAL_LAYER_SLOT_CAPTURE_MASK, 0,
			"dropping the link clears the inherited channel on the next plan")


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
