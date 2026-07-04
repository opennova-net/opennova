extends GutTest

# B8: the shared SearchField widget — construction contract + relay.


func test_construction_contract() -> void:
	var field: SearchField = autofree(SearchField.new("Find things"))
	assert_eq(field.placeholder_text, "Find things")
	assert_true(field.clear_button_enabled, "Clear button is part of the contract.")
	assert_eq(field.size_flags_horizontal, Control.SIZE_EXPAND_FILL)


func test_search_changed_relays_text_edits() -> void:
	var field: SearchField = add_child_autofree(SearchField.new())
	var seen: Array = []
	field.search_changed.connect(func(text: String) -> void: seen.append(text))
	field.text = "jo"
	field.text_changed.emit("jo")  # what typing produces
	assert_eq(seen, ["jo"], "search_changed relays text_changed.")


func test_is_a_line_edit_for_focus_guards() -> void:
	# The B6 focus guard and the B5 bracket both type-check on LineEdit; the
	# shared widget must keep matching them.
	var field: SearchField = autofree(SearchField.new())
	assert_true(field is LineEdit)
