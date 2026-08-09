extends GutTest

# The per-model _process residual fix: clock-DERIVED render work (material
# generators, PANM transforms, lights, the env push) runs only while the model
# can render, while time-ACCUMULATING state (commanded part anims, the skeletal
# clip clock) advances regardless — a door commanded open while culled is open
# when next seen, and every skipped value re-derives from the absolute clock on
# the next visible frame. Retail computes these constants per SUBMITTED model
# only [orig: Terrain_RenderSectorModels @ 0x5c5d30].

const HOUSE_3DI := "res://../fixtures/threedi/3di3/House.3di"
const PMP_3DI := "res://../fixtures/3dp/Pmpjk01/Pmpjk01.3di"
const ARMRY_3DI := "res://../fixtures/3dp/armry01/Armry01.3di"
const SHED_3DI := "res://../fixtures/threedi/3di3/Shed.3di"
const ANIM_FIXTURES := "res://../fixtures/anim"


# The render-work gates are pinned through OBSERVABLES on real native models:
# the environment stamp lands on surface-material uniforms, and PANM transform
# derivation lands on the Robj part nodes — never through instrumentation
# overrides.
func _fresh_env_state(dir_color: Vector3) -> EnvLightState:
	var state := EnvLightState.new()
	var values := EnvLightValues.retail_noon_defaults()
	values.dir_color = dir_color
	state.publish(values)
	return state


func _first_material(model: ObjectModel) -> ShaderMaterial:
	var materials: Array = model.get_surface_materials()
	assert(materials.size() > 0)
	return materials[0] as ShaderMaterial


func _animated_part_node(model: ObjectModel) -> Node3D:
	# Part 0 is the fixture's static root; the PANM channels drive parts 1+.
	var parts: Dictionary = model.get_render_part_nodes()
	assert(parts.has(1))
	return parts[1] as Node3D


func _object_data(path: String) -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(path)), OK,
			"the committed object fixture opens: %s" % path)
	return data


# Spy model with a real document installed through the production mutator.
# Rebuild itself exercises the observed methods, so reset those setup calls
# before measuring an explicit runtime frame.
func _spy_model(path := HOUSE_3DI) -> ObjectModel:
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.set_object_data(_object_data(path))
	return model


func _loaded_skeletal() -> SkeletalAnim:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(ANIM_FIXTURES)), OK,
			"the committed animation fixture root mounts")
	var skeletal := SkeletalAnim.new()
	assert_true(skeletal.load_from_resource_root(root, "soldier.adm"),
			"soldier.adm loads: %s" % skeletal.get_last_error())
	return skeletal


