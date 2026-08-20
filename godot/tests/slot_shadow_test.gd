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
	var shadow := SlotShadow.new()
	add_child_autofree(shadow)
	shadow.set_environment_node(environment)
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
	var shadow := SlotShadow.new()
	add_child_autofree(shadow)
	shadow.set_environment_node(environment)
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


func test_local_player_first_person_drape_gates() -> void:
	# Retail skips the local player's own drape in first person while
	# prone-latched or below shadow detail 2
	# (RenderSlot_DrawAllDrapes @0x5d6e70..0x5d6e90).
	var environment := _environment()
	var camera := _camera()
	camera.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -10), Vector3.UP)
	var shadow := SlotShadow.new()
	add_child_autofree(shadow)
	shadow.set_environment_node(environment)
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


func test_terrain_material_chains_the_shared_drape_passes() -> void:
	var drape: ShaderMaterial = SlotShadow.get_drape_material()
	assert_not_null(drape, "the shared drape material exists")
	assert_not_null(drape.next_pass,
			"the authored-blob pass chains behind the silhouette pass")
	assert_true(drape.shader.resource_path.ends_with(
			"slot_shadow_drape.gdshader"))
