extends GutTest

# Step-5 polish: the Tracks drag-source payload and the double-click-to-open
# Advanced drawer affordance.

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const BankModeScene = preload("res://modtools/music/ui/bank_mode.tscn")
const LiveModeScene = preload("res://modtools/music/ui/live_mode.tscn")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"


func test_track_drag_payload_shape():
	var doc = MusicEditorDocument.new()
	assert_eq(doc.open_bank(BANK_FIXTURE), OK, "bank opens")
	var bm: Control = BankModeScene.instantiate()
	add_child_autofree(bm)
	bm.bind_document(doc)
	await get_tree().process_frame
	var payload: Dictionary = bm._track_drag_payload(0)
	assert_eq(String(payload.get("kind", "")), "mus_track", "drag payload is a track")
	assert_eq(int(payload.get("index", -1)), 0, "payload carries the row index")
	assert_ne(String(payload.get("name", "")), "", "payload carries the resolved track name")
	assert_true(bm._track_drag_payload(-1).is_empty(), "out-of-range index yields no payload")


func test_track_drag_payload_empty_without_bank():
	var doc = MusicEditorDocument.new()
	var bm: Control = BankModeScene.instantiate()
	add_child_autofree(bm)
	bm.bind_document(doc)
	await get_tree().process_frame
	assert_true(bm._track_drag_payload(0).is_empty(), "no bank loaded -> no drag payload")


func test_double_click_node_opens_advanced_drawer():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var drawer = lm.get_node("%AdvancedDrawer")
	assert_false(drawer.visible, "drawer hidden by default")
	var ev := InputEventMouseButton.new()
	ev.button_index = MOUSE_BUTTON_LEFT
	ev.double_click = true
	lm._on_node_gui_input(ev, "Begin")
	assert_true(drawer.visible, "double-clicking a state opens the raw-script drawer")
