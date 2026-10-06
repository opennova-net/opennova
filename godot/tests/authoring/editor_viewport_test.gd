extends GutTest

## The viewports headless through the editor's wire seam (ADR 0046 S13 V7; S9j, S9k, S10p, S13 V5):
## the editor boots with no ImGui context, a document's viewport is read through query_json's
## `viewport` (state, items, hit, notes, render) and changed through request_json's set_viewport and
## edit_in_viewport, and its device (the runtime's MenuFrame, ObjectModel and Camera3D in an offscreen
## SubViewport, get_viewport_device) takes what changed at the next pump. The menu: a new project's
## STARTUP shows its title where the game draws it, in its font; the device places every window where
## the viewport's own headless compile does, and the render check's compile of the screen (render) is
## the same; the game's hit test finds the title; an unsaved string table and stylesheet show at once;
## the options hold a window in a state; a drag of a window's handles writes its POSITION on the grid
## as one undo step, several selected windows move and arrange in one step each; a TABLE draws through
## the device (its rows' cells, a SUBST image, a clip rect) where the compile places it; a device given
## up retires its SubViewport, freed at the next frame; a focused edit box's caret blinks on the
## preview clock through the device's own frame (S13 V8); the game's pointer draws through the device's
## frame where a client holds it (DI-08). The model: drawn as it would save, at the level
## the portable half picks; a user point projects within half a pixel of where the device's camera
## puts it; a user point's edit builds nothing, a light's builds the scene again; the options hold a
## level and a CTRL register; a part's marker rides the part the device draws at the clock the two
## share, a hit at its pixel names its record and a drag lands it on the pixel, one undo step; a table
## plays its clip on the rig at the preview clock's tick, its bones on the wire where the device's
## skeleton stands them (S17); a model destroyed as the item naming it is (DI-10: its husk drawn in its
## place, the pieces' sections hidden, the destroy fade's registers on it, the death sound heard); two
## models on devices of their own; the
## device cache holds four, the least recently used given up and made again at the camera it kept.
## S13 V6: a device builds its picture over the frames after the pump that takes the Rebuild (a
## model's textures, meshes, scene and pose; a menu screen's textures not decoded yet, then its
## configure), so a viewport reads `loading` before `ready` and the tests await `ready` after what
## builds again; a JO-sized model loads over several frames within the build budget, the last scene
## kept until its scene unit, one unit a frame at a budget of 0, a level held swapped in place; two
## builds at once share the frame's budget.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const ARMORY := "res://../fixtures/threedi/synth/armory.3di"
const CRATE := "res://../fixtures/threedi/synth/crate.3di"
## The JO-sized model (S13 V6), as large as the 967 the game ships come near their 99th percentile
## (six levels, 4,304 triangles at the first and 10,409 in all, 16 materials, 29 texture rows, 18
## textures): the materials (a texture each, every third a detail texture too), the parts, the quads
## across each part's sheet at the first level (halved at each level after), the levels, the
## textures' side in pixels.
const LARGE_MATERIALS := 16
const LARGE_PARTS := 12
const LARGE_CELLS := 16
const LARGE_LEVELS := 5
const LARGE_SIDE := 256
const ROCKING := "res://../fixtures/threedi/synth/house_lod0_sine_rotx.3di"
const SKINNED := "res://../fixtures/threedi/o3d/skinned.o3d"
const SKIN_CLIPS := """o3a 1
adm SKIN.adm
row anim_reset "reset"
row anim_walk_forward "walk"
clip reset
fps 30
flags 0x1
frames 1
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
clip walk
fps 30
flags 0x1
frames 4
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0.3826834 0.9238795
 k 0 0 0.7071068 0.7071068
 k 0 0 0.3826834 0.9238795
 k 0 0 0 1
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x1 0.9 1.7
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x2 0.9 1.7
event 0 0 0 0x2 0.9 1.7
"""

## S17: a set whose idle turns the pelvis a quarter about x from its frame 1, swinging the spine and the
## leg off the axis (the bones the wire reports against the device's skeleton).
const BEND_CLIPS := """o3a 1
adm BEND.adm
row anim_reset "rest"
row anim_idle "bend"
clip rest
fps 30
flags 0x1
frames 1
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
clip bend
fps 30
flags 0x1
frames 2
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0.7071068 0 0 0.7071068
 k 0.7071068 0 0 0.7071068
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
"""

## A screen with a TABLE (the table layer the menu frame compiles since the trunk's third master
## sync): two columns, the first a BITMAP_DRAW column whose "1" cells draw chk.tga (SUBST FILE).
const TABLE_MENU := ("<SCREEN>\n\t<NAME>TBL</NAME>\n\t<WINDOW type=\"window\" name=\"ROOT\">\n"
		+ "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\n"
		+ "\t\t<FONT><NAME>%DEF_FONTNAME_LG%</NAME><DEFAULT_FG>%DEF_TEXT_FG%</DEFAULT_FG></FONT>\n"
		+ "\t\t<APPEARANCE type=\"color\" state=\"default\">FF203040</APPEARANCE>\n"
		+ "\t\t<WINDOW type=\"table\" name=\"LIST\">\n"
		+ "\t\t\t<POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>400</RIGHT><BOTTOM>300</BOTTOM></POSITION>\n"
		+ "\t\t\t<APPEARANCE type=\"color\" state=\"default\">FF101010</APPEARANCE>\n"
		+ "\t\t\t<COLUMN count=\"2\" spacing=\"0\">\n"
		+ "\t\t\t\t<HEADER column=\"0\" width=\"60\">Pick</HEADER>\n"
		+ "\t\t\t\t<HEADER column=\"1\" width=\"200\">Name</HEADER>\n"
		+ "\t\t\t\t<BODY column=\"0\" BITMAP_DRAW></BODY>\n"
		+ "\t\t\t\t<BODY column=\"1\"></BODY>\n"
		+ "\t\t\t\t<SUBST column=\"0\" value=\"1\" FILE>chk.tga</SUBST>\n"
		+ "\t\t\t</COLUMN>\n"
		+ "\t\t\t<ITEMS><APPEARANCE type=\"color\" state=\"selected\">FF336699</APPEARANCE></ITEMS>\n"
		+ "\t\t\t<MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>\n"
		+ "\t\t</WINDOW>\n\t</WINDOW>\n</SCREEN>\n")

## A screen with an edit box in the stylesheet's large font (its caret blinks while it is focused).
const CARET_MENU := ("<SCREEN>\n\t<NAME>CARET</NAME>\n\t<WINDOW type=\"window\" name=\"ROOT\">\n"
		+ "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\n"
		+ "\t\t<FONT><NAME>%DEF_FONTNAME_LG%</NAME><DEFAULT_FG>%DEF_TEXT_FG%</DEFAULT_FG></FONT>\n"
		+ "\t\t<WINDOW type=\"edit\" name=\"NAME_BOX\">\n"
		+ "\t\t\t<POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>400</RIGHT><BOTTOM>140</BOTTOM></POSITION>\n"
		+ "\t\t</WINDOW>\n\t</WINDOW>\n</SCREEN>\n")

var _dirs: Array[String] = []
var _app: Node = null
## The typed seam the tests knew, over the app's request_json and query_json (S13 A5).
var _seam: RefCounted = null


func before_each() -> void:
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor viewport %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


func after_each() -> void:
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()
	# A device's rebuild queues its model's old nodes for deletion (ObjectModel's clear); the frame
	# frees them before GUT counts orphans.
	await get_tree().process_frame


# --- the viewport through the wire seam ------------------------------------------------------------

## A read of the viewport over `path` ("" the active document's) by `op`, the viewport query's answer
## ({"error": why} when it refused).
func _viewport(op: String, args := {}, path := "") -> Dictionary:
	var params: Dictionary = args.duplicate()
	params["op"] = op
	if not path.is_empty():
		params["path"] = path
	return _seam.query("viewport", params)


## The viewport's envelope, every item and note on its one page (a screen has fewer than 200).
func _state(path := "") -> Dictionary:
	return _viewport("state", {"limit": 200}, path)


## A request on the viewport (set_viewport, edit_in_viewport), its device synced at once (the pump):
## the request's answer.
func _ask(fields: Dictionary, path := "") -> Dictionary:
	var request: Dictionary = fields.duplicate()
	if not path.is_empty():
		request["path"] = path
	var answer: Dictionary = _seam.request(request)
	_app.pump()
	return answer


## A set_viewport of `change`: true when done.
func _change(change: Dictionary, path := "") -> bool:
	return bool(_ask({"kind": "set_viewport", "viewport": change}, path).get("outcome", {}).get("done", false))


## A set_viewport's refusal ("" when it was done).
func _refusal(change: Dictionary, path := "") -> String:
	var outcome: Dictionary = _ask({"kind": "set_viewport", "viewport": change}, path).get("outcome", {})
	if bool(outcome.get("done", false)):
		return ""
	var findings: Array = outcome.get("findings", [])
	return String(findings[0].get("message", "?")) if not findings.is_empty() else "?"


## An edit_in_viewport's drag (`drag` its object): true when done.
func _drag(drag: Dictionary, path := "") -> bool:
	return bool(_ask({"kind": "edit_in_viewport", "drag": drag}, path).get("outcome", {}).get("done", false))


## An edit_in_viewport's command: true when done.
func _command(name: String, ids: Array, path := "") -> bool:
	var command := {"name": name}
	if not ids.is_empty():
		command["ids"] = ids
	return bool(_ask({"kind": "edit_in_viewport", "command": command}, path).get("outcome", {}).get("done", false))


## The viewport `state` names, ready on its device: a frame at a time until the device made its
## picture (its next pump takes what the viewport asks; S13 V6: a device builds its picture over the
## frames that follow, `loading` until it is built).
func _await_ready(path := "") -> Dictionary:
	var state := _state(path)
	for _frame in 600:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0:
			break
		await get_tree().process_frame
		state = _state(path)
	return state


## The device drawing the viewport `state` describes: its SubViewport, null for none.
func _device(state: Dictionary) -> SubViewport:
	return _app.get_viewport_device(String(state.get("path", "")), String(state.get("kind", "")))


