extends GutTest

const CreditsEditorBlockListScene = preload("res://modtools/credits/credits_editor_block_list.gd")

func _make_resource_with_one_text_entry() -> CbinCreditsResource:
	var res := CbinCreditsResource.new()
	var entry := CbinTextEntry.new()
	entry.set_text("Hello")
	res.add_entry(entry)
	return res

func test_card_survives_text_edit() -> void:
	var list := CreditsEditorBlockListScene.new() as VBoxContainer
	add_child_autofree(list)
	list.set_size(Vector2(400, 600))
	await get_tree().process_frame

	var res := _make_resource_with_one_text_entry()
	list.set_resource(res)
	await get_tree().process_frame

	# Layout for T6+ VBoxContainer-based block list:
	# VBox/Card[0]
	var card_node := list.get_child(0)
	assert_not_null(card_node, "card should exist")
	var card_id := card_node.get_instance_id()

	var entry := res.get_entry(0) as CbinTextEntry
	entry.set_text("Edited")
	await get_tree().process_frame

	var same_card := instance_from_id(card_id)
	assert_not_null(same_card, "card must still be alive after entry mutation")
	if same_card != null:
		assert_true(same_card.is_inside_tree(), "card stays in tree")

func test_list_rebuilds_on_add() -> void:
	var list := CreditsEditorBlockListScene.new() as VBoxContainer
	add_child_autofree(list)
	await get_tree().process_frame

	var res := _make_resource_with_one_text_entry()
	list.set_resource(res)
	await get_tree().process_frame

	res.add_entry(CbinNewlineEntry.new())
	await get_tree().process_frame

	var card_count := 0
	for child in list.get_children():
		if child is PanelContainer:
			card_count += 1
	assert_eq(card_count, 2, "list rebuilt with both cards after add")

func test_text_edit_caret_preserved_during_rapid_typing() -> void:
	const CreditsEditorBlockCardScene = preload("res://modtools/credits/credits_editor_block_card.tscn")

	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	await get_tree().process_frame

	var entry := CbinTextEntry.new()
	entry.set_text("")
	card.bind(entry)
	await get_tree().process_frame

	var text_edit: LineEdit = card.get_node("%TextEdit")
	text_edit.grab_focus()
	await get_tree().process_frame
	assert_true(text_edit.has_focus(), "text edit grabs focus")

	# Simulate rapid typing: user types "abc" character by character.
	# Each emission is what fires when the user types one character — the
	# entry mutation triggers a refresh; with the fix in place, the focused
	# LineEdit's text must NOT be reassigned.
	text_edit.text = "a"
	text_edit.caret_column = 1
	entry.set_text("a")
	await get_tree().process_frame

	text_edit.text = "ab"
	text_edit.caret_column = 2
	entry.set_text("ab")
	await get_tree().process_frame

	text_edit.text = "abc"
	text_edit.caret_column = 3
	entry.set_text("abc")
	await get_tree().process_frame

	# Caret should still be at column 3 (end of typed text); the focused
	# LineEdit must not have been clobbered by _refresh.
	assert_eq(text_edit.text, "abc", "user-typed text preserved")
	assert_eq(text_edit.caret_column, 3, "caret stays at end of user input, not reset by refresh")

func test_focusing_child_control_selects_card() -> void:
	var list := CreditsEditorBlockListScene.new() as VBoxContainer
	add_child_autofree(list)
	await get_tree().process_frame

	var res := CbinCreditsResource.new()
	for label in ["First", "Second"]:
		var entry := CbinTextEntry.new()
		entry.set_text(label)
		res.add_entry(entry)

	list.set_resource(res)
	await get_tree().process_frame

	var second_entry := res.get_entry(1)
	var card = list.card_for_entry(second_entry)
	assert_not_null(card, "second card should exist")
	if card == null:
		return

	var text_edit: LineEdit = card.get_node("%TextEdit")
	text_edit.grab_focus()
	await get_tree().process_frame

	assert_eq(list._selected_entry, second_entry,
		"focusing a child editor control should select its card.")
