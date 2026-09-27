extends GutTest

# The throwable present pass (EntityPresenter's ThrowablePresenter member, ADR
# 0043 d9) on the typed surfaces (ADR 0034): the visual rows are pure data
# through the public present_throwable_visuals data leg (production
# present_passes() drains the typed Simulation), the placer is a real
# MissionObjectPlacer over the injected fixture model, the fx/anchors
# collaborators are a REAL EffectWorld over an in-memory catalog plus a REAL
# ItemEffectDirector (ADR 0043 rule 11: their group report, owner-binding
# census and anchor registry are the read seams), and the item database is the
# real fixture items.def (id 101883 = the 3rd-person HE grenade).

static var _item_db: ItemDatabase = null


func before_all() -> void:
	_item_db = ItemDatabase.new()
	assert_eq(_item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK,
			"the fixture items.def loads (101883 carries graphic Frag_3rd)")


const ROUND_GRAPHIC := "Frag_3rd"  # fixture items.def 101883's graphic
const ROUND_MODEL_3DI := "res://../fixtures/threedi/synth/armory.3di"
const MOVE_EFFECT := "Effect_SmokeToss"  # the effects_table tag-1 round effect
const POSITION_EPS := Vector3(0.001, 0.001, 0.001)


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


# One synthetic in-memory particle catalog authoring the move effect (the
# effect_world_test recipe): a FOREVEREMIT definition, so a round-bound group
# stays live until the pass stops it and the report shows the detach.
func _catalog_file() -> ParticleFile:
	return ParticleFixture.catalog(self, "toss dots",
			"emit_dur = 0.1;\nemit_rate = 50;\nemit_burst = 4;\nage = 0.2;\nalpha = 1;\nscale = 1;\nflags = FOREVEREMIT;\n", [MOVE_EFFECT])


# A REAL EntityPresenter with its throwable pass wired over the placer, the
# item database and (when given) the effect world + anchor registry.
func _make_presenter(container: Node3D, placer: MissionObjectPlacer,
		fx: EffectWorld = null, anchors: ItemEffectDirector = null) -> EntityPresenter:
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(null, null, placer)
	presenter.setup_passes(container, _item_db, null, null, fx, null, null, anchors)
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


func test_move_effect_spawns_once_follows_full_round_pose_and_stops_with_round() -> void:
	# effects_table tag 1 is a continuously attached round effect in retail:
	# AmmoDef+0x70 -> round+0x1CC at @0x4e9f58/@0x4ea8ae, updated by
	# @0x5f7410 and released by Projectile_ReleaseEffects @0x4e8280.
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := _round_placer()
	var anchors := ItemEffectDirector.new()
	var fx := PresentPassFixture.make_fx(self, anchors, _catalog_file())
	var presenter := _make_presenter(container, placer, fx, anchors)
	var first_pos := Vector3(2, 3, 4)
	var first_rot := Vector3(15, 35, -12)
	var row := _row(7, 1883, first_pos, first_rot, MOVE_EFFECT)
	var owner_key := "throwable-move:7"

	presenter.present_throwable_visuals([row])
	var live := PresentPassFixture.live_owned_row(fx, owner_key)
	assert_not_null(live, "the live round spawns one move group")
	assert_eq(PresentPassFixture.owned_rows(fx, owner_key).size(), 1)
	assert_eq(live.name, MOVE_EFFECT)
	assert_eq(presenter.get_throwable_present_stats().move_effects, 1)
	var group_id := int(live.id)
	assert_true(anchors.has_effect_anchor(owner_key), "the live group has a pose resolver")
	var first_transform: Variant = anchors.resolve_owner_transform(owner_key)
	assert_true(first_transform is Transform3D)
	if first_transform is Transform3D:
		assert_eq((first_transform as Transform3D).origin, first_pos)
		assert_true((first_transform as Transform3D).basis.is_equal_approx(
				MissionObjectPlacer.bms_to_godot_basis(first_rot)),
				"the effect follows the round's complete spin basis")

	var next_pos := Vector3(-8, 6, 12)
	var next_rot := Vector3(-20, 110, 32)
	row = _row(7, 1883, next_pos, next_rot, MOVE_EFFECT)
	presenter.present_throwable_visuals([row])
	assert_eq(PresentPassFixture.owned_rows(fx, owner_key).size(), 1, "pose updates never respawn the move group")
	var next_transform: Variant = anchors.resolve_owner_transform(owner_key)
	assert_true(next_transform is Transform3D)
	if next_transform is Transform3D:
		assert_eq((next_transform as Transform3D).origin, next_pos)
		assert_true((next_transform as Transform3D).basis.is_equal_approx(
				MissionObjectPlacer.bms_to_godot_basis(next_rot)))
	# The fixed-tick owner sync carries that pose into the particle scene.
	fx.advance_fixed_tick(0.0)
	var followed := PresentPassFixture.live_owned_row(fx, owner_key)
	assert_almost_eq(PresentPassFixture.emitter_position(followed), next_pos, POSITION_EPS,
			"the emitter rides the round's live position")
	assert_almost_eq(PresentPassFixture.emitter_forward(followed),
			MissionObjectPlacer.bms_to_godot_basis(next_rot).z.normalized(), POSITION_EPS,
			"the emitter forward rides the round's spin basis")

	presenter.present_throwable_visuals([])
	assert_eq(presenter.get_throwable_present_stats().move_effects, 0)
	assert_false(anchors.has_effect_anchor(owner_key), "round removal retires its anchor")
	assert_true(PresentPassFixture.row_detached(fx, group_id),
			"round removal stops emission on the exact spawned group")
	assert_false(fx.has_owner_binding(owner_key),
			"round removal releases its generation-scoped effect identity")
	assert_true(fx.has_no_owner_bindings())
	presenter.teardown()


