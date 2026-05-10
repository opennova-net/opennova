extends GutTest

const CreditsEditorScene = preload("res://modtools/credits/credits_editor.tscn")
const CreditsEditorDocument = preload("res://modtools/credits/credits_editor_document.gd")
const KDA_SOURCE_PATH := "res://assets/credits/nlist.kda"
const TEMP_DIR := "user://test_credits_scroll_sync"

var _kda_path: String = ""


func before_each() -> void:
	var abs := ProjectSettings.globalize_path(TEMP_DIR)
	DirAccess.make_dir_recursive_absolute(abs)
	_kda_path = TEMP_DIR.path_join("nlist_%d.kda" % Time.get_ticks_usec())
	var src := FileAccess.open(ProjectSettings.globalize_path(KDA_SOURCE_PATH), FileAccess.READ)
	if src != null:
		var dst := FileAccess.open(ProjectSettings.globalize_path(_kda_path), FileAccess.WRITE)
		if dst != null:
			dst.store_buffer(src.get_buffer(src.get_length()))
			dst.close()
		src.close()


func after_each() -> void:
	_cleanup_dir(TEMP_DIR)
	_kda_path = ""


func _cleanup_dir(dir_path: String) -> void:
	var abs := ProjectSettings.globalize_path(dir_path)
	var da := DirAccess.open(abs)
	if da == null:
		return
	da.list_dir_begin()
	var fname := da.get_next()
	while fname != "":
		if not da.current_is_dir():
			da.remove(fname)
		fname = da.get_next()
	da.list_dir_end()
	DirAccess.remove_absolute(abs)


func test_selecting_entry_seeks_preview() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	editor.set_size(Vector2(1280, 720))
	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	var err := doc.open_kda(_kda_path)
	assert_eq(err, OK)
	editor.set_document(doc)
	await get_tree().process_frame
	await get_tree().process_frame  # let the player rebuild

	var player: NovaCreditsPlayer = editor.get_node("%Player")
	var block_list = editor.get_node("%BlockList")

	var initial_offset := player.get_scroll_offset()
	# Pick an entry deep into the list.
	var target_index := mini(50, doc.resource.get_entry_count() - 1)
	var target_entry := doc.resource.get_entry(target_index)

	block_list.select_entry(target_entry)
	await get_tree().process_frame

	var new_offset := player.get_scroll_offset()
	assert_ne(new_offset, initial_offset, "preview scrolls when a deep entry is selected")
	var expected_y := player.content_y_for_entry(target_index) + player.get_size().y * 0.5
	assert_almost_eq(new_offset, expected_y, 1.0, "preview scrolled near the entry's Y + viewport half")

func test_fixed_image_uses_logical_stream_y_for_sync() -> void:
	var player := NovaCreditsPlayer.new()
	add_child_autofree(player)
	player.set_size(Vector2(640, 480))
	await get_tree().process_frame

	var res := CbinCreditsResource.new()
	res.set_vertical_space(20)
	res.add_entry(CbinNewlineEntry.new())
	var fixed := CbinImageEntry.new()
	fixed.set_advances_y(false)
	fixed.set_display_y(333)
	res.add_entry(fixed)
	player.set_credits_resource(res)
	await get_tree().process_frame
	player.rebuild()

	assert_almost_eq(player.content_y_for_entry(1), 20.0, 1.0, "fixed image reports its stream Y, not overlay display Y")
	player.set_scroll_offset(player.content_y_for_entry(1) + player.get_size().y * 0.5)
	assert_eq(player.entry_index_at_scroll_center(), 1, "fixed image can be selected from the centered preview stream")

func test_scrolling_image_advances_by_rendered_height() -> void:
	var player := NovaCreditsPlayer.new()
	add_child_autofree(player)
	player.set_size(Vector2(640, 480))
	await get_tree().process_frame

	var texture := ResourceLoader.load("res://assets/textures/bink.tga") as Texture2D
	assert_not_null(texture, "bink.tga fixture should be loadable for layout regression.")
	if texture == null:
		return

	var res := CbinCreditsResource.new()
	res.set_vertical_space(20)
	var image := CbinImageEntry.new()
	image.set_advances_y(true)
	image.set_texture(texture)
	image.set_texture_name("bink.tga")
	res.add_entry(image)
	var text := CbinTextEntry.new()
	text.set_text("After image")
	res.add_entry(text)

	player.set_credits_resource(res)
	await get_tree().process_frame
	player.rebuild()

	var expected_y := texture.get_size().y + res.get_vertical_space()
	assert_almost_eq(player.content_y_for_entry(1), expected_y, 1.0,
		"entry after a scrolling image should start after the rendered image height plus spacing.")

