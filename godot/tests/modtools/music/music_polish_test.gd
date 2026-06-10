extends GutTest

# Step-5 polish: the Tracks drag-source payload and the double-click-to-open
# Advanced drawer affordance.

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const BankModeScene = preload("res://modtools/music/ui/bank_mode.tscn")
const LiveModeScene = preload("res://modtools/music/ui/live_mode.tscn")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_polish_pair.sbf"
const PAIR_SCRIPT := "user://music_polish_pair.bin"


func after_each() -> void:
	_rm(PAIR_BANK)
	_rm(PAIR_SCRIPT)


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


func test_double_click_node_drills_into_program_view():
	# Double-click drills into the state's logic-graph blueprint (the sole authoring
	# surface; there is no raw-script drawer anymore).
	var doc = MusicEditorDocument.new()
	_copy(BANK_FIXTURE, PAIR_BANK)
	_copy(SCRIPT_FIXTURE, PAIR_SCRIPT)
	doc.open_pair(PAIR_BANK)
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var ev := InputEventMouseButton.new()
	ev.button_index = MOUSE_BUTTON_LEFT
	ev.double_click = true
	lm._on_node_gui_input(ev, "Begin")
	await get_tree().process_frame
	assert_true(lm._program_view.visible, "double-clicking a state drills into its logic graph")
	assert_false(lm.get_node("%SectionMap").visible, "the map hides while drilled in")


func _copy(src_path: String, dst_path: String) -> void:
	var src := FileAccess.open(src_path, FileAccess.READ)
	assert_not_null(src, "fixture readable: %s" % src_path)
	if src == null:
		return
	var dst := FileAccess.open(dst_path, FileAccess.WRITE)
	if dst != null:
		dst.store_buffer(src.get_buffer(src.get_length()))
		dst.close()
	src.close()


func _rm(path: String) -> void:
	var abs := ProjectSettings.globalize_path(path)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)
