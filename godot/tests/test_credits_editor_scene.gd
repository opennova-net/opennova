extends GutTest

const CreditsEditorScene = preload("res://modtools/credits/credits_editor.tscn")
const CreditsEditorBlockCardScene = preload("res://modtools/credits/credits_editor_block_card.tscn")
const CreditsEditorDocument = preload("res://modtools/credits/credits_editor_document.gd")

func test_block_card_scene_instantiates() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	assert_not_null(card, "block card scene must instantiate (parse + load)")
	add_child_autofree(card)

func test_editor_scene_instantiates() -> void:
	var editor = CreditsEditorScene.instantiate()
	assert_not_null(editor, "editor scene must instantiate")
	add_child_autofree(editor)
	await get_tree().process_frame
	# Process one frame so @onready bindings fire and any %-marker resolution surfaces.

func test_editor_panes_clip_preview_and_list_overflow() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var left_pane: Control = editor.get_node("HSplit/LeftPane")
	var right_pane: Control = editor.get_node("HSplit/RightPane")
	var preview_host: Control = editor.get_node("%PreviewHost")
	var player: NovaCreditsPlayer = editor.get_node("%Player")

	assert_true(left_pane.clip_contents, "left editor pane clips card overflow at the split")
	assert_true(right_pane.clip_contents, "right preview pane clips preview overflow at the split")
	assert_true(preview_host.clip_contents, "preview host clips its contents")
	assert_true(player.clip_contents, "player clips generated fixed-image overlays")

func test_editor_binds_document_and_loads_kda() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	editor.set_document(doc)
	var err: int = doc.open_kda("res://assets/credits/nlist.kda")
	assert_eq(err, OK, "open_kda returns OK for the bundled fixture")
	await get_tree().process_frame
	assert_gt(doc.resource.get_entry_count(), 0, "fixture has entries")

func test_block_card_binds_text_entry() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	await get_tree().process_frame

	var entry := CbinTextEntry.new()
	entry.set_text("Hello")
	# Pass the justify value through an int variable — the GDScript parser
	# rejects CbinEntry.CBIN_JUSTIFY_CENTER as a direct argument to set_justify()
	# because the scoped enum type (CbinEntry.CbinJustify) doesn't match the
	# globally-cast type (godot.CbinJustify). Assigning to int first is the
	# workaround.
	var center: int = CbinEntry.CBIN_JUSTIFY_CENTER
	entry.set_justify(center)
	card.bind(entry, PackedStringArray())
	assert_eq(card.get_entry(), entry)

func test_block_card_missing_image_name_commits_on_focus_loss() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	await get_tree().process_frame

	var entry := CbinImageEntry.new()
	card.bind(entry, PackedStringArray())
	await get_tree().process_frame

	var path_edit: LineEdit = card.get_node("%ImagePathEdit")
	path_edit.text = "missing_credits_image.png"
	path_edit.focus_exited.emit()
	await get_tree().process_frame

	assert_eq(entry.get_texture_name(), "missing_credits_image.png", "missing filename is preserved for save/warnings")
	assert_null(entry.get_texture(), "missing image clears any stale texture")

func test_block_card_image_path_uses_case_and_extension_fallback() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	await get_tree().process_frame

	var entry := CbinImageEntry.new()
	card.bind(entry, PackedStringArray())
	await get_tree().process_frame

	var path_edit: LineEdit = card.get_node("%ImagePathEdit")
	path_edit.text = "CR1.PCX"
	path_edit.text_submitted.emit("CR1.PCX")
	await get_tree().process_frame

	assert_eq(entry.get_texture_name(), "CR1.PCX", "typed filename is preserved")
	assert_not_null(entry.get_texture(), "image editor resolves case/extension variants like the KDA loader")

func test_visual_source_buttons_are_exclusive() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var visual: Button = editor.get_node("%VisualButton")
	var source: Button = editor.get_node("%SourceButton")
	source.button_pressed = true
	await get_tree().process_frame

	assert_false(visual.button_pressed, "visual is unpressed when source is active")
	assert_true(source.button_pressed, "source is active")

	visual.button_pressed = true
	await get_tree().process_frame
	assert_true(visual.button_pressed, "visual is active")
	assert_false(source.button_pressed, "source is unpressed when visual is active")

func test_preview_pause_resume_and_stop_show_first_entries() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	editor.set_size(Vector2(1280, 720))
	await get_tree().process_frame

	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	var entry := CbinTextEntry.new()
	entry.set_text("Preview")
	doc.resource.add_entry(entry)
	editor.set_document(doc)
	await get_tree().process_frame
	await get_tree().process_frame

	var player: NovaCreditsPlayer = editor.get_node("%Player")
	var play: Button = editor.get_node("HSplit/RightPane/PreviewHost/Toolbar/Play")
	var pause: Button = editor.get_node("HSplit/RightPane/PreviewHost/Toolbar/Pause")
	var stop: Button = editor.get_node("HSplit/RightPane/PreviewHost/Toolbar/Stop")

	play.pressed.emit()
	await get_tree().process_frame
	assert_true(player.is_playing(), "play starts preview")

	pause.pressed.emit()
	await get_tree().process_frame
	assert_false(player.is_playing(), "pause stops active playback without resetting")
	assert_eq(play.text, "Resume", "play button becomes resume while paused")

	play.pressed.emit()
	await get_tree().process_frame
	assert_true(player.is_playing(), "resume continues playback")

	stop.pressed.emit()
	await get_tree().process_frame
	assert_false(player.is_playing(), "stop ends playback")
	assert_almost_eq(player.get_scroll_offset(), player.get_size().y, 1.0, "stop returns preview to the first entries")
