extends GutTest

## S24 B8 (ADR 0046 S24, In the views): the mission's logic in its 3D view, in a window, the editor's ImGui pass
## attached and the Document window's canvas drawing the device each frame, over a project minted from the fixtures
## (the mission, its terrain and textures, its env, the item table, the models). A tests/windowed/ script, run by
## `scripts/test_godot.sh --suite core --windowed`.
## - Pick in view: a chip's pick for event 1's trigger's entity (the workspace's document pick), then a click on a
##   shown item's mark: the trigger's entity is that item's SSN, and the pick ends. The click is the wire's
##   (`edit_in_viewport`'s click command), which drives the canvas a person clicks, its frame followed, a press and
##   a release at the mark's picture pixel (ViewportModel::click), over the window's own device: imgui-godot reads
##   ImGui's pointer from the OS's each frame (DisplayServer.mouse_get_position), which no test may move, so an
##   event pushed through the window's input moves nothing there.
## - A group's colour drawn: coloured by group, the window's own picture holds the walker's group's ring colour
##   (the editor's palette, mission_overlay.h's mission_group_rgb) about its mark; coloured by team, it does not.

const EditorFixture := preload("res://tests/authoring/editor_fixture.gd")
const MISSION_PATH := "missions/synth_logic.bms"

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	_seam = await EditorFixture.boot(self, "mission logic device", _dirs, 3)
	_app = _seam.app if _seam != null else null


func after_each() -> void:
	await EditorFixture.teardown(self, _dirs)


func _state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": MISSION_PATH, "limit": 200})


func _frames(count: int) -> void:
	for _frame in count:
		await get_tree().process_frame


## The project, the mission open and ready, its device drawn by the Document window's canvas; the device.
func _open_mission() -> SubViewport:
	var root := EditorFixture.new_project(self, _seam, _dirs, "Mission Logic Device Game")
	var models := ["pump.3di", "armory.3di", "shed.3di"]
	EditorFixture.mint_tmap_mission(
		self, _seam, root, {"small": true, "items": true, "models": models}
	)
	assert_true(_seam.open_document(MISSION_PATH))
	var state := _state()
	for _frame in 900:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)):
			break
		await get_tree().process_frame
		state = _state()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var device: SubViewport = _app.get_viewport_device(MISSION_PATH, "mission")
	assert_not_null(device)
	if device == null:
		return null
	for _frame in 600:
		if _app.get_viewport_canvas_at(MISSION_PATH, "mission") != Vector2.ZERO:
			break
		await get_tree().process_frame
	assert_ne(_app.get_viewport_canvas_at(MISSION_PATH, "mission"), Vector2.ZERO, "the canvas drew the picture")
	# Every entity and area framed, so the people stand on the picture.
	assert_true(_seam.done({"kind": "edit_in_viewport", "path": MISSION_PATH, "command": {"name": "frame", "kind": "mission"}}))
	await _frames(20)
	return device


## The first mark of `kind` on the picture in the viewport's items (its window pixel: where the canvas drew the
## picture, in the screen's pixels as ImGui's platform windows place it, less the window's place on the screen, plus
## its picture pixel), with its id; {} for none.
func _shown(device: SubViewport, kind: String) -> Dictionary:
	var at: Vector2 = _app.get_viewport_canvas_at(MISSION_PATH, "mission") - Vector2(get_window().position)
	for item: Variant in _state().get("items", []):
		var mark: Dictionary = item
		if String(mark.get("kind", "")) != kind or not mark.has("screen"):
			continue
		var pixel := Vector2(float(mark["screen"][0]), float(mark["screen"][1]))
		if pixel.x < 8.0 or pixel.y < 8.0 or pixel.x > float(device.size.x) - 8.0 or pixel.y > float(device.size.y) - 8.0:
			continue
		return {"id": int(mark["id"]), "at": at + pixel, "pixel": pixel}
	return {}


## Event 1's first trigger, by its record identity, and the value of its `slot`'s chip (the board query, S24).
func _trigger() -> Dictionary:
	var board: Dictionary = _seam.query("mission_logic", {"path": MISSION_PATH, "op": "board"})
	var cards: Array = board.get("cards", [])
	if cards.is_empty():
		return {}
	var triggers: Array = (cards[0] as Dictionary).get("triggers", [])
	if triggers.is_empty():
		return {}
	var line: Dictionary = triggers[0]
	var values := {}
	for chip: Variant in line.get("chips", []):
		values[int((chip as Dictionary).get("slot", -1))] = int((chip as Dictionary).get("value", 0))
	return {"record": int(line.get("record", 0)), "values": values}


