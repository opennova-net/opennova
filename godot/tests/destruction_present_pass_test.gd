extends GutTest

# DestructionPresentPass on the typed surfaces (ADR 0034): event/piece rows are
# pure data through the public present_drained data leg (production present()
# drains the typed Simulation), the collaborators are real
# MissionObjectPlacer/EffectWorld/ItemEffectDirector/WirePresentPass subclasses
# capturing the typed calls, the entity index is a real EntityIndex over real
# ObjectModels, and the item database is the real fixture items.def
# (item 1291 = the dune buggy with husk Dbuggy1X).
#
# The live settling-pose follow (a REAL sim's present-effect state feeding the
# node-less graft) is exercised by the real-sim test at the bottom; the
# injected-pose multi-update follow of the old sim double had no typed
# equivalent without native pose injection and is covered there by the sim's
# authoritative initial pose plus the yaw-only tilt-retention leg.

const DestructionPresentPass := preload('res://game/world/destruction_present_pass.gd')
const MissionPresentation := preload('res://game/world/mission_presentation.gd')

const BUGGY_ITEM_ID := 1291  # fixture items.def 101291, husk Dbuggy1X

static var _item_db: ItemDatabase = null


func before_all() -> void:
	_item_db = ItemDatabase.new()
	assert_eq(_item_db.load(ProjectSettings.globalize_path(
			'res://../fixtures/def/items.def')), OK,
			'the fixture items.def loads (101291 carries husk Dbuggy1X)')


class CaptureFx:
	extends EffectWorld
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

	func spawn_effect(name: String, position: Vector3,
			orientation: Vector3 = Vector3.ZERO) -> int:
		spawns.append({
			'effect': name,
			'position': position,
			'orientation': orientation,
		})
		return spawns.size()


class CaptureAnchors:
	extends ItemEffectDirector
	var anchors: Dictionary = {}
	var registrations: Array = []
	var unregistrations: Array = []

	func register_effect_anchor(owner_key: Variant, resolver: Callable) -> void:
		anchors[owner_key] = resolver
		registrations.append(owner_key)
		super.register_effect_anchor(owner_key, resolver)

	func unregister_effect_anchor(owner_key: Variant) -> void:
		anchors.erase(owner_key)
		unregistrations.append(owner_key)
		super.unregister_effect_anchor(owner_key)

	func anchor_position(owner_key: Variant) -> Variant:
		if not anchors.has(owner_key):
			return null
		var resolver: Callable = anchors[owner_key]
		return resolver.call() if resolver.is_valid() else null


# A REAL wire presenter with injected per-handle avatars (native methods
# cannot be intercepted from GDScript; register_wire_node is the seam).
func _wire_resolver(nodes: Dictionary = {}) -> WirePresentPass:
	var presenter := WirePresentPass.new()
	for handle in nodes:
		presenter.register_wire_node(int(handle), nodes[handle])
	return presenter


const HUSK_GRAPHIC := 'Dbuggy1X'  # fixture items.def 101291's husk stage
const HUSK_MODEL_3DI := 'res://../fixtures/3dp/armry01/Armry01.3di'


# A REAL placer whose husk graphic resolves through the injected fixture
# model (no resource root needed). resolvable=false leaves the graphic
# unregistered so build_model_from_graphic fails naturally.
func _husk_placer(resolvable := true) -> MissionObjectPlacer:
	var placer := MissionObjectPlacer.new()
	if resolvable:
		var data := ObjectData.new()
		assert_eq(data.open_file(ProjectSettings.globalize_path(HUSK_MODEL_3DI)),
				OK, 'the husk fixture model loads')
		placer.register_object_data(HUSK_GRAPHIC, data)
	return placer


# Every husk graft the pass built anywhere under `root` (individual grafts
# are named HuskModel, batched HuskModel_<bms>; Godot may suffix collisions).
func _husk_models(root: Node) -> Array:
	var out: Array = []
	var stack: Array = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		if n is ObjectModel and String(n.name).begins_with('HuskModel'):
			out.append(n)
		for child in n.get_children():
			stack.push_back(child)
	return out


