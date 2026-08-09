extends GutTest

# ThrowablePresentPass on the typed surfaces (ADR 0034): the visual rows are
# pure data through the public present_visuals data leg (production present()
# drains the typed Simulation), the placer/fx/anchors collaborators are real
# MissionObjectPlacer/EffectWorld/ItemEffectDirector subclasses capturing the
# typed calls, and the item database is the real fixture items.def
# (id 101883 = the 3rd-person HE grenade).

const ThrowablePresentPass := preload("res://game/world/throwable_present_pass.gd")
const MissionObjectPlacer := preload("res://game/mission/mission_object_placer.gd")

static var _item_db: ItemDatabase = null


func before_all() -> void:
	_item_db = ItemDatabase.new()
	assert_eq(_item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK,
			"the fixture items.def loads (101883 carries graphic Frag_3rd)")


class CapturePlacer:
	extends MissionObjectPlacer
	var built: Array[Node3D] = []

	func build_model_from_graphic(_graphic: String, _adm_name: String,
			parent: Node3D, _clip_key: String = "",
			_rig_graphic: String = "") -> ObjectModel:
		var model := ObjectModel.new()
		parent.add_child(model)
		built.append(model)
		return model


class CaptureFx:
	extends EffectWorld
	var spawns: Array[Dictionary] = []
	var stopped: Array[int] = []
	var released: Array[Variant] = []
	var next_group_id := 90
	var allow_spawn := true

	func spawn_effect_owned_request(owner_key: Variant, effect: String,
			position: Vector3, orientation: Vector3 = Vector3.ZERO) -> Dictionary:
		next_group_id += 1
		spawns.append({
			"owner_key": owner_key,
			"effect": effect,
			"position": position,
			"orientation": orientation,
		})
		if not allow_spawn:
			return {"spawned": false, "effect_handle": 0, "group_id": 0}
		return {
			"spawned": true,
			"effect_handle": 7,
			"group_id": next_group_id,
		}

	func stop_group(group_id: int) -> void:
		stopped.append(group_id)

	func release_effect_binding(owner_key: Variant) -> void:
		released.append(owner_key)


class CaptureAnchors:
	extends ItemEffectDirector
	var anchors: Dictionary = {}

	func register_effect_anchor(owner_key: Variant, resolver: Callable) -> void:
		anchors[owner_key] = resolver
		super.register_effect_anchor(owner_key, resolver)

	func unregister_effect_anchor(owner_key: Variant) -> void:
		anchors.erase(owner_key)
		super.unregister_effect_anchor(owner_key)


func _make_fx() -> CaptureFx:
	var fx := CaptureFx.new()
	add_child_autofree(fx)
	return fx


func test_present_uses_the_canonical_bms_basis_and_godot_position() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := CapturePlacer.new()
	var presenter := ThrowablePresentPass.new()
	presenter.setup(null, container, placer, _item_db)

	var position := Vector3(12.5, -4.0, 33.25)
	for rotation in [Vector3.ZERO, Vector3(0, 90, 0), Vector3(20, 35, -15)]:
		presenter.present_visuals([{
			"key": 7,
			"item_id": 1883,
			"pos": position,
			"rotation_deg": rotation,
		}])
		assert_eq(placer.built.size(), 1, "the same live round reuses its model")
		var model := placer.built[0]
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
	var placer := CapturePlacer.new()
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var presenter := ThrowablePresentPass.new()
	presenter.setup(null, container, placer, _item_db,
			func() -> Variant: return fx, anchors)
	var first_pos := Vector3(2, 3, 4)
	var first_rot := Vector3(15, 35, -12)
	var row := {
		"key": 7,
		"item_id": 1883,
		"pos": first_pos,
		"rotation_deg": first_rot,
		"move_effect": "Effect_SmokeToss",
	}

	presenter.present_visuals([row])
	assert_eq(fx.spawns.size(), 1, "the live round spawns one move group")
	assert_eq(presenter.get_stats().move_effects, 1)
	var owner_key: Variant = fx.spawns[0].get("owner_key")
	assert_true(anchors.anchors.has(owner_key), "the live group has a pose resolver")
	var first_transform: Transform3D = (anchors.anchors[owner_key] as Callable).call()
	assert_eq(first_transform.origin, first_pos)
	assert_true(first_transform.basis.is_equal_approx(
			MissionObjectPlacer.bms_to_godot_basis(first_rot)),
			"the effect follows the round's complete spin basis")

	var next_pos := Vector3(-8, 6, 12)
	var next_rot := Vector3(-20, 110, 32)
	row["pos"] = next_pos
	row["rotation_deg"] = next_rot
	presenter.present_visuals([row])
	assert_eq(fx.spawns.size(), 1, "pose updates never respawn the move group")
	var next_transform: Transform3D = (anchors.anchors[owner_key] as Callable).call()
	assert_eq(next_transform.origin, next_pos)
	assert_true(next_transform.basis.is_equal_approx(
			MissionObjectPlacer.bms_to_godot_basis(next_rot)))

	presenter.present_visuals([])
	assert_eq(presenter.get_stats().move_effects, 0)
	assert_false(anchors.anchors.has(owner_key), "round removal retires its anchor")
	assert_eq(fx.stopped, [91],
			"round removal stops emission on the exact spawned group")
	assert_eq(fx.released, [owner_key],
			"round removal releases its generation-scoped effect identity")
	presenter.teardown()


