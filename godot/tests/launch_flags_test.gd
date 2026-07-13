extends GutTest

# Public launch contract: only /d and /exp are retail-facing. ONED's resource
# root is a private handoff behind Godot's separator; /game is obsolete.


func test_defaults_to_packed_base_game() -> void:
	var launch := NovaLaunchFlags.parse(PackedStringArray(), "saved_exp")
	assert_false(launch.loose_override)
	assert_false(launch.has_expansion_override)
	assert_eq(launch.expansion, "saved_exp")
	assert_eq(launch.oned_resource_root, "")


func test_d_enables_retail_loose_override() -> void:
	var launch := NovaLaunchFlags.parse(PackedStringArray(["/D"]))
	assert_true(launch.loose_override, "/d is case-insensitive")


func test_exp_is_authoritative_and_normalized() -> void:
	var launch := NovaLaunchFlags.parse(
		PackedStringArray(["/exp", "  jox01  "]), "stale_saved_exp")
	assert_true(launch.has_expansion_override)
	assert_eq(launch.expansion, "jox01")


func test_missing_exp_value_does_not_consume_another_option() -> void:
	var launch := NovaLaunchFlags.parse(
		PackedStringArray(["/exp", "/d"]), "saved_exp")
	assert_true(launch.loose_override)
	assert_false(launch.has_expansion_override)
	assert_eq(launch.expansion, "saved_exp")


func test_game_is_not_a_supported_runtime_option() -> void:
	var launch := NovaLaunchFlags.parse(
		PackedStringArray(["/game", "jodemo"]), "saved_exp")
	assert_false(launch.loose_override)
	assert_false(launch.has_expansion_override)
	assert_eq(launch.expansion, "saved_exp")


func test_oned_root_only_activates_with_d() -> void:
	var ignored := NovaLaunchFlags.parse(PackedStringArray([
		"--oned-resource-root", "C:/authoring/root",
	]))
	assert_eq(ignored.oned_resource_root, "",
		"the private handoff cannot silently turn a retail launch into loose-only")

	var launch := NovaLaunchFlags.parse(PackedStringArray([
		"/d", "--oned-resource-root", "C:/authoring/root",
	]))
	assert_eq(launch.oned_resource_root, "C:/authoring/root")
