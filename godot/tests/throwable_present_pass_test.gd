extends GutTest

const ThrowablePresentPass := preload("res://engine/world/throwable_present_pass.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")


class SimStub:
	extends RefCounted
	var visuals: Array = []

	func get_throwable_visuals() -> Array:
		return visuals


class ItemDbStub:
	extends RefCounted

	func get_graphic(def_id: int) -> String:
		return "GREN_MODEL" if def_id == 101883 else ""


class PlacerStub:
	extends RefCounted
	var built: Array[Node3D] = []

	func build_model_from_graphic(_graphic: String, _anim: String,
			parent: Node3D, _clip_key: String = "", _env_node: Node = null,
			_rig_graphic: String = "") -> Node3D:
		var model := Node3D.new()
		parent.add_child(model)
		built.append(model)
		return model


class EffectWorldStub:
	extends RefCounted
	var spawns: Array[Dictionary] = []
	var stopped: Array[int] = []
	var released: Array[Variant] = []
	var next_group_id := 90
	var allow_spawn := true

	func spawn_effect_owned_request(owner_key: Variant, effect: String,
			position: Vector3, orientation: Vector3) -> Dictionary:
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


class GameWorldStub:
	extends RefCounted
	var anchors: Dictionary = {}

	func register_effect_anchor(owner_key: Variant, resolver: Callable) -> void:
		anchors[owner_key] = resolver

	func unregister_effect_anchor(owner_key: Variant) -> void:
		anchors.erase(owner_key)


func test_present_uses_the_canonical_bms_basis_and_godot_position() -> void:
	var sim := SimStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := PlacerStub.new()
	var presenter = ThrowablePresentPass.new()
	presenter.setup(sim, container, placer, ItemDbStub.new(), null)

	var position := Vector3(12.5, -4.0, 33.25)
	for rotation in [Vector3.ZERO, Vector3(0, 90, 0), Vector3(20, 35, -15)]:
		sim.visuals = [{
			"key": 7,
			"item_id": 1883,
			"pos": position,
			"rotation_deg": rotation,
		}]
		presenter.present()
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
	var sim := SimStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := PlacerStub.new()
	var fx := EffectWorldStub.new()
	var world := GameWorldStub.new()
	var presenter = ThrowablePresentPass.new()
	presenter.setup(sim, container, placer, ItemDbStub.new(), null,
			func() -> Variant: return fx, world)
	var first_pos := Vector3(2, 3, 4)
	var first_rot := Vector3(15, 35, -12)
	sim.visuals = [{
		"key": 7,
		"item_id": 1883,
		"pos": first_pos,
		"rotation_deg": first_rot,
		"move_effect": "Effect_SmokeToss",
	}]

	presenter.present()
	assert_eq(fx.spawns.size(), 1, "the live round spawns one move group")
	assert_eq(presenter.get_stats().move_effects, 1)
	var owner_key: Variant = fx.spawns[0].get("owner_key")
	assert_true(world.anchors.has(owner_key), "the live group has a pose resolver")
	var first_transform: Transform3D = (world.anchors[owner_key] as Callable).call()
	assert_eq(first_transform.origin, first_pos)
	assert_true(first_transform.basis.is_equal_approx(
			MissionObjectPlacer.bms_to_godot_basis(first_rot)),
			"the effect follows the round's complete spin basis")

	var next_pos := Vector3(-8, 6, 12)
	var next_rot := Vector3(-20, 110, 32)
	sim.visuals[0]["pos"] = next_pos
	sim.visuals[0]["rotation_deg"] = next_rot
	presenter.present()
	assert_eq(fx.spawns.size(), 1, "pose updates never respawn the move group")
	var next_transform: Transform3D = (world.anchors[owner_key] as Callable).call()
	assert_eq(next_transform.origin, next_pos)
	assert_true(next_transform.basis.is_equal_approx(
			MissionObjectPlacer.bms_to_godot_basis(next_rot)))

	sim.visuals = []
	presenter.present()
	assert_eq(presenter.get_stats().move_effects, 0)
	assert_false(world.anchors.has(owner_key), "round removal retires its anchor")
	assert_eq(fx.stopped, [91],
			"round removal stops emission on the exact spawned group")
	assert_eq(fx.released, [owner_key],
			"round removal releases its generation-scoped effect identity")
	presenter.teardown()


