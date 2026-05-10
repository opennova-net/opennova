extends GutTest


func test_from_text_empty_string_returns_false_and_preserves_entries() -> void:
	var res := CbinCreditsResource.new()
	var entry := CbinTextEntry.new()
	entry.set_text("existing")
	res.add_entry(entry)
	assert_eq(res.get_entry_count(), 1, "Should start with one entry.")

	var ok: bool = res.from_text("")
	assert_false(ok, "from_text('') should return false.")
	assert_eq(res.get_entry_count(), 1, "Entry count should be unchanged after failed parse.")


func test_from_text_malformed_tilde_f_returns_false_and_preserves_entries() -> void:
	var res := CbinCreditsResource.new()
	var entry := CbinTextEntry.new()
	entry.set_text("existing")
	res.add_entry(entry)
	assert_eq(res.get_entry_count(), 1, "Should start with one entry.")

	# Malformed ~F: only one pipe separator (missing y field).
	var ok: bool = res.from_text("[TEXT]\n~F0|cr1.png")
	assert_false(ok, "from_text with malformed ~F (one pipe) should return false.")
	assert_eq(res.get_entry_count(), 1, "Entry count should be unchanged after failed parse.")


func test_from_text_valid_input_returns_true_and_updates_resource() -> void:
	var res := CbinCreditsResource.new()
	var entry := CbinTextEntry.new()
	entry.set_text("old entry")
	res.add_entry(entry)
	assert_eq(res.get_entry_count(), 1, "Should start with one entry.")

	var ok: bool = res.from_text("[ENV]\nscroll_rate=1.5\n[TEXT]\nHello\n")
	assert_true(ok, "from_text with valid input should return true.")
	assert_eq(res.get_entry_count(), 1, "Should have exactly one entry after parse.")
	assert_eq(res.get_scroll_rate(), 1.5, "scroll_rate should be updated to 1.5.")

	var parsed_entry := res.get_entry(0)
	assert_not_null(parsed_entry, "Parsed entry should not be null.")
	if parsed_entry == null:
		return
	assert_true(parsed_entry.get_class() == "CbinTextEntry",
		"Entry should be a CbinTextEntry, got: %s" % parsed_entry.get_class())
	assert_eq((parsed_entry as CbinTextEntry).get_text(), "Hello",
		"Parsed text entry should contain 'Hello'.")
