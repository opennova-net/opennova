extends GutTest

# The destruction present pass (EntityPresenter's DestructionPresenter member,
# ADR 0043 d9) on the typed surfaces (ADR 0034): event/piece rows are pure
# data through the public present_destruction_drained data leg (production
# present_passes() drains the typed Simulation), the collaborators are a real
# MissionObjectPlacer plus a REAL EffectWorld over an in-memory catalog and a
# REAL ItemEffectDirector (ADR 0043 rule 11: the group report, the anchor
# registry and the owner-pose resolve are the read seams), the entity index is
# a real EntityIndex over real ObjectModels, the runtime-only resolver is the
# presenter's own wire registry with injected nodes, and the item database is
# the real fixture items.def (item 1291 = the dune buggy with husk Dbuggy1X).
#
# The live settling-pose follow (a REAL sim's present-effect state feeding the
# node-less graft) is exercised by the real-sim test at the bottom; the
# injected-pose multi-update follow of the old sim double had no typed
# equivalent without native pose injection and is covered there by the sim's
# authoritative initial pose plus the yaw-only tilt-retention leg.


const BUGGY_ITEM_ID := 1291  # fixture items.def 101291, husk Dbuggy1X
const POSITION_EPS := Vector3(0.001, 0.001, 0.001)

# Every effect name the drained rows below spawn: the piece-type trails, the
# wreck families, and the resolved debris/glass transients.
const CATALOG_EFFECTS: PackedStringArray = [
	'Effect_VexpM', 'Effect_VexpS', 'Effect_VehExplode',
	'Effect_Family1', 'Effect_Family2', 'Effect_Family3', 'Effect_Transient',
	'Effect_TreeFoliageExp', 'Effect_BldGlassExp']

static var _item_db: ItemDatabase = null


func before_all() -> void:
	_item_db = ItemDatabase.new()
	assert_eq(_item_db.load(ProjectSettings.globalize_path(
			'res://../fixtures/def/items.def')), OK,
			'the fixture items.def loads (101291 carries husk Dbuggy1X)')


# One synthetic in-memory particle catalog authoring every effect the rows
# name (the effect_world_test recipe): one FOREVEREMIT definition shared by
# one effect per name, so the real EffectWorld interns and spawns them without
# a resource root and an owned group stays live until its owner is retired.
func _catalog_file(effect_names: PackedStringArray) -> ParticleFile:
	var def := ParticleDef.new()
	def.id = 'wreck dots'
	def.emit_dur = 0.1
	def.emit_rate = 50.0
	def.emit_burst = 4
	def.age = 0.2
	def.alpha = 1.0
	def.scale_value = 1.0
	def.flags = ParticleDef.FLAG_FOREVER_EMIT
	var file := ParticleFile.new()
	var particles: Array = file.particles
	particles.append(def)
	file.particles = particles
	var effects: Array = file.effects
	for effect_name in effect_names:
		var effect := ParticleEffect.new()
		effect.id = effect_name
		effect.pdefs = PackedStringArray(['wreck dots'])
		effects.append(effect)
	file.effects = effects
	return file


# A REAL EffectWorld over the in-memory catalog, its owner poses resolved by
# `anchors` (the production wiring ItemEffectDirector.on_effect_world_started
# performs for GameWorld's world): advance_fixed_tick(0.0) is the sync.
func _make_fx(anchors: ItemEffectDirector) -> EffectWorld:
	var fx := EffectWorld.new()
	add_child_autofree(fx)
	fx.load_particle_file(_catalog_file(CATALOG_EFFECTS))
	fx.set_owner_position_provider(anchors.resolve_owner_transform)
	return fx


# The live (still attached) group report row owned by `key`, or {} when none.
func _live_owned_row(fx: EffectWorld, key: String) -> EffectGroupReport:
	for row_v in fx.get_debug_group_report():
		var row := row_v as EffectGroupReport
		if row.owner_key == key and not row.detached:
			return row
	return null


# Every group report row owned by `key`, attached or detached.
func _owned_rows(fx: EffectWorld, key: String) -> Array:
	var out: Array = []
	for row_v in fx.get_debug_group_report():
		var row := row_v as EffectGroupReport
		if row.owner_key == key:
			out.append(row)
	return out


# Every unowned (transient) group report row.
func _transient_rows(fx: EffectWorld) -> Array:
	var out: Array = []
	for row_v in fx.get_debug_group_report():
		var row := row_v as EffectGroupReport
		if row.owner_key == null:
			out.append(row)
	return out


