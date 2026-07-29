extends GutTest

# DebugSnapshotWriter: the pick-enrichment contract against a sim exposing
# ALL four live-card accessors with their REAL return shapes — including
# get_present_effect_state_for_bms_id's PackedVector3Array, the exact shape
# that aborted enrichment in the first field dump (a mistyped assumption must
# degrade, never lose the pick). Plus the _jsonable conversion rules.

const Writer := preload("res://engine/debug/debug_snapshot_writer.gd")


class RichCardSim:
	extends Node

	func has_local_player() -> bool:
		return true

	func get_local_player_position() -> Vector3:
		return Vector3(10, 5, -20)

	func get_local_player_yaw_deg() -> float:
		return 45.0

	func get_local_player_pitch_deg() -> float:
		return 0.0

	func get_logic_tick() -> int:
		return 999

	func get_entity_count() -> int:
		return 2

	func get_entity_debug(index: int) -> Dictionary:
		if index != 1:
			return {"kind": 9, "index": 9, "bms_id": 9}
		return {"kind": 2, "index": 14, "bms_id": 1484, "state_name": "idle",
				"health": 80, "position": Vector3(1, 2, 3),
				"kz_points": PackedVector3Array([Vector3.ONE])}

	func get_world_entity_debug(net_id: int) -> Dictionary:
		if net_id != 212:
			return {}
		return {"net_id": 212, "position": Vector3(4, 5, 6), "alive": true}

	func get_destruction_debug(bms_id: int) -> Dictionary:
		if bms_id != 1484:
			return {}
		return {"item_id": 55, "health": 80, "health_max": 100, "has_husk": true}

	# The real binding's shape: a PackedVector3Array, NOT a Dictionary.
	func get_present_effect_state_for_bms_id(bms_id: int) -> PackedVector3Array:
		if bms_id != 1484:
			return PackedVector3Array()
		return PackedVector3Array([Vector3(1, 2, 3), Vector3(0, 90, 0)])


class RichCardRuntime:
	extends Node
	var sim := RichCardSim.new()

	func _init() -> void:
		add_child(sim)

	func get_sim() -> RichCardSim:
		return sim

	func get_mission_file() -> String:
		return "00TRg.bms"

	func get_mission_name() -> String:
		return "Training: Grenade Launcher"


func _ctx_with_runtime() -> NovaDebugContext:
	var runtime := RichCardRuntime.new()
	add_child_autofree(runtime)
	var ctx := NovaDebugContext.new()
	ctx.runtime_source = func(): return runtime
	return ctx


func _entity_pick() -> Dictionary:
	return {"hit": true, "entity_handle": 4130, "pool": 2, "kind": 2, "index": 14,
			"bms_id": 1484, "net_id": 212, "item_id": 55, "name": "RckS07",
			"position_godot": Vector3(1, 3, -2), "bound_radius": 4.0,
			"hit_position_godot": Vector3(1, 4, -2), "hit_normal_godot": Vector3.UP,
			"distance_units": 45.5, "hit_class": "static", "section": 1, "face": 17,
			"bone": -1, "hit_zone": -1, "surface_type": 3, "material_flags": 0,
			"tick": 777, "source": "crosshair",
			"ray_origin_godot": Vector3.ZERO, "ray_dir_godot": Vector3.FORWARD}


func test_enrichment_survives_every_real_card_shape() -> void:
	var snapshot := Writer.capture(_ctx_with_runtime(), [_entity_pick()])
	assert_false(snapshot.is_empty(), "the pose anchored")
	assert_eq(int(snapshot.get("pick_count", -1)), 1)
	var entry: Dictionary = (snapshot.get("picks", []) as Array)[0]
	assert_false((entry.get("identity", {}) as Dictionary).is_empty(),
			"the pick was never lost")
	assert_false(bool(entry.get("stale", true)), "live cards were found")

	var entity_debug: Dictionary = entry.get("entity_debug", {})
	assert_eq(int(entity_debug.get("bms_id", 0)), 1484,
			"the AI card matched by kind/index scan")
	var kz: Variant = entity_debug.get("kz_points")
	assert_typeof(kz, TYPE_ARRAY)
	assert_eq(float((kz[0] as Dictionary).get("x", 0.0)), 1.0,
			"packed arrays inside cards convert to JSON records")

	assert_eq(int((entry.get("world_entity_debug", {}) as Dictionary).get("net_id", 0)), 212)
	assert_true(bool((entry.get("destruction", {}) as Dictionary).get("has_husk", false)))

	# THE regression: the effect state is a PackedVector3Array — it must land
	# as a JSON list, not abort the dump (the first field dump lost its pick
	# to exactly this).
	var effect_state: Variant = entry.get("effect_state")
	assert_typeof(effect_state, TYPE_ARRAY)
	assert_eq((effect_state as Array).size(), 2)
	assert_eq(float((effect_state[1] as Dictionary).get("y", 0.0)), 90.0)


func test_cardless_pick_reports_stale_with_identity_intact() -> void:
	var ctx := _ctx_with_runtime()
	var pick := _entity_pick()
	pick["kind"] = 7  # matches no AI card
	pick["bms_id"] = 5555  # matches no destruction/effect state
	pick["net_id"] = 0
	var snapshot := Writer.capture(ctx, [pick])
	var entry: Dictionary = (snapshot.get("picks", []) as Array)[0]
	assert_true(bool(entry.get("stale", false)))
	assert_eq(String((entry.get("identity", {}) as Dictionary).get("name", "")), "RckS07")
	assert_eq(int((entry.get("pick", {}) as Dictionary).get("face", -1)), 17,
			"the replayable ray survives even when nothing resolves live")


func test_write_and_default_path_roundtrip() -> void:
	var snapshot := Writer.capture(_ctx_with_runtime(), [])
	var target := OS.get_cache_dir().path_join(
			"opennova_writer_test_%d.json" % Time.get_ticks_usec())
	var result: Dictionary = Writer.write(snapshot, target)
	assert_eq(String(result.get("error", "?")), "")
	var path := String(result.get("path", ""))
	assert_true(FileAccess.file_exists(path))
	var payload: Dictionary = JSON.parse_string(
			FileAccess.open(path, FileAccess.READ).get_as_text())
	assert_eq(String(payload.get("schema", "")), "opennova.debug_snapshot.v1")
	DirAccess.remove_absolute(path)

	var a := Writer.default_path(snapshot)
	var b := Writer.default_path(snapshot)
	assert_ne(a, b, "the sequence never repeats a default path")
	assert_string_contains(a, "00TRg")
