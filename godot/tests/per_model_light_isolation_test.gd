extends GutTest

## Per-draw owner light isolation (the D-RLIT-4 per-model close): each visible
## ObjectModel is its own draw context — owned pool lights reach ONLY their
## owner entity's draws, world lights reach every nearby draw, and the
## selected three land as per-instance shader parameters
## [orig: the per-draw collect @ 0x5aa250 feeding update_light_slots
## @ 0x5abc50 with that draw's owner group].

const PMP_3DI := "res://../fixtures/threedi/synth/pump.3di"
const SHED_3DI := "res://../fixtures/threedi/synth/shed.3di"
# shed with its one LGHT authored onto subobject 2 (origin, 100-wu radius),
# minted once from the retired edit surface (fixtures/README.md).
const SYN_SHED_LGHT0_SUB2 := "res://../fixtures/threedi/synth/shed_lght0_sub2_origin_atten100.3di"


func _fixture_object_data(model: String) -> ObjectData:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth")), OK)
	var data := ObjectData.new()
	assert_eq(data.open_from_resource_root(root, model), OK,
			"%s loads as a per-model light fixture" % model)
	return data


func _placed_model(parent: Node, position: Vector3) -> ObjectModel:
	var model := ObjectModel.new()
	parent.add_child(model)
	model.position = position
	model.set_object_data(_fixture_object_data("house.3di"))
	return model


func _surface_instance(model: ObjectModel) -> GeometryInstance3D:
	var instances := model.find_children("*", "GeometryInstance3D", true, false)
	assert_gt(instances.size(), 0, "the built model exposes surface instances")
	if instances.is_empty():
		return null
	return instances[0] as GeometryInstance3D


func _multi_part_model(parent: Node) -> ObjectModel:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(PMP_3DI)), OK,
			"the five-ROBJ building fixture loads")
	var model := ObjectModel.new()
	parent.add_child(model)
	model.set_object_data(data)
	return model


func _part_surface(model: ObjectModel, robj_index: int) -> GeometryInstance3D:
	var parts: Dictionary = model.get_render_part_nodes()
	var part := parts.get(robj_index) as Node3D
	if part == null:
		return null
	for child in part.get_children():
		if child is GeometryInstance3D:
			return child as GeometryInstance3D
	return null


func _static_light_atlas(scene: LightScene) -> Image:
	var image := scene.get_static_light_rows_image()
	assert_not_null(image,
			"the static selector retains a read-only snapshot of the uploaded atlas")
	return image


func test_owned_light_reaches_only_its_owner_model() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	# The world creates MissionObjects during mission load; a bare scene test
	# provides the same container shape itself.
	var container: Node = world.get_node_or_null("MissionObjects")
	if container == null:
		container = Node3D.new()
		container.name = "MissionObjects"
		world.add_child(container)
	assert_not_null(container)
	var owner_model := _placed_model(container, Vector3(0.0, 0.0, 0.0))
	owner_model.set_meta("entity_ref", {"wire_handle": 77})
	var bystander := _placed_model(container, Vector3(4.0, 0.0, 0.0))
	assert_eq(EffectLightDirector.owner_id_for_node(owner_model),
			EffectLightDirector.owner_id_for_wire(77),
			"a wire-stamped model owns its tagged wire identity")
	assert_eq(EffectLightDirector.owner_id_for_node(bystander),
			bystander.get_instance_id(),
			"an unstamped model falls back to its instance id")

	var director := EffectLightDirector.new()
	director.setup(world, Callable(), Callable())
	# One world light between the models (an unowned authored record), one
	# light owned by entity 77 (the muzzle-glow shape).
	var world_light := director.spawn_light_record({
		"position": Vector3(2.0, 1.0, 0.0),
		"atten_end": 8.0,
	}, Transform3D.IDENTITY)
	assert_gt(world_light, 0)
	director.on_muzzle_fire(77, Vector3(0.0, 1.0, 0.0))
	var camera := Camera3D.new()
	world.add_child(camera)
	camera.position = Vector3(2.0, 1.0, 6.0)
	director.render_frame(camera)

	var owner_surface := _surface_instance(owner_model)
	var bystander_surface := _surface_instance(bystander)
	if owner_surface == null or bystander_surface == null:
		return
	assert_eq(
			float(owner_surface.get_instance_shader_parameter(
					"u_point_light_count")), 2.0,
			"the owner draw receives the world light AND its muzzle glow")
	assert_eq(
			float(bystander_surface.get_instance_shader_parameter(
					"u_point_light_count")), 1.0,
			"a bystander draw receives only the world light")
	var report := director.get_report()
	assert_eq(report.selection_mode, "per_model_objects",
			"the gameplay pass reports per-model selection")
	assert_eq(report.owner_isolation, "per_model")