func test_two_move_effect_closures_track_and_retire_their_own_rounds() -> void:
	var sim := SimStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var fx := EffectWorldStub.new()
	var world := GameWorldStub.new()
	var presenter = ThrowablePresentPass.new()
	presenter.setup(sim, container, PlacerStub.new(), ItemDbStub.new(), null,
			func() -> Variant: return fx, world)
	var pos_a := Vector3(1, 2, 3)
	var pos_b := Vector3(10, 20, 30)
	sim.visuals = [
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
	presenter.present()

	assert_eq(fx.spawns.size(), 2)
	var owner_a := "throwable-move:1027"
	var owner_b := "throwable-move:2059"
	var transform_a: Transform3D = (world.anchors[owner_a] as Callable).call()
	var transform_b: Transform3D = (world.anchors[owner_b] as Callable).call()
	assert_eq(transform_a.origin, pos_a,
			"the first closure captures the first round lifetime")
	assert_eq(transform_b.origin, pos_b,
			"the second closure captures the second round lifetime")

	var next_a := Vector3(-3, 8, 14)
	var next_b := Vector3(42, -2, 6)
	sim.visuals[0]["pos"] = next_a
	sim.visuals[1]["pos"] = next_b
	presenter.present()
	transform_a = (world.anchors[owner_a] as Callable).call()
	transform_b = (world.anchors[owner_b] as Callable).call()
	assert_eq(transform_a.origin, next_a)
	assert_eq(transform_b.origin, next_b)
	assert_eq(fx.spawns.size(), 2, "two pose updates reuse their own groups")

	sim.visuals = [sim.visuals[1]]
	presenter.present()
	assert_false(world.anchors.has(owner_a))
	assert_true(world.anchors.has(owner_b),
			"retiring the first round leaves the second anchor live")
	transform_b = (world.anchors[owner_b] as Callable).call()
	assert_eq(transform_b.origin, next_b)
	assert_eq(fx.stopped, [91],
			"only the first round's emitter group stops")
	assert_eq(fx.released, [owner_a])

	sim.visuals = []
	presenter.present()
	assert_eq(fx.stopped, [91, 92])
	assert_eq(fx.released, [owner_a, owner_b])
	presenter.teardown()


func test_same_slot_new_generation_replaces_the_owned_effect_group() -> void:
	# Fixed-tick catch-up can expire and reuse a native pool slot between two
	# present calls. The C++ key combines slot+generation, so the presenter must
	# see that as a retirement plus a fresh spawn even for the same effect name.
	var sim := SimStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var fx := EffectWorldStub.new()
	var world := GameWorldStub.new()
	var presenter = ThrowablePresentPass.new()
	presenter.setup(sim, container, PlacerStub.new(), ItemDbStub.new(), null,
			func() -> Variant: return fx, world)
	sim.visuals = [{
		"key": 1024, # generation 1, slot 0
		"item_id": 1883,
		"pos": Vector3(1, 0, 0),
		"rotation_deg": Vector3.ZERO,
		"move_effect": "Effect_SmokeToss",
	}]
	presenter.present()
	sim.visuals = [{
		"key": 2048, # generation 2, the same slot 0
		"item_id": 1883,
		"pos": Vector3(20, 0, 0),
		"rotation_deg": Vector3.ZERO,
		"move_effect": "Effect_SmokeToss",
	}]
	presenter.present()

	assert_eq(fx.spawns.size(), 2,
			"same-slot replacement starts a fresh effect lifetime")
	assert_eq(fx.stopped, [91],
			"the outgoing generation stops instead of teleporting")
	assert_eq(fx.released, ["throwable-move:1024"],
			"the outgoing generation drops its effect-world token mappings")
	assert_false(world.anchors.has("throwable-move:1024"))
	assert_true(world.anchors.has("throwable-move:2048"))
	var replacement_transform: Transform3D = \
			(world.anchors["throwable-move:2048"] as Callable).call()
	assert_eq(replacement_transform.origin, Vector3(20, 0, 0))
	presenter.teardown()


func test_rejected_move_effect_spawn_leaves_no_transform_or_anchor_state() -> void:
	var sim := SimStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var world := GameWorldStub.new()
	var presenter = ThrowablePresentPass.new()
	sim.visuals = [{
		"key": 1024,
		"item_id": 1883,
		"pos": Vector3(9, 8, 7),
		"rotation_deg": Vector3.ZERO,
		"move_effect": "Effect_SmokeToss",
	}]

	# A missing provider is a normal startup/teardown ordering case. It must not
	# leave an unowned transform that retirement can never discover.
	presenter.setup(sim, container, PlacerStub.new(), ItemDbStub.new(), null,
			Callable(), world)
	presenter.present()
	assert_eq(presenter.get_stats().move_effects, 0)
	assert_eq(presenter.get_stats().move_effect_transforms, 0,
			"a null provider does not leak round transform state")

	var fx := EffectWorldStub.new()
	fx.allow_spawn = false
	presenter.setup(sim, container, PlacerStub.new(), ItemDbStub.new(), null,
			func() -> Variant: return fx, world)
	presenter.present()
	assert_eq(fx.spawns.size(), 1, "the provider rejected one real request")
	assert_eq(fx.released, ["throwable-move:1024"],
			"a rejected native spawn releases its provisional token mappings")
	assert_true(world.anchors.is_empty(),
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
	var sim := SimStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := PlacerStub.new()
	var presenter = ThrowablePresentPass.new()
	presenter.setup(sim, container, placer, ItemDbStub.new(), null)
	sim.visuals = [{
		"key": 19,
		"item_id": 1883,
		"pos": Vector3(4, 5, 6),
		"rotation_deg": Vector3.ZERO,
	}]

	presenter.present()
	assert_eq(placer.built.size(), 1,
			"the remote flying-round snapshot materializes its TrcrID item")
	var live_stats := presenter.get_stats()
	assert_true(live_stats is ThrowablePresentPass.Stats)
	assert_eq(live_stats.live, 1)
	var model := placer.built[0]
	assert_eq(model.position, Vector3(4, 5, 6))

	sim.visuals = []
	presenter.present()
	assert_eq(presenter.get_stats().live, 0)
	assert_true(model.is_queued_for_deletion(),
			"the model retires when the remote visual round slot is gone")
	presenter.teardown()