func test_hidden_model_skips_render_work_but_advances_part_anims() -> void:
	var model := _spy_model()
	var material := _first_material(model)
	model.play_part_anim(1, 1, 1.0)
	model.visible = false

	var hidden_color := Vector3(0.9, 0.1, 0.1)
	model.set_environment_state(_fresh_env_state(hidden_color))
	var before: Vector3 = material.get_shader_parameter("u_dir_light_color")
	model.advance_runtime_frame(0.5)
	assert_eq(material.get_shader_parameter("u_dir_light_color"), before,
			"a hidden model pushes no environment state")
	assert_eq(int(model.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 31 * 1048,
			"the commanded part anim still advanced while hidden")

	model.visible = true
	model.advance_runtime_frame(0.5)
	assert_eq(material.get_shader_parameter("u_dir_light_color"), hidden_color,
			"render work resumes on the visible frame")
	assert_eq(int(model.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 62 * 1048,
			"two half-second render frames preserve retail's fixed 16 ms tick count")
	model.advance_runtime_frame(0.008)
	assert_eq(int(model.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 65536,
			"the 63rd retail tick strictly overshoots and clamps the sweep endpoint")


func test_hidden_fast_path_skips_the_env_push() -> void:
	var model := _spy_model()  # static house -> the fast path
	var material := _first_material(model)
	model.visible = false
	var fast_color := Vector3(0.2, 0.8, 0.3)
	model.set_environment_state(_fresh_env_state(fast_color))
	var before: Vector3 = material.get_shader_parameter("u_dir_light_color")
	model.advance_runtime_frame(0.016)
	assert_eq(material.get_shader_parameter("u_dir_light_color"), before,
			"hidden fast-path frames do nothing")
	model.visible = true
	model.advance_runtime_frame(0.016)
	assert_eq(material.get_shader_parameter("u_dir_light_color"), fast_color,
			"a visible fast-path frame keeps the env gate")


func test_live_panm_transforms_rederive_on_the_visible_frame() -> void:
	var model := _spy_model(PMP_3DI)
	var clock := PanmClock.new()
	clock.set_time_ms_for_test(0)
	model.set_panm_clock(clock)
	var part := _animated_part_node(model)
	var poison := Transform3D(Basis(), Vector3(123.0, 456.0, 789.0))
	model.visible = false
	part.transform = poison
	clock.set_time_ms_for_test(400)
	model.advance_runtime_frame(0.016)
	clock.set_time_ms_for_test(800)
	model.advance_runtime_frame(0.016)
	assert_eq(part.transform, poison, "no PANM evaluation while hidden")
	model.visible = true
	clock.set_time_ms_for_test(1200)
	model.advance_runtime_frame(0.016)
	assert_ne(part.transform, poison,
			"the visible frame re-derives transforms from the absolute clock")


func test_retail_runtime_does_not_submit_model_authored_lght() -> void:
	# Jointops loads LGHT into the model resource but has no gameplay read of
	# that field after load. Runtime models therefore keep the shader's
	# count-zero defaults even when the source file carries authored lights.
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	var data := _object_data(ARMRY_3DI)
	assert_gt(data.get_light_count(), 0, "the fixture carries object lights")
	model.set_object_data(data)
	var materials: Array = model.get_surface_materials()
	if materials.is_empty():
		pass_test("fixture built no surface materials under this renderer")
		return
	var material := materials[0] as ShaderMaterial
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 0,
			"gameplay parity leaves parsed LGHT disabled")
	model.advance_runtime_frame(0.0)
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 0,
			"runtime frames do not inject parsed LGHT")


func test_explicit_editor_preview_can_show_model_authored_lght() -> void:
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.position = Vector3(11.0, 13.0, 17.0)
	model.set_model_light_preview_enabled(true)
	var data := _object_data(ARMRY_3DI)
	for light_index in range(data.get_light_count()):
		assert_true(data.set_light_field(light_index, "subobject", -1))
		assert_true(data.set_light_field(light_index, "position", Vector3(4.0, 5.0, 6.0)))
	model.set_object_data(data)
	var materials: Array = model.get_surface_materials()
	if materials.is_empty():
		pass_test("fixture built no surface materials under this renderer")
		return
	var material := materials[0] as ShaderMaterial
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 1,
			"the opt-in object-editor preview can inspect authored LGHT")
	var lights: Array = data.evaluate_lights(0, {})
	var dominant: Dictionary = {}
	var best_intensity := -1.0
	for raw_light in lights:
		var light: Dictionary = raw_light
		var intensity := float(light.get("intensity", 1.0))
		if intensity > best_intensity:
			best_intensity = intensity
			dominant = light
	var model_position: Vector3 = dominant.get("position", Vector3.ZERO)
	var subobject := int(dominant.get("subobject", -1))
	var expected_world := model.global_transform * model_position
	var part_nodes: Dictionary = model.get_render_part_nodes()
	if subobject >= 0 and part_nodes.has(subobject):
		var rest := Transform3D.IDENTITY
		for raw_submesh in data.build_lod_submeshes(model.get_active_lod()):
			var submesh: Dictionary = raw_submesh
			if int(submesh.get("robj_index", -1)) == subobject:
				rest.origin = submesh.get("abs", Vector3.ZERO)
				break
		var part := part_nodes[subobject] as Node3D
		expected_world = part.global_transform * (rest.affine_inverse() * model_position)
	assert_eq(material.get_shader_parameter("u_local_light_position"), expected_world,
			"preview maps model-space LGHT through its attached part's live transform once")
	material.set_shader_parameter("u_local_light_count", 99)
	model.advance_runtime_frame(0.0)
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 99,
			"an identical preview evaluation pushes nothing")


