extends "res://tests/shader_resource_contract_test.gd"

## Rewrites tests/object_shader_resource_hashes.golden.json from the live
## shader sources after a WITNESSED shader change, then fails on purpose so a
## regen run is never mistaken for a green validation. The suite never
## collects this file (it is not a *_test.gd); run it alone:
##
##   "$GODOT_BIN" --headless --path godot -s addons/gut/gut_cmdln.gd \
##       -gtest=res://tests/tools/shader_hashes_regen.gd -gunit_test_name=test_regen -gexit
##
## then review the golden diff before committing it.


func test_regen() -> void:
	var payload := _hash_payload()
	var file := FileAccess.open(HASH_GOLDEN_PATH, FileAccess.WRITE)
	assert_not_null(file, "the golden path is writable")
	if file != null:
		file.store_string(JSON.stringify(payload, "  ", true) + "\n")
		file.close()
	fail_test("shader hash golden rewritten at %s; a regen run is deliberately red" % HASH_GOLDEN_PATH)
