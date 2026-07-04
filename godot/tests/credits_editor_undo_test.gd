extends GutTest

# B2: credits undo — the document opts into SnapshotEditSession via the
# CbinCreditsResource to_text/from_text pair. ACCEPTED BEHAVIOR: undo/redo
# rebuilds the entry objects (from_text), so entry IDENTITY resets — the same
# thing a source-view Apply does; the block list reconciles by entry and
# rebuilds its cards.

const CreditsEditorDocument = preload("res://modtools/credits/credits_editor_document.gd")
const CreditsWorkspaceScript = preload("res://modtools/credits/credits_workspace.gd")
const BlockListScript = preload("res://modtools/credits/credits_editor_block_list.gd")


func _text_entry(text: String) -> CbinTextEntry:
	var e := CbinTextEntry.new()
	e.set_text(text)
	return e


func test_typing_burst_folds_into_one_step_and_identity_resets() -> void:
	var doc = autofree(CreditsEditorDocument.new())
	doc.resource.add_entry(_text_entry("original"))
	doc.clear_history()
	var before_entry: CbinEntry = doc.resource.get_entry(0)

	doc.begin_edit()
	(doc.resource.get_entry(0) as CbinTextEntry).set_text("a")
	doc.begin_edit()  # idempotent while the session is open
	(doc.resource.get_entry(0) as CbinTextEntry).set_text("ab")
	doc.flush_edit()

	assert_true(doc.can_undo(), "One committed burst should be undoable.")
	doc.undo()
	assert_eq((doc.resource.get_entry(0) as CbinTextEntry).get_text(), "original",
		"Undo should restore the pre-burst text in one step.")
	# Entry identity reset is the accepted from_text behavior (matches Apply).
	assert_ne(doc.resource.get_entry(0), before_entry,
		"Undo rebuilds entries; identity is NOT preserved (accepted).")
	assert_true(doc.can_redo(), "Undo should leave a redo step.")
	doc.redo()
	assert_eq((doc.resource.get_entry(0) as CbinTextEntry).get_text(), "ab",
		"Redo should reapply the whole burst.")


func test_push_undo_step_is_equal_gated() -> void:
	var doc = autofree(CreditsEditorDocument.new())
	doc.resource.add_entry(_text_entry("x"))
	doc.clear_history()

	# A mutation that changes nothing must not record a step.
	var same_text: String = doc.resource.to_text()
	doc.push_undo_step(func() -> void: doc.resource.from_text(same_text))
	assert_false(doc.can_undo(), "A no-op mutation should be equal-gated away.")

	doc.push_undo_step(func() -> void: doc.resource.add_entry(_text_entry("y")))
	assert_true(doc.can_undo(), "A real structural change should record a step.")
	doc.undo()
	assert_eq(doc.resource.get_entry_count(), 1, "Undo should drop the added entry.")


func test_block_list_insert_delete_move_are_single_steps() -> void:
	var doc = autofree(CreditsEditorDocument.new())
	var list = add_child_autofree(BlockListScript.new())
	list.set_document(doc)
	list.set_resource(doc.resource)

	list.add_text()
	assert_eq(doc.resource.get_entry_count(), 1, "add_text inserts an entry.")
	assert_true(doc.can_undo(), "Insert should record one step.")

	# Distinct texts: reordering IDENTICAL entries serializes identically and
	# is (correctly) equal-gated away, so give the rows observable identities.
	(doc.resource.get_entry(0) as CbinTextEntry).set_text("A")
	doc.push_undo_step(func() -> void: doc.resource.add_entry(_text_entry("B")))
	await get_tree().process_frame  # deferred card reconcile
	assert_eq(doc.resource.get_entry_count(), 2)

	# Move the selected (second) entry up: one step. Undo rebuilds entries, so
	# compare TEXT, not identity (the accepted from_text behavior).
	list.select_entry(doc.resource.get_entry(1))
	assert_true(list.move_selected(-1), "Alt+Up on the selection should act.")
	assert_eq((doc.resource.get_entry(0) as CbinTextEntry).get_text(), "B",
		"Order should have swapped.")
	doc.undo()
	assert_eq((doc.resource.get_entry(0) as CbinTextEntry).get_text(), "A",
		"Undo should restore the order.")

	# Delete a selected entry: one step. Undo reset entry identity, so
	# re-select through the rebuilt resource before deleting.
	await get_tree().process_frame
	list.select_entry(doc.resource.get_entry(1))
	assert_true(list.delete_selected(), "Delete on the selection should act.")
	assert_eq(doc.resource.get_entry_count(), 1)
	doc.undo()
	assert_eq(doc.resource.get_entry_count(), 2, "Undo should restore the deletion.")


func test_workspace_derives_undo_from_document() -> void:
	var ws = autofree(CreditsWorkspaceScript.new())
	var doc = ws.get_editor_document()
	assert_not_null(doc, "Credits workspace exposes its document.")
	doc.push_undo_step(func() -> void: doc.resource.add_entry(_text_entry("z")))
	assert_true(ws.can_undo(), "Workspace can_undo derives from the document.")
	ws.undo()
	assert_eq(doc.resource.get_entry_count(), 0, "Workspace undo routes to the document.")


func test_new_and_open_clear_history() -> void:
	var doc = autofree(CreditsEditorDocument.new())
	doc.push_undo_step(func() -> void: doc.resource.add_entry(_text_entry("q")))
	assert_true(doc.can_undo())
	doc.create_new()
	assert_false(doc.can_undo(), "create_new drops the old document's history.")
	assert_false(doc.is_dirty, "create_new starts clean.")