func test_released_move_effect_retires_and_respawns_when_live_again() -> void:
	# The sim's emitter liveness (round+0x1CC): a ClipWaterFx round dipping
	# under the water plane RELEASES its emitter, and surfacing acquires a new
	# one — the release is not latched [orig: Projectile_UpdatePhysics
	# @0x4ea019..0x4ea03e; the lazy spawn @0x4e9f58..0x4e9f94].
	var container := Node3D.new()
	add_child_autofree(container)
	var anchors := ItemEffectDirector.new()
	var fx := PresentPassFixture.make_fx(self, anchors, _catalog_file())
	var presenter := _make_presenter(container, _round_placer(), fx, anchors)
	var row := _row(3075, 1883, Vector3(3, 1, 2), Vector3.ZERO, MOVE_EFFECT)
	presenter.present_throwable_visuals([row])
	var owner_key := "throwable-move:3075"
	var first := PresentPassFixture.live_owned_row(fx, owner_key)
	assert_not_null(first, "a live emitter spawns its group")
	var first_id := int(first.id)

	row = _row(3075, 1883, Vector3(3, -1, 2), Vector3.ZERO, MOVE_EFFECT, false)
	presenter.present_throwable_visuals([row])
	assert_true(PresentPassFixture.row_detached(fx, first_id),
			"the released emitter stops its group while the round stays live")
	assert_false(fx.has_owner_binding(owner_key),
			"the release drops the effect identity so the handle is forgotten")
	assert_false(anchors.has_effect_anchor(owner_key))
	assert_eq(presenter.get_throwable_present_stats().move_effects, 0)
	assert_eq(presenter.get_throwable_present_stats().move_effect_transforms, 0,
			"a released round keeps no stale transform")

	row = _row(3075, 1883, Vector3(3, 1.5, 2), Vector3.ZERO, MOVE_EFFECT, true)
	presenter.present_throwable_visuals([row])
	var resumed_row := PresentPassFixture.live_owned_row(fx, owner_key)
	assert_not_null(resumed_row,
			"surfacing acquires a FRESH group for the same round")
	var second_id := int(resumed_row.id)
	assert_ne(second_id, first_id, "the stopped group is never revived")
	assert_eq(int(presenter.get_throwable_present_stats().move_effects), 1)
	assert_true(anchors.has_effect_anchor(owner_key))
	var resumed: Variant = anchors.resolve_owner_transform(owner_key)
	assert_true(resumed is Transform3D)
	if resumed is Transform3D:
		assert_eq((resumed as Transform3D).origin, Vector3(3, 1.5, 2))

	presenter.present_throwable_visuals([])
	assert_true(PresentPassFixture.row_detached(fx, second_id),
			"round removal stops the second group")
	assert_true(fx.has_no_owner_bindings())
	presenter.teardown()


