extends GutTest

## The mission viewport over a shipped mission (ADR 0046 S14 V11; OPENNOVA_JO_DIR): the largest
## mission by entities (ASH_I1gA.bms, 2,193 entities: editor_mission_viewport's retail row prints it)
## imported with its dependencies into a new project on the install (its closure: the import the
## flow's end-to-end script makes), opened, and its picture built over the Shell's frames at a
## budget of 0 (one unit a frame) to `ready`: the units in the build's order (the environment, the
## terrain's files, the terrain a tile a step, the sky, the water, the item table, a unit per graphic,
## the placement's units, the static shadows bound, the pose), the terrain built (the body's `ground`), every entity drawn
## placed (one placement, none lifted or hidden). Prints what the build cost on the frames (frames,
## frame_us, unit_us, total_us), each label's units and time, and the three longest units: the
## measurement the first-picture budget and the stepped placement (D5) are read against.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const MISSION := "ASH_I1gA.bms"
## How long the picture's build may stand still (no unit run) before it counts as a hang: the build
## takes as long as it takes while it moves (the import's settle is the seam's, likewise by progress).
const BUILD_STALL_MS := 120000

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	if RetailData.install().is_empty():
		return
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor retail mission viewport %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


func after_each() -> void:
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()
	await get_tree().process_frame


func _state(path: String) -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": path, "limit": 1})


## The state's progress and its device's build ({} for none: a progress is null once ready).
func _progress(state: Dictionary) -> Dictionary:
	var progress: Variant = state.get("progress")
	return progress if progress is Dictionary else {}


func _build(state: Dictionary) -> Dictionary:
	var device: Variant = state.get("device")
	var build: Variant = device.get("build") if device is Dictionary else null
	return build if build is Dictionary else {}


## A request served and its operation settled (the seam's request settles it for as long as it
## moves): whether it was done.
func _served(fields: Dictionary) -> bool:
	var answer: Dictionary = _seam.request(fields)
	var done := bool(answer.get("outcome", {}).get("done", false))
	assert_true(done, "%s: %s" % [String(fields.get("kind", "")), str(answer)])
	return done and bool(_seam.settled)


func test_the_largest_mission_builds() -> void:
	var install := RetailData.install()
	if install.is_empty():
		pending("OPENNOVA_JO_DIR (a game install holding %s) is required" % MISSION)
		return
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor retail mission %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Retail Mission Viewport"))
	assert_true(_served({"kind": "apply_project_settings", "settings": {"game_install": install, "mission": true}}))
	var started := Time.get_ticks_msec()
	assert_true(_served({"kind": "preview_install_import", "names": [MISSION], "with_dependencies": true}))
	var plan: Dictionary = _seam.query("import_preview", {"limit": 1})
	assert_gt(int(plan.get("count", 0)), 0, "the closure planned: %s" % str(plan).left(400))
	assert_true(_served({"kind": "import_files", "planned": true}))
	gut.p("%s's closure: %d files, %.1f MB, imported in %.1f s" % [MISSION, int(plan.get("count", 0)),
			float(plan.get("total_bytes", 0)) / 1e6, float(Time.get_ticks_msec() - started) / 1000.0])

	_app.build_budget_ms = 0
	assert_true(_seam.open_document(MISSION), "%s opens" % MISSION)
	var path := String(_seam.state(["documents"]).get("documents", {}).get("active", ""))
	assert_true(path.to_lower().ends_with(MISSION.to_lower()), path)
	_app.pump()
	var state := _state(path)
	assert_eq(String(state.get("status", "")), "loading", str(state).left(400))
	# One unit a frame: what each frame's unit cost is the build's total_us moving, the unit the one
	# the progress named before it ran (its label is what the next unit makes).
	var labels: PackedStringArray = []
	var units_by_label := {}
	var us_by_label := {}
	var units: Array = []
	var built: Dictionary = _build(state)
	var moved_at := Time.get_ticks_msec()
	while String(state.get("status", "")) == "loading" and Time.get_ticks_msec() - moved_at < BUILD_STALL_MS:
		var label := String(_progress(state).get("label", ""))
		var done := int(_progress(state).get("done", 0))
		var spent := int(built.get("total_us", 0))
		await get_tree().process_frame
		state = _state(path)
		var now: Dictionary = _build(state)
		var ran := int(_progress(state).get("done", done)) - done
		if String(state.get("status", "")) != "loading":
			ran = maxi(ran, 1)
		if ran <= 0:
			built = now
			continue
		moved_at = Time.get_ticks_msec()
		if labels.is_empty() or labels[labels.size() - 1] != label:
			labels.append(label)
		var cost := int(now.get("total_us", spent)) - spent
		units.append([cost, label, int(units_by_label.get(label, 0)) + 1])
		units_by_label[label] = int(units_by_label.get(label, 0)) + ran
		us_by_label[label] = int(us_by_label.get(label, 0)) + cost
		built = now
	_app.pump()
	state = _state(path)
	assert_eq(String(state.get("status", "")), "ready", str(state).left(400))
	built = _build(state)
	assert_eq(int(built.get("done", 0)), int(built.get("total", -1)), str(built))
	assert_eq(int(state.get("builds", 0)), 1)
	assert_eq(labels, PackedStringArray(["environment", "terrain files", "terrain", "sky", "water", "items", "models",
			"place", "shadows", "pose"]))
	for label in ["terrain", "models", "place"]:
		assert_gt(int(units_by_label.get(label, 0)), 0, "the build's %s units" % label)
	assert_true(bool(state.get("body", {}).get("ground", false)), "the terrain built: a surface a ray lands on")
	assert_eq(int(_app.get_mission_device_count(path, "placements")), 1, "one placement")
	assert_gt(int(_app.get_mission_device_count(path, "placed")), 0, "the entities placed")
	assert_eq(int(_app.get_mission_device_count(path, "lifted")), 0)
	assert_eq(int(_app.get_mission_device_count(path, "hidden")), 0)

	units.sort_custom(func(a: Array, b: Array) -> bool: return int(a[0]) > int(b[0]))
	var longest := PackedStringArray()
	for i in mini(3, units.size()):
		var label := String(units[i][1])
		longest.append("%s %d of %d, %.1f ms" % [label, int(units[i][2]), int(units_by_label.get(label, 0)),
				float(units[i][0]) / 1000.0])
	var by_label := PackedStringArray()
	for label in labels:
		by_label.append("%s %d units %.1f ms" % [label, int(units_by_label.get(label, 0)),
				float(us_by_label.get(label, 0)) / 1000.0])
	gut.p("%s at 0 ms a frame: %d units over %d frames, frame_us %d, unit_us %d, total_us %d; %d placed, %d lifted, place %.1f ms" % [
			MISSION, int(built.get("total", 0)), int(built.get("frames", 0)), int(built.get("frame_us", 0)),
			int(built.get("unit_us", 0)), int(built.get("total_us", 0)), int(_app.get_mission_device_count(path, "placed")),
			int(_app.get_mission_device_count(path, "lifted")), float(_app.get_mission_device_count(path, "place_us")) / 1000.0])
	gut.p("  by label: " + ", ".join(by_label))
	gut.p("  the three longest units: " + ", ".join(longest))
