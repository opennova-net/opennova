extends GutTest

# DebugSnapshotWriter: the pick-enrichment contract against a REAL
# MissionRuntime + Simulation (the context is typed, ADR 0034): the pose
# anchors on the auto-spawned host player, enrichment joins the sim's live
# entity cards by kind/index, real card containers (Vector3 fields, packed
# arrays, the PackedVector3Array effect state) convert to JSON records
# instead of aborting the dump, and a cardless pick degrades to stale with
# its identity and replayable ray intact. Plus the write/default-path rules.

const Writer := preload("res://game/debug/debug_snapshot_writer.gd")
const MissionRuntime := preload("res://game/world/mission_runtime.gd")


func _runtime() -> MissionRuntime:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	mission.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)  # KIND_ORGANIC
	var container := Node3D.new()
	add_child_autofree(container)
	var runtime := MissionRuntime.new()
	add_child_autofree(runtime)
	var placer := MissionObjectPlacer.new()
	assert_eq(runtime.setup(mission, container, {
		"placer": placer,
		"mission_file": "00TRg.bms",
		"mission_name": "Training: Grenade Launcher",
	}), 2, "one authored organic + the auto-spawned host player")
	return runtime


func _ctx_with_runtime(runtime: MissionRuntime) -> DebugContext:
	var ctx := DebugContext.new()
	ctx.runtime_source = func(): return runtime
	return ctx


## A pick card whose identity matches one REAL live AI card, so enrichment
## joins it exactly like a crosshair pick on that entity would.
func _entity_pick(card: Dictionary) -> Dictionary:
	return {"hit": true, "entity_handle": -1, "pool": int(card.get("pool", -1)),
			"kind": int(card.get("kind", -1)), "index": int(card.get("index", -1)),
			"bms_id": int(card.get("bms_id", 0)), "net_id": 0,
			"item_id": int(card.get("item_id", 0)), "name": String(card.get("name", "")),
			"position_godot": Vector3(1, 3, -2), "bound_radius": 4.0,
			"hit_position_godot": Vector3(1, 4, -2), "hit_normal_godot": Vector3.UP,
			"distance_units": 45.5, "hit_class": "static", "section": 1, "face": 17,
			"bone": -1, "hit_zone": -1, "surface_type": 3, "material_flags": 0,
			"tick": 777, "source": "crosshair",
			"ray_origin_godot": Vector3.ZERO, "ray_dir_godot": Vector3.FORWARD}


func test_enrichment_joins_real_cards_and_converts_containers() -> void:
	var runtime := _runtime()
	var sim := runtime.get_sim()
	assert_gt(int(sim.get_entity_count()), 0, "the mission spawned AI entities")
	var card: Dictionary = sim.get_entity_debug(0)
	assert_false(card.is_empty(), "the first AI card is live")

	var snapshot := Writer.capture(
			_ctx_with_runtime(runtime), [_entity_pick(card)])
	assert_false(snapshot.is_empty(), "the pose anchored on the host player")
	assert_eq(int(snapshot.get("pick_count", -1)), 1)
	var entry: Dictionary = (snapshot.get("picks", []) as Array)[0]
	assert_false((entry.get("identity", {}) as Dictionary).is_empty(),
			"the pick was never lost")
	assert_false(bool(entry.get("stale", true)), "a live card was found")

	var entity_debug: Dictionary = entry.get("entity_debug", {})
	assert_eq(int(entity_debug.get("kind", -2)), int(card.get("kind", -1)),
			"the AI card matched by kind/index scan")
	# Real cards carry Vector3 fields; the writer must land them as JSON
	# records ({x,y,z}), never raw Variants.
	var position: Variant = entity_debug.get("position")
	assert_typeof(position, TYPE_DICTIONARY)
	assert_true((position as Dictionary).has("x"),
			"Vector3 card fields convert to JSON records")


func test_effect_state_packed_array_lands_as_json_list() -> void:
	# The real accessor shape: get_present_effect_state_for_bms_id returns a
	# PackedVector3Array. THE regression: that shape must flow through the
	# card store as a JSON list (or stay empty), never abort the dump.
	var runtime := _runtime()
	var sim := runtime.get_sim()
	var state: PackedVector3Array = sim.get_present_effect_state_for_bms_id(1)
	assert_typeof(state, TYPE_PACKED_VECTOR3_ARRAY,
			"the binding really returns the packed shape")
	var card: Dictionary = sim.get_entity_debug(0)
	var pick := _entity_pick(card)
	pick["bms_id"] = 1  # forces the effect-state fetch through _store_card
	var snapshot := Writer.capture(_ctx_with_runtime(runtime), [pick])
	assert_eq(int(snapshot.get("pick_count", -1)), 1,
			"the dump completed with the packed effect state in the path")


func test_cardless_pick_reports_stale_with_identity_intact() -> void:
	var runtime := _runtime()
	var pick := _entity_pick({})
	pick["kind"] = 7  # matches no AI card
	pick["bms_id"] = 5555  # matches no destruction/effect state
	pick["net_id"] = 0
	pick["name"] = "RckS07"
	var snapshot := Writer.capture(_ctx_with_runtime(runtime), [pick])
	var entry: Dictionary = (snapshot.get("picks", []) as Array)[0]
	assert_true(bool(entry.get("stale", false)))
	assert_eq(String((entry.get("identity", {}) as Dictionary).get("name", "")), "RckS07")
	assert_eq(int((entry.get("pick", {}) as Dictionary).get("face", -1)), 17,
			"the replayable ray survives even when nothing resolves live")


func test_write_and_default_path_roundtrip() -> void:
	var snapshot := Writer.capture(_ctx_with_runtime(_runtime()), [])
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
