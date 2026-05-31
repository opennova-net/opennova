extends GutTest

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const ScriptModeScene = preload("res://modtools/music/ui/script_mode.tscn")
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"


func test_script_mode_loads_decompiled_text():
	var doc = MusicEditorDocument.new()
	var err := doc.open_script(SCRIPT_FIXTURE)
	assert_eq(err, OK, "open_script succeeded")
	var sm: Control = ScriptModeScene.instantiate()
	add_child_autofree(sm)
	sm.bind_document(doc)
	# bind_document defers the first refresh until _ready fires; flush a frame
	# so the CodeEdit picks up the decompiled body before we inspect it.
	await get_tree().process_frame
	var ce: CodeEdit = sm.get_node("VBox/CodeEdit")
	assert_gt(ce.text.length(), 100, "decompiled text non-trivial")


func test_text_edit_marks_dirty():
	var doc = MusicEditorDocument.new()
	var err := doc.open_script(SCRIPT_FIXTURE)
	assert_eq(err, OK, "open_script succeeded")
	var sm: Control = ScriptModeScene.instantiate()
	add_child_autofree(sm)
	sm.bind_document(doc)
	await get_tree().process_frame
	var ce: CodeEdit = sm.get_node("VBox/CodeEdit")
	ce.text = ce.text + "\n// added"
	ce.text_changed.emit()
	assert_true(doc._script_dirty, "edit marks document dirty")
