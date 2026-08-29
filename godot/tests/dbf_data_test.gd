extends GutTest

## DbfData wrapper: parses a mission dialog bank and resolves a dialog id to
## its LWF set-name(s). Uses the minted repo fixture (one level above res://;
## tests/fixtures/minimal_dbf_gen.cpp).

func test_dbf_resolves_dialog_to_def_id() -> void:
	var path := ProjectSettings.globalize_path("res://").path_join("../fixtures/dbf/synth_bank.dbf")
	if not FileAccess.file_exists(path):
		pass_test("repo fixture fixtures/dbf/synth_bank.dbf not present; skipping")
		return
	var dbf := DbfData.new()
	assert_eq(dbf.open_file(path), OK, "DBF parses")
	assert_eq(dbf.get_dialog_count(), 11, "the synthetic bank has 11 dialog groups")
	assert_true(dbf.has_dialog("dlg001"))
	assert_true(dbf.has_dialog("DLG001"), "lookup is case-insensitive")
	assert_false(dbf.has_dialog("nope"))
	var defs := dbf.resolve_dialog("dlg001")
	assert_gt(defs.size(), 0, "dlg001 maps to at least one def_id")
	assert_eq(String(defs[0]), "SynR100", "dlg001 -> SynR100")