## The first node of the class `type` the device holds (a MenuFrame, an ObjectModel, a Camera3D).
func _device_node(state: Dictionary, type: String) -> Node:
	var device := _device(state)
	if device == null:
		return null
	var found := device.find_children("*", type, true, false)
	return found[0] if not found.is_empty() else null


func _item(state: Dictionary, name: String) -> Dictionary:
	for item: Variant in state.get("items", []):
		if item is Dictionary and String(item.get("name", "")) == name:
			return item as Dictionary
	return {}


func _items_of(state: Dictionary, kind: String) -> Array:
	var rows: Array = []
	for row: Variant in state.get("items", []):
		if row is Dictionary and String(row.get("kind", "")) == kind:
			rows.append(row)
	return rows


func _vector(values: Variant) -> Vector3:
	var list: Array = values if values is Array else [0, 0, 0]
	return Vector3(float(list[0]), float(list[1]), float(list[2]))


func _write(path: String, bytes: PackedByteArray) -> void:
	assert_eq(DirAccess.make_dir_recursive_absolute(path.get_base_dir()), OK)
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "wrote %s" % path)
	if file != null:
		file.store_buffer(bytes)
		file.close()


## An uncompressed 32-bit TGA of `size` x `size`, top-left first, one colour.
func _tga(size: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18)
	bytes[2] = 2
	bytes[12] = size & 0xFF
	bytes[13] = (size >> 8) & 0xFF
	bytes[14] = size & 0xFF
	bytes[15] = (size >> 8) & 0xFF
	bytes[16] = 32
	bytes[17] = 0x28
	for i in size * size:
		bytes.append_array(PackedByteArray([40, 200, 255, 255]))
	return bytes


func _string_record(table: String, key: String) -> int:
	assert_true(_seam.open_document(table))
	for i in _seam.get_row_count():
		var row: int = _seam.get_row_id(i)
		for id: int in _seam.get_child_records(row, "string"):
			if String(_seam.get_field(id, "key")) == key:
				return id
	return 0


func _new_project(title: String) -> String:
	var dir := OS.get_cache_dir().path_join("opennova editor viewport project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, title))
	return dir


## A new project with `model` copied into its models/ as `name`, the files scanned again.
func _new_project_with(model: String, name: String) -> bool:
	_new_project("Model Viewport Game")
	_add_model(model, name)
	return true


## A copy of `model` written into the open project's models/ as `name`, the files scanned again.
func _add_model(model: String, name: String) -> void:
	var root: String = _seam.get_project_root()
	_write(root.path_join("models").path_join(name), FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(model)))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")


func _first_child(kind: String) -> int:
	var row: int = _seam.get_row_id(0)
	var ids: PackedInt64Array = _seam.get_child_records(row, kind)
	return ids[0] if ids.size() > 0 else 0


# --- the menu --------------------------------------------------------------------------------------

func test_menu_follows_its_tables_and_its_style() -> void:
	if _app == null:
		return
	assert_false(_app.is_available(), "headless: no ImGui context")
	assert_true(String(_viewport("state").get("error", "")).contains("no document is open"), "no project, no document")
	_new_project("Preview Game")
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(String(_viewport("state").get("error", "")).contains("no document is open"))
	# Read before the editor's next frame: the menu's viewport (its first read makes it), no device
	# attached to it yet and nothing built; the frames after attach one (the empty states the preview
	# tools answered with no project, no menu or no model are no wire's now: the query refuses with no
	# document open, and a stylesheet shows in no viewport).
	_app.request_json(JSON.stringify({"kind": "open_document", "path": "main.mnu"}))
	var before := _state()
	assert_eq(String(before.get("kind", "")), "menu", str(before))
	assert_false(bool(before.get("device", {}).get("attached", true)), str(before))
	assert_eq(int(before.get("builds", -1)), 0, str(before))
	assert_true(_seam.settle())
	var opened := await _await_ready()
	assert_eq(String(opened.get("status", "")), "ready", "opened, a menu shows its first screen: %s" % str(opened))
	assert_eq(String(opened.get("kind", "")), "menu")
	assert_eq(String(opened.get("body", {}).get("screen", {}).get("name", "")), "STARTUP")
	assert_true(bool(opened.get("device", {}).get("attached", false)), "its device attached: %s" % str(opened))
	var menu := String(opened.get("path", ""))
	var title: int = _seam.find_record("TITLE")
	var main: int = _seam.find_record("MAIN")
	var exit: int = _seam.find_record("EXIT")
	assert_gt(title, 0)
	assert_true(_seam.select_record(title))

	# STARTUP as the game draws it.
	var preview := _state()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	assert_true(bool(preview.get("current", false)))
	assert_eq(preview.get("body", {}).get("missing", [1]).size(), 0, str(preview))
	assert_eq(preview.get("body", {}).get("unreadable", [1]).size(), 0, str(preview))
	var widget := _item(preview, "TITLE")
	assert_eq(String(widget.get("text", "")), "Preview Game", str(widget))
	assert_eq(int(widget.get("id", 0)), title)
	var rect: Array = widget.get("rect", [])
	assert_eq(rect.size(), 4, str(widget))
	if rect.size() != 4:
		return
	assert_gt(float(rect[3]), float(rect[1]), "a text-sized height")
	assert_eq(int(rect[1]), 75 + 120, "absolute: MAIN's top plus its own")
	assert_eq(String(widget.get("font", "")), "Arial16b.fnt")
	assert_eq(String(widget.get("text_color", "")), "FFFFFFFF")
	# The device (the runtime's MenuFrame over Godot's decoders) placed every window where the
	# viewport's headless compile did.
	await get_tree().process_frame
	preview = _state()
	var placed := 0
	for item: Variant in preview.get("items", []):
		var compiled := item as Dictionary
		if compiled.has("rect"):
			assert_eq(str(compiled.get("device_rect", [])), str(compiled.get("rect", [])), String(compiled.get("name", "")))
			placed += 1
	assert_eq(placed, int(preview.get("count", -1)), "every window placed")
	assert_eq(placed, 3, "MAIN, TITLE and EXIT")

	# The render check compiled the same screen headless (texture sizes from their headers): every
	# window where the device's compile placed it, the same notes; the viewport's render of the row is
	# the menu_render query's answer.
	var screen_id := int(preview.get("body", {}).get("screen", {}).get("id", 0))
	var render := _viewport("render", {"row": screen_id, "limit": 200})
	assert_eq(String(render.get("status", "")), "ready", str(render))
	assert_true(bool(render.get("current", false)), str(render))
	var ours: Array = preview.get("items", [])
	var theirs: Array = render.get("widgets", [])
	assert_eq(theirs.size(), ours.size())
	for i in mini(ours.size(), theirs.size()):
		var mine := ours[i] as Dictionary
		var other := theirs[i] as Dictionary
		assert_eq(String(other.get("name", "")), String(mine.get("name", "")))
		assert_eq(str(other.get("rect", [])), str(mine.get("rect", [])), String(mine.get("name", "")))
	var our_notes: Array[String] = []
	for note: Variant in preview.get("notes", []):
		our_notes.append("%s %s" % [String((note as Dictionary).get("name", "")), String((note as Dictionary).get("code", ""))])
	var their_notes: Array[String] = []
	for note: Variant in render.get("notes", []):
		their_notes.append("%s %s" % [String((note as Dictionary).get("name", "")), String((note as Dictionary).get("code", ""))])
	assert_eq(their_notes, our_notes)
	# MAIN's CUSTOM appearance: the shell's hook, a note the preview alone shows.
	assert_true(our_notes.has("MAIN appearance_custom"), str(our_notes))
	var missing_render: Variant = JSON.parse_string(String(_seam.get_menu_render_json("nope.mnu", screen_id)))
	assert_true(String((missing_render as Dictionary).get("error", "")).contains("no menu 'nope.mnu'"), str(missing_render))

	# The game's hit test at its centre finds it; above MAIN there is nothing.
	var hit := _viewport("hit", {"x": (float(rect[0]) + float(rect[2])) / 2.0, "y": (float(rect[1]) + float(rect[3])) / 2.0})
	assert_eq(int(hit.get("id", 0)), title, str(hit))
	assert_eq(String(hit.get("name", "")), "TITLE")
	assert_eq(int(_viewport("hit", {"x": 5.0, "y": 5.0}).get("index", 0)), -1)

	# An unsaved string table edit shows: MAIN names menutxt.bin, TITLE's STRING is an id. With the
	# table active the viewport is named by the menu's path (a string table shows in no viewport).
	var created: Variant = JSON.parse_string(String(_app.request_json(JSON.stringify(
			{"kind": "create_file", "path": "menutxt.bin", "file_kind": "strings"}))))
	assert_true(created is Dictionary and bool((created as Dictionary).get("ok", false)), str(created))
	var exit_string := _string_record("menutxt.bin", "MM_Exit")
	assert_gt(exit_string, 0)
	assert_true(String(_viewport("state").get("error", "")).contains("shows in no viewport"))
	assert_true(_seam.open_document("main.mnu"))
	assert_true(_seam.write_field(main, "text_rsrc"))
	assert_true(_seam.set_field(main, "text_rsrc", "menutxt.bin"))
	assert_true(_seam.set_field(title, "string.type", "ID"))
	assert_true(_seam.set_field(title, "string.value", "MM_Exit"))
	assert_eq(String(_item(_state(), "TITLE").get("text", "")), "Exit")
	assert_true(_seam.open_document("menutxt.bin"))
	assert_true(_seam.set_field(exit_string, "text", "Leave"))
	assert_true(_seam.is_document_dirty(), "the table edit is not saved")
	preview = _state(menu)
	assert_eq(String(preview.get("body", {}).get("screen", {}).get("name", "")), "STARTUP", "still the menu's screen")
	assert_eq(String(_item(preview, "TITLE").get("text", "")), "Leave", str(preview))

	# An unsaved stylesheet colour shows.
	assert_true(_seam.open_document("menu_style.mns"))
	var fg: int = _seam.find_record("DEF_TEXT_FG")
	assert_gt(fg, 0)
	assert_true(_seam.set_field(fg, "value", "FFFF0000"))
	assert_eq(String(_item(_state(menu), "TITLE").get("text_color", "")), "FFFF0000")

	# Options: EXIT held under the mouse, every window shown; an unknown option refused.
	assert_true(_change({"options": {"force_id": exit, "force_state": "mouseover", "show_hidden": true}}, menu))
	var options: Dictionary = _state(menu).get("options", {})
	assert_eq(int(options.get("force_id", 0)), exit, str(options))
	assert_true(bool(options.get("show_hidden", false)))
	assert_true(_refusal({"options": {"force_state": "sideways"}}, menu).contains("force_state"))
	assert_true(_refusal({"options": {"bogus": 1}}, menu).contains("bogus"))
	# The device's size, an object (headless: no canvas sizes it); a flat width is no member.
	assert_true(_change({"device": {"width": 1024, "height": 768}}, menu))
	var device: Dictionary = _state(menu).get("device", {})
	assert_eq(int(device.get("width", 0)), 1024, str(device))
	assert_eq(int(device.get("height", 0)), 768)
	assert_false(bool(device.get("canvas_sized", true)))
	assert_eq(_device(_state(menu)).size, Vector2i(1024, 768), "the device draws at the viewport's size")
	assert_true(_refusal({"width": 800}, menu).contains("width"))
	assert_true(_change({"device": {"width": 800, "height": 600}}, menu))

	_seam.close_project()
	_seam.resolve_unsaved(1) # Discard
	assert_true(String(_viewport("state").get("error", "")).contains("no document is open"))