func test_unchanged_selection_is_reapplied_after_model_rebuild() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var model := _placed_model(container, Vector3.ZERO)
	var first_surface := _surface_instance(model)
	if first_surface == null:
		return
	var first_id := first_surface.get_instance_id()
	var scene := LightScene.new()
	assert_gt(scene.spawn_model_light({
		"position": Vector3(0.0, 1.0, 0.0),
		"atten_end": 8.0,
	}), 0)
	var models: Array[Node3D] = [model]
	var owners := PackedInt64Array([model.get_instance_id()])
	assert_eq(scene.render_model_frame(models, owners,
			PackedInt64Array([0]), PackedInt32Array([0]),
			PackedByteArray([0]), Vector3.ONE, 0, null), 1)
	assert_eq(float(first_surface.get_instance_shader_parameter(
			"u_point_light_count")), 1.0)

	model.rebuild()
	await get_tree().process_frame
	var rebuilt_surface := _surface_instance(model)
	if rebuilt_surface == null:
		return
	assert_ne(rebuilt_surface.get_instance_id(), first_id,
			"rebuild replaces the rendered surface instances")
	assert_eq(scene.render_model_frame(models, owners,
			PackedInt64Array([0]), PackedInt32Array([0]),
			PackedByteArray([0]), Vector3.ONE, 0, null), 1)
	assert_eq(float(rebuilt_surface.get_instance_shader_parameter(
			"u_point_light_count")), 1.0,
			"an unchanged selection reaches freshly rebuilt material instances")


func test_zero_wire_handle_remains_an_owned_light_identity() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var container: Node = world.get_node_or_null("MissionObjects")
	if container == null:
		container = Node3D.new()
		container.name = "MissionObjects"
		world.add_child(container)
	var owner_model := _placed_model(container, Vector3.ZERO)
	owner_model.set_meta("entity_ref", {"wire_handle": 0})
	var bystander := _placed_model(container, Vector3(3.0, 0.0, 0.0))
	var tagged_zero := EffectLightDirector.owner_id_for_node(owner_model)
	assert_ne(tagged_zero, 0,
			"wire H=0 never aliases LightScene's unowned sentinel")
	assert_eq(tagged_zero, EffectLightDirector.owner_id_for_wire(0))

	var director := EffectLightDirector.new()
	director.setup(world, Callable(), Callable())
	director.on_muzzle_fire(0, Vector3(0.0, 1.0, 0.0))
	var camera := Camera3D.new()
	world.add_child(camera)
	camera.position = Vector3(1.0, 1.0, 5.0)
	director.render_frame(camera)
	var owner_surface := _surface_instance(owner_model)
	var bystander_surface := _surface_instance(bystander)
	if owner_surface == null or bystander_surface == null:
		return
	assert_eq(float(owner_surface.get_instance_shader_parameter(
			"u_point_light_count")), 1.0,
			"the valid zero-handle owner's own draw receives its muzzle light")
	assert_eq(float(bystander_surface.get_instance_shader_parameter(
			"u_point_light_count")), 0.0,
			"the tagged zero owner does not leak as a world light")