# The group report row with `group_id`, or {} once swept.
func _row_by_id(fx: EffectWorld, group_id: int) -> EffectGroupReport:
	for row_v in fx.get_debug_group_report():
		var row := row_v as EffectGroupReport
		if int(row.id) == group_id:
			return row
	return null


# Whether the group with `group_id` is detached; `swept` once the report no
# longer lists it.
func _row_detached(fx: EffectWorld, group_id: int, swept: bool = false) -> bool:
	var row := _row_by_id(fx, group_id)
	return swept if row == null else row.detached


func _emitter_position(row: EffectGroupReport) -> Vector3:
	if row == null or row.emitters.is_empty():
		return Vector3.INF
	return (row.emitters[0] as EffectEmitterReport).position


func _emitter_forward(row: EffectGroupReport) -> Vector3:
	if row == null or row.emitters.is_empty():
		return Vector3.INF
	return (row.emitters[0] as EffectEmitterReport).forward


# A REAL EntityPresenter with its destruction pass wired: `sim` (nullable) is
# the live pose source, `container` hosts the node-less husk grafts, `index`
# resolves authored entities, `placer`/`item_db` build the husk models,
# `anchors` is the owner-anchor registry, `fx` the effect world; `wire_nodes`
# are injected per-handle avatars for the runtime-only resolve (native
# methods cannot be intercepted from GDScript; register_wire_node is the seam).
func _make_presenter(sim: Simulation, container: Node3D, index: EntityIndex,
		placer: MissionObjectPlacer, item_db: ItemDatabase, anchors: ItemEffectDirector,
		fx: EffectWorld, wire_nodes: Dictionary = {}) -> EntityPresenter:
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(sim, index, placer)
	for handle in wire_nodes:
		presenter.register_wire_node(int(handle), wire_nodes[handle])
	presenter.setup_passes(container, item_db, null, null, fx, null, null, anchors)
	return presenter


const HUSK_GRAPHIC := 'Dbuggy1X'  # fixture items.def 101291's husk stage
const HUSK_MODEL_3DI := 'res://../fixtures/threedi/synth/armory.3di'


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
		index: int = -1) -> ObjectModel:
	model.entity_ref = EntityRef.make(kind, index, bms_id)
	return model


func _piece(slot: int, generation: int, type_index: int, pos: Vector3,
		settled: bool = false) -> DeathPieceRow:
	# The rows resolve the type's trail through the ONE native table
	# (world/destruction death_piece_trail_effect, S12b): types 1 and 2 of
	# these tests author Effect_VexpM / Effect_VexpS, type 0 none
	# [orig: g_death_piece_types @ 0x8404f0 +0x2C].
	return DeathPieceRow.make(slot, generation, type_index, pos, settled)


func _make_pass(fx: EffectWorld, anchors: ItemEffectDirector) -> EntityPresenter:
	return _make_presenter(null, null, null, null, null, anchors, fx)


# Ring-slot lifecycle regressions.
func test_active_slot_reuse_replaces_the_presented_incarnation() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var presenter := _make_pass(fx, anchors)
	var key := 'piece:7'
	presenter.present_destruction_drained(null, [_piece(7, 11, 1, Vector3(1, 2, 3))])
	var first := _live_owned_row(fx, key)
	assert_not_null(first, 'a new piece spawns its trail as one owned group')
	assert_eq(_owned_rows(fx, key).size(), 1)
	assert_eq(first.name, 'Effect_VexpM')
	var first_id := int(first.id)
	presenter.present_destruction_drained(null, [_piece(7, 11, 1, Vector3(4, 5, 6))])
	assert_eq(_owned_rows(fx, key).size(), 1, 'the same generation never respawns')
	assert_eq(anchors.resolve_owner_transform(key), Vector3(4, 5, 6))
	fx.advance_fixed_tick(0.0)
	assert_almost_eq(_emitter_position(_live_owned_row(fx, key)), Vector3(4, 5, 6),
			POSITION_EPS, 'the presented incarnation follows the live piece pose')
	presenter.present_destruction_drained(null, [_piece(7, 12, 2, Vector3(8, 9, 10))])
	assert_eq(_owned_rows(fx, key).size(), 2, 'a new generation spawns a fresh incarnation')
	var replacement := _live_owned_row(fx, key)
	assert_not_null(replacement)
	assert_eq(replacement.name, 'Effect_VexpS')
	assert_ne(int(replacement.id), first_id)
	assert_true(_row_detached(fx, first_id),
			'the replaced incarnation is detached, never re-posed')
	assert_eq(anchors.resolve_owner_transform(key), Vector3(8, 9, 10))
	presenter.teardown()


