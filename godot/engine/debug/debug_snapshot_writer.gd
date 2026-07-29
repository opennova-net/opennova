class_name DebugSnapshotWriter
## The debug snapshot: one JSON file reproducing "I was standing HERE, aiming
## THERE, and THAT entity was doing THIS" — the pose-dump workflow grown to
## carry picked entities. The mission/player/view blocks keep the exact shape
## the pose-replay and perf-fire probes already read; picks[] adds one
## enriched section per picked entity (identity + transform + the pick ray +
## the whole live debug cards, so a reader can both REPLAY the shot and know
## what the entity was doing at pick time).
##
## Static and UI-free: pages call capture()/write() and surface the returned
## error strings on their own status labels. Never throws; a partial write is
## deleted.

const SCHEMA := "opennova.debug_snapshot.v1"
const SNAPSHOT_DIR := "user://debug/snapshots"
const DEFAULT_PLAYER_FOV_H_DEG := 80.0
const MICROSECONDS_PER_SECOND := 1_000_000.0
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

## The no-overwrite sequence for default paths (rapid consecutive snapshots
## never collide even within one microsecond stamp).
static var _sequence := 0


## Sample the live runtime NOW (never a cached label beat) into the snapshot
## dictionary. Empty when there is no local player to anchor the pose —
## the pose-dump contract.
static func capture(ctx: NovaDebugContext, picks: Array = []) -> Dictionary:
	var runtime := ctx.runtime()
	var sim := ctx.sim()
	var snapshot := _capture_local_player_pose(runtime, sim, ctx)
	if snapshot.is_empty():
		return {}
	var enriched: Array = []
	for pick in picks:
		if pick is Dictionary:
			enriched.append(_enrich_pick(pick, ctx))
	snapshot["picks"] = enriched
	snapshot["pick_count"] = enriched.size()
	return snapshot


## Write one captured snapshot. Returns {path: String, error: String} — path
## is the absolute file on success (error empty), error is the artist-facing
## failure line otherwise (path empty).
static func write(snapshot: Dictionary, target_path: String) -> Dictionary:
	var absolute := ProjectSettings.globalize_path(target_path)
	var directory := absolute.get_base_dir()
	var mkdir_error := DirAccess.make_dir_recursive_absolute(directory)
	if mkdir_error != OK:
		return {"path": "", "error":
				"Could not create snapshot folder: %s" % error_string(mkdir_error)}
	var file := FileAccess.open(absolute, FileAccess.WRITE)
	if file == null:
		return {"path": "", "error": "Could not write snapshot: %s" %
				error_string(FileAccess.get_open_error())}
	file.store_string(JSON.stringify(snapshot, "\t") + "\n")
	file.flush()
	var write_error := file.get_error()
	file.close()
	if write_error != OK:
		DirAccess.remove_absolute(absolute)
		return {"path": "", "error":
				"Could not finish snapshot: %s" % error_string(write_error)}
	return {"path": absolute, "error": ""}


## The timestamped default location under the OpenNova user-data folder:
## <mission_stem>_<utc>_<usec>_<seq>.json, never overwriting.
static func default_path(snapshot: Dictionary) -> String:
	var mission: Dictionary = snapshot.get("mission", {})
	var mission_stem := String(mission.get("file", "")).get_file().get_basename()
	if mission_stem.is_empty():
		mission_stem = String(mission.get("name", ""))
	if mission_stem.is_empty():
		mission_stem = "mission"
	mission_stem = mission_stem.validate_filename().replace(" ", "_")
	var stamp := String(snapshot.get("captured_at_utc", "")) \
			.replace("-", "").replace(":", "")
	var unix_usec := int(Time.get_unix_time_from_system() * MICROSECONDS_PER_SECOND)
	var target_path := ""
	while target_path.is_empty() or FileAccess.file_exists(target_path):
		_sequence += 1
		target_path = SNAPSHOT_DIR.path_join("%s_%s_%d_%03d.json" % [
			mission_stem, stamp, unix_usec, _sequence])
	return target_path