## Batched static sources spawn through reattach() with a tagged static owner:
## a subobject-attached record must be owner-scoped (retail attaches the
## spawning entity whenever the record's attach bone != 0 [orig:
## Entity_SpawnGlowEffects @ 0x56c8ae]) — before the fix it spawned unowned
## and leaked onto every nearby draw. A subobject-0 record stays a world
## light every draw receives.
func test_static_source_subobject_light_is_owner_scoped() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var container: Node = world.get_node_or_null("MissionObjects")
	if container == null:
		container = Node3D.new()
		container.name = "MissionObjects"
		world.add_child(container)
	# house.3di carries no light records: these draws only ever see what the
	# static source spawns.
	var model_a := _placed_model(container, Vector3(0.0, 0.0, 0.0))
	var model_b := _placed_model(container, Vector3(3.0, 0.0, 0.0))
	# shed.3di carries one authored record; the fixture attaches it to
	# subobject 2 to model the armory lamp shape.
	var lit := ObjectData.new()
	assert_eq(lit.open_file(ProjectSettings.globalize_path(SYN_SHED_LGHT0_SUB2)), OK)
	assert_eq(lit.get_light_count(), 1)
	assert_eq(int(lit.get_light_info(0).get("subobject", -1)), 2,
			"the fixture attaches the record to subobject 2")
	var director := EffectLightDirector.new()
	director.setup(world, func() -> Array:
		return [{
			"object_data": lit,
			"world_transform": Transform3D(Basis.IDENTITY,
					Vector3(1.5, 0.0, 0.0)),
		}], Callable())
	director.reattach()
	assert_eq(director.get_report().live, 1,
			"the static source spawns its subobject-attached record")
	var camera := Camera3D.new()
	world.add_child(camera)
	camera.position = Vector3(1.5, 1.0, 6.0)
	director.render_frame(camera)
	var surface_a := _surface_instance(model_a)
	var surface_b := _surface_instance(model_b)
	if surface_a == null or surface_b == null:
		return
	assert_eq(float(surface_a.get_instance_shader_parameter(
			"u_point_light_count")), 0.0,
			"a static subobject light is scoped to its own building, " +
			"not a nearby draw")
	assert_eq(float(surface_b.get_instance_shader_parameter(
			"u_point_light_count")), 0.0,
			"no bystander draw receives the owned static light")
	# The same record detached (subobject 0) is a mission-start world light:
	# reload the pristine shed (subobject 0, atten 0..3) into the ObjectData the
	# source closure holds.
	assert_eq(lit.open_file(ProjectSettings.globalize_path(SHED_3DI)), OK)
	assert_eq(int(lit.get_light_info(0).get("subobject", -1)), 0)
	director.reattach()
	director.render_frame(camera)
	assert_eq(float(surface_a.get_instance_shader_parameter(
			"u_point_light_count")), 1.0,
			"a subobject-0 static record lights every nearby draw")
	assert_eq(float(surface_b.get_instance_shader_parameter(
			"u_point_light_count")), 1.0)