# A real EntityIndex over real ObjectModels, built through the placer's
# construction-time entry channel (the one production path — never a scan).
func _index_of(entries: Array) -> EntityIndex:
	var index := EntityIndex.new()
	index.build(entries, [])
	return index


func _entry(model: ObjectModel, bms_id: int, kind: int = -1,
		index: int = -1) -> Dictionary:
	return {'model': model, 'ref': {
		'bms_id': bms_id, 'kind': kind, 'index': index,
		'group': -1, 'team': -1, 'position': Vector3.ZERO}}


func _piece(slot: int, generation: int, type_index: int, pos: Vector3,
		settled: bool = false) -> Dictionary:
	# The drain rows carry the type's trail from the ONE native table
	# (world/destruction death_piece_trail_effect, S12b) — mirror the rows
	# these tests use [orig: g_death_piece_types @ 0x8404f0 +0x2C].
	var trail_by_type := {0: "", 1: "Effect_VexpM", 2: "Effect_VexpS"}
	return {
		'slot': slot,
		'generation': generation,
		'type_index': type_index,
		'trail': String(trail_by_type.get(type_index, "")),
		'pos': pos,
		'settled': settled,
	}


func _make_fx() -> CaptureFx:
	var fx := CaptureFx.new()
	add_child_autofree(fx)
	return fx


func _make_pass(fx: CaptureFx, anchors: CaptureAnchors) -> DestructionPresentPass:
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, null, null, null, null, anchors, Callable(),
			func(): return fx)
	return presenter


# Ring-slot lifecycle regressions.
func test_active_slot_reuse_replaces_the_presented_incarnation() -> void:
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var presenter := _make_pass(fx, anchors)
	var key := 'piece:7'
	presenter.present_drained({}, [_piece(7, 11, 1, Vector3(1, 2, 3))])
	assert_eq(fx.owned_spawns.size(), 1)
	presenter.present_drained({}, [_piece(7, 11, 1, Vector3(4, 5, 6))])
	assert_eq(fx.owned_spawns.size(), 1)
	assert_eq(anchors.anchor_position(key), Vector3(4, 5, 6))
	presenter.present_drained({}, [_piece(7, 12, 2, Vector3(8, 9, 10))])
	assert_eq(fx.owned_spawns.size(), 2)
	if fx.owned_spawns.size() == 2:
		assert_eq(String(fx.owned_spawns[1]['effect']), 'Effect_VexpS')
		assert_eq(String(fx.owned_spawns[1]['owner']), key)
	assert_eq(anchors.anchor_position(key), Vector3(8, 9, 10))
	presenter.teardown()


func test_slot_reuse_to_a_no_trail_type_detaches_the_old_owner() -> void:
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var presenter := _make_pass(fx, anchors)
	var key := 'piece:3'
	presenter.present_drained({}, [_piece(3, 30, 1, Vector3(2, 3, 4))])
	assert_eq(fx.owned_spawns.size(), 1)
	assert_true(anchors.anchors.has(key))
	presenter.present_drained({}, [_piece(3, 31, 0, Vector3(8, 8, 8))])
	assert_eq(fx.owned_spawns.size(), 1)
	assert_eq(anchors.unregistrations.count(key), 1)
	assert_false(anchors.anchors.has(key))
	presenter.teardown()


func test_settled_slot_reuse_replaces_the_presented_incarnation() -> void:
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var presenter := _make_pass(fx, anchors)
	var key := 'piece:13'
	presenter.present_drained({}, [_piece(13, 20, 1, Vector3(1, 4, 2))])
	assert_eq(fx.owned_spawns.size(), 1)
	presenter.present_drained({}, [_piece(13, 20, 1, Vector3(1, 0, 2), true)])
	assert_eq(fx.owned_spawns.size(), 1)
	assert_eq(anchors.anchor_position(key), Vector3(1, 0, 2))
	presenter.present_drained({}, [_piece(13, 21, 2, Vector3(9, 3, 5))])
	assert_eq(fx.owned_spawns.size(), 2)
	if fx.owned_spawns.size() == 2:
		assert_eq(String(fx.owned_spawns[1]['effect']), 'Effect_VexpS')
	assert_eq(anchors.anchor_position(key), Vector3(9, 3, 5))
	presenter.teardown()


