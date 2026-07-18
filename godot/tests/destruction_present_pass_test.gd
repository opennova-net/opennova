extends GutTest

const DestructionPresentPass := preload('res://engine/world/destruction_present_pass.gd')
const MissionObjectPlacer := preload('res://engine/mission/mission_object_placer.gd')


class SimStub:
	extends RefCounted
	var pieces: Array = []
	var events: Dictionary = {}
	var present_states: Dictionary = {}

	func drain_destruction_events() -> Dictionary:
		var drained := events
		events = {}
		return drained

	func get_death_pieces() -> Array:
		return pieces

	func get_present_effect_state_for_bms_id(bms_id: int) -> PackedVector3Array:
		return present_states.get(bms_id, PackedVector3Array())


class FxStub:
	extends RefCounted
	var owned_spawns: Array = []
	var spawns: Array = []

	func spawn_effect_owned(owner_key: Variant, effect: String, position: Vector3,
			orientation: Vector3 = Vector3.ZERO) -> int:
		owned_spawns.append({
			'owner': owner_key,
			'effect': effect,
			'position': position,
			'orientation': orientation,
		})
		return owned_spawns.size()

	func spawn_effect(effect: String, position: Vector3,
			orientation: Vector3 = Vector3.ZERO) -> int:
		spawns.append({
			'effect': effect,
			'position': position,
			'orientation': orientation,
		})
		return spawns.size()


class WorldStub:
	extends RefCounted
	var anchors: Dictionary = {}
	var registrations: Array = []
	var unregistrations: Array = []

	func register_effect_anchor(owner_key: Variant, resolver: Callable) -> void:
		anchors[owner_key] = resolver
		registrations.append(owner_key)

	func unregister_effect_anchor(owner_key: Variant) -> void:
		anchors.erase(owner_key)
		unregistrations.append(owner_key)

	func anchor_position(owner_key: Variant) -> Variant:
		if not anchors.has(owner_key):
			return null
		var resolver: Callable = anchors[owner_key]
		return resolver.call() if resolver.is_valid() else null


class IndexStub:
	extends RefCounted
	var nodes: Dictionary = {}
	var by_kind_index: Dictionary = {}
	var resolve_calls: Array = []

	func resolve(bms_id: int, kind: int, index: int):
		resolve_calls.append([bms_id, kind, index])
		var node: Variant = nodes.get(bms_id) if bms_id != 0 else null
		if node == null:
			node = by_kind_index.get('%d:%d' % [kind, index])
		return node

	func resolve_single(bms_id: int):
		return nodes.get(bms_id)


class ItemDbStub:
	extends RefCounted

	func get_husk(_def_id: int) -> String:
		return 'HuskGraphic'

	func get_huskfinal(_def_id: int) -> String:
		return ''


class PlacerStub:
	extends RefCounted
	var built: Array = []
	var built_env_nodes: Array = []
	var hidden: Array = []
	var shown: Array = []
	var batched_transforms: Dictionary = {}
	var build_success := true

	func build_model_from_graphic(_graphic: String, _anim: String,
			parent: Node3D, _clip_key: String = '', env_node: Node = null,
			_rig_graphic: String = '') -> Node3D:
		built_env_nodes.append(env_node)
		if not build_success:
			return null
		var model := Node3D.new()
		parent.add_child(model)
		built.append(model)
		return model

	func hide_static_instance(bms_id: int) -> Variant:
		hidden.append(bms_id)
		return batched_transforms.get(bms_id)

	func show_static_instance(bms_id: int) -> bool:
		shown.append(bms_id)
		return batched_transforms.has(bms_id)


func _piece(slot: int, generation: int, type_index: int, pos: Vector3,
		settled: bool = false) -> Dictionary:
	return {
		'slot': slot,
		'generation': generation,
		'type_index': type_index,
		'pos': pos,
		'settled': settled,
	}


func _make_pass(sim: SimStub, fx: FxStub, world: WorldStub):
	var presenter = DestructionPresentPass.new()
	presenter.setup(sim, null, null, null, null, world, Callable(),
			func(): return fx)
	return presenter


