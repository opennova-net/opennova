extends GutTest

const Session := preload("res://modtools/game_run_session.gd")


class FakeSession:
	extends GameRunSession

	var valid_root := "C:/assets"
	var existing_files := {}
	var cwd_supported := true
	var retail_error := ""
	var staged_result := {
		"ok": true,
		"exe": "C:/staged/Jointops.exe",
		"packed_dir": "C:/staged",
	}
	var spawned: Array = []
	var stage_calls: Array = []
	var killed: Array = []
	var waited: Array = []
	var released: Array = []
	var events: Array = []
	var alive := {}
	var next_pid := 4000
	var kill_ok := true
	var wait_ok := true
	var oned_exe := "C:/tools/opennova-modtools.exe"
	var project_dir := "C:/project"
	var dev_mode := true
	var now_msec := 0

	func _valid_resource_dir(path: String) -> bool:
		return path.replace("\\", "/") == valid_root

	func _file_exists(path: String) -> bool:
		var clean := path.replace("\\", "/")
		if bool(staged_result.get("ok", false)) \
				and clean == String(staged_result.get("exe", "")).replace("\\", "/"):
			return true
		return bool(existing_files.get(clean, false))

	func _supports_working_directory() -> bool:
		return cwd_supported

	func _retail_install_error(_retail_dir: String) -> String:
		return retail_error

	func _stage_retail(resource_dir: String, retail_dir: String) -> Dictionary:
		stage_calls.append({"resource_dir": resource_dir, "retail_dir": retail_dir})
		events.append("stage")
		return staged_result.duplicate(true)

	func _spawn_process(path: String, args: PackedStringArray, cwd: String) -> int:
		var pid := next_pid
		next_pid += 1
		spawned.append({"path": path, "args": args, "cwd": cwd, "pid": pid})
		events.append("spawn")
		alive[pid] = true
		return pid

	func _process_is_alive(pid: int) -> bool:
		return bool(alive.get(pid, false))

	func _kill_process(pid: int) -> bool:
		killed.append(pid)
		events.append("kill")
		if kill_ok:
			alive[pid] = false
		return kill_ok

	func _wait_for_exit(pid: int, timeout_msec: int) -> bool:
		waited.append({"pid": pid, "timeout": timeout_msec})
		events.append("wait")
		return wait_ok

	func _release_process(pid: int) -> void:
		released.append(pid)
		events.append("release")

	func _oned_executable_path() -> String:
		return oned_exe

	func _project_dir() -> String:
		return project_dir

	func _is_dev_mode() -> bool:
		return dev_mode

	func _now_msec() -> int:
		return now_msec


func test_runtime_flags_are_only_the_loose_resource_contract() -> void:
	var request := Session.Request.opennova("C:/assets", "JODEMO", "jox01")
	assert_eq(Session.runtime_flags(request), PackedStringArray([
		"/d",
		"--resource-dir", "C:/assets",
		"--loose-root",
		"/exp", "jox01",
		"/game", "jodemo",
	]))

	var base := Session.Request.opennova("C:/assets", "", "")
	assert_eq(Session.runtime_flags(base), PackedStringArray([
		"/d",
		"--resource-dir", "C:/assets",
		"--loose-root",
		"/game", "jo",
	]))


func test_dev_launch_plan_runs_the_game_scene_without_a_pack() -> void:
	assert_eq(Session.RUNTIME_SCENE, "res://game/game_runtime_root.tscn",
			"ONED launches through the debug embedding decision")
	var request := Session.Request.opennova("C:/assets", "jo", "")
	var plan := Session.launch_plan(
		"C:/godot/godot.exe", "C:/project", true, request,
		func(_path: String) -> bool: return false)
	assert_not_null(plan)
	assert_eq(plan.path, "C:/godot/godot.exe")
	assert_eq(plan.cwd, "")
	assert_eq(plan.args, PackedStringArray([
		"--path", "C:/project",
		Session.RUNTIME_SCENE, "--",
		"/d", "--resource-dir", "C:/assets", "--loose-root", "/game", "jo",
	]))


func test_packaged_plan_finds_the_sibling_opennova_executable() -> void:
	var expected := "C:/dist/opennova.exe"
	var request := Session.Request.opennova("C:/assets", "jo", "")
	var plan := Session.launch_plan(
		"C:/dist/opennova-modtools.exe", "C:/project", false, request,
		func(path: String) -> bool: return path.replace("\\", "/") == expected)
	assert_not_null(plan)
	assert_eq(plan.path.replace("\\", "/"), expected)
	assert_eq(plan.args[0], "--")
	assert_true(plan.args.has("--resource-dir"))
	assert_false(plan.args.has("--pack-game"))


func test_invalid_resource_dir_refuses_before_spawn_or_stage() -> void:
	var session := FakeSession.new()
	var statuses: Array = []
	session.status_changed.connect(
		func(text: String, kind: StringName) -> void:
			statuses.append({"text": text, "kind": kind}))
	assert_false(session.run_opennova("C:/missing"))
	assert_false(session.run_retail("C:/missing", "C:/retail"))
	assert_true(session.spawned.is_empty())
	assert_true(session.stage_calls.is_empty())
	assert_eq(statuses[-1]["kind"], &"warn")
	assert_string_contains(String(statuses[-1]["text"]), "resource directory")


