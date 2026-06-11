extends GutTest

const CreditsEditorBlockListScene = preload("res://modtools/credits/credits_editor_block_list.gd")

func _make_resource_with(n: int) -> CbinCreditsResource:
	var res := CbinCreditsResource.new()
	for i in range(n):
		var e := CbinTextEntry.new()
		e.set_text("Entry %d" % i)
		res.add_entry(e)
	return res

func test_add_text_with_no_selection_appends() -> void:
	var list := CreditsEditorBlockListScene.new() as VBoxContainer
	add_child_autofree(list)
	var res := _make_resource_with(3)
	list.set_resource(res)
	await get_tree().process_frame

	list.add_text()
	await get_tree().process_frame
	assert_eq(res.get_entry_count(), 4, "added one entry")
	var last := res.get_entry(3) as CbinTextEntry
	assert_not_null(last, "last entry exists")
	assert_eq(last.get_text(), "New text", "appended at end")

func test_add_text_after_selected_inserts_at_index_plus_one() -> void:
	var list := CreditsEditorBlockListScene.new() as VBoxContainer
	add_child_autofree(list)
	var res := _make_resource_with(5)
	list.set_resource(res)
	await get_tree().process_frame

	var selected := res.get_entry(2)
	list.select_entry(selected)
	await get_tree().process_frame

	list.add_text()
	await get_tree().process_frame
	assert_eq(res.get_entry_count(), 6, "added one entry")
	var inserted := res.get_entry(3) as CbinTextEntry
	assert_not_null(inserted)
	assert_eq(inserted.get_text(), "New text", "inserted right after selection")
	assert_eq((res.get_entry(2) as CbinTextEntry).get_text(), "Entry 2")
	assert_eq((res.get_entry(4) as CbinTextEntry).get_text(), "Entry 3")

func test_add_text_selects_inserted_entry_and_emits_after_reconcile() -> void:
	var list := CreditsEditorBlockListScene.new() as VBoxContainer
	add_child_autofree(list)
	var res := _make_resource_with(3)
	list.set_resource(res)
	await get_tree().process_frame

	var selected := res.get_entry(0)
	list.select_entry(selected)
	await get_tree().process_frame

	var seen := []
	list.selection_changed.connect(func(entry): seen.append(entry))
	list.add_text()
	await get_tree().process_frame
	await get_tree().process_frame

	var inserted := res.get_entry(1)
	assert_eq(list._selected_entry, inserted, "inserted entry becomes selected")
	assert_true(seen.has(inserted), "selection_changed emits for the inserted entry after cards reconcile")

func test_deleting_selected_entry_selects_next_fallback() -> void:
	var list := CreditsEditorBlockListScene.new() as VBoxContainer
	add_child_autofree(list)
	var res := _make_resource_with(3)
	list.set_resource(res)
	await get_tree().process_frame

	var selected := res.get_entry(1)
	var expected_next := res.get_entry(2)
	list.select_entry(selected)
	await get_tree().process_frame

	var seen := []
	list.selection_changed.connect(func(entry): seen.append(entry))
	var selected_card = list.card_for_entry(selected)
	assert_not_null(selected_card, "selected card exists before delete")
	list._on_card_delete(selected_card)
	await get_tree().process_frame

	assert_eq(res.get_entry_count(), 2, "delete removes one entry")
	assert_eq(list._selected_entry, expected_next, "next entry becomes selected after deleting selected card")
	assert_true(seen.has(expected_next), "selection_changed emits selected delete fallback")

func test_consecutive_adds_chain_after_each_other() -> void:
	var list := CreditsEditorBlockListScene.new() as VBoxContainer
	add_child_autofree(list)
	var res := _make_resource_with(3)
	list.set_resource(res)
	await get_tree().process_frame

	list.select_entry(res.get_entry(0))
	list.add_text()  # lands at index 1
	await get_tree().process_frame
	list.add_text()  # should land at index 2 (right after first inserted)
	await get_tree().process_frame
	assert_eq(res.get_entry_count(), 5)
	assert_eq((res.get_entry(0) as CbinTextEntry).get_text(), "Entry 0")
	assert_eq((res.get_entry(1) as CbinTextEntry).get_text(), "New text")
	assert_eq((res.get_entry(2) as CbinTextEntry).get_text(), "New text")
	assert_eq((res.get_entry(3) as CbinTextEntry).get_text(), "Entry 1")

func test_card_click_emits_request_select() -> void:
	const CreditsEditorBlockCardScene = preload("res://modtools/credits/credits_editor_block_card.tscn")
	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	var entry := CbinTextEntry.new()
	card.bind(entry)
	await get_tree().process_frame

	var got := []
	card.request_select.connect(func(c): got.append(c))

	var ev := InputEventMouseButton.new()
	ev.button_index = MOUSE_BUTTON_LEFT
	ev.pressed = true
	# Synthesize gui_input firing on the panel itself.
	if card.has_signal("gui_input"):
		card.gui_input.emit(ev)
	await get_tree().process_frame

	assert_eq(got.size(), 1, "request_select fired once")
	assert_eq(got[0], card, "emitted with the card itself")
