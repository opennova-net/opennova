extends GutTest

## Per-draw owner light isolation (the D-RLIT-4 per-model close): each visible
## ObjectModel is its own draw context — owned pool lights reach ONLY their
## owner entity's draws, world lights reach every nearby draw, and the
## selected four land as per-instance shader parameters
## [orig: the per-draw collect @ 0x5aa250 feeding update_light_slots
## @ 0x5abc50 with that draw's owner group].


func _fixture_object_data(model: String) -> ObjectData:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3")), OK)
	var data := ObjectData.new()
	assert_eq(data.open_from_resource_root(root, model), OK,
			"%s loads as a per-model light fixture" % model)
	return data


func _placed_model(parent: Node, position: Vector3) -> ObjectModel:
	var model := ObjectModel.new()
	parent.add_child(model)
	model.position = position
	model.set_object_data(_fixture_object_data("House.3di"))
	return model


func _surface_instance(model: ObjectModel) -> GeometryInstance3D:
	var instances := model.find_children("*", "GeometryInstance3D", true, false)
	assert_gt(instances.size(), 0, "the built model exposes surface instances")
	if instances.is_empty():
		return null
	return instances[0] as GeometryInstance3D


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
	assert_eq(EffectLightDirector.owner_id_for_node(owner_model), 77,
			"a wire-stamped model owns its wire handle")
	assert_eq(EffectLightDirector.owner_id_for_node(bystander),
			bystander.get_instance_id(),
			"an unstamped model falls back to its instance id")

	var director := EffectLightDirector.new()
	director.setup(world, Callable())
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


## Batched static sources spawn through reattach() with a synthetic owner:
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
	# House.3di carries no light records: these draws only ever see what the
	# static source spawns.
	var model_a := _placed_model(container, Vector3(0.0, 0.0, 0.0))
	var model_b := _placed_model(container, Vector3(3.0, 0.0, 0.0))
	# Shed.3di carries one authored record (atten 0..3); attach it to
	# subobject 2 in-memory to model the armory lamp shape.
	var lit := _fixture_object_data("Shed.3di")
	assert_eq(lit.get_light_count(), 1)
	assert_true(lit.set_light_field(0, "subobject", 2))
	var director := EffectLightDirector.new()
	director.setup(world, func() -> Array:
		return [{
			"object_data": lit,
			"world_transform": Transform3D(Basis.IDENTITY,
					Vector3(1.5, 0.0, 0.0)),
		}])
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
	# The same record detached (subobject 0) is a mission-start world light.
	assert_true(lit.set_light_field(0, "subobject", 0))
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
		"owner_entity": owner_model.get_instance_id(),
		"owner_section": 2,
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
	assert_eq(scene.render_model_frame(models, owners, Vector3.ONE, 0, null),
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
	assert_eq(scene.render_model_frame(models, owners, Vector3.ONE, 0, null),
			0, "a cleared pool lights nothing")
	assert_eq(float(near_surface.get_instance_shader_parameter(
			"u_point_light_count")), 0.0,
			"a lit model returns to count zero when its lights die")
