extends GutTest

# The per-model _process residual fix: clock-DERIVED render work (material
# generators, PANM transforms, lights) runs only while the model
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
# PANM transform derivation lands on the Robj part nodes and the per-entity
# lighting factors land on instance uniforms, never through instrumentation
# overrides.
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


func test_hidden_model_advances_commanded_part_anims() -> void:
	var model := _spy_model()
	model.play_part_anim(1, 1, 1.0)
	model.visible = false

	model.advance_runtime_frame(0.5)
	assert_eq(int(model.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 31 * 1048,
			"the commanded part anim still advanced while hidden")

	model.visible = true
	model.advance_runtime_frame(0.5)
	assert_eq(int(model.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 62 * 1048,
			"two half-second render frames preserve retail's fixed 16 ms tick count")
	model.advance_runtime_frame(0.008)
	assert_eq(int(model.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 65536,
			"the 63rd retail tick strictly overshoots and clamps the sweep endpoint")


# The per-entity lighting factors are stamped on the direct GeometryInstance3D
# children of every ROBJ part node and of the model's own Skeleton3D (skinned
# submeshes bind there instead of under a part).
func _entity_light_instances(model: ObjectModel) -> Array[GeometryInstance3D]:
	var parents: Array[Node] = []
	var parts: Dictionary = model.get_render_part_nodes()
	for key in parts.keys():
		parents.append(parts[key] as Node3D)
	if model.has_skeleton():
		parents.append(model.get_skeleton())
	var out: Array[GeometryInstance3D] = []
	for parent in parents:
		for child in parent.get_children():
			var instance := child as GeometryInstance3D
			if instance != null:
				out.append(instance)
	return out


func _assert_entity_light(model: ObjectModel, expected: Vector4,
		message: String) -> void:
	var instances := _entity_light_instances(model)
	assert_gt(instances.size(), 0, message + " (at least one surface instance)")
	for instance in instances:
		var actual: Variant = instance.get_instance_shader_parameter("u_entity_light")
		assert_true(actual is Vector4 and (actual as Vector4).is_equal_approx(expected),
				"%s: %s carries %s, expected %s" % [
						message, instance.name, actual, expected])


func test_entity_lighting_context_stamps_every_surface_instance() -> void:
	# Retail pushes the per-entry factors (effectScale, the interior-parented
	# flag, the parent interior's daylight t) per batch entry on top of the
	# pass-global lighting block. Here they are ONE instance uniform, written
	# on every surface instance at each context edge, never per frame, and
	# instances recreated by a rebuild come up already stamped.
	# [orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]
	var expected := Vector4(0.25, 1.0, 0.4, 0.0)
	var rigid := _spy_model(PMP_3DI)
	rigid.set_entity_lighting_context(0.25, true, 0.4)
	_assert_entity_light(rigid, expected,
			"every rigid part instance carries the entity lighting factors")
	rigid.set_entity_lighting_context(0.25, true, 0.4)
	_assert_entity_light(rigid, expected,
			"an identical context is a no-op that leaves the stamp in place")
	rigid.rebuild()
	_assert_entity_light(rigid, expected,
			"instances recreated by a rebuild come up stamped")

	var skinned := ObjectModel.new()
	add_child_autofree(skinned)
	skinned.set_process(false)
	skinned.set_skeletal_anim(_loaded_skeletal())
	skinned.set_object_data(_object_data(SHED_3DI))
	assert_true(skinned.has_skeleton(), "the rigid fixture fake-skins into a skeleton")
	skinned.set_entity_lighting_context(0.25, true, 0.4)
	_assert_entity_light(skinned, expected,
			"skinned submeshes bound to the Skeleton3D carry the same factors")


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


func test_gameplay_keeps_the_editor_local_lght_uniforms_disabled() -> void:
	# Gameplay consumes authored LGHT through the shared EffectWorld pool. The
	# ObjectModel-local preview path must stay disabled or the same authored
	# light would be submitted twice through independent shader inputs.
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
			"gameplay does not duplicate EffectWorld LGHT through preview uniforms")
	model.advance_runtime_frame(0.0)
	assert_eq(int(material.get_shader_parameter("u_local_light_count")), 0,
			"runtime frames keep the local duplicate-light route disabled")


func test_dynamic_material_typed_runtime_matches_public_evaluator() -> void:
	# Armry material 3 is an authored FLICKER-controlled RGB generator. The
	# runtime model now keeps the native result typed through the ShaderMaterial
	# write; the public Dictionary evaluator remains the independent tooling
	# boundary used as the exact-value oracle here.
	var data := _object_data(ARMRY_3DI)
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.set_object_data(data)
	var material: ShaderMaterial = null
	var indices := model.get_surface_material_indices()
	var materials := model.get_surface_materials()
	for index in range(indices.size()):
		if int(indices[index]) == 3:
			material = materials[index] as ShaderMaterial
			break
	assert_not_null(material, "the armory fixture submits dynamic material 3")
	if material == null:
		return
	for flicker in [0x2000, 0xC000]:
		model.set_ctrl_value("FLICKER", flicker)
		var expected: Dictionary = data.eval_material_runtime(
				3, 0, {"FLICKER": flicker})
		assert_eq(material.get_shader_parameter("u_uv_transform_u"),
				expected.get("uv_transform_u"))
		assert_eq(material.get_shader_parameter("u_uv_transform_v"),
				expected.get("uv_transform_v"))
		assert_eq(material.get_shader_parameter("u_rgb_mod"),
				expected.get("rgb_mod"),
				"typed hot path preserves the controlled RGB result")
		assert_eq(material.get_shader_parameter("u_alpha_mod"),
				expected.get("alpha_mod"))


func _mesh_instances_below(root: Node) -> Array[MeshInstance3D]:
	var out: Array[MeshInstance3D] = []
	for child in root.get_children():
		if child is MeshInstance3D:
			out.append(child as MeshInstance3D)
		out.append_array(_mesh_instances_below(child))
	return out


func _transparent_mesh_instances(root: Node) -> Array[MeshInstance3D]:
	var out: Array[MeshInstance3D] = []
	for mesh in _mesh_instances_below(root):
		var material := mesh.material_override as ShaderMaterial
		if material == null or material.shader == null:
			continue
		var path := material.shader.resource_path
		if "/alpha" in path or "/additive" in path or "/multiplicative" in path:
			out.append(mesh)
	return out


func _mesh_world_center(mesh: MeshInstance3D) -> Vector3:
	return mesh.global_transform * mesh.mesh.get_aabb().get_center()


func test_transparent_strips_bin_independently_on_both_camera_sides() -> void:
	# The native importer preserves each 3DI strip as one MeshInstance3D. This
	# fixture has transparent strips spread across the armory; rotate its widest
	# center axis vertical, put the water plane through the spread, and prove
	# each strip receives its own live Q1/Q2 material priority. A shared material
	# or object-origin classifier cannot satisfy these assertions.
	var model := _spy_model(ARMRY_3DI)
	var transparent := _transparent_mesh_instances(model)
	assert_gt(transparent.size(), 1, "the armory fixture carries multiple alpha strips")
	if transparent.size() < 2:
		return

	var minimum := _mesh_world_center(transparent[0])
	var maximum := minimum
	for mesh in transparent:
		var center := _mesh_world_center(mesh)
		minimum = minimum.min(center)
		maximum = maximum.max(center)
	var span := maximum - minimum
	if span.x >= span.y and span.x >= span.z:
		model.basis = Basis(Vector3.FORWARD, PI * 0.5)
	elif span.z >= span.y:
		model.basis = Basis(Vector3.RIGHT, -PI * 0.5)

	var low := INF
	var high := -INF
	for mesh in transparent:
		var height := _mesh_world_center(mesh).y
		low = minf(low, height)
		high = maxf(high, height)
	assert_gt(high - low, 0.01, "transparent strip centers span the water plane")
	if high - low <= 0.01:
		return

	var cache := ObjectShaderCache.get_singleton()
	var water_height := (low + high) * 0.5
	cache.set_water_plane(water_height, true)
	model.advance_runtime_frame(0.0)
	var material_ids := {}
	var above_count := 0
	var below_count := 0
	for mesh in transparent:
		var material := mesh.material_override as ShaderMaterial
		material_ids[material.get_instance_id()] = true
		var height := _mesh_world_center(mesh).y
		above_count += 1 if height >= water_height else 0
		below_count += 1 if height < water_height else 0
		assert_eq(material.render_priority, cache.alpha_rung_for_height(height),
				"camera-above priority follows this strip's transformed center")
	assert_eq(material_ids.size(), transparent.size(),
			"each transparent strip owns the priority-bearing material")
	assert_gt(above_count, 0)
	assert_gt(below_count, 0)

	cache.set_water_plane(water_height, false)
	model.advance_runtime_frame(0.0)
	for mesh in transparent:
		var height := _mesh_world_center(mesh).y
		var material := mesh.material_override as ShaderMaterial
		assert_eq(material.render_priority, cache.alpha_rung_for_height(height),
				"underwater camera mirrors the far/camera-side ladder")

	# Moving the model across the plane re-classifies without a water change:
	# the transform notification lands with the frame and re-runs the
	# classifier in place (no runtime-walk wake).
	var lowest_mesh: MeshInstance3D = transparent[0]
	for mesh in transparent:
		if _mesh_world_center(mesh).y < _mesh_world_center(lowest_mesh).y:
			lowest_mesh = mesh
	var lift := water_height - _mesh_world_center(lowest_mesh).y + 0.5
	model.global_position += Vector3(0.0, lift, 0.0)
	await get_tree().process_frame
	for mesh in transparent:
		var material := mesh.material_override as ShaderMaterial
		assert_eq(material.render_priority,
				cache.alpha_rung_for_height(_mesh_world_center(mesh).y),
				"a moved model re-classifies its strips from the new centers")
	cache.clear_water_plane()


func test_classified_alpha_strips_do_not_keep_a_still_model_awake() -> void:
	# The classifier is change-driven: once the plane and the model are still,
	# a shared-clock model whose only runtime work was its strips parks again
	# (retail recomputes per frame inside a walk it already runs; the result
	# is the same, the per-frame server round trips are not). Any fixture
	# with alpha strips and no live per-frame work serves; the static house
	# is the parking fixture the clock tests already use.
	var cache := ObjectShaderCache.get_singleton()
	cache.clear_water_plane()
	var model: ObjectModel = null
	var transparent: Array = []
	for path in [SHED_3DI, HOUSE_3DI, ARMRY_3DI]:
		var candidate := _spy_model(path)
		var clock := PanmClock.new()
		clock.set_time_ms_for_test(0)
		candidate.set_panm_clock(clock)
		candidate.advance_runtime_frame(0.016)
		var strips := _transparent_mesh_instances(candidate)
		# A fixture that parks with no water plane and owns alpha strips.
		if not candidate.is_runtime_frame_awake() and not strips.is_empty():
			model = candidate
			transparent = strips
			break
	if model == null:
		pending("no committed fixture both parks and carries alpha strips")
		return
	cache.set_water_plane(_mesh_world_center(transparent[0]).y, true)
	assert_true(model.is_runtime_frame_awake(),
			"a water plane change wakes every model with alpha strips once")
	model.advance_runtime_frame(0.016)
	assert_false(model.is_runtime_frame_awake(),
			"a still model with classified alpha strips parks after one frame")
	for mesh in transparent:
		var material := mesh.material_override as ShaderMaterial
		assert_eq(material.render_priority,
				cache.alpha_rung_for_height(_mesh_world_center(mesh).y))
	# Re-publishing the same plane is not a change and wakes nothing.
	cache.set_water_plane(_mesh_world_center(transparent[0]).y, true)
	assert_false(model.is_runtime_frame_awake(),
			"an unchanged water plane does not wake parked models")
	cache.clear_water_plane()


func test_bone_path_transparents_follow_the_entity_side_selector() -> void:
	# The bone collector does not derive a deformed center for every strip. Its
	# caller passes bit 0x20 when the ENTITY belongs below water, so every alpha
	# strip of one skeletal submission joins the same Q1/Q2 queue.
	var model := _spy_model(ARMRY_3DI)
	model.set_skeletal_anim(_loaded_skeletal())
	var transparent := _transparent_mesh_instances(model)
	assert_gt(transparent.size(), 1, "skeletal armory retains alpha strips")
	if transparent.size() < 2:
		return

	# Pick a plane between the entity origin and the most vertically displaced
	# strip. A rigid per-strip classifier would put that strip on the other side;
	# the retail bone path must keep every strip with the entity.
	var entity_height := model.global_position.y
	var displaced_height := entity_height
	for mesh in transparent:
		var height := _mesh_world_center(mesh).y
		if absf(height - entity_height) > absf(displaced_height - entity_height):
			displaced_height = height
	assert_gt(absf(displaced_height - entity_height), 0.01,
			"fixture has a strip center away from its skeletal entity origin")
	if absf(displaced_height - entity_height) <= 0.01:
		return

	var cache := ObjectShaderCache.get_singleton()
	var water_height := (entity_height + displaced_height) * 0.5
	cache.set_water_plane(water_height, true)
	model.advance_runtime_frame(0.0)
	var expected := cache.alpha_rung_for_height(entity_height)
	assert_ne(cache.alpha_rung_for_height(displaced_height), expected,
			"the selected strip center is deliberately across the plane")
	for mesh in transparent:
		var material := mesh.material_override as ShaderMaterial
		assert_eq(material.render_priority, expected,
				"bone-path alpha follows the entity-side submit selector")
	cache.clear_water_plane()


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
		# Entity ground shadows drape TERRAIN ONLY: retail's render-slot
		# patches are terrain-following meshes, so a live silhouette never
		# lands on another model (RenderSlot_DrawAllDrapes @0x5d6e20 /
		# render_sector_model @0x5d5ca0 — docs/render/render-lighting-re.md).
		# The drape next pass lives on the terrain material (SlotShadow).
		assert_null((material as ShaderMaterial).next_pass,
				"world-model materials carry no shadow-receiver next pass")

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
# their private age keeps accumulating. These tests pin the park/re-arm
# contract itself.

func _clocked_spy_model() -> ObjectModel:
	var model := _spy_model()
	var clock := PanmClock.new()
	clock.set_time_ms_for_test(0)
	model.set_panm_clock(clock)
	return model


func test_idle_clocked_model_parks_after_one_runtime_frame() -> void:
	var model := _clocked_spy_model()  # static house: no live per-frame work
	model.wake_runtime_frame()
	model.advance_runtime_frame(0.016)
	assert_false(model.is_runtime_frame_awake(),
			"a shared-clock model with no live per-frame work parks itself")


func test_clockless_playing_model_stays_awake() -> void:
	# The OED-preview carve-out: no shared clock + playing means the private
	# age accumulates per frame, so the model must keep processing.
	var model := _spy_model()
	model.wake_runtime_frame()
	model.advance_runtime_frame(0.016)
	assert_true(model.is_runtime_frame_awake(),
			"a clockless playing model keeps its private preview clock running")


func test_mutators_rearm_processing_and_park_when_drained() -> void:
	var model := _clocked_spy_model()
	model.advance_runtime_frame(0.016)
	assert_false(model.is_runtime_frame_awake(), "baseline: parked while idle")

	model.set_ctrl_value("VEHICLE_SPECIAL1", 1024)
	assert_true(model.is_runtime_frame_awake(), "a CTRL write re-arms the runtime frame")
	model.advance_runtime_frame(0.016)
	assert_false(model.is_runtime_frame_awake(),
			"an inline-applied CTRL write leaves no pending work: parked again")

	model.play_part_anim(1, 1, 1.0)
	assert_true(model.is_runtime_frame_awake(), "a commanded part anim re-arms")
	model.advance_runtime_frame(0.016)
	assert_true(model.is_runtime_frame_awake(),
			"a live part-anim sweep is per-frame work: stays awake")


func test_visibility_edge_rearms_for_one_runtime_frame() -> void:
	var model := _clocked_spy_model()
	model.advance_runtime_frame(0.016)
	assert_false(model.is_runtime_frame_awake(), "baseline: parked while idle")
	model.visible = false
	assert_true(model.is_runtime_frame_awake(),
			"a visibility edge re-arms the runtime frame")
	model.advance_runtime_frame(0.016)
	assert_false(model.is_runtime_frame_awake(), "a hidden idle model parks again")
	model.visible = true
	assert_true(model.is_runtime_frame_awake(),
			"re-shown models wake for one catch-up frame")
	model.advance_runtime_frame(0.016)
	assert_false(model.is_runtime_frame_awake(), "and park once that frame is done")


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


func test_on_screen_edges_are_readable_by_the_present_walk() -> void:
	var model := _clocked_spy_model()
	assert_true(model.is_on_screen(),
			"the on-screen default reports submitted")
	model.set_on_screen(false)
	assert_false(model.is_on_screen(),
			"leaving the camera reports the off-screen edge the walk gates on")
	model.set_on_screen(true)
	assert_true(model.is_on_screen(),
			"re-entering the camera reports submitted again")


func test_off_screen_model_advances_clocks_but_skips_render_derives() -> void:
	var model := _spy_model(PMP_3DI)  # live PANM: always has runtime work
	var clock := PanmClock.new()
	clock.set_time_ms_for_test(0)
	model.set_panm_clock(clock)
	var part := _animated_part_node(model)
	model.set_on_screen(false)
	var poison := Transform3D(Basis(), Vector3(123.0, 456.0, 789.0))
	part.transform = poison
	model.play_part_anim(1, 1, 1.0)

	model.advance_runtime_frame(0.5)
	assert_eq(part.transform, poison, "no PANM evaluation while off camera")
	assert_eq(int(model.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 31 * 1048,
			"the commanded part anim still advanced while off camera")

	model.set_on_screen(true)
	assert_true(model.is_runtime_frame_awake(),
			"re-entering the screen wakes the model for the catch-up frame")
	clock.set_time_ms_for_test(1200)
	model.advance_runtime_frame(0.5)
	assert_ne(part.transform, poison,
			"the submitted frame re-derives transforms from the absolute clock")