func test_reset_runtime_state_restores_individual_visuals_and_retires_anchors() -> void:
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var placer := _husk_placer()
	var container := Node3D.new()
	add_child_autofree(container)
	var intact := ObjectModel.new()
	container.add_child(intact)
	var originally_visible := Node3D.new()
	var originally_hidden := Node3D.new()
	originally_hidden.visible = false
	var static_caster := MeshInstance3D.new()
	static_caster.layers = Water.VISUAL_LAYER_STATIC_SHADOW_CASTER
	intact.add_child(originally_visible)
	intact.add_child(originally_hidden)
	intact.add_child(static_caster)
	var index := _index_of([_entry(intact, 41)])
	var events := {
		'husk_swaps': [{
			'bms_id': 41,
			'item_id': BUGGY_ITEM_ID,
		}],
		'effects': [{
			'effect': 'Effect_VehExplode',
			'family': 2,
			'attach_net_id': 91,
			'attach_bms_id': 41,
			'pos': Vector3(3, 4, 5),
		}],
	}
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, container, index, placer, _item_db, anchors, Callable(),
			func(): return fx)

	presenter.present_drained(events, [_piece(5, 70, 1, Vector3(6, 7, 8))])

	assert_false(originally_visible.visible)
	assert_false(originally_hidden.visible)
	assert_false(static_caster.visible,
			"the intact individual caster follows the visible model into the husk swap")
	assert_eq(_husk_models(self).size(), 1)
	assert_true(anchors.anchors.has('wreck:91:2'))
	assert_true(anchors.anchors.has('piece:5'))
	var graft: ObjectModel = _husk_models(self)[0]
	assert_true(graft.is_static_shadow_caster_enabled(),
			"the individual husk replaces the intact static silhouette")

	presenter.reset_runtime_state()

	assert_true(originally_visible.visible,
			'the intact child returns to its authored visibility')
	assert_false(originally_hidden.visible,
			'an authored hidden child is not forced visible')
	assert_true(static_caster.visible,
			"reset restores the intact caster with its visible model")
	assert_true(graft.is_queued_for_deletion())
	assert_eq(anchors.anchors.size(), 0)
	assert_true(anchors.unregistrations.has('wreck:91:2'))
	assert_true(anchors.unregistrations.has('piece:5'))
	var unregister_count := anchors.unregistrations.size()
	presenter.reset_runtime_state()
	assert_eq(anchors.unregistrations.size(), unregister_count,
			'reset is public and idempotent')


func test_reset_runtime_state_restores_batched_static_and_removes_husk_graft() -> void:
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var placer := _husk_placer()
	var container := Node3D.new()
	add_child_autofree(container)
	var placed := Transform3D(Basis.IDENTITY, Vector3(9, 8, 7))
	placer.register_static_instance(77, 'StaticProp', 0, placed, true)
	var events := {
		'husk_swaps': [{
			'bms_id': 77,
			'item_id': BUGGY_ITEM_ID,
		}],
	}
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, container, _index_of([]), placer, _item_db, anchors,
			Callable(), func(): return fx)

	presenter.present_drained(events, [])

	assert_true(placer.is_static_instance_hidden(77))
	assert_eq(_husk_models(self).size(), 1)
	var graft: ObjectModel = _husk_models(self)[0]
	assert_eq(graft.transform, placed)
	assert_true(graft.is_static_shadow_caster_enabled(),
			"the batched husk inherits the carved slot's static-caster admission")

	presenter.reset_runtime_state()

	assert_false(placer.is_static_instance_hidden(77),
			'the pass uses the placer public inverse to restore the static')
	assert_true(graft.is_queued_for_deletion())


func test_failed_individual_husk_build_keeps_the_intact_visual_visible() -> void:
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var placer := _husk_placer(false)
	var container := Node3D.new()
	add_child_autofree(container)
	var intact := ObjectModel.new()
	container.add_child(intact)
	var visual := Node3D.new()
	intact.add_child(visual)
	var index := _index_of([_entry(intact, 41)])
	var events := {
		'husk_swaps': [{
			'bms_id': 41,
			'item_id': BUGGY_ITEM_ID,
		}],
	}
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, container, index, placer, _item_db, anchors,
			Callable(), func(): return fx)

	presenter.present_drained(events, [])

	assert_true(visual.visible,
			'a failed graft build must leave the retail intact-graphic fallback standing')
	assert_eq(_husk_models(self).size(), 0)
	assert_eq(presenter.get_stats().no_husk, 1)
	presenter.teardown()