func _mesh_instances_below(root: Node) -> Array[MeshInstance3D]:
	var out: Array[MeshInstance3D] = []
	for child in root.get_children():
		if child is MeshInstance3D:
			out.append(child as MeshInstance3D)
		out.append_array(_mesh_instances_below(child))
	return out


func test_world_model_shadow_casting_is_explicit_and_receiving_stays_enabled() -> void:
	# Retail's offscreen silhouette pass admits people and DynamicShadow items,
	# never every loaded model. A ObjectModel therefore starts receiver-only;
	# the item-aware placer explicitly opts eligible entities into casting.
	# [orig: Entity_InitFromModel @0x40E1BC..0x40E1F7]
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.set_object_data(_object_data(HOUSE_3DI))
	var meshes := _mesh_instances_below(model)
	assert_gt(meshes.size(), 0, "the fixture builds renderable mesh instances")
	for mesh in meshes:
		assert_eq(mesh.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
				"ordinary/static models cannot enter the retail dynamic-shadow pass")

	var materials: Array = model.get_surface_materials()
	assert_gt(materials.size(), 0, "the fixture builds object materials")
	for material in materials:
		var receiver := (material as ShaderMaterial).next_pass as ShaderMaterial
		assert_not_null(receiver,
				"world models receive eligible entity silhouettes in a separate pass")
		if receiver != null:
			assert_true(receiver.shader.code.contains("1.0 - ATTENUATION"),
					"the next pass consumes shadow attenuation only")

	model.set_shadow_caster_enabled(true)
	for mesh in meshes:
		assert_eq(mesh.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
				"an eligible entity explicitly enters the silhouette pass")
		assert_ne(mesh.layers & Water.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER, 0,
				"the dynamic light can select the caster independently of receivers")
		assert_eq(mesh.layers & Water.VISUAL_LAYER_STATIC_SHADOW_CASTER, 0)
	model.set_static_shadow_caster_enabled(true)
	for mesh in meshes:
		assert_ne(mesh.layers & Water.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER, 0,
				"an item can participate in both witnessed projection systems")
		assert_ne(mesh.layers & Water.VISUAL_LAYER_STATIC_SHADOW_CASTER, 0,
				"static terrain projection uses its own caster layer")
	model.set_static_shadow_caster_enabled(false)
	model.set_shadow_caster_enabled(false)
	for mesh in meshes:
		assert_eq(mesh.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
				"the policy remains live across an item/presentation change")
		assert_eq(mesh.layers & Water.VISUAL_LAYER_SHADOW_CASTER_MASK, 0)


func test_projected_shadow_receiver_rejects_incomplete_material_coverage() -> void:
	assert_true(ObjectModel.material_supports_projected_shadow_receiver(
			ObjectShaderCache.BLEND_OPAQUE, 0),
			"a one-sided opaque surface can use the simple attenuation catcher")
	assert_false(ObjectModel.material_supports_projected_shadow_receiver(
			ObjectShaderCache.BLEND_ALPHA, 0),
			"an alpha-blind next pass must not darken a transparent polygon")
	assert_false(ObjectModel.material_supports_projected_shadow_receiver(
			ObjectShaderCache.BLEND_OPAQUE,
			ObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST),
			"alpha-tested holes must not become a solid shadow card")
	assert_false(ObjectModel.material_supports_projected_shadow_receiver(
			ObjectShaderCache.BLEND_OPAQUE,
			ObjectShaderCache.MATERIAL_FLAG_TWO_SIDED),
			"the one-sided catcher cannot safely cover a two-sided base surface")


func test_hidden_skeletal_clock_advances_without_writing_bones() -> void:
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.set_skeletal_anim(_loaded_skeletal())
	model.set_object_data(_object_data(SHED_3DI))
	assert_true(model.has_skeleton(), "the rigid fixture fake-skins into a skeleton")
	model.play_body_clip("anim_walk_forward")
	model.set_playing(true)
	model.advance_runtime_frame(0.0)

	var skeleton := model.get_skeleton()
	var poison := Vector3(123.0, 456.0, 789.0)
	skeleton.set_bone_pose_position(0, poison)
	model.visible = false
	model.advance_runtime_frame(0.1)

	assert_true(skeleton.get_bone_pose_position(0).is_equal_approx(poison),
			"a hidden frame leaves the submitted pose untouched")
	assert_almost_eq(model.get_animation_time(), 0.1, 0.0001,
			"the clip clock accumulated while unposed")

	model.visible = true
	model.advance_runtime_frame(0.0)
	assert_false(skeleton.get_bone_pose_position(0).is_equal_approx(poison),
			"the next visible frame applies the pending pose")