func test_player_scroll_during_playback_updates_list_selection() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	editor.set_size(Vector2(1280, 720))
	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	var err := doc.open_kda(_kda_path)
	assert_eq(err, OK)
	editor.set_document(doc)
	await get_tree().process_frame
	await get_tree().process_frame

	var player: NovaCreditsPlayer = editor.get_node("%Player")
	var block_list = editor.get_node("%BlockList")

	player.play()
	await get_tree().process_frame
	var deep_index := mini(40, doc.resource.get_entry_count() - 1)
	var deep_y := player.content_y_for_entry(deep_index) + player.get_size().y * 0.5
	player.set_scroll_offset(deep_y)
	await get_tree().process_frame

	var sel = block_list._selected_entry
	assert_not_null(sel, "list selection updated to follow playback scroll")

func test_player_scroll_while_stopped_updates_list_selection() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	editor.set_size(Vector2(1280, 720))
	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	for i in range(12):
		var entry := CbinTextEntry.new()
		entry.set_text("Entry %d" % i)
		doc.resource.add_entry(entry)
	editor.set_document(doc)
	await get_tree().process_frame
	await get_tree().process_frame

	var player: NovaCreditsPlayer = editor.get_node("%Player")
	var block_list = editor.get_node("%BlockList")
	var target_index := 8
	var target_y := player.content_y_for_entry(target_index) + player.get_size().y * 0.5
	player.set_scroll_offset(target_y)
	await get_tree().process_frame

	assert_eq(block_list._selected_entry, doc.resource.get_entry(target_index), "stopped preview scrub updates editor selection")

func test_mouse_wheel_over_preview_scrubs_preview() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	editor.set_size(Vector2(1280, 720))
	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	for i in range(20):
		var entry := CbinTextEntry.new()
		entry.set_text("Entry %d" % i)
		doc.resource.add_entry(entry)
	editor.set_document(doc)
	await get_tree().process_frame
	await get_tree().process_frame

	var player: NovaCreditsPlayer = editor.get_node("%Player")
	var block_list = editor.get_node("%BlockList")
	var ev := InputEventMouseButton.new()
	ev.button_index = MOUSE_BUTTON_WHEEL_DOWN
	ev.pressed = true
	player.gui_input.emit(ev)
	await get_tree().process_frame

	assert_gt(player.get_scroll_offset(), 0.0, "wheel down over preview advances the preview offset")
	assert_not_null(block_list._selected_entry, "wheel scrub syncs editor selection")

func test_editor_scroll_sync_uses_centered_card() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	editor.set_size(Vector2(1280, 720))
	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	for i in range(40):
		var entry := CbinTextEntry.new()
		entry.set_text("Entry %d" % i)
		doc.resource.add_entry(entry)
	editor.set_document(doc)
	await get_tree().process_frame
	await get_tree().process_frame

	var player: NovaCreditsPlayer = editor.get_node("%Player")
	var block_list = editor.get_node("%BlockList")
	var block_scroll: ScrollContainer = editor.get_node("%BlockScroll")
	var target_index := 14
	var target_entry := doc.resource.get_entry(target_index)
	var target_card: Control = block_list.card_for_entry(target_entry)
	assert_not_null(target_card)

	block_scroll.scroll_vertical = int(target_card.position.y + target_card.size.y * 0.5 - block_scroll.size.y * 0.5)
	await get_tree().process_frame
	editor._on_block_scroll(block_scroll.scroll_vertical)

	var expected_y := player.content_y_for_entry(target_index) + player.get_size().y * 0.5
	assert_almost_eq(player.get_scroll_offset(), expected_y, 1.0, "list scroll sync targets the card nearest viewport center")