func test_slot_reuse_to_a_no_trail_type_detaches_the_old_owner() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var presenter := _make_pass(fx, anchors)
	var key := 'piece:3'
	presenter.present_destruction_drained(null, [_piece(3, 30, 1, Vector3(2, 3, 4))])
	assert_eq(_owned_rows(fx, key).size(), 1)
	assert_true(anchors.has_effect_anchor(key))
	presenter.present_destruction_drained(null, [_piece(3, 31, 0, Vector3(8, 8, 8))])
	assert_eq(_owned_rows(fx, key).size(), 1, 'a no-trail type spawns nothing')
	assert_false(anchors.has_effect_anchor(key), 'the outgoing owner retires its anchor')
	# With no anchor left, the next fixed-tick owner sync resolves nothing for
	# the key and the old group detaches.
	fx.advance_fixed_tick(0.0)
	var rows := _owned_rows(fx, key)
	assert_eq(rows.size(), 1)
	if rows.size() == 1:
		assert_true((rows[0] as EffectGroupReport).detached,
				'the old owner is detached instead of riding the reused slot')
	presenter.teardown()


func test_settled_slot_reuse_replaces_the_presented_incarnation() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var presenter := _make_pass(fx, anchors)
	var key := 'piece:13'
	presenter.present_destruction_drained(null, [_piece(13, 20, 1, Vector3(1, 4, 2))])
	assert_eq(_owned_rows(fx, key).size(), 1)
	var first_id := int(_live_owned_row(fx, key).id)
	presenter.present_destruction_drained(null, [_piece(13, 20, 1, Vector3(1, 0, 2), true)])
	assert_eq(_owned_rows(fx, key).size(), 1, 'settling never respawns the incarnation')
	assert_eq(anchors.resolve_owner_transform(key), Vector3(1, 0, 2))
	fx.advance_fixed_tick(0.0)
	assert_almost_eq(_emitter_position(_live_owned_row(fx, key)), Vector3(1, 0, 2),
			POSITION_EPS, 'the settled piece keeps presenting at its rest pose')
	presenter.present_destruction_drained(null, [_piece(13, 21, 2, Vector3(9, 3, 5))])
	assert_eq(_owned_rows(fx, key).size(), 2)
	var replacement := _live_owned_row(fx, key)
	assert_not_null(replacement)
	assert_eq(replacement.name, 'Effect_VexpS')
	assert_true(_row_detached(fx, first_id),
			'the settled incarnation is replaced, not revived')
	assert_eq(anchors.resolve_owner_transform(key), Vector3(9, 3, 5))
	presenter.teardown()


func test_reset_runtime_state_restores_individual_visuals_and_retires_anchors() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
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
	var events := DestructionDrain.make(
			[HuskSwapEvent.make(41, BUGGY_ITEM_ID)],
			[DestructionEffectEvent.make('Effect_VehExplode', Vector3(3, 4, 5), 2, Vector3.ZERO, 91, 41)])
	var presenter := _make_presenter(null, container, index, placer, _item_db, anchors, fx)

	presenter.present_destruction_drained(events, [_piece(5, 70, 1, Vector3(6, 7, 8))])

	assert_false(originally_visible.visible)
	assert_false(originally_hidden.visible)
	assert_false(static_caster.visible,
			"the intact individual caster follows the visible model into the husk swap")
	assert_eq(_husk_models(self).size(), 1)
	assert_true(anchors.has_effect_anchor('wreck:91:2'))
	assert_true(anchors.has_effect_anchor('piece:5'))
	assert_not_null(_live_owned_row(fx, 'wreck:91:2'),
			'the fire family spawned its owned wreck group')
	assert_not_null(_live_owned_row(fx, 'piece:5'),
			'the piece spawned its owned trail group')
	var graft: ObjectModel = _husk_models(self)[0]
	assert_true(graft.is_static_shadow_caster_enabled(),
			"the individual husk replaces the intact static silhouette")

	presenter.reset_wire_runtime_state()

	assert_true(originally_visible.visible,
			'the intact child returns to its authored visibility')
	assert_false(originally_hidden.visible,
			'an authored hidden child is not forced visible')
	assert_true(static_caster.visible,
			"reset restores the intact caster with its visible model")
	assert_true(graft.is_queued_for_deletion())
	assert_false(anchors.has_effect_anchor('wreck:91:2'), 'reset retires the wreck anchor')
	assert_false(anchors.has_effect_anchor('piece:5'), 'reset retires the piece anchor')
	# Retired anchors resolve nothing: the next fixed-tick owner sync detaches
	# both owned groups from the prior incarnation.
	fx.advance_fixed_tick(0.0)
	assert_null(_live_owned_row(fx, 'wreck:91:2'),
			'the retired wreck group no longer follows an owner')
	assert_null(_live_owned_row(fx, 'piece:5'),
			'the retired piece group no longer follows an owner')
	# The old call-count ledger has no public read; the idempotence pin is that
	# a second reset leaves the restored state exactly as it was.
	presenter.reset_wire_runtime_state()
	assert_true(originally_visible.visible, 'reset is public and idempotent')
	assert_false(originally_hidden.visible)
	assert_true(static_caster.visible)
	assert_false(anchors.has_effect_anchor('wreck:91:2'))
	assert_false(anchors.has_effect_anchor('piece:5'))