# --- Event-driven runtime scheduling: models self-park while idle -----------
# A joiner streams the whole mission (800+ live NovaObjectModels); the fix
# stops idle models from paying a per-frame _process. Every mutator that can
# create per-frame work re-arms processing, and one runtime frame with no
# live work parks the model again. Placed mission/wire models always carry the
# shared PANM clock (mission_object_placer sets it on every model path);
# clockless playing models are the OED-preview carve-out and stay awake so
# their private age keeps accumulating. The staggered environment-restamp
# wake is live-verified (it depends on the wall frame counter); these tests
# pin the park/re-arm contract itself.

func _clocked_spy_model() -> ObjectModel:
	var model := _spy_model()
	var clock := PanmClock.new()
	clock.set_time_ms_for_test(0)
	model.set_panm_clock(clock)
	return model


func test_idle_clocked_model_parks_after_one_runtime_frame() -> void:
	var model := _clocked_spy_model()  # static house: no live per-frame work
	model.set_process(true)
	model.advance_runtime_frame(0.016)
	assert_false(model.is_processing(),
			"a shared-clock model with no live per-frame work parks itself")


func test_clockless_playing_model_stays_awake() -> void:
	# The OED-preview carve-out: no shared clock + playing means the private
	# age accumulates per frame, so the model must keep processing.
	var model := _spy_model()
	model.set_process(true)
	model.advance_runtime_frame(0.016)
	assert_true(model.is_processing(),
			"a clockless playing model keeps its private preview clock running")


func test_mutators_rearm_processing_and_park_when_drained() -> void:
	var model := _clocked_spy_model()
	model.advance_runtime_frame(0.016)
	assert_false(model.is_processing(), "baseline: parked while idle")

	model.set_ctrl_value("VEHICLE_SPECIAL1", 1024)
	assert_true(model.is_processing(), "a CTRL write re-arms the runtime frame")
	model.advance_runtime_frame(0.016)
	assert_false(model.is_processing(),
			"an inline-applied CTRL write leaves no pending work: parked again")

	model.play_part_anim(1, 1, 1.0)
	assert_true(model.is_processing(), "a commanded part anim re-arms")
	model.advance_runtime_frame(0.016)
	assert_true(model.is_processing(),
			"a live part-anim sweep is per-frame work: stays awake")


func test_visibility_edge_rearms_for_one_restamp_frame() -> void:
	var model := _clocked_spy_model()
	model.advance_runtime_frame(0.016)
	assert_false(model.is_processing(), "baseline: parked while idle")
	model.visible = false
	assert_true(model.is_processing(),
			"a visibility edge re-arms the env-restamp check")
	model.advance_runtime_frame(0.016)
	assert_false(model.is_processing(), "a hidden idle model parks again")
	model.visible = true
	assert_true(model.is_processing(),
			"re-shown models re-check the env generation missed while hidden")
	model.advance_runtime_frame(0.016)
	assert_false(model.is_processing(), "and park once the restamp is done")


# --- Camera-submission gate: retail computes per SUBMITTED model ------------
# [orig: Terrain_RenderSectorModels @0x5c5d30]. set_on_screen is the public
# seam the bounds notifier's screen_entered/exited signals drive; headless
# contexts never fire the notifier, so the flag defaults on and these tests
# exercise the gate through the same seam.

func test_model_carries_a_submission_notifier_sized_to_its_bounds() -> void:
	var model := _clocked_spy_model()
	var notifier := model.get_node_or_null("ScreenNotifier")
	assert_not_null(notifier, "a built model carries its submission notifier")
	assert_gt((notifier as VisibleOnScreenNotifier3D).aabb.size.length(), 0.0,
			"the notifier AABB covers the mesh bounds")


