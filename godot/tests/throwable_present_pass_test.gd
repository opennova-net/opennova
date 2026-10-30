extends GutTest

# The throwable present pass (EntityPresenter's ThrowablePresenter member, ADR
# 0043 d9) on the typed surfaces (ADR 0034): the visual rows are pure data
# through the public present_throwable_visuals data leg (production
# present_passes() drains the typed Simulation), the placer is a real
# MissionObjectPlacer over the injected fixture model, and the item database is
# the real fixture items.def (id 101883 = the 3rd-person HE grenade). A round's
# move emitter is the engine's (world::RoundEmitters; ctest round_emitters).

static var _item_db: ItemDatabase = null


func before_all() -> void:
	_item_db = ItemDatabase.new()
	assert_eq(_item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK,
			"the fixture items.def loads (101883 carries graphic Frag_3rd)")


const ROUND_GRAPHIC := "Frag_3rd"  # fixture items.def 101883's graphic
const ROUND_MODEL_3DI := "res://../fixtures/threedi/synth/armory.3di"


# A REAL placer whose round graphic resolves through the injected fixture
# model (native methods cannot be intercepted from GDScript).
func _round_placer() -> MissionObjectPlacer:
	var placer := MissionObjectPlacer.new()
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ROUND_MODEL_3DI)),
			OK, "the round fixture model loads")
	placer.register_object_data(ROUND_GRAPHIC, data)
	return placer


# Every thrown-round model the pass built under `root` (named
# Throwable_<item_id>; Godot may suffix collisions).
func _thrown_models(root: Node) -> Array:
	var out: Array = []
	var stack: Array = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		if n is ObjectModel and String(n.name).begins_with("Throwable_"):
			out.append(n)
		for child in n.get_children():
			stack.push_back(child)
	return out


# A REAL EntityPresenter with its throwable pass wired over the placer and the
# item database.
func _make_presenter(container: Node3D, placer: MissionObjectPlacer) -> EntityPresenter:
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(null, null, placer)
	presenter.setup_passes(container, _item_db, null, null, null, null, null)
	return presenter


# One visual row shaped like Simulation::get_throwable_visuals emits.
func _row(key: int, item_id: int, pos: Vector3, rotation_deg: Vector3,
		move_effect: String = "", move_effect_live: bool = true) -> ThrowableVisualRow:
	return ThrowableVisualRow.make(key, item_id, pos, rotation_deg, move_effect, move_effect_live)


func test_present_uses_the_canonical_bms_basis_and_godot_position() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := _round_placer()
	var presenter := _make_presenter(container, placer)

	var position := Vector3(12.5, -4.0, 33.25)
	for rotation in [Vector3.ZERO, Vector3(0, 90, 0), Vector3(20, 35, -15)]:
		presenter.present_throwable_visuals([_row(7, 1883, position, rotation)])
		assert_eq(_thrown_models(container).size(), 1, "the same live round reuses its model")
		var model: ObjectModel = _thrown_models(container)[0]
		assert_eq(model.position, position, "Godot-space position is applied verbatim")
		assert_true(model.basis.is_equal_approx(
				MissionObjectPlacer.bms_to_godot_basis(rotation)),
				"rotation %s uses the shared BMS placement convention" % rotation)

	presenter.teardown()


# A flying round's model is on no water mirror on either side of the water (its
# row's population is the RoundPool, which no reflected collect walks); a placed
# device's model keeps the pool-1 entity's rule, on both mirrors below the water.
# [orig: Terrain_CollectVisibleEntitiesForReflection @0x5c90a0 - the calls
# @0x5c913d, @0x5c9145, @0x5c914e; Terrain_CollectVisibleEntities @0x5c9160 -
# the RoundPool_GetBase call @0x5c91c1]
func test_a_flying_rounds_model_is_on_no_mirror_and_a_placed_device_keeps_its_rule() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := _round_placer()
	var presenter := _make_presenter(container, placer)
	var device_key := (1 << 62) | 0x1005
	presenter.present_throwable_visuals([
		_row(7, 1883, Vector3(1, 2, 3), Vector3.ZERO),
		ThrowableVisualRow.make(device_key, 1883, Vector3(4, 5, 6), Vector3.ZERO, "", false, true),
	])
	var models := _thrown_models(container)
	assert_eq(models.size(), 2, "one model per row")
	var round_model: ObjectModel = null
	var device_model: ObjectModel = null
	for m in models:
		var model := m as ObjectModel
		if model.position.is_equal_approx(Vector3(1, 2, 3)):
			round_model = model
		else:
			device_model = model
	assert_not_null(round_model)
	assert_not_null(device_model)
	if round_model == null or device_model == null:
		presenter.teardown()
		return
	assert_false(round_model.mirror_collected, "the round's population is the RoundPool")
	assert_true(device_model.mirror_collected, "the device is a pool-1 entity")
	const WORLD_NO_MIRROR := 1 << 16
	const MAIN_VIEW_NO_MIRROR := 1 << 22
	const MIRROR_BITS := (1 << 24) | (1 << 25)
	var below := Water.REFLECTION_CULL_MASK | MIRROR_BITS | WORLD_NO_MIRROR | MAIN_VIEW_NO_MIRROR
	var round_layers := 0
	for node in round_model.find_children("*", "MeshInstance3D", true, false):
		round_layers |= (node as MeshInstance3D).layers
	var device_layers := 0
	for node in device_model.find_children("*", "MeshInstance3D", true, false):
		device_layers |= (node as MeshInstance3D).layers
	assert_ne(round_layers, 0, "the round's model carries visual instances")
	assert_eq(round_layers & below, 0, "no mirror camera admits the round's model")
	assert_ne(device_layers & below, 0, "the below-water mirrors admit the device's")
	presenter.teardown()


func test_remote_flying_round_snapshot_builds_and_retires_its_model() -> void:
	# A joiner's decoded tag-2 throwable is just a visual RoundSim snapshot at
	# this seam. It must materialize while the round is live, then disappear
	# when the visual motor releases the slot. This does not synthesize a placed
	# device: a future 0x59/0x12 decode would have to add that state explicitly.
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := _round_placer()
	var presenter := _make_presenter(container, placer)

	presenter.present_throwable_visuals([_row(19, 1883, Vector3(4, 5, 6), Vector3.ZERO)])
	assert_eq(_thrown_models(container).size(), 1,
			"the remote flying-round snapshot materializes its TrcrID item")
	var live_stats := presenter.get_throwable_present_stats()
	assert_true(live_stats is ThrowablePresentStats)
	assert_eq(live_stats.live, 1)
	var model: ObjectModel = _thrown_models(container)[0]
	assert_eq(model.position, Vector3(4, 5, 6))

	presenter.present_throwable_visuals([])
	assert_eq(presenter.get_throwable_present_stats().live, 0)
	assert_true(model.is_queued_for_deletion(),
			"the model retires when the remote visual round slot is gone")
	presenter.teardown()


func test_reset_wire_runtime_state_clears_the_models() -> void:
	# The Stop -> Play boundary reaches the throwable pass through the
	# presenter's one reset (the restart signal's single connect): every
	# presented model retires.
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := _make_presenter(container, _round_placer())
	presenter.present_throwable_visuals([_row(7, 1883, Vector3(2, 3, 4), Vector3.ZERO)])
	assert_eq(presenter.get_throwable_present_stats().live, 1)
	var model: ObjectModel = _thrown_models(container)[0]

	presenter.reset_wire_runtime_state()

	assert_eq(presenter.get_throwable_present_stats().live, 0, "reset frees every presented model")
	assert_true(model.is_queued_for_deletion())
	presenter.teardown()