func test_failed_batched_husk_build_does_not_carve_the_static_instance() -> void:
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var placer := _husk_placer(false)
	placer.register_static_instance(77, 'StaticProp', 0,
			Transform3D(Basis.IDENTITY, Vector3(9, 8, 7)), false)
	var container := Node3D.new()
	add_child_autofree(container)
	var events := {
		'husk_swaps': [{
			'bms_id': 77,
			'item_id': BUGGY_ITEM_ID,
		}],
	}
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, container, _index_of([]), placer, _item_db, anchors,
			Callable(), func(): return fx)

	presenter.present_drained(events, [])

	assert_false(placer.is_static_instance_hidden(77),
			'a failed graft build must not carve a visible hole in a static batch')
	assert_eq(container.get_child_count(), 0)
	assert_eq(presenter.get_stats().no_husk, 1)
	presenter.teardown()


func test_zero_bms_husks_use_distinct_spawn_origins_for_identity_and_lookup() -> void:
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var placer := _husk_placer()
	var container := Node3D.new()
	add_child_autofree(container)
	var first := ObjectModel.new()
	var first_visual := Node3D.new()
	first.add_child(first_visual)
	container.add_child(first)
	var second := ObjectModel.new()
	var second_visual := Node3D.new()
	second.add_child(second_visual)
	container.add_child(second)
	var index := _index_of([_entry(first, 0, 3, 5), _entry(second, 0, 4, 6)])
	var first_origin := (3 << 24) | 5
	var second_origin := (4 << 24) | 6
	var events := {
		'husk_swaps': [{
			'bms_id': 0,
			'spawn_origin': first_origin,
			'item_id': BUGGY_ITEM_ID,
		}, {
			'bms_id': 0,
			'spawn_origin': second_origin,
			'item_id': BUGGY_ITEM_ID,
		}],
	}
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, container, index, placer, _item_db, anchors,
			Callable(), func(): return fx)

	presenter.present_drained(events, [])

	assert_eq(_husk_models(self).size(), 2,
			'distinct zero-BMS entities own distinct husk cache entries')
	assert_false(first_visual.visible,
			'the first event resolved its own node through its decoded mission origin')
	assert_false(second_visual.visible,
			'the second event resolved its own node through its decoded mission origin')
	assert_false(placer.is_static_instance_hidden(0),
			'individual origin fallback does not touch the static batch map')
	presenter.teardown()
	assert_true(first_visual.visible)
	assert_true(second_visual.visible,
			'each canonical owner retains its own intact-visibility restore state')


func test_synthetic_husks_use_distinct_wire_handles_for_identity_and_lookup() -> void:
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var placer := _husk_placer()
	var container := Node3D.new()
	add_child_autofree(container)
	var first := ObjectModel.new()
	var first_visual := Node3D.new()
	first.add_child(first_visual)
	container.add_child(first)
	var second := ObjectModel.new()
	var second_visual := Node3D.new()
	second.add_child(second_visual)
	container.add_child(second)
	var resolver := _wire_resolver({ 0x1004: first, 0x1005: second })
	# Pre-fix fallback: both synthetic children collapse to this same authored
	# identity because they have no BMS id and share the non-BMS origin sentinel.
	var index := _index_of([_entry(first, 0, 255, 16777215)])
	var events := {
		'husk_swaps': [{
			'bms_id': 0,
			'spawn_origin': SpawnOrigin.NONE,
			'wire_handle': 0x1004,
			'item_id': BUGGY_ITEM_ID,
		}, {
			'bms_id': 0,
			'spawn_origin': SpawnOrigin.NONE,
			'wire_handle': 0x1005,
			'item_id': BUGGY_ITEM_ID,
		}],
	}
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, container, index, placer, _item_db, anchors,
			Callable(), func(): return fx, resolver)

	presenter.present_drained(events, [])

	assert_eq(_husk_models(self).size(), 2,
			'distinct synthetic entities own distinct husk cache entries')
	assert_false(first_visual.visible)
	assert_false(second_visual.visible)
	presenter.teardown()
	assert_true(first_visual.visible)
	assert_true(second_visual.visible,
			'each dynamic owner retains its own intact-visibility restore state')