## S9k1: a drag of a window's handles through edit_in_viewport, as the canvas writes it: TITLE moved
## by (13, 5) on the grid lands on the screen's grid (MAIN's top is 75), its text still sizing its
## height; one undo puts it back; its right edge dragged without the snap; an unknown handle and a
## record that is no window of the screen are refused; the held state's checked, open list and focus.
func test_a_drag_moves_a_window_on_the_grid() -> void:
	if _app == null:
		return
	_new_project("Drag Game")
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(_seam.open_document("main.mnu"))
	var title: int = _seam.find_record("TITLE")
	assert_gt(title, 0)
	assert_true(_seam.select_record(title))
	var start: Array = _item(_state(), "TITLE").get("rect", [])
	assert_eq(start.size(), 4, str(start))
	if start.size() != 4:
		return
	assert_eq(int(start[1]), 75 + 120)

	assert_true(_drag({"id": title, "handle": "move", "by": [13, 5], "snap": 1}))
	var moved: Array = _item(_state(), "TITLE").get("rect", [])
	assert_eq(moved.size(), 4, str(moved))
	if moved.size() != 4:
		return
	assert_eq(int(moved[0]) % 8, 0, "left on the grid: %s" % str(moved))
	assert_eq(int(moved[1]) % 8, 0, "top on the grid: %s" % str(moved))
	assert_eq(int(moved[0]), 16)
	assert_eq(int(moved[1]), 200)
	assert_eq(int(_seam.get_field(title, "position.left")), 16)
	assert_eq(int(_seam.get_field(title, "position.top")), 125)
	assert_null(_seam.get_field(title, "position.bottom"), "its text still sizes its height")
	assert_true(_seam.is_document_dirty())
	_seam.undo()
	assert_eq(str(_item(_state(), "TITLE").get("rect", [])), str(start), "one undo puts it back")
	assert_false(_seam.is_document_dirty())

	assert_true(_drag({"id": title, "handle": "right", "by": [-100, 3]}))
	assert_eq(int(_seam.get_field(title, "position.right")), 700)
	assert_eq(int(_seam.get_field(title, "position.left")), 0)
	assert_false(_drag({"id": title, "handle": "middle", "by": [1, 1], "snap": 1}), "an unknown handle")
	assert_false(_drag({"id": 999999, "handle": "move", "by": [1, 1], "snap": 1}), "no window of the screen")

	assert_true(_change({"options": {"force_id": title, "force_state": "selected", "checked": true, "popup_open": true,
			"focus": true}}))
	var options: Dictionary = _state().get("options", {})
	assert_eq(String(options.get("force_state", "")), "selected", str(options))
	assert_true(bool(options.get("checked", false)))
	assert_true(bool(options.get("popup_open", false)))
	assert_true(bool(options.get("focus", false)))
	assert_false(_change({"options": {"focus": 1}}), "a flag takes true or false")

	_seam.close_project()
	_seam.resolve_unsaved(1) # Discard
	assert_true(String(_viewport("state").get("error", "")).contains("no document is open"))


## S13 V5 (the review): a TABLE through the device, after the trunk's third master sync made the
## table layer the menu frame's (MenuTableRow cells, SUBST images, a widget's clip rect): the device's
## frame draws the screen (quads, a textured one more for the SUBST image a row's "1" cell names,
## with the rows as a shell hands them and a clip rect on the table), and places the table where the
## viewport's own headless compile does.
func test_a_table_draws_through_the_device() -> void:
	if _app == null:
		return
	_new_project("Table Game")
	assert_eq(_seam.create_missing_files(), 0)
	var root: String = _seam.get_project_root()
	_write(root.path_join("table.mnu"), TABLE_MENU.to_utf8_buffer())
	_write(root.path_join("chk.tga"), _tga(16))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	assert_true(_seam.open_document("table.mnu"))
	var table: int = _seam.find_record("LIST")
	assert_gt(table, 0)
	assert_true(_seam.select_record(table))
	# S13 V6: chk.tga is not decoded yet, so the screen is configured over the frames: the texture a
	# unit, then the configure (two units).
	_app.pump()
	var preview := _state()
	assert_eq(String(preview.get("status", "")), "loading", str(preview))
	assert_eq(int(preview.get("progress", {}).get("total", 0)), 2, str(preview))
	preview = await _await_ready()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	assert_eq(int(preview.get("device", {}).get("build", {}).get("total", 0)), 2, str(preview))
	assert_eq(String(preview.get("body", {}).get("screen", {}).get("name", "")), "TBL")
	# Its texture kept, the screen configured again (an option) is configured as it is taken.
	assert_true(_change({"options": {"show_hidden": true}}))
	preview = _state()
	assert_eq(String(preview.get("status", "")), "ready", "configured whole: %s" % str(preview))
	assert_eq(int(preview.get("device", {}).get("build", {}).get("total", 0)), 1, str(preview))
	assert_true(_change({"options": {"show_hidden": false}}))
	var frame: Object = _device_node(preview, "MenuFrame")
	assert_not_null(frame, "the device's frame")
	if frame == null:
		return
	var index: int = frame.widget_index("LIST")
	assert_true(index >= 0, "the table among the frame's widgets")
	var drawn = frame.get_draw_list_stats()
	assert_gt(drawn.quads, 0, "the device draws the screen")
	assert_gt(drawn.widgets_drawn, 1, "the root and the table")
	# Its rows as a shell hands them (CTableWnd_AddRow): the "1" cell draws the SUBST image; a clip
	# rect on the table (CWnd_SetClipRect) keeps its picture.
	frame.set_widget_table_cells(index, [PackedStringArray(["1", "alpha"]), PackedStringArray(["0", "beta"])])
	frame.set_widget_clip_rect(index, true, Rect2i(100, 100, 300, 120))
	var rows = frame.get_draw_list_stats()
	assert_gt(rows.quads_textured, drawn.quads_textured, "the SUBST image drawn for the row that names it")
	assert_gt(rows.quads, drawn.quads)
	# Where the device placed the table is where the viewport's own compile did.
	var widget := _item(_state(), "LIST")
	assert_eq(widget.get("rect", []).size(), 4, str(widget))
	assert_eq(str(widget.get("device_rect", [])), str(widget.get("rect", [])), str(widget))


## S13 V5 (the review): a device given up (its menu closed) retires its SubViewport to the editor,
## which frees it at its next frame, never under the frame that may have drawn its texture; the menu
## opened again gets a device of its own.
func test_a_closed_menus_device_is_freed_at_the_next_frame() -> void:
	if _app == null:
		return
	_new_project("Retire Game")
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(_seam.open_document("main.mnu"))
	var opened := await _await_ready()
	assert_eq(String(opened.get("status", "")), "ready", str(opened))
	var viewport := _device(opened)
	assert_not_null(viewport, "the menu's device")
	if viewport == null:
		return
	assert_not_null(_device_node(opened, "MenuFrame"), "the frame lies in the device's SubViewport")
	var id := viewport.get_instance_id()
	assert_true(_seam.done({"kind": "close_document", "path": String(opened.get("path", ""))}))
	_app.pump()
	assert_null(_device(opened), "the device given up")
	assert_true(is_instance_id_valid(id), "its SubViewport retired, not freed, the frame it went")
	await get_tree().process_frame
	await get_tree().process_frame
	assert_false(is_instance_id_valid(id), "freed at the next frame")
	assert_true(_seam.open_document("main.mnu"))
	var again := await _await_ready()
	assert_eq(String(again.get("status", "")), "ready")
	var device := _device(again)
	assert_not_null(device, "the menu opened again has a device of its own")
	if device != null:
		assert_ne(device.get_instance_id(), id)