## The corona owner visible-section gate [orig: the sectorFilter leg of
## EffectWorld_RenderLightCoronas @ 0x5ab027 -> Terrain_IsBuildingSectionBitSet
## @ 0x5c6960]: an owned corona draws only while its owner's section bit is
## set in the occlusion verdict mask; owners without a verdict pass.
func test_owned_corona_gates_on_owner_section_visibility() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var owner_model := _placed_model(container, Vector3.ZERO)
	var scene := LightScene.new()
	assert_gt(scene.spawn_model_light({
		"position": Vector3(0.0, 1.0, 0.0),
		"atten_end": 4.0,
		"attach_bone": 2,
		"spawning_entity": owner_model.get_instance_id(),
	}), 0)
	var models: Array[Node3D] = [owner_model]
	var owners := PackedInt64Array([owner_model.get_instance_id()])
	# No occlusion verdict yet (mask -1): the owner is not in the table and
	# the corona passes like retail's non-building owners.
	var rows: Array = scene.collect_corona_rows(Vector3(0.0, 1.0, 10.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 0, 0, null, models,
			owners, {})
	assert_eq(rows.size(), 3,
			"an owner without an occlusion verdict passes the gate")
	# The occlusion pass hides section 2: the owned corona disappears.
	owner_model.set_section_visibility_mask(~(1 << 2))
	rows = scene.collect_corona_rows(Vector3(0.0, 1.0, 10.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 0, 0, null, models,
			owners, {})
	assert_eq(rows.size(), 0,
			"a hidden owner section suppresses the owned corona")
	owner_model.set_section_visibility_mask(1 << 2)
	rows = scene.collect_corona_rows(Vector3(0.0, 1.0, 10.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 0, 0, null, models,
			owners, {})
	assert_eq(rows.size(), 3,
			"a visible owner section admits the owned corona")


func test_render_model_frame_returns_lit_model_count_and_clears() -> void:
	var scene := LightScene.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var near_model := _placed_model(container, Vector3.ZERO)
	var far_model := _placed_model(container, Vector3(4096.0, 0.0, 0.0))
	assert_gt(scene.spawn_glow({
		"position": Vector3(0.0, 1.0, 0.0),
		"radius": 8.0,
		"color": Color.WHITE,
	}), 0)
	var models: Array[Node3D] = [near_model, far_model]
	var owners := PackedInt64Array([0, 0])
	var no_interior := PackedInt64Array([0, 0])
	var no_sections := PackedInt32Array([0, 0])
	assert_eq(scene.render_model_frame(models, owners, no_interior,
			no_sections, PackedByteArray([0, 0]), Vector3.ONE, 0, null),
			1, "one model sits inside the light's AABB")
	var near_surface := _surface_instance(near_model)
	if near_surface == null:
		return
	assert_eq(float(near_surface.get_instance_shader_parameter(
			"u_point_light_count")), 1.0)
	var posr: Vector4 = near_surface.get_instance_shader_parameter(
			"u_point_light_posr_0")
	assert_almost_eq(posr.x, 0.0, 0.01)
	assert_almost_eq(posr.y, 1.0, 0.01)
	# atten2 = 15 / range^2 with range = 8 * 1.25 [orig:
	# Light_GetPointLightParams @ 0x5a9180].
	assert_almost_eq(posr.w, 0.15, 0.001)
	var color: Vector4 = near_surface.get_instance_shader_parameter(
			"u_point_light_color_0")
	assert_almost_eq(color.w, 10.0, 0.001, "color.w carries the range cutoff")
	# The light dies; the next pass clears the instance count.
	scene.clear()
	assert_eq(scene.render_model_frame(models, owners, no_interior,
			no_sections, PackedByteArray([0, 0]), Vector3.ONE, 0, null),
			0, "a cleared pool lights nothing")
	assert_eq(float(near_surface.get_instance_shader_parameter(
			"u_point_light_count")), 0.0,
			"a lit model returns to count zero when its lights die")


func test_static_rows_pack_owner_isolated_selections_and_clear_in_place() -> void:
	var scene := LightScene.new()
	assert_gt(scene.spawn_glow({
		"position": Vector3(0.0, 1.0, 0.0),
		"radius": 8.0,
		"color": Color.WHITE,
	}), 0)
	assert_gt(scene.spawn_model_light({
		"position": Vector3(0.25, 1.0, 0.0),
		"atten_end": 8.0,
		"attach_bone": 2,
		"spawning_entity": 101,
	}), 0)
	var bounds := PackedVector3Array([
		Vector3(-1.0, 0.0, -1.0), Vector3(2.0, 2.0, 2.0),
		Vector3(-1.0, 0.0, -1.0), Vector3(2.0, 2.0, 2.0),
		Vector3(-1.0, 0.0, -1.0), Vector3(2.0, 2.0, 2.0),
	])
	assert_eq(scene.render_static_frame(bounds,
			PackedInt64Array([101, 202, 101]),
			PackedInt32Array([2, 0, 2]),
			PackedInt64Array([0, 0, 0]),
			PackedInt32Array([0, 0, 0]),
			PackedByteArray([1, 1, 0]), Vector3.ONE, 0, null), 2,
			"both active rows receive at least the nearby world light")
	var report := scene.get_report()
	assert_eq(int(report.get("static_rows", -1)), 3)
	assert_eq(int(report.get("static_draws", -1)), 2,
			"the inactive stable row is not submitted for selection")
	assert_eq(int(report.get("lit_static_draws", -1)), 2)
	assert_has(RenderingServer.global_shader_parameter_get_list(),
			&"opennova_static_point_light_rows",
			"the shader global declares the atlas as a project-wide sampler")
	var atlas := _static_light_atlas(scene)
	if atlas == null:
		return
	assert_eq(atlas.get_format(), Image.FORMAT_RGBAF)
	assert_eq(atlas.get_width(), 9)
	assert_eq(atlas.get_height(), 3)
	assert_almost_eq(atlas.get_pixel(0, 0).r, 2.0, 0.001,
			"the owner row receives the world and section-owned lights")
	assert_almost_eq(atlas.get_pixel(0, 1).r, 1.0, 0.001,
			"the bystander row receives only the unowned world light")
	assert_almost_eq(atlas.get_pixel(0, 2).r, 0.0, 0.001,
			"an inactive row stays zero without moving later atlas identities")
	var world_posr := atlas.get_pixel(1, 1)
	assert_true(Vector3(world_posr.r, world_posr.g, world_posr.b)
			.is_equal_approx(Vector3(0.0, 1.0, 0.0)))
	assert_almost_eq(world_posr.a, 0.15, 0.001,
			"posr alpha carries the witnessed attenuation2")
	assert_almost_eq(atlas.get_pixel(2, 1).a, 10.0, 0.001,
			"color alpha carries the point-light range cutoff")
	scene.clear_render_output()
	atlas = _static_light_atlas(scene)
	if atlas == null:
		return
	assert_almost_eq(atlas.get_pixel(0, 0).r, 0.0, 0.001)
	assert_almost_eq(atlas.get_pixel(0, 1).r, 0.0, 0.001,
			"camera loss synchronously zeroes every published static row")
	report = scene.get_report()
	assert_eq(int(report.get("static_rows", -1)), 0)
	assert_eq(int(report.get("static_draws", -1)), 0)
	assert_eq(int(report.get("lit_static_draws", -1)), 0)


func test_static_building_rows_rescope_owned_lights_per_robj() -> void:
	var scene := LightScene.new()
	var owner := 0x2000000000042
	for section in [2, 4]:
		assert_gt(scene.spawn_model_light({
			"position": Vector3(float(section), 1.0, 0.0),
			"atten_end": 100.0,
			"attach_bone": section,
			"spawning_entity": owner,
		}), 0)
	var bounds := PackedVector3Array([
		Vector3(-10.0, -10.0, -10.0), Vector3(20.0, 20.0, 20.0),
		Vector3(-10.0, -10.0, -10.0), Vector3(20.0, 20.0, 20.0),
	])
	assert_eq(scene.render_static_frame(bounds,
			PackedInt64Array([0, 0]), PackedInt32Array([2, 4]),
			PackedInt64Array([owner, owner]), PackedInt32Array([0, 0]),
			PackedByteArray([1, 1]), Vector3.ONE, 0, null), 2)
	var atlas := _static_light_atlas(scene)
	if atlas == null:
		return
	assert_almost_eq(atlas.get_pixel(0, 0).r, 1.0, 0.001)
	assert_almost_eq(atlas.get_pixel(0, 1).r, 1.0, 0.001)
	assert_almost_eq(atlas.get_pixel(1, 0).r, 2.0, 0.01,
			"ROBJ 2 receives only its section-2 attached light")
	assert_almost_eq(atlas.get_pixel(1, 1).r, 4.0, 0.01,
			"ROBJ 4 receives only its section-4 attached light")


## The blink-box owner + interior light group (the D-RLIT-4 interior close):
## an unattached record spawned inside a building's blink volume binds to the
## CONTAINING building + that volume's section, and each draw declares the
## building it stands inside so the room's light reaches exactly the draws in
## that room [orig: Entity_SpawnGlowEffects @ 0x56c8bd..0x56c8db;
## setup_terrain_effect_for_entity @ 0x5c74a0 -> Lighting_SetInteriorLightGroup
## @ 0x5a90e0; the gate Light_PassesActiveGroups @ 0x5a9120].
func test_blink_owned_light_reaches_only_its_own_interior_section() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	# Three draws in the lamp's reach: the containing building, a model inside
	# its section 2, and a model inside section 5 of the same building.
	var building := _placed_model(container, Vector3.ZERO)
	var in_room := _placed_model(container, Vector3(1.0, 0.0, 0.0))
	var other_room := _placed_model(container, Vector3(2.0, 0.0, 0.0))
	var building_id := building.get_instance_id()

	var scene := LightScene.new()
	# An unattached record (attach bone 0) spawned by a NON-building entity
	# standing inside building/section 2: the containing building owns it.
	assert_gt(scene.spawn_model_light({
		"position": Vector3(1.0, 1.0, 0.0),
		"atten_end": 8.0,
		"attach_bone": 0,
		"spawning_entity": in_room.get_instance_id(),
		"spawner_is_building": false,
		"blink_owner_entity": building_id,
		"blink_section": 2,
	}), 0)

	var models: Array[Node3D] = [in_room, other_room, building]
	var owners := PackedInt64Array([in_room.get_instance_id(),
			other_room.get_instance_id(), building_id])
	var interior_owners := PackedInt64Array([building_id, building_id, 0])
	var interior_sections := PackedInt32Array([2, 5, 0])
	assert_eq(scene.render_model_frame(models, owners, interior_owners,
			interior_sections, PackedByteArray([0, 0, 0]),
			Vector3.ONE, 0, null), 2,
			"the room light reaches its own room and its owner building")

	var in_room_surface := _surface_instance(in_room)
	var other_surface := _surface_instance(other_room)
	if in_room_surface == null or other_surface == null:
		return
	assert_eq(float(in_room_surface.get_instance_shader_parameter(
			"u_point_light_count")), 1.0,
			"a draw standing in the light's own section receives it")
	assert_eq(float(other_surface.get_instance_shader_parameter(
			"u_point_light_count")), 0.0,
			"a draw in another section of the same building does not")

	# Outdoors (no interior group at all): the interior lamp never leaks.
	var outdoor_owners := PackedInt64Array([0, 0, 0])
	var outdoor_sections := PackedInt32Array([0, 0, 0])
	assert_eq(scene.render_model_frame(models, owners, outdoor_owners,
			outdoor_sections, PackedByteArray([0, 0, 0]),
			Vector3.ONE, 0, null), 1,
			"only the owner building's own draw keeps the light outdoors")


## Retail skips the blink query outright for a BUILDING, so a building's own
## unattached records stay world lights even though its volumes contain them
## [orig: the ItemType_Building gate @ 0x56c7ec].
func test_a_buildings_own_unattached_record_stays_a_world_light() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var building := _placed_model(container, Vector3.ZERO)
	var bystander := _placed_model(container, Vector3(2.0, 0.0, 0.0))
	var scene := LightScene.new()
	assert_gt(scene.spawn_model_light({
		"position": Vector3(1.0, 1.0, 0.0),
		"atten_end": 8.0,
		"attach_bone": 0,
		"spawning_entity": building.get_instance_id(),
		"spawner_is_building": true,
		# Even with a blink hit reported, the building gate suppresses it.
		"blink_owner_entity": building.get_instance_id(),
		"blink_section": 3,
	}), 0)
	var models: Array[Node3D] = [building, bystander]
	var owners := PackedInt64Array([building.get_instance_id(),
			bystander.get_instance_id()])
	assert_eq(scene.render_model_frame(models, owners,
			PackedInt64Array([0, 0]), PackedInt32Array([0, 0]),
			PackedByteArray([0, 0]), Vector3.ONE, 0, null), 2,
			"an unowned world light reaches every nearby draw")


## A building's OWN model draw re-scopes the active owner section once per
## ROBJ. Attached section-2 and section-4 lights therefore reach only those
## two render objects, even though every part shares one entity owner.
## [orig: Terrain_RenderSectorModels @0x5c5e07 pushes building/section 0;
## collect_render_objects_for_batch @0x5d8ff7 re-scopes owner section].
func test_building_owned_lights_are_selected_per_robj() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var building := _multi_part_model(container)
	building.set_meta("entity_ref", {
		"kind": MissionData.KIND_BUILDING,
		"wire_handle": 0x1002,
	})
	assert_eq(building.get_render_part_nodes().size(), 5)
	var owner := EffectLightDirector.owner_id_for_node(building)
	var scene := LightScene.new()
	for section in [2, 4]:
		assert_gt(scene.spawn_model_light({
			"position": Vector3(0.0, 2.0, 0.0),
			"atten_end": 1000.0,
			"attach_bone": section,
			"spawning_entity": owner,
		}), 0)
	var models: Array[Node3D] = [building]
	assert_eq(scene.render_model_frame(models, PackedInt64Array([owner]),
			PackedInt64Array([0]), PackedInt32Array([0]),
			PackedByteArray([1]), Vector3.ONE, 0, null), 1,
			"one building model is lit across its expanded ROBJ contexts")
	for section in range(5):
		var surface := _part_surface(building, section)
		assert_not_null(surface, "ROBJ %d exposes a surface" % section)
		if surface == null:
			continue
		var count := float(surface.get_instance_shader_parameter(
				"u_point_light_count"))
		assert_eq(count, 1.0 if section == 2 or section == 4 else 0.0,
				"ROBJ %d receives only its section-owned light" % section)
	assert_eq(String(scene.get_report().get("owner_isolation", "")),
			"per_robj_buildings")
