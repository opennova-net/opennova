extends "res://tests/env_parity_vectors_test.gd"

## Prints the ENV-1 vector table (EXPECTED_BYTES / EXPECTED_FLOATS) from the
## live environment stack, then fails on purpose so a regen run is never
## mistaken for a green validation. The suite never collects this file (it is
## not a *_test.gd); run it alone:
##
##   "$GODOT_BIN" --headless --path godot -s addons/gut/gut_cmdln.gd \
##       -gtest=res://tests/tools/env_vectors_regen.gd -gunit_test_name=test_regen -gexit
##
## and paste the table between the ### markers into env_parity_vectors_test.gd.
## The policy header of that test applies: a re-dump needs a witness citation
## in the same commit, never a tolerance change.


func test_regen() -> void:
	var tables := _collect_all()
	_print_dump(tables[0], tables[1])
	fail_test("regen run: the vector table is printed above; a regen run is deliberately red")