## S9k2: several windows through edit_in_viewport. TITLE and EXIT selected together: a move of EXIT
## (a selected window) takes TITLE with it, one undo step; EXIT's left edge aligned to TITLE's (the
## first window named) and EXIT sent to the back of MAIN's windows, one undo step each; the refusals.
func test_several_windows_move_and_arrange_in_one_step() -> void:
	if _app == null:
		return
	_new_project("Arrange Game")
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(_seam.open_document("main.mnu"))
	var main: int = _seam.find_record("MAIN")
	var title: int = _seam.find_record("TITLE")
	var exit: int = _seam.find_record("EXIT")
	assert_gt(exit, 0)
	assert_true(_seam.select_record(title))
	assert_true(_seam.select_record(exit, "add"))
	assert_eq(_seam.get_selected_records().size(), 2)
	var preview := _state()
	var title_start: Array = _item(preview, "TITLE").get("rect", [])
	var exit_start: Array = _item(preview, "EXIT").get("rect", [])
	assert_eq(title_start.size(), 4, str(preview))
	assert_eq(exit_start.size(), 4, str(preview))
	if title_start.size() != 4 or exit_start.size() != 4:
		return

	# A move of EXIT moves TITLE as far.
	assert_true(_drag({"id": exit, "handle": "move", "by": [8, 16]}))
	preview = _state()
	var title_moved: Array = _item(preview, "TITLE").get("rect", [])
	var exit_moved: Array = _item(preview, "EXIT").get("rect", [])
	assert_eq(title_moved.size(), 4, str(preview))
	assert_eq(exit_moved.size(), 4, str(preview))
	if title_moved.size() != 4 or exit_moved.size() != 4:
		return
	assert_eq(int(title_moved[0]), int(title_start[0]) + 8, str(title_moved))
	assert_eq(int(title_moved[1]), int(title_start[1]) + 16, str(title_moved))
	assert_eq(int(exit_moved[0]), int(exit_start[0]) + 8, str(exit_moved))
	assert_eq(int(exit_moved[1]), int(exit_start[1]) + 16, str(exit_moved))
	_seam.undo()
	preview = _state()
	assert_eq(str(_item(preview, "TITLE").get("rect", [])), str(title_start), "one undo puts both back")
	assert_eq(str(_item(preview, "EXIT").get("rect", [])), str(exit_start))
	assert_false(_seam.is_document_dirty())

	# EXIT's left edge to TITLE's, its width kept; then EXIT first among MAIN's windows.
	assert_true(_command("align_left", [title, exit]))
	assert_eq(int(_seam.get_field(exit, "position.left")), int(_seam.get_field(title, "position.left")))
	assert_eq(int(_seam.get_field(exit, "position.right")), 120)
	assert_true(_command("send_to_back", [exit]))
	var order: PackedInt64Array = _seam.get_child_records(main, "window")
	assert_eq(order.size(), 2, str(order))
	if order.size() == 2:
		assert_eq(order[0], exit, "sent to the back: drawn first")
	_seam.undo()
	_seam.undo()
	assert_false(_seam.is_document_dirty(), "one undo step each")
	assert_eq(int(_seam.get_field(exit, "position.left")), 340)

	# Refusals: two windows cannot be distributed, an unknown command, one window cannot be aligned, a
	# record the viewport does not show.
	assert_false(_command("distribute_horizontally", [title, exit]))
	assert_false(_command("align_middle", [title, exit]))
	assert_false(_command("align_left", [title]))
	assert_false(_command("align_left", [999999, exit]))
	assert_false(_seam.is_document_dirty())

	_seam.close_project()
	_seam.resolve_unsaved(1) # Discard
	assert_true(String(_viewport("state").get("error", "")).contains("no document is open"))


## S13 V8: a focused edit box's caret on the preview clock, through the device's own frame (the Shell's
## MenuViewportApplier and its tick): the clock paused in the blink's hidden half, the frame draws no
## caret; sought into the shown half, the next frame's tick sets the frame's clock and it draws the
## caret (a glyph more); sought into the next hidden half, it is gone again. The picture is never made
## again for it (the viewport's builds stand): the frame is drawn again, not configured again.
func test_the_caret_blinks_on_the_preview_clock() -> void:
	if _app == null:
		return
	_new_project("Caret Game")
	assert_eq(_seam.create_missing_files(), 0)
	var root: String = _seam.get_project_root()
	_write(root.path_join("caret.mnu"), CARET_MENU.to_utf8_buffer())
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	assert_true(_seam.open_document("caret.mnu"))
	var box: int = _seam.find_record("NAME_BOX")
	assert_gt(box, 0)
	assert_true(_change({"options": {"force_id": box, "focus": true}, "clock": {"playing": false, "time_ms": 256}}))
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var builds := int(state.get("builds", 0))
	var frame: Object = _device_node(state, "MenuFrame")
	assert_not_null(frame, "the device's frame")
	if frame == null:
		return
	await get_tree().process_frame
	var hidden: int = frame.get_draw_list_stats().glyphs
	assert_true(_change({"clock": {"time_ms": 768}}))
	await get_tree().process_frame
	var shown: int = frame.get_draw_list_stats().glyphs
	assert_gt(shown, hidden, "the caret drawn in the blink's shown half")
	assert_true(_change({"clock": {"time_ms": 1280}}))
	await get_tree().process_frame
	assert_eq(frame.get_draw_list_stats().glyphs, hidden, "and gone in the next hidden half")
	assert_eq(int(_state().get("builds", -1)), builds, "the frame drawn again, the picture never made again")


## DI-08: the game's pointer through the device's own frame (the Shell's MenuViewportApplier and its
## tick). A new project's MAIN names the blank pointer; a hit says the pointer the game draws there; held
## at a point by a client (pointer_at, the wire's hover) the frame's cursor pass draws it there, one
## textured quad more, the screen never configured again for it; Pointer off draws none, a point let go
## draws none, and a point off the picture is refused.
func test_the_pointer_draws_where_it_is_held() -> void:
	if _app == null:
		return
	_new_project("Pointer Game")
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(_seam.open_document("main.mnu"))
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var options: Dictionary = state.get("options", {})
	assert_true(bool(options.get("pointer", false)), "the pointer drawn by default: %s" % str(options))
	assert_null(options.get("pointer_at", 0), "held nowhere")
	var hit := _viewport("hit", {"x": 400, "y": 300})
	var pointer: Dictionary = hit.get("pointer", {})
	assert_true(bool(pointer.get("drawn", false)), str(hit))
	assert_eq(String(pointer.get("file", "")), "newarow1.tga", str(pointer))
	assert_eq(String(pointer.get("name", "")), "MAIN", str(pointer))
	assert_eq(int(pointer.get("width", 0)), 32)
	var frame: Object = _device_node(state, "MenuFrame")
	assert_not_null(frame, "the device's frame")
	if frame == null:
		return
	await get_tree().process_frame
	var builds := int(_state().get("builds", 0))
	var bare = frame.get_draw_list_stats()
	assert_true(_change({"options": {"pointer_at": [400, 300]}}))
	await get_tree().process_frame
	var pointed = frame.get_draw_list_stats()
	assert_eq(pointed.quads, bare.quads + 1, "the cursor pass draws the pointer")
	assert_eq(pointed.quads_textured, bare.quads_textured + 1, "with its image")
	var held_at: Array = _state().get("options", {}).get("pointer_at", [])
	assert_eq(held_at.size(), 2, str(held_at))
	if held_at.size() == 2:
		assert_eq(Vector2(float(held_at[0]), float(held_at[1])), Vector2(400, 300))
	assert_true(_change({"options": {"pointer": false}}))
	await get_tree().process_frame
	assert_eq(frame.get_draw_list_stats().quads, bare.quads, "Pointer off: none drawn")
	assert_true(_change({"options": {"pointer": true}}))
	await get_tree().process_frame
	assert_eq(frame.get_draw_list_stats().quads, bare.quads + 1, "on again where it is held")
	assert_true(_change({"options": {"pointer_at": null}}))
	await get_tree().process_frame
	assert_eq(frame.get_draw_list_stats().quads, bare.quads, "let go: none drawn")
	assert_true(_refusal({"options": {"pointer_at": [900, 10]}}).contains("pointer_at"))
	assert_eq(int(_state().get("builds", -1)), builds, "the screen never configured again for the pointer")


# --- the model -------------------------------------------------------------------------------------