func test_reset_wire_runtime_state_clears_the_wreck_fire_registry() -> void:
	# The Stop -> Play boundary reaches the destruction pass through the
	# presenter's one reset (the restart signal's single connect): the burning
	# wreck registry the crackle updates read through has_active_wreck_fire
	# empties with the anchors.
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var container := Node3D.new()
	add_child_autofree(container)
	var wreck := ObjectModel.new()
	container.add_child(wreck)
	var presenter := _make_presenter(null, container, _index_of([]), _husk_placer(),
			_item_db, anchors, fx, { 0x1004: wreck })
	var fire_key := 'wreck:wire:4100:2'
	presenter.present_destruction_drained(DestructionDrain.make([], [
			DestructionEffectEvent.make('Effect_Family2', Vector3(1, 1, 1),
					2, Vector3.ZERO, 0, 0, 0x1004, Simulation.SPAWN_ORIGIN_NONE)]), [])
	assert_true(presenter.has_active_wreck_fire(fire_key),
			'the presented fire family enters the wreck crackle set')
	assert_true(anchors.has_effect_anchor(fire_key))

	presenter.reset_wire_runtime_state()

	assert_false(presenter.has_active_wreck_fire(fire_key),
			'reset empties the wreck-fire registry')
	assert_false(anchors.has_effect_anchor(fire_key), 'reset retires the wreck anchor')
	presenter.teardown()


func test_individual_husk_keeps_the_intact_models_mirror_population() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var placer := _husk_placer()
	var container := Node3D.new()
	add_child_autofree(container)
	var intact := ObjectModel.new()
	intact.mirror_reflected = true
	container.add_child(intact)
	var index := _index_of([_entry(intact, 41)])
	var events := DestructionDrain.make([HuskSwapEvent.make(41, BUGGY_ITEM_ID)])
	var presenter := _make_presenter(null, container, index, placer, _item_db, anchors, fx)

	presenter.present_destruction_drained(events, [])

	assert_eq(_husk_models(self).size(), 1)
	if _husk_models(self).size() == 1:
		var graft := _husk_models(self)[0] as ObjectModel
		assert_true(graft.mirror_reflected,
				"an individual husk inherits the intact entity's witnessed reflect bit")
		var surfaces := graft.find_children("*", "MeshInstance3D", true, false)
		assert_gt(surfaces.size(), 0, "the replacement fixture builds render surfaces")
		for surface in surfaces:
			var mesh := surface as MeshInstance3D
			assert_ne(mesh.layers & Water.VISUAL_LAYER_WORLD, 0,
					"the replacement is admitted by the above-water mirror camera")
			assert_eq(mesh.layers & Water.VISUAL_LAYER_WORLD_NO_MIRROR, 0,
					"the replacement leaves the main-scene-only population")
	presenter.teardown()


