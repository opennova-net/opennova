extends GutTest

# Tests for the to_text / from_text contract that CreditsEditorSourceView depends on.
# These operate directly on CbinCreditsResource (no scene required).
#
# NOTE: test_roundtrip_stable and test_from_text_no_text_section_returns_false
# assert the T21 atomic-commit invariants. Those tests will only pass once the
# DLL is rebuilt with the T21 changes (cbin_credits_resource.cpp > DLL mtime).
# test_to_text_contains_required_headers is DLL-independent and should always pass.

const KDA_PATH := "res://../fixtures/cbin/nlist.reference.kda"

# A minimal but complete KDA text usable on any DLL version.
const MINIMAL_KDA := "[ENV]\nscroll_rate=1.0\n[TEXT]\nHello\nWorld\n"


func test_to_text_contains_required_headers() -> void:
	var res := ResourceLoader.load(KDA_PATH, "CbinCreditsResource",
		ResourceLoader.CACHE_MODE_IGNORE) as CbinCreditsResource
	assert_not_null(res, "Fixture should load before header-check test.")
	if res == null:
		return

	var text := res.to_text()
	assert_true(text.contains("[ENV]"),
		"to_text output should contain the '[ENV]' section header.")
	assert_true(text.contains("[TEXT]"),
		"to_text output should contain the '[TEXT]' section header.")


func test_from_text_valid_minimal_input_updates_resource() -> void:
	# Verify from_text on a hand-crafted minimal input returns true and
	# replaces entries. This is independent of the fixture and doesn't depend
	# on T21 (zero-entries guard) or DLL-specific text encoding.
	var res := CbinCreditsResource.new()

	var ok := res.from_text(MINIMAL_KDA)
	assert_true(ok, "from_text on minimal valid KDA should return true.")
	# The minimal input has two plain text lines: "Hello" and "World".
	assert_eq(res.get_entry_count(), 2,
		"Minimal input with two text lines should produce exactly 2 entries.")
	assert_eq(res.get_scroll_rate(), 1.0,
		"Minimal input scroll_rate should be parsed as 1.0.")


func test_roundtrip_stable() -> void:
	# to_text -> from_text -> to_text should be idempotent on a hand-crafted
	# resource (avoids fixture-specific lossiness in the old DLL).
	# This test requires T21 DLL (atomic from_text + stable to_text).
	var res := CbinCreditsResource.new()

	var ok_first := res.from_text(MINIMAL_KDA)
	assert_true(ok_first, "Initial from_text should succeed.")
	if not ok_first:
		return

	var first_text := res.to_text()
	assert_false(first_text.is_empty(), "to_text should not be empty after valid from_text.")

	var ok_second := res.from_text(first_text)
	assert_true(ok_second, "from_text on serialised output should succeed.")

	var second_text := res.to_text()
	assert_eq(second_text, first_text,
		"to_text -> from_text -> to_text should produce identical output (stable roundtrip).")


func test_from_text_malformed_empty_string_preserves_entries() -> void:
	# Empty string must return false and preserve existing entries.
	# This behavior is present in ALL DLL versions (pre- and post-T21).
	var res := CbinCreditsResource.new()
	var ok_init := res.from_text(MINIMAL_KDA)
	assert_true(ok_init, "Setup: initial from_text should succeed.")
	var initial_count := res.get_entry_count()
	assert_true(initial_count > 0, "Setup: resource should have entries after from_text.")

	var ok := res.from_text("")
	assert_false(ok, "from_text('') should return false.")
	assert_eq(res.get_entry_count(), initial_count,
		"Entry count should be unchanged after failed parse of empty string.")