func test_model_draws_through_the_device() -> void:
	if _app == null:
		return
	assert_false(_app.is_available(), "headless: no ImGui context")
	assert_true(_new_project_with(ARMORY, "armory.3di"))
	assert_true(String(_viewport("state").get("error", "")).contains("no document is open"))
	assert_true(_seam.open_document("models/armory.3di"))

	# The open model as it would save, at the level the portable half picks.
	var preview := await _await_ready()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	assert_true(bool(preview.get("current", false)))
	assert_eq(int(preview.get("builds", 0)), 1)
	var model: ObjectModel = _device_node(preview, "ObjectModel")
	var camera: Camera3D = _device_node(preview, "Camera3D")
	assert_not_null(model)
	assert_not_null(camera)
	if model == null or camera == null:
		return
	assert_not_null(model.get_object_data(), "the device holds the model")
	assert_eq(model.get_active_lod(), int(preview.get("body", {}).get("lod", {}).get("shown", -2)))
	assert_eq(String(preview.get("kind", "")), "model")
	assert_eq(String(preview.get("units", "")), "pixels")
	assert_true(bool(preview.get("device", {}).get("attached", false)))
	var size: Vector2i = (camera.get_viewport() as SubViewport).size
	assert_eq(size.x, int(preview.get("device", {}).get("width", 0)), "the device at the viewport's size")
	assert_eq(size.y, int(preview.get("device", {}).get("height", 0)))
	assert_false(bool(preview.get("device", {}).get("canvas_sized", true)), "headless: no canvas sizes it")
	# Headless, the device's size is the viewport's to set: a `device` object, never flat keys.
	assert_true(_change({"device": {"width": 320, "height": 240}}))
	assert_eq((camera.get_viewport() as SubViewport).size, Vector2i(320, 240))
	assert_eq(int(_state().get("device", {}).get("width", 0)), 320)
	assert_ne(_refusal({"width": 640}), "", "the device's size is the device's member, the refusal says why")
	assert_true(_change({"device": {"width": size.x, "height": size.y}}))
	preview = _state()

	# Every user point within half a pixel of where the device's camera projects it.
	var points: Array = _items_of(preview, "user_point")
	assert_gt(points.size(), 0)
	var projected := 0
	for point: Variant in points:
		var screen: Variant = point.get("screen")
		if screen == null:
			continue
		var at := camera.unproject_position(_vector(point.get("position")))
		assert_almost_eq(at.x, float(screen[0]), 0.5, "user point %s x" % point.get("name"))
		assert_almost_eq(at.y, float(screen[1]), 0.5, "user point %s y" % point.get("name"))
		projected += 1
	assert_gt(projected, 0, "a user point on the picture")

	# A user point's edit builds nothing; a light's builds the scene again.
	var serial: int = model.get_scene_build_serial()
	var user_point := _first_child("user_point")
	assert_gt(user_point, 0)
	assert_true(_seam.set_field(user_point, "position.x", float(_seam.get_field(user_point, "position.x")) + 1.0))
	_app.pump()
	preview = _state()
	assert_true(bool(preview.get("current", false)))
	assert_eq(int(preview.get("builds", 0)), 1, "a user point is the overlays' alone")
	assert_eq(model.get_scene_build_serial(), serial, "the scene is not built again")
	var light := _first_child("light")
	assert_gt(light, 0)
	assert_true(_seam.set_field(light, "start.r", 12))
	_state()
	_app.pump()
	preview = _state()
	assert_eq(int(preview.get("builds", 0)), 2)
	# Built over the frames that follow (S13 V6): loading first, the scene swapped in once built.
	assert_eq(String(preview.get("status", "")), "loading", str(preview))
	preview = await _await_ready()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	assert_eq(int(preview.get("device", {}).get("build", {}).get("generation", 0)), 2, str(preview))
	assert_ne(model.get_scene_build_serial(), serial, "a light's edit builds the scene again")

	# The options: a level held and one of the model's registers (armory's FLICKER) on it.
	var registers: Array = preview.get("body", {}).get("registers", [])
	assert_gt(registers.size(), 0, "the fixture declares a CTRL register")
	var register := String(registers[0].get("name", "")) if registers.size() > 0 else "FLICKER"
	assert_true(_change({"options": {"lod": 0, "ctrl": {register: 3}}}))
	preview = _state()
	assert_eq(int(preview.get("options", {}).get("lod", -1)), 0)
	assert_eq(int(preview.get("body", {}).get("registers", [{}])[0].get("value", 0)), 3)
	assert_eq(model.get_active_lod(), 0)
	assert_eq(int(model.get_ctrl_values().get(register, 0)), 3, str(model.get_ctrl_values()))
	assert_true(_change({"options": {"lod": "auto", "ctrl": {}}}))
	assert_false(model.get_ctrl_values().has(register), "a register let go reads 0 again")
	assert_ne(_refusal({"options": {"bogus": 1}}), "")
	assert_ne(_refusal({"camera": {"distance": -1.0}}), "")

	# The camera backs away: the device draws whatever Auto picks there.
	assert_true(_change({"camera": {"distance": 5000.0}}))
	preview = _state()
	var lod: Dictionary = preview.get("body", {}).get("lod", {})
	assert_eq(model.get_active_lod(), int(lod.get("shown", -2)))
	assert_eq(int(lod.get("shown", -2)), int(lod.get("auto", -3)))
	assert_true(_change({"camera": {"frame": true}}))
	assert_lt(float(_state().get("camera", {}).get("distance", 5000.0)), 5000.0, "framed again")


func test_markers_ride_the_parts_the_device_draws() -> void:
	if _app == null:
		return
	assert_true(_new_project_with(ROCKING, "house.3di"))
	assert_true(_seam.open_document("models/house.3di"))
	assert_eq(String((await _await_ready()).get("status", "")), "ready", "built over its frames (S13 V6)")
	# The ground point moved off the axis the part turns about.
	var point := _first_child("user_point")
	assert_gt(point, 0)
	assert_true(_seam.set_field(point, "position.x", 2.0))
	# The clock held at a quarter second (a seek): the device's part node and the overlay pose alike.
	assert_true(_change({"clock": {"playing": false, "time_ms": 250}}))
	await get_tree().process_frame
	await get_tree().process_frame
	var preview := await _await_ready()
	assert_eq(int(preview.get("clock", {}).get("time_ms", -1)), 250)
	# The clock's members are the clock's (its rate among them), never the options'.
	assert_ne(_refusal({"options": {"time_ms": 250}}), "")
	assert_true(_change({"clock": {"rate": 2.0}}))
	assert_eq(float(_state().get("clock", {}).get("rate", 0.0)), 2.0)
	assert_true(_change({"clock": {"rate": 1.0}}))
	var marker: Dictionary = _items_of(preview, "user_point")[0]
	var model: ObjectModel = _device_node(preview, "ObjectModel")
	assert_not_null(model)
	if model == null:
		return
	var parts: Dictionary = model.get_render_part_nodes()
	assert_true(parts.has(0), str(parts.keys()))
	var node: Node3D = parts.get(0)
	# The point as authored, in the viewport's space (the model's axes, x mirrored): x 2.0 in the
	# .o3d's axes is the model's z.
	var rest := Vector3(0.0, 0.0, 2.0)
	var carried: Vector3 = model.global_transform.affine_inverse() * (node.global_transform * rest)
	var at := _vector(marker.get("position"))
	assert_almost_eq(carried.x, at.x, 0.001, "x")
	assert_almost_eq(carried.y, at.y, 0.001, "y")
	assert_almost_eq(carried.z, at.z, 0.001, "z")
	assert_gt(absf(at.x), 0.01, "the part has turned the point off its rest")
	# A hit at the marker's pixel names its record.
	var screen: Array = marker.get("screen", [0, 0])
	var hit := _viewport("hit", {"x": float(screen[0]), "y": float(screen[1])})
	assert_eq(String(hit.get("kind", "")), "user_point", str(hit))
	assert_eq(int(hit.get("id", 0)), point)
	# Dragged to 30 pixels right: the marker is drawn there, the part still carrying it.
	var target := Vector2(float(screen[0]) + 30.0, float(screen[1]))
	assert_true(_drag({"id": point, "handle": "place", "to": [target.x, target.y]}))
	var dragged: Dictionary = _items_of(_state(), "user_point")[0]
	var now: Array = dragged.get("screen", [0, 0])
	assert_almost_eq(float(now[0]), target.x, 0.5)
	assert_almost_eq(float(now[1]), target.y, 0.5)
	assert_false(_drag({"id": point, "handle": "twist", "to": [target.x, target.y]}), "an unknown handle")
	# One undo step takes it back.
	_seam.undo()
	var undone: Array = _items_of(_state(), "user_point")[0].get("screen", [0, 0])
	assert_almost_eq(float(undone[0]), float(screen[0]), 0.5)


func test_a_table_plays_on_its_rig() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor viewport rig %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir.path_join("project"), "Rig Game"))
	var source := dir.path_join("source")
	_write(source.path_join("skinned.o3d"),
			FileAccess.get_file_as_string(ProjectSettings.globalize_path(SKINNED)).to_utf8_buffer())
	_write(source.path_join("skin.o3a"), SKIN_CLIPS.to_utf8_buffer())
	_app.request_json(JSON.stringify({"kind": "import_files", "imports": [
		{"path": source.path_join("skinned.o3d")}, {"path": source.path_join("skin.o3a")}]}))
	_write(dir.path_join("project/defs/items.def"),
			TestFs.crlf("begin \"Skinned Thing\"\nid 100200\ntype building\ngraphic skinned\nanim_def skin\nend\n").to_utf8_buffer())
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	assert_true(_seam.open_document("anims/SKIN.adm"))
	var walk: int = _seam.find_record("anim_walk_forward")
	assert_gt(walk, 0)
	assert_true(_seam.select_record(walk))
	assert_true(_change({"clock": {"playing": false, "ticks": 0}}))
	var preview := await _await_ready()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	var animation: Dictionary = preview.get("body", {}).get("animation", {})
	assert_eq(String(animation.get("model", "")).to_lower(), "skinned.3di")
	assert_true(bool(animation.get("rig", false)))
	assert_eq(String(animation.get("key", "")), "anim_walk_forward")
	assert_eq(animation.get("events", []).size(), 2)
	await get_tree().process_frame
	var model: ObjectModel = _device_node(preview, "ObjectModel")
	assert_not_null(model)
	if model == null:
		return
	assert_true(model.has_skeleton(), "the rig is bound to the skinned model")
	assert_eq(model.get_active_body_clip(), "anim_walk_forward")
	var skeleton := model.get_skeleton()
	var at_rest := skeleton.get_bone_pose_rotation(0)
	# A quarter of the way in, the root has turned.
	assert_true(_change({"clock": {"ticks": 8}}))
	await get_tree().process_frame
	var turned := skeleton.get_bone_pose_rotation(0)
	assert_gt(at_rest.angle_to(turned), 0.05, "the clip poses the skeleton at the clip clock")
	assert_eq(int(_state().get("body", {}).get("animation", {}).get("ticks", -1)), 8)
	assert_eq(int(_state().get("clock", {}).get("ticks", -1)), 8, "the preview clock's ticks")