# --- Pose capture (the opennova.player_pose.v1 shape, kept verbatim) ---------

static func _capture_local_player_pose(runtime: Object, sim: Object,
		ctx: NovaDebugContext) -> Dictionary:
	if runtime == null or sim == null or not sim.has_method("has_local_player") \
			or not bool(sim.has_local_player()):
		return {}
	if not sim.has_method("get_local_player_position") \
			or not sim.has_method("get_local_player_yaw_deg") \
			or not sim.has_method("get_local_player_pitch_deg"):
		return {}

	var position_godot: Vector3 = sim.get_local_player_position()
	var position_bms: Vector3 = MissionObjectPlacer.godot_to_bms_position(position_godot)
	var yaw_deg := float(sim.get_local_player_yaw_deg())
	var pitch_deg := float(sim.get_local_player_pitch_deg())
	var view: Dictionary = sim.get_local_player_view() \
			if sim.has_method("get_local_player_view") else {}
	var view_roll_deg := float(view.get("fp_roll_deg", 0.0))
	var yaw_rad := deg_to_rad(yaw_deg)
	var pitch_rad := deg_to_rad(pitch_deg)
	var forward_godot := Vector3(
			sin(yaw_rad) * cos(pitch_rad),
			sin(pitch_rad),
			-cos(yaw_rad) * cos(pitch_rad))
	var mission_file := String(runtime.get_mission_file()) \
			if runtime.has_method("get_mission_file") else ""
	var mission_name := String(runtime.get_mission_name()) \
			if runtime.has_method("get_mission_name") else ""

	return {
		"schema": SCHEMA,
		"captured_at_utc": Time.get_datetime_string_from_system(true, false) + "Z",
		"logic_tick": int(sim.get_logic_tick()) if sim.has_method("get_logic_tick") else -1,
		"mission": {
			"file": mission_file,
			"name": mission_name,
		},
		"player": {
			"position_bms": _vector3_record(position_bms),
			"position_godot": _vector3_record(position_godot),
			"orientation_mission_deg": {
				"yaw": yaw_deg,
				"pitch": pitch_deg,
				"view_roll": view_roll_deg,
			},
			"forward_godot": _vector3_record(forward_godot),
		},
		"view": {
			"fov_horizontal_deg": float(view.get(
					"fov_h_deg", DEFAULT_PLAYER_FOV_H_DEG)),
			"scope_engaged": bool(view.get("scope_engaged", false)),
			"mounted": bool(view.get("mounted", false)),
			"camera": _capture_camera_snapshot(ctx),
		},
		"coordinate_conventions": {
			"bms": "Mission coordinates (x, y horizontal; z up)",
			"godot": "Global world coordinates (x, z horizontal; y up)",
			"yaw": "Mission yaw: 0 faces BMS +Y / Godot -Z",
			"pitch": "Positive looks up",
			"camera_rotation": "Godot world-space quaternion",
		},
	}


static func _capture_camera_snapshot(ctx: NovaDebugContext) -> Dictionary:
	var context := ctx.view_context()
	if context == null:
		return {}
	var camera := context.camera
	if camera == null or not is_instance_valid(camera):
		return {}
	var camera_transform := camera.global_transform
	var camera_basis := camera_transform.basis.orthonormalized()
	var camera_position := camera_transform.origin
	var viewport_size := Vector2.ZERO
	var viewport := camera.get_viewport()
	if viewport != null:
		viewport_size = viewport.get_visible_rect().size
	var viewport_aspect := viewport_size.x / viewport_size.y \
			if viewport_size.y > 0.0 else 0.0
	var mode := "unknown"
	if context.camera_mode_known:
		mode = "third_person" if context.third_person else "first_person"
	return {
		"mode": mode,
		"position_godot": _vector3_record(camera_position),
		"position_bms": _vector3_record(
				MissionObjectPlacer.godot_to_bms_position(camera_position)),
		"orientation_quaternion_godot": _quaternion_record(
				camera_basis.get_rotation_quaternion()),
		"right_godot": _vector3_record(camera_basis.x),
		"up_godot": _vector3_record(camera_basis.y),
		"forward_godot": _vector3_record(-camera_basis.z),
		"godot_fov_deg": camera.fov,
		"projection": int(camera.projection),
		"keep_aspect": int(camera.keep_aspect),
		"viewport_size": _vector2_record(viewport_size),
		"viewport_aspect": viewport_aspect,
		"near": camera.near,
		"far": camera.far,
	}