func test_two_move_effect_closures_track_and_retire_their_own_rounds() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var presenter := ThrowablePresentPass.new()
	presenter.setup(null, container, CapturePlacer.new(), _item_db,
			func() -> Variant: return fx, anchors)
	var pos_a := Vector3(1, 2, 3)
	var pos_b := Vector3(10, 20, 30)
	var rows := [
		{
			"key": 1027,
			"item_id": 1883,
			"pos": pos_a,
			"rotation_deg": Vector3(5, 15, 25),
			"move_effect": "Effect_SmokeToss",
		},
		{
			"key": 2059,
			"item_id": 1883,
			"pos": pos_b,
			"rotation_deg": Vector3(-5, 70, -25),
			"move_effect": "Effect_SmokeToss",
		},
	]
	presenter.present_visuals(rows)

	assert_eq(fx.spawns.size(), 2)
	var owner_a := "throwable-move:1027"
	var owner_b := "throwable-move:2059"
	var transform_a: Transform3D = (anchors.anchors[owner_a] as Callable).call()
	var transform_b: Transform3D = (anchors.anchors[owner_b] as Callable).call()
	assert_eq(transform_a.origin, pos_a,
			"the first closure captures the first round lifetime")
	assert_eq(transform_b.origin, pos_b,
			"the second closure captures the second round lifetime")

	var next_a := Vector3(-3, 8, 14)
	var next_b := Vector3(42, -2, 6)
	rows[0]["pos"] = next_a
	rows[1]["pos"] = next_b
	presenter.present_visuals(rows)
	transform_a = (anchors.anchors[owner_a] as Callable).call()
	transform_b = (anchors.anchors[owner_b] as Callable).call()
	assert_eq(transform_a.origin, next_a)
	assert_eq(transform_b.origin, next_b)
	assert_eq(fx.spawns.size(), 2, "two pose updates reuse their own groups")

	presenter.present_visuals([rows[1]])
	assert_false(anchors.anchors.has(owner_a))
	assert_true(anchors.anchors.has(owner_b),
			"retiring the first round leaves the second anchor live")
	transform_b = (anchors.anchors[owner_b] as Callable).call()
	assert_eq(transform_b.origin, next_b)
	assert_eq(fx.stopped, [91],
			"only the first round's emitter group stops")
	assert_eq(fx.released, [owner_a])

	presenter.present_visuals([])
	assert_eq(fx.stopped, [91, 92])
	assert_eq(fx.released, [owner_a, owner_b])
	presenter.teardown()