func test_opennova_launches_directly_and_never_stages() -> void:
	var session := FakeSession.new()
	assert_true(session.run_opennova("C:/assets", "JODEMO", "jox01"))
	assert_eq(session.spawned.size(), 1)
	assert_true(session.stage_calls.is_empty())
	var launch: Dictionary = session.spawned[0]
	var args := launch["args"] as PackedStringArray
	assert_eq(args[args.find("--resource-dir") + 1], "C:/assets")
	assert_eq(args[args.find("/game") + 1], "jodemo")
	assert_eq(args[args.find("/exp") + 1], "jox01")
	for forbidden in ["localres.pff", "resource.pff", "--pack-game", "--loose-mission"]:
		assert_false(args.has(forbidden))
	assert_eq(session.get_state()["mode"], "opennova")
	assert_true(session.is_running())


func test_starting_another_target_stops_the_first_then_spawns_from_poll() -> void:
	var session := FakeSession.new()
	assert_true(session.run_opennova("C:/assets"))
	var first_pid := int(session.get_state()["pid"])
	assert_true(session.run_retail("C:/assets", "C:/retail"))
	assert_eq(session.killed, [first_pid])
	assert_true(session.waited.is_empty(), "the stop never blocks on the exit")
	assert_true(session.is_stopping())
	assert_eq(session.get_state()["state"], "stopping")
	assert_eq(session.spawned.size(), 1, "the second run waits for the first to exit")
	session.poll()  # the fake kill already marked the process dead
	assert_eq(session.released, [first_pid])
	assert_eq(session.events, ["spawn", "kill", "release", "stage", "spawn"])
	assert_eq(session.get_state()["mode"], "retail")
	assert_true(session.is_running())


func test_stop_reports_the_deadline_when_the_process_lingers() -> void:
	var session := FakeSession.new()
	assert_true(session.run_opennova("C:/assets"))
	var pid := int(session.get_state()["pid"])
	assert_true(session.stop())
	session.alive[pid] = true  # the kill was sent but the process has not exited
	session.now_msec = Session.STOP_WAIT_MSEC
	session.poll()
	assert_true(session.is_running(), "a lingering process stays owned")
	assert_eq(session.get_last_error(), "The running process did not exit after it was stopped.")
	assert_true(session.released.is_empty())


func test_retail_stages_then_launches_with_only_retail_flags_and_cwd() -> void:
	var session := FakeSession.new()
	assert_true(session.run_retail("C:/assets", "C:/retail"))
	assert_eq(session.stage_calls, [{
		"resource_dir": "C:/assets",
		"retail_dir": "C:/retail",
	}])
	assert_eq(session.spawned.size(), 1)
	var launch: Dictionary = session.spawned[0]
	assert_eq(launch["path"], "C:/staged/Jointops.exe")
	assert_eq(launch["cwd"], "C:/staged")
	assert_eq(Array(launch["args"] as PackedStringArray), ["/w", "/d", "/FRISK"])
	for flag in ["--resource-dir", "--loose-root", "/game", "/exp"]:
		assert_false((launch["args"] as PackedStringArray).has(flag))


func test_retail_readiness_is_read_only_and_explains_missing_install() -> void:
	var session := FakeSession.new()
	session.retail_error = "No game.cfg in C:/retail."
	assert_eq(
		session.retail_readiness("C:/assets", "C:/retail"),
		"No game.cfg in C:/retail.")
	assert_true(session.stage_calls.is_empty())
	assert_true(session.spawned.is_empty())


func test_retail_stage_failure_never_spawns() -> void:
	var session := FakeSession.new()
	session.staged_result = {"ok": false, "error": "staging failed"}
	assert_false(session.run_retail("C:/assets", "C:/retail"))
	assert_eq(session.stage_calls.size(), 1)
	assert_true(session.spawned.is_empty())
	assert_eq(session.get_last_error(), "staging failed")


func test_natural_exit_is_polled_and_released() -> void:
	var session := FakeSession.new()
	assert_true(session.run_opennova("C:/assets"))
	var pid := int(session.get_state()["pid"])
	session.alive[pid] = false
	session.poll()
	assert_false(session.is_running())
	assert_eq(session.released, [pid])
	assert_eq(session.get_state()["state"], "stopped")


func test_stop_failure_preserves_the_owned_process() -> void:
	var session := FakeSession.new()
	session.kill_ok = false
	assert_true(session.run_opennova("C:/assets"))
	var pid := int(session.get_state()["pid"])
	assert_false(session.stop())
	assert_true(session.is_running())
	assert_eq(session.get_state()["pid"], pid)
	assert_eq(session.get_last_error(), "Could not stop the running process.")
	assert_true(session.released.is_empty())


func test_shutdown_stops_and_releases_the_child_synchronously() -> void:
	var session := FakeSession.new()
	assert_true(session.run_opennova("C:/assets"))
	var pid := int(session.get_state()["pid"])
	assert_true(session.shutdown())
	assert_eq(session.killed, [pid])
	assert_eq(session.released, [pid])
	assert_false(session.is_running())


func test_shutdown_waits_for_a_stop_already_under_way() -> void:
	var session := FakeSession.new()
	assert_true(session.run_opennova("C:/assets"))
	var pid := int(session.get_state()["pid"])
	assert_true(session.stop())
	session.alive[pid] = true  # still exiting when the app closes
	assert_true(session.shutdown())
	assert_eq(int(session.waited[0]["timeout"]), Session.STOP_WAIT_MSEC)
	assert_eq(session.released, [pid])
	assert_false(session.is_running())