## DI-04: a clip's footstep events heard as it runs. The session fires each on the body's ticks through
## the pairing item's profile (the envelope's sounds_fired), and the Shell starts each one's wave.
func test_a_clip_s_footsteps_reach_the_shell() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor viewport steps %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	var root := dir.path_join("project")
	assert_true(_seam.new_project(root, "Steps Game"))
	var source := dir.path_join("source")
	_write(source.path_join("skinned.o3d"),
			FileAccess.get_file_as_string(ProjectSettings.globalize_path(SKINNED)).to_utf8_buffer())
	_write(source.path_join("skin.o3a"), SKIN_CLIPS.to_utf8_buffer())
	_app.request_json(JSON.stringify({"kind": "import_files", "imports": [
		{"path": source.path_join("skinned.o3d")}, {"path": source.path_join("skin.o3a")}]}))
	_write(root.path_join("defs/items.def"), TestFs.crlf("begin \"Walker\"\nid 100200\ntype person\ngraphic skinned\n"
			+ "anim_def skin\nsound_profile walker\nend\n").to_utf8_buffer())
	_write(root.path_join("SndProf.def"), TestFs.crlf("begin \"default\"\nend\nbegin \"walker\"\n"
			+ "\tSSLFootGND STEP_L 0 0 0\n\tSSRFootGND STEP_R 0 0 0\nend\n").to_utf8_buffer())
	for name in ["step_l.wav", "step_r.wav"]:
		_write(root.path_join("sounds").path_join(name), _wave_bytes(0.2))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	# The bank made in the editor: a set a foot, each one layer playing its wave.
	assert_true(_seam.done({"kind": "create_file", "path": "game.lwf"}))
	var edits := []
	for foot: String in ["L", "R"]:
		var wave: String = "w" + foot
		var named: String = "s" + foot
		edits.append_array([
			{"op": "add", "kind": "wave", "as": wave}, {"op": "set", "id": wave, "field": "name", "value": "STEP_" + foot},
			{"op": "set", "id": wave, "field": "file", "value": "step_%s.wav" % foot.to_lower()},
			{"op": "add", "kind": "set", "as": named}, {"op": "set", "id": named, "field": "name", "value": "STEP_" + foot},
			{"op": "add", "kind": "layer", "parent": named, "as": named + "l"},
			{"op": "add", "kind": "member", "parent": named + "l", "field": "wave", "value": "STEP_" + foot},
		])
	assert_true(_seam.done({"kind": "edit_record", "path": "game.lwf", "edits": edits, "open_first": true}))
	assert_true(_seam.done({"kind": "save_all"}))
	assert_true(_seam.open_document("anims/SKIN.adm"))
	var walk: int = _seam.find_record("anim_walk_forward")
	assert_true(_seam.select_record(walk))
	assert_true(_change({"clock": {"playing": false, "ticks": 0}}))
	var preview := await _await_ready()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	var animation: Dictionary = preview.get("body", {}).get("animation", {})
	assert_eq(String(animation.get("sound_profile", {}).get("name", "")), "walker", str(animation.get("sound_profile")))
	# Run: the walk loops, its feet firing on the NPC body's odd ticks, each heard.
	assert_true(_change({"clock": {"playing": true, "ticks": 0}}))
	var started_before: int = _app.get_clip_voices_started()
	for _frame in 600:
		if _app.get_clip_voices_started() >= started_before + 2:
			break
		await get_tree().process_frame
	assert_gte(_app.get_clip_voices_started(), started_before + 2, "the Shell started a wave a footstep")
	var fired: Array = _state().get("body", {}).get("animation", {}).get("sounds_fired", [])
	assert_gte(fired.size(), 2, str(fired))
	var sets := {}
	for sound: Variant in fired:
		assert_eq(int(sound.get("tick", 0)) % 2, 1, "an NPC's body reads on odd ticks: %s" % str(sound))
		assert_eq(String(sound.get("state", "")), "played", str(sound))
		sets[String(sound.get("set", ""))] = true
	assert_true(sets.has("STEP_L") and sets.has("STEP_R"), str(sets.keys()))
	assert_true(_change({"clock": {"playing": false}}))


## DI-10: a model's damage state, played as the game destroys the item naming it. The crate is the pump's
## graphic (gnrc, its husk the armory: four parts, two of them chunks that always fly off): destroyed past
## the swap, the device draws the armory in its place (not the document's picture), the sections the pieces
## left hidden, the destroy fade's six registers on the model at the clock's values; Play destroy from the
## death fires the death sound, which the Shell plays.
func test_a_destroyed_item_swaps_in_its_husk() -> void:
	if _app == null:
		return
	assert_true(_new_project_with(CRATE, "crate.3di"))
	_add_model(ARMORY, "armory.3di")
	var root: String = _seam.get_project_root()
	_write(root.path_join("defs/items.def"), TestFs.crlf("begin \"Pump station\"\nid 100500\ntype object\n"
			+ "graphic crate\nai_function gnrc\nhusk armory\nhusk_sub_part_types 01_HULL 02_WHEEL 03_CHUNK_M 04_CHUNK_S\n"
			+ "destroy_timing 0.5 1.0 0.25\nsounddeath EXPLO_PUMP\nend\n").to_utf8_buffer())
	_write(root.path_join("sounds/explo_pump.wav"), _wave_bytes(0.2))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	# The bank made in the editor: the death sound's set, one layer playing its wave.
	assert_true(_seam.done({"kind": "create_file", "path": "game.lwf"}))
	assert_true(_seam.done({"kind": "edit_record", "path": "game.lwf", "open_first": true, "edits": [
		{"op": "add", "kind": "wave", "as": "w"}, {"op": "set", "id": "w", "field": "name", "value": "EXPLO_PUMP"},
		{"op": "set", "id": "w", "field": "file", "value": "explo_pump.wav"},
		{"op": "add", "kind": "set", "as": "s"}, {"op": "set", "id": "s", "field": "name", "value": "EXPLO_PUMP"},
		{"op": "add", "kind": "layer", "parent": "s", "as": "l"},
		{"op": "add", "kind": "member", "parent": "l", "field": "wave", "value": "EXPLO_PUMP"},
	]}))
	assert_true(_seam.done({"kind": "save_all"}))
	assert_true(_seam.open_document("models/crate.3di"))
	assert_true(_change({"clock": {"playing": false, "ticks": 0}}))
	var preview := await _await_ready()
	var model: ObjectModel = _device_node(preview, "ObjectModel")
	assert_not_null(model)
	if model == null:
		return
	assert_eq(model.get_object_data().get_light_count(), 0, "the crate, intact")
	var damage: Dictionary = preview.get("body", {}).get("damage", {})
	assert_eq(String(damage.get("uses", [{}])[0].get("item", "")), "Pump station", str(damage))
	assert_eq(String(damage.get("husk_file", "")), "armory.3di", str(damage))

	# Destroyed, the clock past the swap and into the fade: the husk drawn, its pieces gone, the fade on it.
	var builds := int(preview.get("builds", 0))
	assert_true(_change({"options": {"damage": {"state": "destroyed"}}, "clock": {"ticks": 93, "playing": false}}))
	preview = await _await_ready()
	for _frame in 600:
		if int(preview.get("builds", 0)) > builds and String(preview.get("status", "")) == "ready":
			break
		await get_tree().process_frame
		preview = _state()
	damage = preview.get("body", {}).get("damage", {})
	assert_eq(String(damage.get("drawn", "")), "armory.3di", str(damage))
	assert_false(bool(preview.get("current", true)), "the husk is not the document's picture")
	model = _device_node(preview, "ObjectModel")
	assert_eq(model.get_object_data().get_light_count(), 2, "the armory, the husk, drawn in the crate's place")
	assert_true((model.get_node("Robj_0") as Node3D).visible and (model.get_node("Robj_1") as Node3D).visible,
			"the hull and the WHEEL its chance keeps")
	assert_false((model.get_node("Robj_2") as Node3D).visible, "a CHUNK_M flown off")
	assert_false((model.get_node("Robj_3") as Node3D).visible, "a CHUNK_S flown off")
	var phases: Array = damage.get("frame", {}).get("phases", [])
	var values: Dictionary = model.get_ctrl_values()
	assert_eq(int(values.get("OBJECT_DESTROY01", 0)), 65536, str(values))
	assert_eq(int(values.get("OBJECT_DESTROY", -1)), int(phases[0]) if phases.size() == 6 else -2, str(values))

	# Play destroy: from the death, the clock run; the death sound four ticks on, heard.
	var started_before: int = _app.get_clip_voices_started()
	assert_true(_change({"clock": {"playing": true, "ticks": 0}}))
	for _frame in 600:
		if _app.get_clip_voices_started() > started_before:
			break
		await get_tree().process_frame
	assert_gt(_app.get_clip_voices_started(), started_before, "the Shell started the death sound")
	var fired: Array = _state().get("body", {}).get("damage", {}).get("sounds_fired", [])
	assert_eq(fired.size(), 1, str(fired))
	if fired.size() == 1:
		assert_eq(int(fired[0].get("tick", -1)), 4, str(fired))
		assert_eq(String(fired[0].get("set", "")), "EXPLO_PUMP")
		assert_eq(String(fired[0].get("state", "")), "played", str(fired))
	assert_true(_change({"clock": {"playing": false}, "options": {"damage": {"state": "intact"}}}))


func _wave_bytes(seconds: float) -> PackedByteArray:
	var samples := int(22050 * seconds)
	var data := PackedByteArray()
	data.resize(samples * 2)
	for i in samples:
		data.encode_s16(i * 2, int(sin(i * 0.1) * 8000.0))
	var head := PackedByteArray()
	head.resize(44)
	head.encode_u32(0, 0x46464952) # RIFF
	head.encode_u32(4, 36 + data.size())
	head.encode_u32(8, 0x45564157) # WAVE
	head.encode_u32(12, 0x20746d66) # "fmt "
	head.encode_u32(16, 16)
	head.encode_u16(20, 1) # PCM
	head.encode_u16(22, 1) # mono
	head.encode_u32(24, 22050)
	head.encode_u32(28, 44100)
	head.encode_u16(32, 2)
	head.encode_u16(34, 16)
	head.encode_u32(36, 0x61746164) # data
	head.encode_u32(40, data.size())
	head.append_array(data)
	return head


