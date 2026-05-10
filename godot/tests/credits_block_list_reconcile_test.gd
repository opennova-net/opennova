extends GutTest

const CreditsEditorBlockListScene = preload("res://modtools/credits/credits_editor_block_list.gd")

func _make_resource_with(n: int) -> CbinCreditsResource:
	var res := CbinCreditsResource.new()
	for i in range(n):
		var e := CbinTextEntry.new()
		e.set_text("Entry %d" % i)
		res.add_entry(e)
	return res

func _snapshot_card_ids(list: VBoxContainer) -> Array:
	var ids := []
	for child in list.get_children():
		if child is PanelContainer:
			ids.append(child.get_instance_id())
	return ids

func _live_card_count(list: VBoxContainer) -> int:
	var n := 0
	for child in list.get_children():
		if child is PanelContainer and is_instance_valid(child):
			n += 1
	return n

func test_single_insert_reuses_existing_cards() -> void:
	var list := CreditsEditorBlockListScene.new() as VBoxContainer
	add_child_autofree(list)
	var res := _make_resource_with(5)
	list.set_resource(res)
	await get_tree().process_frame

	var before := _snapshot_card_ids(list)
	assert_eq(before.size(), 5, "five cards before insert")

	var new_entry := CbinTextEntry.new()
	new_entry.set_text("inserted")
	res.insert_entry(2, new_entry)
	await get_tree().process_frame

	var after := _snapshot_card_ids(list)
	assert_eq(after.size(), 6, "six cards after insert")
	var preserved := 0
	for prev_id in before:
		if instance_from_id(prev_id) != null:
			preserved += 1
	assert_eq(preserved, 5, "all five pre-existing cards preserved")

func test_single_delete_frees_one_card() -> void:
	var list := CreditsEditorBlockListScene.new() as VBoxContainer
	add_child_autofree(list)
	var res := _make_resource_with(5)
	list.set_resource(res)
	await get_tree().process_frame

	var before := _snapshot_card_ids(list)
	res.remove_entry(2)
	await get_tree().process_frame

	assert_eq(_live_card_count(list), 4, "four cards remain")
	var preserved := 0
	for prev_id in before:
		if instance_from_id(prev_id) != null:
			preserved += 1
	assert_eq(preserved, 4, "exactly four pre-existing cards survive (one freed)")

func test_drag_reorder_preserves_all_cards() -> void:
	var list := CreditsEditorBlockListScene.new() as VBoxContainer
	add_child_autofree(list)
	var res := _make_resource_with(5)
	list.set_resource(res)
	await get_tree().process_frame

	var before := _snapshot_card_ids(list)
	# Simulate a drag: remove entry at 1, insert it at 3.
	var entry := res.get_entry(1)
	res.remove_entry(1)
	res.insert_entry(3, entry)
	await get_tree().process_frame

	var preserved := 0
	for prev_id in before:
		if instance_from_id(prev_id) != null:
			preserved += 1
	assert_eq(preserved, 5, "all five cards survive a remove+insert reorder")