func test_husk_swap_does_not_rescan_or_rebind_authored_lght() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child(world)
	var root := MissionRoot.new()
	root.name = "MissionRoot"
	world.add_child(root)
	var container := Node3D.new()
	container.name = "MissionObjects"
	root.add_child(container)
	var intact := ObjectModel.new()
	container.add_child(intact)
	var intact_data := ObjectData.new()
	assert_eq(intact_data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth/shed.3di")), OK)
	assert_eq(intact_data.get_light_count(), 1)
	intact.set_object_data(intact_data)
	intact.entity_ref = EntityRef.make(MissionData.KIND_ITEM, -1, 41, 0, 73)
	var director := EffectLightDirector.new()
	director.setup(world, Callable(), Callable())
	director.on_wire_node_spawned(intact, MissionData.KIND_ITEM, BUGGY_ITEM_ID)
	assert_eq(director.get_report().live, 1,
			"the intact graphic contributes its one authored LGHT")

	var presenter := _make_presenter(null, container, _index_of([_entry(intact, 41)]),
			_husk_placer(), _item_db, ItemEffectDirector.new(), null)
	presenter.present_destruction_drained(DestructionDrain.make([HuskSwapEvent.make(41, BUGGY_ITEM_ID)]), [])
	assert_eq(_husk_models(world).size(), 1,
			"the production destruction pass built the authored husk graft")
	assert_eq(director.get_report().live, 1,
			"a husk swap leaves the intact spawn-fixed LGHT and scans no husk LGHT")
	presenter.reset_wire_runtime_state()
	assert_eq(director.get_report().live, 1,
			"restoring the intact graphic performs no authored-light respawn")
	presenter.teardown()
	intact.queue_free()
	await get_tree().process_frame
	assert_eq(director.get_report().live, 0,
			"actual entity retirement clears its one cached EffectWorld handle")
	world.queue_free()
	await get_tree().process_frame


func test_reset_runtime_state_restores_batched_static_and_removes_husk_graft() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var placer := _husk_placer()
	var container := Node3D.new()
	add_child_autofree(container)
	var placed := Transform3D(Basis.IDENTITY, Vector3(9, 8, 7))
	placer.register_static_instance(77, 'StaticProp', 0, placed, true, true)
	var events := DestructionDrain.make([HuskSwapEvent.make(77, BUGGY_ITEM_ID)])
	var presenter := _make_presenter(null, container, _index_of([]), placer, _item_db, anchors, fx)

	presenter.present_destruction_drained(events, [])

	assert_true(placer.is_static_instance_hidden(77))
	assert_eq(_husk_models(self).size(), 1)
	var graft: ObjectModel = _husk_models(self)[0]
	assert_eq(graft.transform, placed)
	assert_true(graft.is_static_shadow_caster_enabled(),
			"the batched husk inherits the carved slot's static-caster admission")
	assert_true(bool(graft.get("mirror_reflected")),
			"the husk keeps the carved slot's reflect policy: destruction "
			+ "never clears entity flag 0x400 [orig: Entity_SpawnFromBMSRecord "
			+ "@ 0x40ed1d..0x40ed2b]")

	presenter.reset_wire_runtime_state()

	assert_false(placer.is_static_instance_hidden(77),
			'the pass uses the placer public inverse to restore the static')
	assert_true(graft.is_queued_for_deletion())


func test_failed_individual_husk_build_keeps_the_intact_visual_visible() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var placer := _husk_placer(false)
	var container := Node3D.new()
	add_child_autofree(container)
	var intact := ObjectModel.new()
	container.add_child(intact)
	var visual := Node3D.new()
	intact.add_child(visual)
	var index := _index_of([_entry(intact, 41)])
	var events := DestructionDrain.make([HuskSwapEvent.make(41, BUGGY_ITEM_ID)])
	var presenter := _make_presenter(null, container, index, placer, _item_db, anchors, fx)

	presenter.present_destruction_drained(events, [])

	assert_true(visual.visible,
			'a failed graft build must leave the retail intact-graphic fallback standing')
	assert_eq(_husk_models(self).size(), 0)
	assert_eq(presenter.get_destruction_present_stats().no_husk, 1)
	presenter.teardown()


func test_failed_batched_husk_build_does_not_carve_the_static_instance() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var placer := _husk_placer(false)
	placer.register_static_instance(77, 'StaticProp', 0,
			Transform3D(Basis.IDENTITY, Vector3(9, 8, 7)), false)
	var container := Node3D.new()
	add_child_autofree(container)
	var events := DestructionDrain.make([HuskSwapEvent.make(77, BUGGY_ITEM_ID)])
	var presenter := _make_presenter(null, container, _index_of([]), placer, _item_db, anchors, fx)

	presenter.present_destruction_drained(events, [])

	assert_false(placer.is_static_instance_hidden(77),
			'a failed graft build must not carve a visible hole in a static batch')
	assert_true(_husk_models(container).is_empty(),
			'no husk graft lands in the container (its only child is the fire pass geometry)')
	assert_eq(presenter.get_destruction_present_stats().no_husk, 1)
	presenter.teardown()


func test_zero_bms_husks_use_distinct_spawn_origins_for_identity_and_lookup() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
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
	var events := DestructionDrain.make(
			[
					HuskSwapEvent.make(0, BUGGY_ITEM_ID, first_origin),
					HuskSwapEvent.make(0, BUGGY_ITEM_ID, second_origin)])
	var presenter := _make_presenter(null, container, index, placer, _item_db, anchors, fx)

	presenter.present_destruction_drained(events, [])

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
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
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
	# Pre-fix fallback: both synthetic children collapse to this same authored
	# identity because they have no BMS id and share the non-BMS origin sentinel.
	var index := _index_of([_entry(first, 0, 255, 16777215)])
	var events := DestructionDrain.make(
			[
					HuskSwapEvent.make(0, BUGGY_ITEM_ID, Simulation.SPAWN_ORIGIN_NONE, 0x1004),
					HuskSwapEvent.make(0, BUGGY_ITEM_ID, Simulation.SPAWN_ORIGIN_NONE, 0x1005)])
	var presenter := _make_presenter(null, container, index, placer, _item_db, anchors, fx,
			{ 0x1004: first, 0x1005: second })

	presenter.present_destruction_drained(events, [])

	assert_eq(_husk_models(self).size(), 2,
			'distinct synthetic entities own distinct husk cache entries')
	assert_false(first_visual.visible)
	assert_false(second_visual.visible)
	presenter.teardown()
	assert_true(first_visual.visible)
	assert_true(second_visual.visible,
			'each dynamic owner retains its own intact-visibility restore state')


func test_missing_synthetic_husk_node_never_falls_back_to_static_zero_id() -> void:
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
	var events := DestructionDrain.make([HuskSwapEvent.make(0, BUGGY_ITEM_ID, Simulation.SPAWN_ORIGIN_NONE, 0x1004)])
	var presenter := _make_presenter(null, container, index, placer, _item_db,
			ItemEffectDirector.new(), null)

	presenter.present_destruction_drained(events, [])

	assert_false(placer.is_static_instance_hidden(0),
			'a missing wire node cannot hide the authored static with BMS id zero')
	assert_true(_husk_models(self).is_empty(),
			'no unattached husk graft is built for a retired runtime row')
	assert_eq(presenter.get_destruction_present_stats().no_husk, 1)


func test_synthetic_wreck_families_use_distinct_moving_wire_anchors() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var container := Node3D.new()
	add_child_autofree(container)
	var first := ObjectModel.new()
	var second := ObjectModel.new()
	container.add_child(first)
	container.add_child(second)
	first.position = Vector3(1, 2, 3)
	second.position = Vector3(7, 8, 9)
	var effects: Array = []
	for wire_handle in [0x1004, 0x1005]:
		for family in [1, 2, 3]:
			effects.append(DestructionEffectEvent.make(
					'Effect_Family%d' % family, Vector3(100, 100, 100),
					family, Vector3.ZERO, 0, 0, wire_handle, Simulation.SPAWN_ORIGIN_NONE))
	# Even when a payload happens to carry a valid dynamic identity, family zero
	# remains the one-shot transient path.
	effects.append(DestructionEffectEvent.make(
			'Effect_Transient', Vector3(20, 30, 40),
			0, Vector3.ZERO, 0, 0, 0x1004, Simulation.SPAWN_ORIGIN_NONE))
	var presenter := _make_presenter(null, container, _index_of([]), _husk_placer(),
			_item_db, anchors, fx, { 0x1004: first, 0x1005: second })

	presenter.present_destruction_drained(DestructionDrain.make([], effects), [])

	var owned := 0
	for row_v in fx.get_debug_group_report():
		if (row_v as EffectGroupReport).owner_key != null:
			owned += 1
	assert_eq(owned, 6,
			'all attached death/fire/other banks stay owned for zero-net children')
	var transients := _transient_rows(fx)
	assert_eq(transients.size(), 1, 'family-zero effects remain transient')
	if transients.size() == 1:
		assert_eq((transients[0] as EffectGroupReport).name, 'Effect_Transient')
		assert_almost_eq(_emitter_position(transients[0]), Vector3(20, 30, 40), POSITION_EPS)
	for wire_handle in [0x1004, 0x1005]:
		var node: Node3D = presenter.resolve_wire_handle(wire_handle)
		for family in [1, 2, 3]:
			var key := 'wreck:wire:%d:%d' % [wire_handle, family]
			assert_true(anchors.has_effect_anchor(key),
					'each sibling/family pair owns a distinct effect group')
			assert_not_null(_live_owned_row(fx, key),
					'%s holds its own live owned group' % key)
			assert_eq(anchors.resolve_owner_transform(key), node.global_transform)
		var fire_key := 'wreck:wire:%d:2' % wire_handle
		assert_true(presenter.has_active_wreck_fire(fire_key),
				'family 2 enters the per-owner wreck crackle set')
	first.position = Vector3(-3, 4, 5)
	second.position = Vector3(12, -2, 6)
	assert_eq(anchors.resolve_owner_transform('wreck:wire:4100:1'), first.global_transform)
	assert_eq(anchors.resolve_owner_transform('wreck:wire:4101:3'), second.global_transform,
			'owned effects follow the live WirePresent nodes after they move')
	# The fixed-tick owner sync carries the moved node poses into the scene.
	fx.advance_fixed_tick(0.0)
	assert_almost_eq(_emitter_position(_live_owned_row(fx, 'wreck:wire:4100:1')),
			first.global_position, POSITION_EPS,
			'the first sibling group rides its moved wire node')
	assert_almost_eq(_emitter_position(_live_owned_row(fx, 'wreck:wire:4101:3')),
			second.global_position, POSITION_EPS,
			'the second sibling group rides its own moved wire node')
	presenter.teardown()


func test_wreck_bank_points_follow_rotation_and_release_independently() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var container := Node3D.new()
	add_child_autofree(container)
	var node := ObjectModel.new()
	container.add_child(node)
	node.position = Vector3(4, 5, 6)
	var presenter := _make_presenter(null, container, _index_of([]), _husk_placer(),
			_item_db, anchors, fx, {0x1004: node})
	var effects: Array = []
	for slot in [0, 1]:
		effects.append(DestructionEffectEvent.make('Effect_Family2', Vector3.ZERO,
				2, Vector3.UP, 0, 0, 0x1004, Simulation.SPAWN_ORIGIN_NONE, false,
				slot, Vector3(2 * slot - 1, 0, 0)))
	presenter.present_destruction_drained(DestructionDrain.make([], effects), [])
	node.rotate_y(PI / 2)
	node.position += Vector3(3, 0, 0)
	fx.advance_fixed_tick(0.0)
	for slot in [0, 1]:
		var key := 'wreck:wire:4100:2' + (':1' if slot else '')
		assert_almost_eq(_emitter_position(_live_owned_row(fx, key)),
				node.global_transform * Vector3(2 * slot - 1, 0, 0), POSITION_EPS)
	var release := DestructionEffectEvent.make('', Vector3.ZERO, 2, Vector3.ZERO,
			0, 0, 0x1004, Simulation.SPAWN_ORIGIN_NONE, true, 1)
	presenter.present_destruction_drained(DestructionDrain.make([], [release]), [])
	assert_false(anchors.has_effect_anchor('wreck:wire:4100:2:1'))
	assert_true(anchors.has_effect_anchor('wreck:wire:4100:2'))
	presenter.teardown()


func test_batched_husk_and_wreck_anchor_follow_the_live_present_pose() -> void:
	# The REAL pose chain: an authored building boots into a real Simulation
	# (through MissionRoot, the production owner), and the node-less batched
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
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(mission, boot_container)
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

	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var placer := _husk_placer()
	var authored_rot := Vector3(17, 40, -12)
	var authored := Transform3D(
			MissionObjectPlacer.bms_to_godot_basis(authored_rot), Vector3(9, 8, 7))
	placer.register_static_instance(0, 'StaticProp', 0, authored, false)
	var container := Node3D.new()
	add_child_autofree(container)
	var events := DestructionDrain.make(
			[HuskSwapEvent.make(0, BUGGY_ITEM_ID, spawn_origin)],
			[DestructionEffectEvent.make(
					'Effect_VehExplode', Vector3(100, 100, 100),
					2, Vector3.ZERO, 91, 0, WireHandle.INVALID, spawn_origin)])
	var presenter := _make_presenter(sim, container, _index_of([]), placer, _item_db, anchors, fx)

	presenter.present_destruction_drained(events, [])

	assert_true(placer.is_static_instance_hidden(0),
			'the node-less husk carves the batch slot')
	assert_eq(_husk_models(self).size(), 1)
	if _husk_models(self).is_empty():
		return
	var graft: ObjectModel = _husk_models(self)[0]
	var expected := Transform3D(authored.basis, state[0])
	assert_true(graft.transform.is_equal_approx(expected),
			'the graft takes the live present origin while keeping the authored tilt')
	var anchor_pose: Variant = anchors.resolve_owner_transform('wreck:91:2')
	assert_true(anchor_pose is Transform3D)
	if anchor_pose is Transform3D:
		assert_true((anchor_pose as Transform3D).is_equal_approx(expected),
				'the owned wreck effect resolves the same live present pose')
	# The fixed-tick owner sync lands the owned group on that pose too.
	fx.advance_fixed_tick(0.0)
	assert_almost_eq(_emitter_position(_live_owned_row(fx, 'wreck:91:2')),
			expected.origin, POSITION_EPS,
			'the wreck group emits from the live present origin')
	presenter.teardown()


func test_resolved_debris_and_glass_effects_present_verbatim() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var presenter := _make_pass(fx, anchors)

	presenter.present_destruction_drained(DestructionDrain.make(
			[],
			[
					DestructionEffectEvent.make('Effect_TreeFoliageExp', Vector3.ZERO, 0, Vector3.RIGHT),
					DestructionEffectEvent.make('Effect_BldGlassExp', Vector3(1, 2, 3), 0, Vector3.UP)],
			1,
			1), [])

	assert_eq(presenter.get_destruction_present_stats().debris_triangles, 1)
	assert_eq(presenter.get_destruction_present_stats().glass_points, 1)
	var transients := _transient_rows(fx)
	assert_eq(transients.size(), 2)
	assert_eq(fx.get_debug_group_report().size(), 2, 'both resolved rows spawn unowned transients')
	if transients.size() == 2:
		var foliage := transients[0] as EffectGroupReport
		var glass := transients[1] as EffectGroupReport
		assert_eq(foliage.name, 'Effect_TreeFoliageExp')
		assert_almost_eq(_emitter_position(foliage), Vector3.ZERO, POSITION_EPS,
				'world origin is a valid authored triangle centroid')
		assert_almost_eq(_emitter_forward(foliage), Vector3.RIGHT, POSITION_EPS)
		assert_eq(glass.name, 'Effect_BldGlassExp')
		assert_almost_eq(_emitter_position(glass), Vector3(1, 2, 3), POSITION_EPS)
		assert_almost_eq(_emitter_forward(glass), Vector3.UP, POSITION_EPS)
	presenter.teardown()


func test_vehicle_respawn_restores_intact_model_and_releases_damage_effects() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var container := Node3D.new()
	add_child_autofree(container)
	var intact := ObjectModel.new()
	container.add_child(intact)
	var visual := Node3D.new()
	intact.add_child(visual)
	var presenter := _make_presenter(null, container, _index_of([_entry(intact, 41)]),
			_husk_placer(), _item_db, anchors, fx)
	var start := DestructionEffectEvent.make('Effect_Family1', Vector3.ZERO, 4,
			Vector3.ZERO, 91, 41)
	presenter.present_destruction_drained(DestructionDrain.make(
			[HuskSwapEvent.make(41, BUGGY_ITEM_ID)], [start]), [])
	assert_false(visual.visible)
	var key := 'wreck:91:4'
	assert_true(fx.has_owner_binding(key))
	var stop := DestructionEffectEvent.make('', Vector3.ZERO, 4, Vector3.ZERO,
			91, 41, 65535, 0, true)
	var restored := HuskSwapEvent.make(41, BUGGY_ITEM_ID, 4294967295, 65535, true)
	presenter.present_destruction_drained(DestructionDrain.make([restored], [stop]), [])
	assert_true(visual.visible, 'respawn restores the intact visual')
	assert_false(anchors.has_effect_anchor(key))
	assert_false(fx.has_owner_binding(key))
	presenter.teardown()