## S17: the bones the wire reports (and the canvas draws) stand where the device's skeleton puts its
## joints, the clip turning the spine a quarter off the axis.
func test_a_clip_poses_the_bones_it_reports() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor viewport bones %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir.path_join("project"), "Bones Game"))
	var source := dir.path_join("source")
	_write(source.path_join("skinned.o3d"),
			FileAccess.get_file_as_string(ProjectSettings.globalize_path(SKINNED)).to_utf8_buffer())
	_write(source.path_join("bend.o3a"), BEND_CLIPS.to_utf8_buffer())
	_app.request_json(JSON.stringify({"kind": "import_files", "imports": [
		{"path": source.path_join("skinned.o3d")}, {"path": source.path_join("bend.o3a")}]}))
	_write(dir.path_join("project/defs/items.def"),
			TestFs.crlf("begin \"Bent Thing\"\nid 100201\ntype building\ngraphic skinned\nanim_def bend\nend\n").to_utf8_buffer())
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	assert_true(_seam.open_document("anims/BEND.adm"))
	var idle: int = _seam.find_record("anim_idle")
	assert_gt(idle, 0)
	assert_true(_seam.select_record(idle))
	var preview := await _await_ready()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	# The clock held at a tick past the clip's frame 1 (the clip newly chosen sought it to 0).
	assert_true(_change({"clock": {"playing": false, "ticks": 4}}))
	await get_tree().process_frame
	await get_tree().process_frame
	var model: ObjectModel = _device_node(preview, "ObjectModel")
	assert_not_null(model)
	if model == null:
		return
	var skeleton := model.get_skeleton()
	var bones: Array = _state().get("body", {}).get("animation", {}).get("bones", [])
	assert_eq(bones.size(), skeleton.get_bone_count())
	var spine: Array = bones[1].get("position", [0, 0, 0]) if bones.size() > 1 else [0, 0, 0]
	# The sign pinned (S17 review): the pelvis's quarter turn swings the spine to -x in the preview's frame.
	assert_almost_eq(Vector3(float(spine[0]), float(spine[1]), float(spine[2])), Vector3(-1, 0, 0),
			Vector3(0.01, 0.01, 0.01), "the spine turned a quarter off the axis")
	for i in bones.size():
		var at: Array = bones[i].get("position", [0, 0, 0])
		var posed: Vector3 = model.global_transform.affine_inverse() * (skeleton.global_transform * skeleton.get_bone_global_pose(i).origin)
		assert_almost_eq(Vector3(float(at[0]), float(at[1]), float(at[2])), posed, Vector3(0.002, 0.002, 0.002),
				"bone %d where the device's skeleton puts it" % i)
	# Where the canvas marks each bone is where the picture draws it: the wire's screen point against
	# the device camera's projection of the skeleton's bone, in the viewport's own pixels.
	var camera: Camera3D = _device_node(preview, "Camera3D")
	assert_not_null(camera)
	var device: Dictionary = _state().get("device", {})
	var size := Vector2(float(device.get("width", 0)), float(device.get("height", 0)))
	assert_true(size.x > 0.0 and size.y > 0.0, "the viewport's size on the wire: %s" % str(device))
	if camera == null or size.x <= 0.0 or size.y <= 0.0:
		return
	var marked := 0
	var drawn_size := Vector2(camera.get_viewport().size)
	for i in bones.size():
		var screen: Variant = bones[i].get("screen")
		assert_true(screen is Array, "bone %d on the screen" % i)
		if not (screen is Array):
			continue
		var world := skeleton.global_transform * skeleton.get_bone_global_pose(i).origin
		var drawn := camera.unproject_position(world) / drawn_size * size
		assert_almost_eq(Vector2(float(screen[0]), float(screen[1])), drawn, Vector2(1.5, 1.5),
				"bone %d marked where the picture draws it" % i)
		marked += 1
	assert_eq(marked, bones.size(), "every bone compared on the screen")


## S13 V5: each open model its own viewport and its own device: the first again keeps its device and
## its picture (no build), its camera its own.
func test_two_models_get_their_own_devices() -> void:
	if _app == null:
		return
	assert_true(_new_project_with(ARMORY, "armory.3di"))
	_add_model(ARMORY, "second.3di")
	assert_true(_seam.open_document("models/armory.3di"))
	var first := await _await_ready()
	assert_eq(String(first.get("status", "")), "ready", str(first))
	var first_device := _device(first)
	assert_not_null(first_device)
	assert_true(_change({"camera": {"distance": 30.0}}))
	assert_true(_seam.open_document("models/second.3di"))
	var second := await _await_ready()
	assert_eq(String(second.get("path", "")), "models/second.3di", str(second))
	var second_device := _device(second)
	assert_not_null(second_device)
	assert_ne(second_device, first_device, "the second model on a device of its own")
	assert_ne(float(second.get("camera", {}).get("distance", 30.0)), 30.0, "its own camera, framed on it")
	assert_eq(_device(first), first_device, "the first keeps its device beside the second's")
	assert_true(_seam.open_document("models/armory.3di"))
	var again := await _await_ready()
	assert_eq(_device(again), first_device, "the first's device kept")
	assert_eq(int(again.get("builds", 0)), int(first.get("builds", -1)), "its picture kept: no build")
	assert_almost_eq(float(again.get("camera", {}).get("distance", 0.0)), 30.0, 0.001, "its camera kept")


## S13 V5: the Shell's devices hold four: a fifth model previewed gives up the least recently used
## (path, kind)'s device, its viewport detached with its state kept; previewed again, it is given a
## device that makes its picture again (a build), at the camera it kept.
func test_the_device_cache_holds_four() -> void:
	if _app == null:
		return
	assert_true(_new_project_with(ARMORY, "m0.3di"))
	for i in range(1, 5):
		_add_model(ARMORY, "m%d.3di" % i)
	assert_true(_seam.open_document("models/m0.3di"))
	var first := await _await_ready()
	assert_eq(int(first.get("builds", 0)), 1, str(first))
	assert_true(_change({"camera": {"yaw": 1.25, "distance": 12.0}}))
	var first_device := _device(first)
	assert_not_null(first_device)
	for i in range(1, 5):
		assert_true(_seam.open_document("models/m%d.3di" % i))
		assert_eq(String((await _await_ready()).get("status", "")), "ready")
	await get_tree().process_frame
	assert_false(is_instance_valid(first_device), "the least recently used device given up, its nodes freed")
	assert_false(bool(_state("models/m0.3di").get("device", {}).get("attached", true)), "its viewport detached")
	assert_true(_seam.open_document("models/m0.3di"))
	var again := await _await_ready()
	assert_true(bool(again.get("device", {}).get("attached", false)), str(again))
	assert_eq(int(again.get("builds", 0)), 2, "a device attached again makes the picture again")
	assert_almost_eq(float(again.get("camera", {}).get("yaw", 0.0)), 1.25, 0.001, "the camera it kept")
	assert_almost_eq(float(again.get("camera", {}).get("distance", 0.0)), 12.0, 0.001)
	assert_not_null(_device(again), "a device of its own again")


# --- builds over frames (S13 V6) ------------------------------------------------------------------

## A 32-bit TGA `side` pixels square (its pixels one colour): a texture the game decodes itself.
func _write_tga(path: String, side: int) -> void:
	var image := Image.create(side, side, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.45, 0.55, 0.35, 1.0))
	var header := PackedByteArray([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		side & 0xFF, side >> 8, side & 0xFF, side >> 8, 32, 8])
	var out := FileAccess.open(path, FileAccess.WRITE)
	out.store_buffer(header)
	out.store_buffer(image.get_data())
	out.close()


## A sheet of `cells` x `cells` quads at x = `x` facing +x (mission axes: y left, z up), two metres
## square, as a strip's vertices and triangles (counter-clockwise about its normal).
func _sheet(lines: PackedStringArray, x: float, cells: int) -> void:
	for row in cells + 1:
		for column in cells + 1:
			var u := float(column) / cells
			var v := float(row) / cells
			lines.append("v %.4f %.4f %.4f 1 0 0 %.4f %.4f" % [x, -1.0 + 2.0 * u, 2.0 * v, u, 1.0 - v])
	for row in cells:
		for column in cells:
			var a := row * (cells + 1) + column
			var b := a + 1
			var c := a + cells + 2
			var d := a + cells + 1
			lines.append("t %d %d %d" % [a, b, c])
			lines.append("t %d %d %d" % [a, c, d])


## A JO-sized model's source written into `dir` as the Blender add-on writes a scene (an .o3d), its
## textures beside it: `materials` materials, each with a diffuse texture and every third a detail
## texture too (32-bit TGAs `side` pixels square); `levels` levels of `parts` parts, a part a sheet
## of `cells` x `cells` quads at the first level, half as many across at each level after; a light
## and a user point. Answers the textures' file names.
func _write_large_model(dir: String, stem: String, materials: int, parts: int, cells: int, levels: int,
		side: int) -> PackedStringArray:
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	var textures := PackedStringArray()
	var lines := PackedStringArray(["o3d 2", "model %s" % stem.to_upper()])
	for index in materials:
		var detail := index % 3 == 2
		lines.append("material %s" % ("FF_MT_OP" if detail else "FF_ST_OP"))
		var diffuse := "%s%02d.tga" % [stem, index]
		lines.append("texture %s 1 0 0 0" % diffuse)
		textures.append(diffuse)
		if detail:
			var second := "%s%02dd.tga" % [stem, index]
			lines.append("texture %s 2 0 0 0" % second)
			textures.append(second)
	for level in levels:
		# The level drawn past a threshold that halves at each level, the last at any distance.
		lines.append("lod %d bldg" % (0 if level == levels - 1 else 400 >> level))
		for part in parts:
			lines.append("part 0 %d 0 0" % (part * 3))
			lines.append("mesh %d 0" % (part % materials))
			_sheet(lines, float(part * 3), maxi(cells >> level, 1))
	lines.append("light 0 1 0 1.5 0 4 0 0 0 255 200 150 255 200 150 0")
	lines.append("userpoint top 0 0 3 0 0 1 0 71")
	var out := FileAccess.open(dir.path_join(stem + ".o3d"), FileAccess.WRITE)
	out.store_string("\n".join(lines) + "\n")
	out.close()
	for texture in textures:
		_write_tga(dir.path_join(texture), side)
	return textures


## The JO-sized model written beside the open project (`dir`/source) and imported with its textures,
## the import settled: how many textures it binds.
func _import_large_model(dir: String, stem: String) -> int:
	var source := dir.path_join("source")
	var textures := _write_large_model(source, stem, LARGE_MATERIALS, LARGE_PARTS, LARGE_CELLS, LARGE_LEVELS,
			LARGE_SIDE)
	var imports: Array = [{"path": source.path_join(stem + ".o3d")}]
	for texture in textures:
		imports.append({"path": source.path_join(texture)})
	var imported: Dictionary = _seam.request({"kind": "import_files", "imports": imports})
	assert_true(bool(imported.get("ok", false)), str(imported))
	assert_true(_seam.settle(), "the import steps across pumps (S13 A3)")
	return textures.size()