func test_two_move_effect_closures_track_and_retire_their_own_rounds() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var anchors := ItemEffectDirector.new()
	var fx := PresentPassFixture.make_fx(self, anchors, _catalog_file())
	var presenter := _make_presenter(container, _round_placer(), fx, anchors)
	var pos_a := Vector3(1, 2, 3)
	var pos_b := Vector3(10, 20, 30)
	var rows: Array[ThrowableVisualRow] = [
		_row(1027, 1883, pos_a, Vector3(5, 15, 25), MOVE_EFFECT),
		_row(2059, 1883, pos_b, Vector3(-5, 70, -25), MOVE_EFFECT),
	]
	presenter.present_throwable_visuals(rows)

	var owner_a := "throwable-move:1027"
	var owner_b := "throwable-move:2059"
	var row_a := PresentPassFixture.live_owned_row(fx, owner_a)
	var row_b := PresentPassFixture.live_owned_row(fx, owner_b)
	assert_not_null(row_a)
	assert_not_null(row_b)
	assert_eq(fx.get_debug_group_report().size(), 2, "one owned group per round")
	var id_a := int(row_a.id)
	var id_b := int(row_b.id)
	var transform_a: Variant = anchors.resolve_owner_transform(owner_a)
	var transform_b: Variant = anchors.resolve_owner_transform(owner_b)
	assert_true(transform_a is Transform3D)
	assert_true(transform_b is Transform3D)
	if transform_a is Transform3D:
		assert_eq((transform_a as Transform3D).origin, pos_a,
				"the first anchor tracks the first round lifetime")
	if transform_b is Transform3D:
		assert_eq((transform_b as Transform3D).origin, pos_b,
				"the second anchor tracks the second round lifetime")

	var next_a := Vector3(-3, 8, 14)
	var next_b := Vector3(42, -2, 6)
	rows[0] = _row(1027, 1883, next_a, Vector3(5, 15, 25), MOVE_EFFECT)
	rows[1] = _row(2059, 1883, next_b, Vector3(-5, 70, -25), MOVE_EFFECT)
	presenter.present_throwable_visuals(rows)
	transform_a = anchors.resolve_owner_transform(owner_a)
	transform_b = anchors.resolve_owner_transform(owner_b)
	if transform_a is Transform3D:
		assert_eq((transform_a as Transform3D).origin, next_a)
	if transform_b is Transform3D:
		assert_eq((transform_b as Transform3D).origin, next_b)
	assert_eq(fx.get_debug_group_report().size(), 2, "two pose updates reuse their own groups")
	fx.advance_fixed_tick(0.0)
	assert_almost_eq(PresentPassFixture.emitter_position(PresentPassFixture.live_owned_row(fx, owner_a)), next_a, POSITION_EPS)
	assert_almost_eq(PresentPassFixture.emitter_position(PresentPassFixture.live_owned_row(fx, owner_b)), next_b, POSITION_EPS,
			"each group follows only its own round")

	presenter.present_throwable_visuals([rows[1]])
	assert_false(anchors.has_effect_anchor(owner_a))
	assert_true(anchors.has_effect_anchor(owner_b),
			"retiring the first round leaves the second anchor live")
	transform_b = anchors.resolve_owner_transform(owner_b)
	if transform_b is Transform3D:
		assert_eq((transform_b as Transform3D).origin, next_b)
	assert_true(PresentPassFixture.row_detached(fx, id_a),
			"only the first round's emitter group stops")
	assert_false(PresentPassFixture.row_detached(fx, id_b, true),
			"the second round's group stays attached")
	assert_false(fx.has_owner_binding(owner_a))
	assert_true(fx.has_owner_binding(owner_b))

	presenter.present_throwable_visuals([])
	assert_true(PresentPassFixture.row_detached(fx, id_b))
	assert_true(fx.has_no_owner_bindings(),
			"both retired rounds released their effect identities")
	presenter.teardown()


func test_same_slot_new_generation_replaces_the_owned_effect_group() -> void:
	# Fixed-tick catch-up can expire and reuse a native pool slot between two
	# present calls. The C++ key combines slot+generation, so the presenter must
	# see that as a retirement plus a fresh spawn even for the same effect name.
	var container := Node3D.new()
	add_child_autofree(container)
	var anchors := ItemEffectDirector.new()
	var fx := PresentPassFixture.make_fx(self, anchors, _catalog_file())
	var presenter := _make_presenter(container, _round_placer(), fx, anchors)
	# key 1024 = generation 1, slot 0; key 2048 = generation 2, the same slot 0
	presenter.present_throwable_visuals([
		_row(1024, 1883, Vector3(1, 0, 0), Vector3.ZERO, MOVE_EFFECT)])
	var outgoing := PresentPassFixture.live_owned_row(fx, "throwable-move:1024")
	assert_not_null(outgoing)
	var outgoing_id := int(outgoing.id)
	presenter.present_throwable_visuals([
		_row(2048, 1883, Vector3(20, 0, 0), Vector3.ZERO, MOVE_EFFECT)])

	assert_eq(fx.get_debug_group_report().size(), 2,
			"same-slot replacement starts a fresh effect lifetime")
	assert_true(PresentPassFixture.row_detached(fx, outgoing_id),
			"the outgoing generation stops instead of teleporting")
	assert_false(fx.has_owner_binding("throwable-move:1024"),
			"the outgoing generation drops its effect-world token mappings")
	assert_true(fx.has_owner_binding("throwable-move:2048"))
	assert_false(anchors.has_effect_anchor("throwable-move:1024"))
	assert_true(anchors.has_effect_anchor("throwable-move:2048"))
	var replacement := PresentPassFixture.live_owned_row(fx, "throwable-move:2048")
	assert_not_null(replacement, "the incoming generation owns a live group")
	assert_ne(int(replacement.id), outgoing_id)
	var replacement_transform: Variant = anchors.resolve_owner_transform("throwable-move:2048")
	assert_true(replacement_transform is Transform3D)
	if replacement_transform is Transform3D:
		assert_eq((replacement_transform as Transform3D).origin, Vector3(20, 0, 0))
	presenter.teardown()