func test_missing_synthetic_husk_node_never_falls_back_to_static_zero_id() -> void:
	var resolver := _wire_resolver()
	var placer := _husk_placer()
	var container := Node3D.new()
	add_child_autofree(container)
	# BMS id zero is valid for an authored static. A missing runtime wire node
	# must not carve this unrelated instance or resolve through the sentinel.
	placer.register_static_instance(0, 'StaticProp', 0, Transform3D.IDENTITY,
			false)
	var authored_zero := ObjectModel.new()
	add_child_autofree(authored_zero)
	var index := _index_of([_entry(authored_zero, 0, 255, 16777215)])
	var events := {
		'husk_swaps': [{
			'bms_id': 0,
			'spawn_origin': SpawnOrigin.NONE,
			'wire_handle': 0x1004,
			'item_id': BUGGY_ITEM_ID,
		}],
	}
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, container, index, placer, _item_db,
			CaptureAnchors.new(), Callable(), Callable(), resolver)

	presenter.present_drained(events, [])

	assert_false(placer.is_static_instance_hidden(0),
			'a missing wire node cannot hide the authored static with BMS id zero')
	assert_true(_husk_models(self).is_empty(),
			'no unattached husk graft is built for a retired runtime row')
	assert_eq(presenter.get_stats().no_husk, 1)


func test_synthetic_wreck_families_use_distinct_moving_wire_anchors() -> void:
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var first := ObjectModel.new()
	var second := ObjectModel.new()
	container.add_child(first)
	container.add_child(second)
	first.position = Vector3(1, 2, 3)
	second.position = Vector3(7, 8, 9)
	var resolver := _wire_resolver({ 0x1004: first, 0x1005: second })
	var effects: Array = []
	for wire_handle in [0x1004, 0x1005]:
		for family in [1, 2, 3]:
			effects.append({
				'effect': 'Effect_Family%d' % family,
				'family': family,
				'attach_net_id': 0,
				'attach_bms_id': 0,
				'attach_wire_handle': wire_handle,
				'attach_spawn_origin': SpawnOrigin.NONE,
				'pos': Vector3(100, 100, 100),
			})
	# Even when a payload happens to carry a valid dynamic identity, family zero
	# remains the one-shot transient path.
	effects.append({
		'effect': 'Effect_Transient',
		'family': 0,
		'attach_net_id': 0,
		'attach_bms_id': 0,
		'attach_wire_handle': 0x1004,
		'attach_spawn_origin': SpawnOrigin.NONE,
		'pos': Vector3(20, 30, 40),
	})
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, container, _index_of([]), _husk_placer(),
			_item_db, anchors, Callable(), func(): return fx, resolver)

	presenter.present_drained({'effects': effects}, [])

	assert_eq(fx.owned_spawns.size(), 6,
			'all attached death/fire/other banks stay owned for zero-net children')
	assert_eq(fx.spawns.size(), 1, 'family-zero effects remain transient')
	assert_eq(fx.spawns[0]['position'], Vector3(20, 30, 40))
	for wire_handle in [0x1004, 0x1005]:
		var node: Node3D = resolver.resolve_wire_handle(wire_handle)
		for family in [1, 2, 3]:
			var key := 'wreck:wire:%d:%d' % [wire_handle, family]
			assert_true(anchors.anchors.has(key),
					'each sibling/family pair owns a distinct effect group')
			assert_eq(anchors.anchor_position(key), node.global_transform)
		var fire_key := 'wreck:wire:%d:2' % wire_handle
		assert_true(presenter.has_active_wreck_fire(fire_key),
				'family 2 enters the per-owner wreck crackle set')
	first.position = Vector3(-3, 4, 5)
	second.position = Vector3(12, -2, 6)
	assert_eq(anchors.anchor_position('wreck:wire:4100:1'), first.global_transform)
	assert_eq(anchors.anchor_position('wreck:wire:4101:3'), second.global_transform,
			'owned effects follow the live WirePresent nodes after they move')
	presenter.teardown()