func test_same_slot_new_generation_replaces_the_owned_effect_group() -> void:
	# Fixed-tick catch-up can expire and reuse a native pool slot between two
	# present calls. The C++ key combines slot+generation, so the presenter must
	# see that as a retirement plus a fresh spawn even for the same effect name.
	var container := Node3D.new()
	add_child_autofree(container)
	var fx := _make_fx()
	var anchors := CaptureAnchors.new()
	var presenter := ThrowablePresentPass.new()
	presenter.setup(null, container, CapturePlacer.new(), _item_db,
			func() -> Variant: return fx, anchors)
	presenter.present_visuals([{
		"key": 1024, # generation 1, slot 0
		"item_id": 1883,
		"pos": Vector3(1, 0, 0),
		"rotation_deg": Vector3.ZERO,
		"move_effect": "Effect_SmokeToss",
	}])
	presenter.present_visuals([{
		"key": 2048, # generation 2, the same slot 0
		"item_id": 1883,
		"pos": Vector3(20, 0, 0),
		"rotation_deg": Vector3.ZERO,
		"move_effect": "Effect_SmokeToss",
	}])

	assert_eq(fx.spawns.size(), 2,
			"same-slot replacement starts a fresh effect lifetime")
	assert_eq(fx.stopped, [91],
			"the outgoing generation stops instead of teleporting")
	assert_eq(fx.released, ["throwable-move:1024"],
			"the outgoing generation drops its effect-world token mappings")
	assert_false(anchors.anchors.has("throwable-move:1024"))
	assert_true(anchors.anchors.has("throwable-move:2048"))
	var replacement_transform: Transform3D = \
			(anchors.anchors["throwable-move:2048"] as Callable).call()
	assert_eq(replacement_transform.origin, Vector3(20, 0, 0))
	presenter.teardown()


func test_rejected_move_effect_spawn_leaves_no_transform_or_anchor_state() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var anchors := CaptureAnchors.new()
	var presenter := ThrowablePresentPass.new()
	var row := {
		"key": 1024,
		"item_id": 1883,
		"pos": Vector3(9, 8, 7),
		"rotation_deg": Vector3.ZERO,
		"move_effect": "Effect_SmokeToss",
	}

	# A missing provider is a normal startup/teardown ordering case. It must not
	# leave an unowned transform that retirement can never discover.
	presenter.setup(null, container, CapturePlacer.new(), _item_db,
			Callable(), anchors)
	presenter.present_visuals([row])
	assert_eq(presenter.get_stats().move_effects, 0)
	assert_eq(presenter.get_stats().move_effect_transforms, 0,
			"a null provider does not leak round transform state")

	var fx := _make_fx()
	fx.allow_spawn = false
	presenter.setup(null, container, CapturePlacer.new(), _item_db,
			func() -> Variant: return fx, anchors)
	presenter.present_visuals([row])
	assert_eq(fx.spawns.size(), 1, "the provider rejected one real request")
	assert_eq(fx.released, ["throwable-move:1024"],
			"a rejected native spawn releases its provisional token mappings")
	assert_true(anchors.anchors.is_empty(),
			"a failed spawn unregisters the provisional owner anchor")
	assert_eq(presenter.get_stats().move_effects, 0)
	assert_eq(presenter.get_stats().move_effect_transforms, 0,
			"a failed spawn does not leak round transform state")
	presenter.teardown()


func test_remote_flying_round_snapshot_builds_and_retires_its_model() -> void:
	# A joiner's decoded tag-2 throwable is just a visual RoundSim snapshot at
	# this seam. It must materialize while the round is live, then disappear
	# when the visual motor releases the slot. This does not synthesize a placed
	# device: a future 0x59/0x12 decode would have to add that state explicitly.
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := CapturePlacer.new()
	var presenter := ThrowablePresentPass.new()
	presenter.setup(null, container, placer, _item_db)

	presenter.present_visuals([{
		"key": 19,
		"item_id": 1883,
		"pos": Vector3(4, 5, 6),
		"rotation_deg": Vector3.ZERO,
	}])
	assert_eq(placer.built.size(), 1,
			"the remote flying-round snapshot materializes its TrcrID item")
	var live_stats := presenter.get_stats()
	assert_true(live_stats is ThrowablePresentPass.Stats)
	assert_eq(live_stats.live, 1)
	var model := placer.built[0]
	assert_eq(model.position, Vector3(4, 5, 6))

	presenter.present_visuals([])
	assert_eq(presenter.get_stats().live, 0)
	assert_true(model.is_queued_for_deletion(),
			"the model retires when the remote visual round slot is gone")
	presenter.teardown()
