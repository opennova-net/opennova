extends GutTest

# The inspector's drag-to-add-play drop sink: gated by the editable flag (the
# can_edit_plays parity gate), targets the shown state, and end-to-end through
# live_mode it adds a play to the model.

const MusicInspectorPanel = preload("res://modtools/music/ui/inspector_panel.gd")
const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const LiveModeScene = preload("res://modtools/music/ui/live_mode.tscn")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_drag_pair.sbf"
const PAIR_SCRIPT := "user://music_drag_pair.bin"


func after_each() -> void:
	_rm(PAIR_BANK)
	_rm(PAIR_SCRIPT)


func _section() -> Dictionary:
	return {"name": "Win000", "index": 3, "is_entry": false, "is_idle_loop": false,
		"edges": [], "plays": []}


func test_drop_sink_gated_by_editable():
	var panel = MusicInspectorPanel.new()
	add_child_autofree(panel)
	await get_tree().process_frame
	panel.show_section(_section(), [], "", false)
	assert_false(panel._can_drop_data(Vector2.ZERO, {"kind": "mus_track", "index": 0}),
		"read-only inspector rejects drops")
	panel.show_section(_section(), [], "", true)
	assert_true(panel._can_drop_data(Vector2.ZERO, {"kind": "mus_track", "index": 0}),
		"editable inspector accepts a track drop")
	assert_false(panel._can_drop_data(Vector2.ZERO, {"kind": "something_else"}),
		"rejects non-track payloads")


func test_drop_emits_add_play_for_shown_section():
	var panel = MusicInspectorPanel.new()
	add_child_autofree(panel)
	await get_tree().process_frame
	panel.show_section(_section(), [], "", true)
	var got := {"section": "", "track": -1}
	panel.add_play_requested.connect(func(s, t):
		got["section"] = String(s)
		got["track"] = t)
	panel._drop_data(Vector2.ZERO, {"kind": "mus_track", "index": 5})
	assert_eq(String(got["section"]), "Win000", "drop targets the shown state")
	assert_eq(int(got["track"]), 5, "drop carries the dropped track index")


func test_live_drop_adds_play_to_model():
	var doc = MusicEditorDocument.new()
	_copy(BANK_FIXTURE, PAIR_BANK)
	_copy(SCRIPT_FIXTURE, PAIR_SCRIPT)
	doc.open_pair(PAIR_BANK)
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	lm._on_inspector_add_play(&"Win000", 0)
	var nm := StringName(doc.mus_script.get_default_script_name())
	var count := -1
	for s in doc.mus_script.get_section_model(nm):
		if String(s.get("name", "")) == "Win000":
			count = (s.get("plays", []) as Array).size()
	assert_eq(count, 7, "dropping a track on a state adds a play to its model")


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