func test_batched_husk_and_wreck_anchor_follow_the_live_present_pose() -> void:
	# The REAL pose chain: an authored building boots into a real Simulation
	# (through MissionPresentation, the production owner), and the node-less batched
	# husk graft plus its owned wreck anchor land on the sim's authoritative
	# present-effect pose — the origin-keyed leg — rather than the carved batch
	# transform. The authored yaw-only entity pose additionally pins the
	# tilt-retention rule: position comes live, while pitch/roll keep the exact
	# carved basis (host/listen poses with real pitch/roll take the live-basis
	# path; peer compact poses are yaw-only by construction).
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	# A real organic entity: organics boot without item defs and their live
	# pose is inherently yaw-only, exactly the compact-peer shape the
	# tilt-retention rule keys on.
	mission.add_entity(3, 0, Vector3(12, 3, -7), Vector3(0, 40, 0))  # KIND_ORGANIC
	var boot_container := Node3D.new()
	add_child_autofree(boot_container)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(mission, boot_container, {})
	var sim := rt.get_sim()
	assert_not_null(sim, 'the runtime boots a real simulation over the mission')
	rt.play()
	var frame_input := MissionFrameInput.new()
	frame_input.delta_seconds = Simulation.tick_dt()
	rt.advance_session_frame(frame_input)
	var spawn_origin := (3 << 24) | 0
	var state: PackedVector3Array = sim.get_present_effect_state_for_origin(3, 0)
	assert_eq(state.size(), 2,
			'the booted entity publishes a present-effect pose by origin')
	if state.size() != 2:
		return
	assert_almost_eq(state[1].x, 0.0, 0.001, 'the authored pose is yaw-only')
	assert_almost_eq(state[1].z, 0.0, 0.001, 'the authored pose is yaw-only')

	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var placer := _husk_placer()
	var authored_rot := Vector3(17, 40, -12)
	var authored := Transform3D(
			MissionObjectPlacer.bms_to_godot_basis(authored_rot), Vector3(9, 8, 7))
	placer.register_static_instance(0, 'StaticProp', 0, authored, false)
	var container := Node3D.new()
	add_child_autofree(container)
	var events := {
		'husk_swaps': [{
			'bms_id': 0,
			'spawn_origin': spawn_origin,
			'item_id': BUGGY_ITEM_ID,
		}],
		'effects': [{
			'effect': 'Effect_VehExplode',
			'family': 2,
			'attach_net_id': 91,
			'attach_bms_id': 0,
			'attach_spawn_origin': spawn_origin,
			'pos': Vector3(100, 100, 100),
		}],
	}
	var presenter := DestructionPresentPass.new()
	presenter.setup(sim, container, _index_of([]), placer, _item_db, anchors,
			Callable(), func(): return fx)

	presenter.present_drained(events, [])

	assert_true(placer.is_static_instance_hidden(0),
			'the node-less husk carves the batch slot')
	assert_eq(_husk_models(self).size(), 1)
	if _husk_models(self).is_empty():
		return
	var graft: ObjectModel = _husk_models(self)[0]
	var expected := Transform3D(authored.basis, state[0])
	assert_true(graft.transform.is_equal_approx(expected),
			'the graft takes the live present origin while keeping the authored tilt')
	var anchor_pose: Variant = anchors.anchor_position('wreck:91:2')
	assert_true(anchor_pose is Transform3D)
	if anchor_pose is Transform3D:
		assert_true((anchor_pose as Transform3D).is_equal_approx(expected),
				'the owned wreck effect resolves the same live present pose')
	presenter.teardown()


func test_zero_origin_debris_event_is_not_discarded() -> void:
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var presenter := _make_pass(fx, anchors)

	presenter.present_drained({
		'debris_bursts': [{
			'bms_id': 0,
			'pos': Vector3.ZERO,
		}],
	}, [])

	assert_eq(presenter.get_stats().bursts, 1)
	assert_eq(fx.spawns.size(), DestructionPresentPass.BURST_COUNT,
			'world origin is a valid authored destruction position')
	presenter.teardown()