# --- Pick enrichment ---------------------------------------------------------

## One snapshot section per pick: identity + transform + the replayable ray,
## then the WHOLE live debug cards (they are already curated surfaces — a
## snapshot reader wants everything the F3 pages could see). A pick whose
## entity no longer resolves keeps its identity/pick blocks with stale=true.
static func _enrich_pick(pick: Dictionary, ctx: NovaDebugContext) -> Dictionary:
	var kind := int(pick.get("kind", -1))
	var index := int(pick.get("index", -1))
	var bms_id := int(pick.get("bms_id", 0))
	var net_id := int(pick.get("net_id", 0))
	var item_id := int(pick.get("item_id", 0))
	var position_godot: Vector3 = pick.get("position_godot", Vector3.ZERO)
	var hit_godot: Vector3 = pick.get("hit_position_godot", Vector3.ZERO)
	var entry := {
		"identity": {
			"entity_handle": int(pick.get("entity_handle", -1)),
			"pool": int(pick.get("pool", -1)),
			"kind": kind,
			"index": index,
			"bms_id": bms_id,
			"net_id": net_id,
			"item_id": item_id,
			"name": String(pick.get("name", "")),
			"display_name": "",
			"graphic": "",
		},
		"transform": {
			"position_godot": _vector3_record(position_godot),
			"position_bms": _vector3_record(
					MissionObjectPlacer.godot_to_bms_position(position_godot)),
			"bound_radius": float(pick.get("bound_radius", 0.0)),
		},
		"pick": {
			"source": String(pick.get("source", "")),
			"picked_at_tick": int(pick.get("tick", -1)),
			"hit_class": String(pick.get("hit_class", "")),
			"ray_origin_godot": _jsonable(pick.get("ray_origin_godot", Vector3.ZERO)),
			"ray_dir_godot": _jsonable(pick.get("ray_dir_godot", Vector3.ZERO)),
			"hit_position_godot": _vector3_record(hit_godot),
			"hit_position_bms": _vector3_record(
					MissionObjectPlacer.godot_to_bms_position(hit_godot)),
			"hit_normal_godot": _jsonable(pick.get("hit_normal_godot", Vector3.ZERO)),
			"distance_units": float(pick.get("distance_units", 0.0)),
			"section": int(pick.get("section", -1)),
			"face": int(pick.get("face", -1)),
			"bone": int(pick.get("bone", -1)),
			"hit_zone": int(pick.get("hit_zone", -1)),
			"surface_type": int(pick.get("surface_type", -1)),
			"material_flags": int(pick.get("material_flags", 0)),
		},
		"stale": false,
		"entity_debug": {},
		"world_entity_debug": {},
		"destruction": {},
		"effect_state": {},
	}

	var world := ctx.world()
	if world != null and world.has_method("get_item_db"):
		var db: Variant = world.get_item_db()
		if db != null and is_instance_valid(db) and item_id > 0:
			if (db as Object).has_method("get_display_name"):
				entry["identity"]["display_name"] = String(db.get_display_name(item_id))
			if (db as Object).has_method("get_graphic"):
				entry["identity"]["graphic"] = String(db.get_graphic(item_id))

	var sim := ctx.sim()
	if sim == null:
		entry["stale"] = true
		return entry
	# The AI card is keyed by pool index, not identity: scan and match on
	# kind/index (+ bms_id when the pick carries one) — the MCP live-card
	# pattern.
	if sim.has_method("get_entity_count") and sim.has_method("get_entity_debug"):
		for i in range(int(sim.get_entity_count())):
			var card: Dictionary = sim.get_entity_debug(i)
			if card.is_empty():
				continue
			if int(card.get("kind", -2)) != kind or int(card.get("index", -2)) != index:
				continue
			if bms_id != 0 and int(card.get("bms_id", 0)) != bms_id:
				continue
			entry["entity_debug"] = _jsonable(card)
			break
	if net_id > 0 and sim.has_method("get_world_entity_debug"):
		_store_card(entry, "world_entity_debug", sim.get_world_entity_debug(net_id))
	if bms_id != 0 and sim.has_method("get_destruction_debug"):
		_store_card(entry, "destruction", sim.get_destruction_debug(bms_id))
	if bms_id != 0 and sim.has_method("get_present_effect_state_for_bms_id"):
		# NOT a Dictionary: this accessor returns the present-pass effect
		# transform vectors as a PackedVector3Array (empty when the entity
		# drives no effect). _store_card takes any shape.
		_store_card(entry, "effect_state",
				sim.get_present_effect_state_for_bms_id(bms_id))
	var stale := true
	for card_key in ["entity_debug", "world_entity_debug", "destruction", "effect_state"]:
		if not _card_is_empty(entry[card_key]):
			stale = false
			break
	entry["stale"] = stale
	return entry


