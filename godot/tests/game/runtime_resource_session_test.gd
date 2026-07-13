extends GutTest


class FakeRoot:
	extends RefCounted

	var calls: Array[Dictionary] = []
	var fail_expansion := ""
	var last_error := ""

	func mount_runtime(dir: String, expansion: String = "", loose_override: bool = false) -> int:
		calls.append({
			"kind": "packed",
			"dir": dir,
			"expansion": expansion,
			"loose_override": loose_override,
		})
		if not fail_expansion.is_empty() and expansion == fail_expansion:
			last_error = "synthetic mount failure"
			return ERR_CANT_OPEN
		return OK

	func mount_loose_runtime(dir: String, expansion: String = "") -> int:
		calls.append({
			"kind": "loose_only",
			"dir": dir,
			"expansion": expansion,
		})
		if not fail_expansion.is_empty() and expansion == fail_expansion:
			last_error = "synthetic mount failure"
			return ERR_CANT_OPEN
		return OK

	func get_last_error() -> String:
		return last_error


func test_default_game_launch_is_packed_only() -> void:
	var root := FakeRoot.new()
	var launch := NovaLaunchFlags.parse(PackedStringArray(), "saved_exp")
	var session := NovaRuntimeResourceSession.new()

	assert_eq(session.start("C:/game", "saved_exp", launch, root), OK)
	assert_same(session.get_root(), root)
	assert_eq(session.get_mode(), NovaRuntimeResourceSession.Mode.PACKED_ONLY)
	assert_eq(root.calls, [{
		"kind": "packed",
		"dir": "C:/game",
		"expansion": "saved_exp",
		"loose_override": false,
	}])


func test_game_d_keeps_pffs_and_adds_loose_override() -> void:
	var root := FakeRoot.new()
	var launch := NovaLaunchFlags.parse(PackedStringArray(["/d"]))
	var session := NovaRuntimeResourceSession.new()

	assert_eq(session.start("C:/game", "", launch, root), OK)
	assert_eq(session.get_mode(), NovaRuntimeResourceSession.Mode.PACKED_WITH_LOOSE_OVERRIDE)
	assert_eq(root.calls[0]["kind"], "packed")
	assert_true(root.calls[0]["loose_override"])


func test_oned_handoff_uses_its_root_in_loose_only_mode() -> void:
	var root := FakeRoot.new()
	var launch := NovaLaunchFlags.parse(PackedStringArray([
		"/d", "--oned-resource-root", "C:/ONED loose root",
	]))
	var session := NovaRuntimeResourceSession.new()

	assert_eq(session.start("C:/saved/game/root", "game_saved_exp", launch, root), OK)
	assert_eq(session.get_mode(), NovaRuntimeResourceSession.Mode.ONED_LOOSE_ONLY)
	assert_true(session.is_oned_session())
	assert_eq(root.calls, [{
		"kind": "loose_only",
		"dir": "C:/ONED loose root",
		"expansion": "",
	}])


func test_explicit_expansion_is_session_authoritative_and_read_only() -> void:
	var persisted: Array[String] = []
	var root := FakeRoot.new()
	var launch := NovaLaunchFlags.parse(
		PackedStringArray(["/exp", "jox01"]), "stale_saved_exp")
	var session := NovaRuntimeResourceSession.new()

	assert_eq(session.start("C:/game", "stale_saved_exp", launch, root,
		func(name: String) -> void: persisted.append(name)), OK)
	assert_eq(session.get_expansion(), "jox01")
	assert_true(session.is_expansion_locked())
	assert_eq(session.select_expansion("other"), ERR_UNAUTHORIZED)
	assert_eq(root.calls.size(), 1, "read-only Mods UI cannot remount")
	assert_true(persisted.is_empty(), "an explicit /exp is never saved")


func test_interactive_expansion_selection_remounts_same_mode_and_persists() -> void:
	var persisted: Array[String] = []
	var root := FakeRoot.new()
	var launch := NovaLaunchFlags.parse(PackedStringArray(["/d"]))
	var session := NovaRuntimeResourceSession.new()
	assert_eq(session.start("C:/game", "", launch, root,
		func(name: String) -> void: persisted.append(name)), OK)

	assert_eq(session.select_expansion("jox01"), OK)
	assert_eq(session.get_expansion(), "jox01")
	assert_eq(root.calls[-1], {
		"kind": "packed",
		"dir": "C:/game",
		"expansion": "jox01",
		"loose_override": true,
	})
	assert_eq(persisted, ["jox01"])


func test_failed_expansion_mount_rolls_back_without_persisting() -> void:
	var persisted: Array[String] = []
	var root := FakeRoot.new()
	var launch := NovaLaunchFlags.parse(PackedStringArray(), "base_exp")
	var session := NovaRuntimeResourceSession.new()
	assert_eq(session.start("C:/game", "base_exp", launch, root,
		func(name: String) -> void: persisted.append(name)), OK)
	root.fail_expansion = "broken"

	assert_eq(session.select_expansion("broken"), ERR_CANT_OPEN)
	assert_eq(session.get_expansion(), "base_exp")
	assert_eq(root.calls[-1]["expansion"], "base_exp", "the previous mount is restored")
	assert_true(persisted.is_empty())
	assert_eq(session.get_last_error(), "synthetic mount failure")