# Ring-slot lifecycle regressions.
func test_active_slot_reuse_replaces_the_presented_incarnation() -> void:
	var sim := SimStub.new()
	var fx := FxStub.new()
	var world := WorldStub.new()
	var presenter = _make_pass(sim, fx, world)
	var key := 'piece:7'
	sim.pieces = [_piece(7, 11, 1, Vector3(1, 2, 3))]
	presenter.present()
	assert_eq(fx.owned_spawns.size(), 1)
	sim.pieces = [_piece(7, 11, 1, Vector3(4, 5, 6))]
	presenter.present()
	assert_eq(fx.owned_spawns.size(), 1)
	assert_eq(world.anchor_position(key), Vector3(4, 5, 6))
	sim.pieces = [_piece(7, 12, 2, Vector3(8, 9, 10))]
	presenter.present()
	assert_eq(fx.owned_spawns.size(), 2)
	if fx.owned_spawns.size() == 2:
		assert_eq(String(fx.owned_spawns[1]['effect']), 'Effect_VexpS')
		assert_eq(String(fx.owned_spawns[1]['owner']), key)
	assert_eq(world.anchor_position(key), Vector3(8, 9, 10))
	presenter.teardown()


func test_slot_reuse_to_a_no_trail_type_detaches_the_old_owner() -> void:
	var sim := SimStub.new()
	var fx := FxStub.new()
	var world := WorldStub.new()
	var presenter = _make_pass(sim, fx, world)
	var key := 'piece:3'
	sim.pieces = [_piece(3, 30, 1, Vector3(2, 3, 4))]
	presenter.present()
	assert_eq(fx.owned_spawns.size(), 1)
	assert_true(world.anchors.has(key))
	sim.pieces = [_piece(3, 31, 0, Vector3(8, 8, 8))]
	presenter.present()
	assert_eq(fx.owned_spawns.size(), 1)
	assert_eq(world.unregistrations.count(key), 1)
	assert_false(world.anchors.has(key))
	presenter.teardown()


func test_settled_slot_reuse_replaces_the_presented_incarnation() -> void:
	var sim := SimStub.new()
	var fx := FxStub.new()
	var world := WorldStub.new()
	var presenter = _make_pass(sim, fx, world)
	var key := 'piece:13'
	sim.pieces = [_piece(13, 20, 1, Vector3(1, 4, 2))]
	presenter.present()
	assert_eq(fx.owned_spawns.size(), 1)
	sim.pieces = [_piece(13, 20, 1, Vector3(1, 0, 2), true)]
	presenter.present()
	assert_eq(fx.owned_spawns.size(), 1)
	assert_eq(world.anchor_position(key), Vector3(1, 0, 2))
	sim.pieces = [_piece(13, 21, 2, Vector3(9, 3, 5))]
	presenter.present()
	assert_eq(fx.owned_spawns.size(), 2)
	if fx.owned_spawns.size() == 2:
		assert_eq(String(fx.owned_spawns[1]['effect']), 'Effect_VexpS')
	assert_eq(world.anchor_position(key), Vector3(9, 3, 5))
	presenter.teardown()


func test_reset_runtime_state_restores_individual_visuals_and_retires_anchors() -> void:
	var sim := SimStub.new()
	var fx := FxStub.new()
	var world := WorldStub.new()
	var index := IndexStub.new()
	var placer := PlacerStub.new()
	var item_db := ItemDbStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var env_node := Node.new()
	add_child_autofree(env_node)
	var intact := Node3D.new()
	container.add_child(intact)
	var originally_visible := Node3D.new()
	var originally_hidden := Node3D.new()
	originally_hidden.visible = false
	intact.add_child(originally_visible)
	intact.add_child(originally_hidden)
	index.nodes[41] = intact
	sim.events = {
		'husk_swaps': [{
			'bms_id': 41,
			'item_id': 9,
		}],
		'effects': [{
			'effect': 'Effect_VehExplode',
			'family': 2,
			'attach_net_id': 91,
			'attach_bms_id': 41,
			'pos': Vector3(3, 4, 5),
		}],
	}
	sim.pieces = [_piece(5, 70, 1, Vector3(6, 7, 8))]
	var presenter = DestructionPresentPass.new()
	presenter.setup(sim, container, index, placer, item_db, world, Callable(),
			func(): return fx, env_node)

	presenter.present()

	assert_false(originally_visible.visible)
	assert_false(originally_hidden.visible)
	assert_eq(placer.built.size(), 1)
	assert_eq(placer.built_env_nodes, [env_node],
			'husk materials receive the live mission environment')
	assert_true(world.anchors.has('wreck:91:2'))
	assert_true(world.anchors.has('piece:5'))
	var graft: Node3D = placer.built[0]

	presenter.reset_runtime_state()

	assert_true(originally_visible.visible,
			'the intact child returns to its authored visibility')
	assert_false(originally_hidden.visible,
			'an authored hidden child is not forced visible')
	assert_true(graft.is_queued_for_deletion())
	assert_eq(world.anchors.size(), 0)
	assert_true(world.unregistrations.has('wreck:91:2'))
	assert_true(world.unregistrations.has('piece:5'))
	var unregister_count := world.unregistrations.size()
	presenter.reset_runtime_state()
	assert_eq(world.unregistrations.size(), unregister_count,
			'reset is public and idempotent')