## Store one live debug card, whatever container the accessor returns
## (Dictionary cards, PackedVector3Array effect state, ...): empties keep the
## entry's typed default, everything else lands JSON-converted. Duck-typed on
## purpose — a mistyped assumption here must degrade, never abort the dump.
static func _store_card(entry: Dictionary, key: String, card: Variant) -> void:
	if _card_is_empty(card):
		return
	entry[key] = _jsonable(card)


static func _card_is_empty(card: Variant) -> bool:
	if card is Dictionary:
		return (card as Dictionary).is_empty()
	var listed: Variant = _jsonable(card)
	if listed is Array:
		return (listed as Array).is_empty()
	return listed == null


# --- JSON plumbing -----------------------------------------------------------

## Recursive Variant -> JSON-native conversion: the embedded debug cards carry
## Vector3s, Quaternions and Packed*Arrays that JSON.stringify would otherwise
## flatten into unreadable strings.
static func _jsonable(value: Variant) -> Variant:
	match typeof(value):
		TYPE_VECTOR2:
			return _vector2_record(value)
		TYPE_VECTOR3:
			return _vector3_record(value)
		TYPE_QUATERNION:
			return _quaternion_record(value)
		TYPE_DICTIONARY:
			var out := {}
			for key in (value as Dictionary):
				out[String(key)] = _jsonable(value[key])
			return out
		TYPE_ARRAY, TYPE_PACKED_VECTOR3_ARRAY, TYPE_PACKED_VECTOR2_ARRAY, \
		TYPE_PACKED_FLOAT32_ARRAY, TYPE_PACKED_FLOAT64_ARRAY, \
		TYPE_PACKED_INT32_ARRAY, TYPE_PACKED_INT64_ARRAY, \
		TYPE_PACKED_STRING_ARRAY, TYPE_PACKED_BYTE_ARRAY:
			var items := []
			for item in value:
				items.append(_jsonable(item))
			return items
		TYPE_OBJECT:
			return null  # live objects never serialize; cards should not carry them
		_:
			return value


static func _vector2_record(value: Vector2) -> Dictionary:
	return {"x": value.x, "y": value.y}


static func _vector3_record(value: Vector3) -> Dictionary:
	return {"x": value.x, "y": value.y, "z": value.z}


static func _quaternion_record(value: Quaternion) -> Dictionary:
	return {"x": value.x, "y": value.y, "z": value.z, "w": value.w}