func _pick_waits() -> bool:
	var workspace: Dictionary = _seam.state(["workspace"]).get("workspace", {})
	for entry: Variant in workspace.get("documents", []):
		var shown: Dictionary = entry
		if String(shown.get("path", "")).ends_with(MISSION_PATH):
			return shown.get("pick") is Dictionary
	return false


func test_a_click_on_the_picture_picks_for_a_chip() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	var device := await _open_mission()
	if device == null:
		return
	var trigger := _trigger()
	assert_gt(int(trigger.get("record", 0)), 0, "event 1's trigger on the board: %s" % str(trigger))
	if trigger.is_empty():
		return
	# The chip's Pick in view: the trigger's entity (param1).
	assert_true(_seam.done({"kind": "set_workspace", "workspace": {"document": {"path": MISSION_PATH,
			"pick": {"record": trigger["record"], "slot": 0}}}}))
	await _frames(5)
	assert_true(_pick_waits(), "the pick waits")
	# A shown item's mark (the pumps: any entity fills an entity slot), clicked at its picture pixel.
	var target := _shown(device, "item")
	assert_false(target.is_empty(), "an item's mark on the picture")
	if target.is_empty():
		return
	var pixel: Vector2 = target["pixel"]
	assert_true(_seam.done({"kind": "edit_in_viewport", "path": MISSION_PATH,
			"command": {"name": "click", "kind": "mission", "at": [pixel.x, pixel.y]}}))
	assert_true(_seam.settle())
	await _frames(3)
	var picked := _trigger()
	var values: Dictionary = picked.get("values", {})
	assert_false(_pick_waits(), "the click ended the pick")
	# The trigger's entity is the clicked item's SSN (its record's `id` field).
	var ssn := -1
	var record: Dictionary = _seam.query("record", {"path": MISSION_PATH, "id": int(target["id"])})
	for field: Variant in record.get("fields", []):
		if String((field as Dictionary).get("id", "")) == "id":
			ssn = int((field as Dictionary).get("value", -1))
	assert_gt(ssn, 0, "the clicked item's SSN: %s" % str(record.get("fields", [])).left(200))
	assert_eq(int(values.get(0, 0)), ssn,
			"the trigger names the item clicked (record %d) by its SSN: %s" % [int(target["id"]), str(values)])


## The group colour of the editor's palette (mission_overlay.cpp's mission_group_rgb: the golden angle a group, three
## saturation and value tiers in turn).
func _group_colour(group: int) -> Color:
	var hue := fmod(float(group) * 0.3819660112501051, 1.0)
	var saturation := [0.85, 0.55, 0.95][group % 3] as float
	var value := [1.0, 0.95, 0.8][(group / 3) % 3] as float
	return Color.from_hsv(hue, saturation, value)


## Whether the window's picture holds `colour` (within `slack` a channel) on a ring of radius 5..7 about `at`.
func _ring_holds(image: Image, at: Vector2, colour: Color, slack := 0.12) -> bool:
	for step in 72:
		var angle := TAU * float(step) / 72.0
		for radius in [5.0, 6.0, 7.0]:
			var pixel := Vector2i(at + Vector2(cos(angle), sin(angle)) * radius)
			if pixel.x < 0 or pixel.y < 0 or pixel.x >= image.get_width() or pixel.y >= image.get_height():
				continue
			var held := image.get_pixelv(pixel)
			if absf(held.r - colour.r) <= slack and absf(held.g - colour.g) <= slack and absf(held.b - colour.b) <= slack:
				return true
	return false


func test_a_group_colour_is_drawn() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	var device := await _open_mission()
	if device == null:
		return
	# A person on the picture: the walker (the first organic) is in group 1, the guard in group 2.
	var walker := _shown(device, "organic")
	assert_false(walker.is_empty(), "a person's mark on the picture")
	if walker.is_empty():
		return
	var group := 1 if int(walker["id"]) == int(_first_organic_id()) else 2
	assert_true(_seam.done({"kind": "set_viewport", "path": MISSION_PATH,
			"viewport": {"kind": "mission", "options": {"color_by": "group"}}}))
	await _frames(10)
	var image := get_viewport().get_texture().get_image()
	assert_true(_ring_holds(image, walker["at"], _group_colour(group)),
			"coloured by group: group %d's colour %s rings the person's mark" % [group, _group_colour(group)])
	assert_true(_seam.done({"kind": "set_viewport", "path": MISSION_PATH,
			"viewport": {"kind": "mission", "options": {"color_by": "team"}}}))
	await _frames(10)
	image = get_viewport().get_texture().get_image()
	assert_false(_ring_holds(image, walker["at"], _group_colour(group)), "coloured by team: no group ring")


func _first_organic_id() -> int:
	for item: Variant in _state().get("items", []):
		if String((item as Dictionary).get("kind", "")) == "organic":
			return int((item as Dictionary)["id"])
	return 0