func test_reset_runtime_state_restores_batched_static_and_removes_husk_graft() -> void:
	var sim := SimStub.new()
	var fx := FxStub.new()
	var world := WorldStub.new()
	var index := IndexStub.new()
	var placer := PlacerStub.new()
	var item_db := ItemDbStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	placer.batched_transforms[77] = Transform3D(Basis.IDENTITY, Vector3(9, 8, 7))
	sim.events = {
		'husk_swaps': [{
			'bms_id': 77,
			'item_id': 11,
		}],
	}
	var presenter = DestructionPresentPass.new()
	presenter.setup(sim, container, index, placer, item_db, world, Callable(),
			func(): return fx)

	presenter.present()

	assert_eq(placer.hidden, [77])
	assert_eq(placer.built.size(), 1)
	var graft: Node3D = placer.built[0]
	assert_eq(graft.transform, placer.batched_transforms[77])

	presenter.reset_runtime_state()

	assert_eq(placer.shown, [77],
			'the pass uses the placer public inverse to restore the static')
	assert_true(graft.is_queued_for_deletion())


func test_failed_individual_husk_build_keeps_the_intact_visual_visible() -> void:
	var sim := SimStub.new()
	var fx := FxStub.new()
	var world := WorldStub.new()
	var index := IndexStub.new()
	var placer := PlacerStub.new()
	placer.build_success = false
	var container := Node3D.new()
	add_child_autofree(container)
	var intact := Node3D.new()
	container.add_child(intact)
	var visual := Node3D.new()
	intact.add_child(visual)
	index.nodes[41] = intact
	sim.events = {
		'husk_swaps': [{
			'bms_id': 41,
			'item_id': 9,
		}],
	}
	var presenter = DestructionPresentPass.new()
	presenter.setup(sim, container, index, placer, ItemDbStub.new(), world,
			Callable(), func(): return fx)

	presenter.present()

	assert_true(visual.visible,
			'a failed graft build must leave the retail intact-graphic fallback standing')
	assert_eq(placer.built.size(), 0)
	assert_eq(presenter.get_stats().no_husk, 1)
	presenter.teardown()


func test_failed_batched_husk_build_does_not_carve_the_static_instance() -> void:
	var sim := SimStub.new()
	var fx := FxStub.new()
	var world := WorldStub.new()
	var placer := PlacerStub.new()
	placer.build_success = false
	placer.batched_transforms[77] = Transform3D(Basis.IDENTITY, Vector3(9, 8, 7))
	var container := Node3D.new()
	add_child_autofree(container)
	sim.events = {
		'husk_swaps': [{
			'bms_id': 77,
			'item_id': 11,
		}],
	}
	var presenter = DestructionPresentPass.new()
	presenter.setup(sim, container, IndexStub.new(), placer, ItemDbStub.new(), world,
			Callable(), func(): return fx)

	presenter.present()

	assert_eq(placer.hidden, [],
			'a failed graft build must not carve a visible hole in a static batch')
	assert_eq(container.get_child_count(), 0)
	assert_eq(presenter.get_stats().no_husk, 1)
	presenter.teardown()