## S13 V6: a JO-sized model builds over frames. Opened, its viewport is `loading` at the pump that
## takes it (the build begun, its units planned: textures, meshes, the scene, the pose), its progress
## never going back, then `ready`; at the editor's build budget each frame's units take no more than
## the budget and one unit's cost (measured, asserted loosely: the longest frame within the budget
## plus the longest unit and a millisecond). A level held once it is built swaps the kept level's rows
## in place, the scene not built again (the review). At a budget of 0 each frame runs one unit, so the
## build takes as many frames as it has units; until its scene unit the device's ObjectModel holds
## the last scene (the last picture kept, nothing half built in it); a light's edit at a unit begins
## the next generation anew, its progress at 0.
func test_a_large_model_builds_over_frames() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor viewport large %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir.path_join("project"), "Large Game"))
	var textures := _import_large_model(dir, "jolarge")
	assert_true(_seam.open_document("models/jolarge.3di"))
	_app.pump()

	# The editor's budget: loading at once, its progress rising, then ready. A first picture builds
	# at the first-picture budget, the wider one (S14: the device holds no picture yet).
	var budget_ms: int = max(_app.build_budget_ms, _app.first_picture_budget_ms)
	assert_gt(_app.build_budget_ms, 0)
	assert_gte(_app.first_picture_budget_ms, _app.build_budget_ms)
	var preview := _state()
	assert_eq(String(preview.get("status", "")), "loading", str(preview))
	var total := int(preview.get("progress", {}).get("total", 0))
	assert_gt(total, textures, "a unit a texture, then the meshes, the scene and the pose")
	var done := 0
	var frames := 0
	while String(preview.get("status", "")) == "loading" and frames < 600:
		var now := int(preview.get("progress", {}).get("done", 0))
		assert_true(now >= done, "the progress never goes back")
		done = now
		await get_tree().process_frame
		frames += 1
		preview = _state()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	var build: Dictionary = preview.get("device", {}).get("build", {})
	gut.p("the JO-sized model at %d ms a frame: %d frames awaited, its build %s" % [budget_ms, frames, str(build)])
	assert_eq(int(build.get("done", 0)), total, str(build))
	assert_gt(int(build.get("frames", 0)), 0, str(build))
	var frame_us := int(build.get("frame_us", 0))
	var unit_us := int(build.get("unit_us", 0))
	assert_lte(frame_us, budget_ms * 1000 + unit_us + 1000,
			"no frame's units past the budget by more than one unit: %s" % str(build))
	var model: ObjectModel = _device_node(preview, "ObjectModel")
	assert_not_null(model)
	if model == null:
		return
	assert_not_null(model.get_object_data(), "the device holds the model")

	# A level held: its rows swapped in, every level kept (the build made them all), the scene not
	# built again and nothing asked of the device but an Update.
	var serial: int = model.get_scene_build_serial()
	assert_true(_change({"options": {"lod": 3}}))
	assert_eq(model.get_active_lod(), 3)
	assert_eq(model.get_scene_build_serial(), serial, "a level swapped in place, the scene not built again")
	preview = _state()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	assert_eq(int(preview.get("builds", 0)), 1, "a level held builds nothing")
	assert_true(_change({"options": {"lod": "auto"}}))

	# A budget of 0: one unit a frame. Until the scene unit the ObjectModel holds the last scene.
	_app.build_budget_ms = 0
	serial = model.get_scene_build_serial()
	var light := _first_child("light")
	assert_gt(light, 0)
	assert_true(_seam.set_field(light, "start.r", 12))
	_app.pump()
	preview = _state()
	assert_eq(String(preview.get("status", "")), "loading", str(preview))
	assert_eq(int(preview.get("builds", 0)), 2)
	done = 0
	for _frame in 3:
		await get_tree().process_frame
		preview = _state()
		var now := int(preview.get("progress", {}).get("done", 0))
		assert_true(now - done <= 1, "one unit a frame: %d after %d" % [now, done])
		done = now
		assert_eq(String(preview.get("progress", {}).get("label", "")), "textures", str(preview))
		assert_eq(model.get_scene_build_serial(), serial, "the last scene kept while the textures decode")
	assert_gt(done, 1, "a unit each frame")
	# Another edit while it builds: the next generation begun anew from its first unit, its progress
	# the newer generation's (A1's never goes back within one).
	assert_true(_seam.set_field(light, "start.r", 13))
	_app.pump()
	preview = _state()
	assert_eq(int(preview.get("builds", 0)), 3)
	assert_eq(int(preview.get("device", {}).get("build", {}).get("generation", 0)), 3)
	assert_eq(int(preview.get("progress", {}).get("generation", 0)), 3, str(preview))
	assert_eq(int(preview.get("progress", {}).get("done", -1)), 0, "begun anew")
	frames = 0
	while String(preview.get("status", "")) == "loading" and frames < total + 10:
		await get_tree().process_frame
		frames += 1
		preview = _state()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	build = preview.get("device", {}).get("build", {})
	gut.p("the JO-sized model at 0 ms a frame: %d frames awaited, its build %s" % [frames, str(build)])
	assert_eq(int(build.get("frames", 0)), total, "one unit a frame, as many frames as units: %s" % str(build))
	assert_eq(int(build.get("generation", 0)), 3, str(build))
	assert_ne(model.get_scene_build_serial(), serial, "the scene built again once its unit ran")


## A screen of `count` windows, each drawing its own texture (tex<n>.tga): a screen whose first
## show decodes them, a unit each (S13 V6).
func _textured_menu(count: int) -> String:
	var windows := ""
	for index in count:
		windows += ("\t\t<WINDOW type=\"static\" name=\"IMG%d\">\n" % index
				+ "\t\t\t<APPEARANCE type=\"image\" state=\"default\">tex%d.tga</APPEARANCE>\n" % index
				+ "\t\t\t<POSITION><LEFT>%d</LEFT><TOP>0</TOP></POSITION>\n" % (index * 40)
				+ "\t\t</WINDOW>\n")
	return ("<SCREEN>\n\t<NAME>TEX</NAME>\n\t<WINDOW type=\"window\" name=\"ROOT\">\n"
			+ "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\n"
			+ "\t\t<APPEARANCE type=\"color\" state=\"default\">FF203040</APPEARANCE>\n"
			+ windows + "\t</WINDOW>\n</SCREEN>\n")


## The units the builds of the viewports over `paths` ran so far (each one's done), and whether one
## of them still loads.
func _units(paths: Array) -> Dictionary:
	var done := 0
	var loading := false
	var spent := 0
	var longest := 0
	for path: String in paths:
		var state := _state(path)
		var build: Dictionary = state.get("device", {}).get("build", {})
		done += int(build.get("done", 0))
		spent += int(build.get("total_us", 0))
		longest = maxi(longest, int(build.get("unit_us", 0)))
		loading = loading or String(state.get("status", "")) == "loading"
	return {"done": done, "loading": loading, "spent": spent, "longest": longest}


## S13 V6 (the review): two devices building at once share the frame's budget, the most recently
## used one's first. At a budget of 0 the frame runs one unit in all (the menu's and the model's
## builds, summed, a unit further each frame, never two); at the editor's budget no frame's units,
## both builds' together, run past the budget by more than the longest unit and a millisecond
## (measured, asserted loosely; S14: the first-picture budget, the wider, while the most recently
## used device holds no picture yet).
func test_two_builds_share_the_frame() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor viewport two builds %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir.path_join("project"), "Two Builds Game"))
	assert_eq(_seam.create_missing_files(), 0)
	_import_large_model(dir, "jotwo")
	var root: String = _seam.get_project_root()
	_write(root.path_join("tex.mnu"), _textured_menu(6).to_utf8_buffer())
	for index in 6:
		_write(root.path_join("tex%d.tga" % index), _tga(32))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	var paths := ["tex.mnu", "models/jotwo.3di"]

	# A budget of 0: both opened, both building, one unit a frame between them.
	var budget_ms: int = _app.build_budget_ms
	_app.build_budget_ms = 0
	for path: String in paths:
		assert_true(_seam.open_document(path))
		_app.pump()
	for path: String in paths:
		var state := _state(path)
		assert_eq(String(state.get("status", "")), "loading", str(state))
	var units := _units(paths)
	var last := int(units["done"])
	var frames := 0
	while bool(units["loading"]) and frames < 600:
		await get_tree().process_frame
		frames += 1
		units = _units(paths)
		assert_eq(int(units["done"]) - last, 1, "one unit a frame in all (frame %d)" % frames)
		last = int(units["done"])
	assert_false(bool(units["loading"]), "both built")
	for path: String in paths:
		assert_eq(String(_state(path).get("status", "")), "ready", path)

	# The editor's budget: both closed and opened again (devices of their own, the menu's textures not
	# decoded on its new frame), both building; each frame's units, both builds' together, within the
	# budget and the longest unit.
	_app.build_budget_ms = budget_ms
	for path: String in paths:
		assert_true(_seam.done({"kind": "close_document", "path": path}))
	_app.pump()
	for path: String in paths:
		assert_true(_seam.open_document(path))
		_app.pump()
	units = _units(paths)
	assert_true(bool(units["loading"]), "built again over the frames")
	var spent := int(units["spent"])
	var longest_frame := 0
	frames = 0
	while bool(units["loading"]) and frames < 600:
		await get_tree().process_frame
		frames += 1
		units = _units(paths)
		longest_frame = maxi(longest_frame, int(units["spent"]) - spent)
		spent = int(units["spent"])
	assert_false(bool(units["loading"]), "both built")
	var widest: int = maxi(budget_ms, _app.first_picture_budget_ms)
	gut.p("two builds at %d ms a frame (%d ms before a first picture): %d frames, the longest frame's units %d us, the longest unit %d us"
			% [budget_ms, widest, frames, longest_frame, int(units["longest"])])
	assert_lte(longest_frame, widest * 1000 + int(units["longest"]) + 1000,
			"the frame's units, both builds', past the budget by no more than one unit")