func test_rejected_move_effect_spawn_leaves_no_transform_or_anchor_state() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var anchors := ItemEffectDirector.new()
	var placer := _round_placer()
	var presenter := _make_presenter(container, placer, null, anchors)
	var row := _row(1024, 1883, Vector3(9, 8, 7), Vector3.ZERO, MOVE_EFFECT)
	var owner_key := "throwable-move:1024"

	# A missing effect world is a normal startup/teardown ordering case. It must
	# not leave an unowned transform that retirement can never discover.
	presenter.present_throwable_visuals([row])
	assert_eq(presenter.get_throwable_present_stats().move_effects, 0)
	assert_eq(presenter.get_throwable_present_stats().move_effect_transforms, 0,
			"a null effect world does not leak round transform state")
	assert_false(anchors.has_effect_anchor(owner_key))

	# The retail master particle switch: every spawn facade answers a rejected
	# (particles_disabled) receipt, exactly the rejection the pass must clean
	# up after. The hidden report proves no group was ever created.
	var fx := PresentPassFixture.make_fx(self, anchors, _catalog_file())
	fx.set_particles_hidden(true)
	presenter.setup_passes(container, _item_db, null, null, fx, null, null, anchors)
	presenter.present_throwable_visuals([row])
	assert_true(fx.get_debug_group_report(true).is_empty(),
			"the rejected request created no group, hidden or otherwise")
	assert_true(fx.has_no_owner_bindings(),
			"a rejected native spawn releases its provisional token mappings")
	assert_false(anchors.has_effect_anchor(owner_key),
			"a failed spawn unregisters the provisional owner anchor")
	assert_eq(presenter.get_throwable_present_stats().move_effects, 0)
	assert_eq(presenter.get_throwable_present_stats().move_effect_transforms, 0,
			"a failed spawn does not leak round transform state")

	# A catalog miss (no stockeffect fallback authored) is the other rejection:
	# the facade allocates the owner's slot/owner identities BEFORE the scene
	# rejects the handle, so the release must actually drop them.
	fx.set_particles_hidden(false)
	presenter.present_throwable_visuals([
		_row(1024, 1883, Vector3(9, 8, 7), Vector3.ZERO, "Effect_NotInCatalog")])
	assert_true(fx.get_debug_group_report().is_empty())
	assert_true(fx.has_no_owner_bindings(),
			"the catalog-miss rejection releases the allocated identities")
	assert_false(anchors.has_effect_anchor(owner_key))
	assert_eq(presenter.get_throwable_present_stats().move_effects, 0)
	assert_eq(presenter.get_throwable_present_stats().move_effect_transforms, 0)
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


func test_reset_wire_runtime_state_clears_the_models_and_move_groups() -> void:
	# The Stop -> Play boundary reaches the throwable pass through the
	# presenter's one reset (the restart signal's single connect): every
	# presented model retires, every round-bound move group stops and releases
	# its anchor + effect identity.
	var container := Node3D.new()
	add_child_autofree(container)
	var anchors := ItemEffectDirector.new()
	var fx := PresentPassFixture.make_fx(self, anchors, _catalog_file())
	var presenter := _make_presenter(container, _round_placer(), fx, anchors)
	var owner_key := "throwable-move:7"
	presenter.present_throwable_visuals([
		_row(7, 1883, Vector3(2, 3, 4), Vector3.ZERO, MOVE_EFFECT)])
	assert_eq(presenter.get_throwable_present_stats().live, 1)
	assert_eq(presenter.get_throwable_present_stats().move_effects, 1)
	var live := PresentPassFixture.live_owned_row(fx, owner_key)
	assert_not_null(live)
	var group_id := int(live.id) if live != null else 0
	var model: ObjectModel = _thrown_models(container)[0]

	presenter.reset_wire_runtime_state()

	var stats := presenter.get_throwable_present_stats()
	assert_eq(stats.live, 0, "reset frees every presented model")
	assert_eq(stats.move_effects, 0, "reset stops every move group")
	assert_eq(stats.move_effect_transforms, 0, "reset forgets every anchor transform")
	assert_true(model.is_queued_for_deletion())
	assert_false(anchors.has_effect_anchor(owner_key), "reset retires the move anchor")
	assert_true(PresentPassFixture.row_detached(fx, group_id), "reset stops emission on the spawned group")
	assert_true(fx.has_no_owner_bindings(), "reset releases the effect identity")
	presenter.teardown()