func test_mirror_eligibility_selects_the_base_visual_layer() -> void:
	# env #30: only vehicles enter the water mirror; every other model rides
	# the no-mirror world layer normal cameras still draw [orig:
	# Entity_InitFromModel @ 0x40e20a entity+36 |= 0x400 iff
	# ItemDefType(+0x5C)==1; Terrain_CollectVisibleEntitiesForReflection
	# @ 0x5c90a0 filters the reflection's collection on it].
	var plain := _spy_model()
	var plain_instances := plain.find_children("*", "MeshInstance3D", true, false)
	assert_gt(plain_instances.size(), 0, "the fixture model builds mesh instances")
	for vi in plain_instances:
		assert_ne((vi as MeshInstance3D).layers
				& Water.VISUAL_LAYER_WORLD_NO_MIRROR, 0,
				"a non-vehicle model rides the no-mirror world layer")
		assert_eq((vi as MeshInstance3D).layers & Water.VISUAL_LAYER_WORLD, 0,
				"a non-vehicle model leaves the mirror-visible layer")

	var vehicle := ObjectModel.new()
	add_child_autofree(vehicle)
	vehicle.set_process(false)
	vehicle.mirror_reflected = true
	vehicle.set_object_data(_object_data(HOUSE_3DI))
	for vi in vehicle.find_children("*", "MeshInstance3D", true, false):
		assert_ne((vi as MeshInstance3D).layers & Water.VISUAL_LAYER_WORLD, 0,
				"a vehicle model stays on the mirror-visible world layer")
		assert_eq((vi as MeshInstance3D).layers
				& Water.VISUAL_LAYER_WORLD_NO_MIRROR, 0,
				"a vehicle model never rides the no-mirror layer")


func test_submission_registry_tracks_offscreen_edges_and_frees_cleanly() -> void:
	var model := _clocked_spy_model()
	var registry := {}
	model.set_submission_registry(registry)
	assert_false(registry.has(model.get_instance_id()),
			"the on-screen default publishes no off-screen claim")
	model.set_on_screen(false)
	assert_true(registry.has(model.get_instance_id()),
			"leaving the camera publishes this model's off-screen claim")
	model.set_on_screen(true)
	assert_false(registry.has(model.get_instance_id()),
			"re-entering the camera withdraws the claim")

	model.set_on_screen(false)
	var replacement := {}
	model.set_submission_registry(replacement)
	assert_false(registry.has(model.get_instance_id()),
			"rebinding erases the claim from the previous registry")
	assert_true(replacement.has(model.get_instance_id()),
			"rebinding republishes the current state into the new registry")

	var id := model.get_instance_id()
	model.free()
	assert_false(replacement.has(id),
			"a freed model leaves no stale off-screen claim behind")


func test_off_screen_model_advances_clocks_but_skips_render_derives() -> void:
	var model := _spy_model(PMP_3DI)  # live PANM: always has runtime work
	var clock := PanmClock.new()
	clock.set_time_ms_for_test(0)
	model.set_panm_clock(clock)
	var part := _animated_part_node(model)
	var material := _first_material(model)
	var offscreen_color := Vector3(0.1, 0.2, 0.9)
	model.set_environment_state(_fresh_env_state(offscreen_color))
	model.set_on_screen(false)
	var poison := Transform3D(Basis(), Vector3(123.0, 456.0, 789.0))
	part.transform = poison
	var before: Vector3 = material.get_shader_parameter("u_dir_light_color")
	model.play_part_anim(1, 1, 1.0)

	model.advance_runtime_frame(0.5)
	assert_eq(material.get_shader_parameter("u_dir_light_color"), before,
			"an off-camera model pushes no environment state")
	assert_eq(part.transform, poison, "no PANM evaluation while off camera")
	assert_eq(int(model.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 31 * 1048,
			"the commanded part anim still advanced while off camera")

	model.set_on_screen(true)
	assert_true(model.is_processing(),
			"re-entering the screen wakes the model for the catch-up frame")
	clock.set_time_ms_for_test(1200)
	model.advance_runtime_frame(0.5)
	assert_ne(part.transform, poison,
			"the submitted frame re-derives transforms from the absolute clock")
	assert_eq(material.get_shader_parameter("u_dir_light_color"), offscreen_color,
			"and catches up the environment restamp")