func test_zero_bms_husks_use_distinct_spawn_origins_for_identity_and_lookup() -> void:
	var sim := SimStub.new()
	var fx := FxStub.new()
	var world := WorldStub.new()
	var index := IndexStub.new()
	var placer := PlacerStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var first := Node3D.new()
	var first_visual := Node3D.new()
	first.add_child(first_visual)
	container.add_child(first)
	var second := Node3D.new()
	var second_visual := Node3D.new()
	second.add_child(second_visual)
	container.add_child(second)
	index.by_kind_index['3:5'] = first
	index.by_kind_index['4:6'] = second
	var first_origin := (3 << 24) | 5
	var second_origin := (4 << 24) | 6
	sim.events = {
		'husk_swaps': [{
			'bms_id': 0,
			'spawn_origin': first_origin,
			'item_id': 11,
		}, {
			'bms_id': 0,
			'spawn_origin': second_origin,
			'item_id': 11,
		}],
	}
	var presenter = DestructionPresentPass.new()
	presenter.setup(sim, container, index, placer, ItemDbStub.new(), world,
			Callable(), func(): return fx)

	presenter.present()

	assert_eq(index.resolve_calls, [[0, 3, 5], [0, 4, 6]],
			'each event resolves through its decoded mission origin')
	assert_eq(placer.built.size(), 2,
			'distinct zero-BMS entities own distinct husk cache entries')
	assert_false(first_visual.visible)
	assert_false(second_visual.visible)
	assert_eq(placer.hidden, [],
			'individual origin fallback does not touch the static batch map')
	presenter.teardown()
	assert_true(first_visual.visible)
	assert_true(second_visual.visible,
			'each canonical owner retains its own intact-visibility restore state')


func test_batched_husk_and_wreck_anchor_follow_the_live_present_pose() -> void:
	var sim := SimStub.new()
	var fx := FxStub.new()
	var world := WorldStub.new()
	var placer := PlacerStub.new()
	placer.batched_transforms[77] = Transform3D(Basis.IDENTITY, Vector3(9, 8, 7))
	var first_pos := Vector3(2, 3, 4)
	var first_rot := Vector3(10, 20, 30)
	sim.present_states[77] = PackedVector3Array([first_pos, first_rot])
	var container := Node3D.new()
	add_child_autofree(container)
	sim.events = {
		'husk_swaps': [{
			'bms_id': 77,
			'item_id': 11,
		}],
		'effects': [{
			'effect': 'Effect_VehExplode',
			'family': 2,
			'attach_net_id': 91,
			'attach_bms_id': 77,
			'pos': Vector3(100, 100, 100),
		}],
	}
	var presenter = DestructionPresentPass.new()
	presenter.setup(sim, container, IndexStub.new(), placer, ItemDbStub.new(), world,
			Callable(), func(): return fx)

	presenter.present()

	var graft: Node3D = placer.built[0]
	var first_expected := Transform3D(
			MissionObjectPlacer.bms_to_godot_basis(first_rot), first_pos)
	assert_eq(graft.transform, first_expected)
	assert_eq(world.anchor_position('wreck:91:2'), first_expected)

	var next_pos := Vector3(-6, 5, 12)
	var next_rot := Vector3(-5, 135, 8)
	sim.present_states[77] = PackedVector3Array([next_pos, next_rot])
	presenter.present()
	var next_expected := Transform3D(
			MissionObjectPlacer.bms_to_godot_basis(next_rot), next_pos)
	assert_eq(graft.transform, next_expected,
			'the node-less graft follows settling motion on later presents')
	assert_eq(world.anchor_position('wreck:91:2'), next_expected,
			'the owned wreck effect resolves the same current present pose')
	presenter.teardown()


func test_zero_origin_debris_event_is_not_discarded() -> void:
	var sim := SimStub.new()
	var fx := FxStub.new()
	var world := WorldStub.new()
	sim.events = {
		'debris_bursts': [{
			'bms_id': 0,
			'pos': Vector3.ZERO,
		}],
	}
	var presenter = _make_pass(sim, fx, world)

	presenter.present()

	assert_eq(presenter.get_stats().bursts, 1)
	assert_eq(fx.spawns.size(), DestructionPresentPass.BURST_COUNT,
			'world origin is a valid authored destruction position')
	presenter.teardown()
