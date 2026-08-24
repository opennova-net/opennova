extends GutTest

const Session := preload("res://modtools/editor/shell/shell_game_session.gd")


func _none_exists(_path: String) -> bool:
	return false


func test_runtime_flags_carry_exact_root_mission_and_editor_identity() -> void:
	var request := Session.GameRunRequest.make(
		Session.Mode.CURRENT_MISSION, "C:/assets", "jox01", "JODEMO")
	request.loose_mission = "alpha.bms"
	request.run_id = "run-17"
	request.descriptor_path = "C:/tmp/run-17.json"

	assert_eq(Session.runtime_flags(request), PackedStringArray([
		"/d",
		"--resource-dir", "C:/assets",
		"--loose-root",
		"/exp", "jox01",
		"/game", "jodemo",
		"--loose-mission", "alpha.bms",
		"--oned-run-id", "run-17",
		"--oned-run-descriptor", "C:/tmp/run-17.json",
	]))


func test_launch_plan_gives_each_child_its_own_godot_log() -> void:
	var request := Session.GameRunRequest.make(
		Session.Mode.GAME, "C:/assets", "", "jo")
	request.log_path = "C:/tmp/run-17.log"
	var plan := Session.launch_plan(
		"C:/godot/godot.exe", "C:/project", true, request,
		Callable(self, "_none_exists"))

	assert_eq(plan.path, "C:/godot/godot.exe")
	assert_eq(plan.args.slice(0, 6), PackedStringArray([
		"--path", "C:/project",
		"--log-file", "C:/tmp/run-17.log",
		Session.RUNTIME_SCENE, "--",
	]))
	assert_gt(plan.args.find("--resource-dir"), plan.args.find("--"),
		"custom game arguments stay behind Godot's separator")
	assert_eq(plan.args[plan.args.find("--oned-run-log") + 1],
			"C:/tmp/run-17.log",
			"the runtime MCP receives the exact engine log path")


func test_launch_plan_finds_runtime_beside_a_macos_editor_bundle() -> void:
	var request := Session.GameRunRequest.make(
			Session.Mode.GAME, "/assets", "", "jo")
	var expected := "/Applications/OpenNova/opennova.app/Contents/MacOS/opennova"
	var plan := Session.launch_plan(
			"/Applications/OpenNova/opennova-modtools.app/Contents/MacOS/opennova-modtools",
			"/project",
			false,
			request,
			func(path: String) -> bool: return path == expected)
	assert_not_null(plan)
	assert_eq(plan.path, expected)


func test_current_mission_validation_requires_saved_top_level_bms() -> void:
	var exists := func(path: String) -> bool: return path.ends_with("alpha.bms")
	var valid := Session.validate_current_mission(
		"C:/assets/alpha.bms", "C:/assets", exists)
	assert_true(valid["ok"])
	assert_eq(valid["name"], "alpha.bms")

	var nested := Session.validate_current_mission(
		"C:/assets/missions/alpha.bms", "C:/assets", exists)
	assert_false(nested["ok"])
	assert_string_contains(String(nested["reason"]), "top-level")

	var metafile := Session.validate_current_mission(
		"C:/assets/alpha.mis", "C:/assets", exists)
	assert_false(metafile["ok"])
	assert_string_contains(String(metafile["reason"]), ".bms")

	var missing := Session.validate_current_mission(
		"C:/assets/bravo.bms", "C:/assets", exists)
	assert_false(missing["ok"])
	assert_string_contains(String(missing["reason"]), "no longer exists")


func test_descriptor_cleanup_removes_only_the_owned_run() -> void:
	for ownership in [
		{"run_id": "another-run", "pid_offset": 0, "removed": false},
		{"run_id": "", "pid_offset": 1, "removed": false},
		{"run_id": "", "pid_offset": 0, "removed": true},
	]:
		var alive := {}
		var session := _make_session([], [], alive, [], [100])
		session.set_runtime_debug_enabled(true)
		assert_true(session.start_mode("game"))
		var state: Dictionary = session.get_state()
		var path := String(state["descriptor_path"])
		var descriptor_run_id := String(ownership["run_id"])
		if descriptor_run_id.is_empty():
			descriptor_run_id = String(state["run_id"])
		var file := FileAccess.open(path, FileAccess.WRITE)
		assert_not_null(file)
		file.store_string(JSON.stringify({
			"run_id": descriptor_run_id,
			"pid": int(state["pid"]) + int(ownership["pid_offset"]),
		}))
		file.close()

		alive[int(state["pid"])] = false
		session.poll()

		assert_eq(
				not FileAccess.file_exists(path),
				bool(ownership["removed"]),
				"cleanup removes only the descriptor owned by this exact run")
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)


func _make_session(
	spawned: Array,
	statuses: Array,
	alive: Dictionary,
	killed: Array,
	clock: Array,
	unsaved_workspaces := PackedStringArray(),
	kill_result: int = OK
) -> ShellGameSession:
	var next_pid := [4000]
	var session := Session.new()
	session.setup(
		func() -> String: return "C:/assets",
		func() -> String: return "",
		func() -> String: return "jo",
		func() -> Dictionary:
			return {"path": "C:/assets/alpha.bms"},
		func() -> PackedStringArray:
			return unsaved_workspaces,
		func(path: String, args: PackedStringArray, cwd: String) -> int:
			var pid: int = int(next_pid[0])
			next_pid[0] += 1
			spawned.append({"path": path, "args": args, "pid": pid, "cwd": cwd})
			alive[pid] = true
			return pid,
		func(path: String) -> bool:
			# Mission exists; sibling packaged executables do not, so tests use
			# the source-binary plan.
			return path.replace("\\", "/").to_lower().ends_with("/alpha.bms"),
		func(text: String, _duration: float, kind: StringName) -> void:
			statuses.append({"text": text, "kind": kind}),
		func(pid: int) -> bool: return bool(alive.get(pid, false)),
		func(pid: int) -> int:
			killed.append(pid)
			if kill_result == OK:
				alive[pid] = false
			return kill_result,
		func() -> int: return int(clock[0]))
	return session


func test_current_mission_capability_poll_preserves_the_last_operation_error() -> void:
	var session := _make_session([], [], {}, [], [100])
	assert_false(session.start_mode("not-a-mode"))
	var operation_error := session.get_last_error()

	assert_true(session.can_run_current_mission())
	assert_eq(session.get_current_mission_unavailable_reason(), "")
	assert_eq(session.get_last_error(), operation_error,
			"toolbar refresh cannot rewrite the session's real operation error")


func test_f6_launches_saved_file_warns_on_dirty_and_exposes_json_safe_state() -> void:
	var spawned: Array = []
	var statuses: Array = []
	var session := _make_session(
		spawned,
		statuses,
		{},
		[],
		[100],
		PackedStringArray(["Mission", "Environment"]))
	session.set_runtime_debug_enabled(true)

	assert_true(session.start_mode("mission"))
	assert_eq(spawned.size(), 1)
	var args := spawned[0]["args"] as PackedStringArray
	assert_eq(args[args.find("--loose-mission") + 1], "alpha.bms")
	assert_eq(args[args.find("--resource-dir") + 1], "C:/assets")
	assert_true(args.has("--oned-run-id"))
	assert_true(args.has("--oned-run-descriptor"))
	assert_true(args.has("--log-file"))
	assert_true(args.has("--oned-run-log"))
	assert_eq(statuses[-1]["kind"], &"warn")
	assert_string_contains(String(statuses[-1]["text"]), "Running saved alpha.bms")
	assert_string_contains(String(statuses[-1]["text"]), "Mission, Environment")
	assert_string_contains(String(statuses[-1]["text"]), "not included")

	var state: Dictionary = session.get_state()
	assert_eq(state["state"], "running")
	assert_eq(state["mode"], "mission")
	assert_eq(state["loose_mission"], "alpha.bms")
	assert_eq(state["unsaved_workspaces"], ["Mission", "Environment"])
	assert_false(String(state["run_id"]).is_empty())
	assert_true(String(state["descriptor_path"]).is_absolute_path())
	assert_true(String(state["log_path"]).is_absolute_path())


func test_f5_and_f6_omit_debug_identity_when_runtime_debug_is_disabled() -> void:
	for mode in ["game", "mission"]:
		var spawned: Array = []
		var session := _make_session(spawned, [], {}, [], [100])

		assert_true(session.start_mode(mode))
		var args := spawned[0]["args"] as PackedStringArray
		assert_false(args.has("--oned-run-id"), "%s has no debug run id" % mode)
		assert_false(args.has("--oned-run-descriptor"),
				"%s has no debug descriptor" % mode)
		assert_false(args.has("--oned-run-log"), "%s has no debug log identity" % mode)
		assert_false(args.has("--log-file"), "%s has no private Godot log" % mode)
		var state: Dictionary = session.get_state()
		assert_eq(state["run_id"], "")
		assert_eq(state["descriptor_path"], "")
		assert_eq(state["log_path"], "")


func test_retired_debug_identity_cannot_clear_a_different_run() -> void:
	var spawned: Array = []
	var session := _make_session(spawned, [], {}, [], [100])
	session.set_runtime_debug_enabled(true)
	assert_true(session.start_mode("game"))
	var run_id := String(session.get_state()["run_id"])
	assert_false(run_id.is_empty())

	assert_false(session.retire_runtime_debug_identity("another-run"))
	assert_eq(session.get_state()["run_id"], run_id)
	assert_true(session.retire_runtime_debug_identity(run_id))
	assert_eq(session.get_state()["run_id"], "")
	assert_eq(session.get_state()["descriptor_path"], "")
	assert_eq(session.get_state()["log_path"], "")
	assert_true(session.get_state()["running"],
			"retiring the debug endpoint does not stop the managed child")


func test_f5_warns_for_every_dirty_workspace_without_mutating_editor_state() -> void:
	var spawned: Array = []
	var statuses: Array = []
	var dirty_reads := [0]
	var session := Session.new()
	session.setup(
		func() -> String: return "C:/assets",
		func() -> String: return "",
		func() -> String: return "jo",
		func() -> Dictionary: return {"path": "C:/assets/alpha.bms"},
		func() -> PackedStringArray:
			dirty_reads[0] += 1
			return PackedStringArray(["Terrain", "Sounds", "Terrain", ""]),
		func(path: String, args: PackedStringArray, cwd: String) -> int:
			spawned.append({"path": path, "args": args})
			return 4000,
		func(_path: String) -> bool: return false,
		func(text: String, _duration: float, kind: StringName) -> void:
			statuses.append({"text": text, "kind": kind}))

	assert_true(session.start_mode("game"))
	assert_eq(spawned.size(), 1)
	assert_eq(dirty_reads[0], 1, "run only observes the shell dirty summary")
	assert_eq(statuses[-1]["kind"], &"warn")
	assert_string_contains(String(statuses[-1]["text"]), "saved loose assets")
	assert_string_contains(String(statuses[-1]["text"]), "Terrain, Sounds")
	assert_eq(session.get_state()["unsaved_workspaces"], ["Terrain", "Sounds"])


func test_second_start_replaces_the_one_tracked_process() -> void:
	var spawned: Array = []
	var alive := {}
	var killed: Array = []
	var session := _make_session(spawned, [], alive, killed, [100])

	assert_true(session.start_mode("game"))
	var first_pid := int(session.get_state()["pid"])
	assert_true(session.start_mode("mission"))

	assert_eq(killed, [first_pid], "without a runtime peer, restart uses the kill fallback")
	assert_eq(spawned.size(), 2)
	assert_eq(session.get_state()["mode"], "mission")
	assert_ne(session.get_state()["pid"], first_pid)


func test_graceful_stop_gets_a_window_then_falls_back_to_kill() -> void:
	var alive := {}
	var killed: Array = []
	var clock := [100]
	var quit_requests: Array = []
	var session := _make_session([], [], alive, killed, clock)
	session.set_runtime_control_hooks(
		Callable(),
		func(run_id: String, descriptor: String) -> bool:
			quit_requests.append({"run_id": run_id, "descriptor": descriptor})
			return true)
	assert_true(session.start_mode("game"))
	var pid := int(session.get_state()["pid"])

	assert_true(session.stop())
	assert_eq(session.get_state()["state"], "stopping")
	assert_eq(quit_requests.size(), 1)
	assert_true(killed.is_empty())

	clock[0] += Session.STOP_GRACE_MSEC
	session.poll()
	assert_eq(killed, [pid])
	assert_eq(session.get_state()["state"], "stopped")


func test_stop_returns_false_when_immediate_kill_fails() -> void:
	var statuses: Array = []
	var alive := {}
	var session := _make_session(
			[], statuses, alive, [], [100], PackedStringArray(), FAILED)
	assert_true(session.start_mode("game"))

	assert_false(session.stop())

	assert_eq(session.get_state()["state"], "running")
	assert_eq(session.get_state()["last_error"],
			"Could not stop the running game.")
	assert_eq(session.get_last_error(), "Could not stop the running game.")
	assert_eq(statuses[-1]["kind"], &"error")


func test_grace_timeout_kill_failure_restores_running_with_error() -> void:
	var statuses: Array = []
	var alive := {}
	var clock := [100]
	var session := _make_session(
			[], statuses, alive, [], clock, PackedStringArray(), FAILED)
	session.set_runtime_control_hooks(
			Callable(),
			func(_run_id: String, _descriptor: String) -> bool:
				return true)
	assert_true(session.start_mode("game"))
	assert_true(session.stop())

	clock[0] += Session.STOP_GRACE_MSEC
	session.poll()

	assert_eq(session.get_state()["state"], "running")
	assert_eq(session.get_state()["last_error"],
			"Could not stop the running game.")
	assert_eq(statuses[-1]["kind"], &"error")


func test_shutdown_preserves_owned_run_when_forced_termination_fails() -> void:
	var statuses: Array = []
	var alive := {}
	var killed: Array = []
	var session := _make_session(
			[], statuses, alive, killed, [100], PackedStringArray(), FAILED)
	session.set_runtime_debug_enabled(true)
	assert_true(session.start_mode("game"))
	var before: Dictionary = session.get_state()
	var pid := int(before["pid"])
	var descriptor_path := String(before["descriptor_path"])
	var descriptor := FileAccess.open(descriptor_path, FileAccess.WRITE)
	assert_not_null(descriptor)
	descriptor.store_string(JSON.stringify({
		"run_id": before["run_id"],
		"pid": pid,
	}))
	descriptor.close()

	assert_false(session.shutdown())

	var after: Dictionary = session.get_state()
	assert_eq(killed, [pid])
	assert_eq(after["state"], "running")
	assert_eq(after["pid"], pid)
	assert_eq(after["run_id"], before["run_id"])
	assert_eq(after["descriptor_path"], descriptor_path)
	assert_true(FileAccess.file_exists(descriptor_path),
			"the live child's descriptor remains owned and discoverable")
	assert_eq(session.get_last_error(),
			"Could not stop the running game during editor shutdown.")
	assert_eq(statuses[-1]["kind"], &"error")

	# A later confirmed exit may safely release the preserved ownership.
	alive[pid] = false
	session.poll()
	assert_eq(session.get_state()["state"], "stopped")
	assert_false(FileAccess.file_exists(descriptor_path))


func test_graceful_restart_waits_then_launches_the_queued_mode() -> void:
	var spawned: Array = []
	var alive := {}
	var killed: Array = []
	var clock := [100]
	var session := _make_session(spawned, [], alive, killed, clock)
	session.set_runtime_control_hooks(
		Callable(),
		func(_run_id: String, _descriptor: String) -> bool:
			return true)
	assert_true(session.start_mode("game"))
	var first_pid := int(session.get_state()["pid"])

	assert_true(session.start_mode("mission"))
	assert_eq(session.get_state()["state"], "stopping")
	assert_eq(spawned.size(), 1)
	alive[first_pid] = false
	session.poll()

	assert_eq(spawned.size(), 2)
	assert_eq(session.get_state()["state"], "running")
	assert_eq(session.get_state()["mode"], "mission")
	assert_ne(int(session.get_state()["pid"]), first_pid)
	assert_true(killed.is_empty(), "natural graceful exit never needs the kill fallback")


func test_natural_exit_is_detected_and_runtime_tools_use_exact_identity() -> void:
	var spawned: Array = []
	var statuses: Array = []
	var alive := {}
	var calls: Array = []
	var session := _make_session(spawned, statuses, alive, [], [100])
	session.set_runtime_debug_enabled(true)
	session.set_runtime_control_hooks(
		func(run_id: String, descriptor: String, tool: String, args: Dictionary) -> Variant:
			calls.append({
				"run_id": run_id,
				"descriptor": descriptor,
				"tool": tool,
				"args": args,
			})
			return {"ok": true})
	assert_true(session.start_mode("game"))
	var state: Dictionary = session.get_state()
	assert_eq(session.call_runtime_tool("snapshot", {"page": "perf"}), {"ok": true})
	assert_eq(calls[0]["run_id"], state["run_id"])
	assert_eq(calls[0]["descriptor"], state["descriptor_path"])

	alive[int(state["pid"])] = false
	session.poll()
	assert_eq(session.get_state()["state"], "stopped")
	assert_string_contains(String(statuses[-1]["text"]), "exited")


# A session wired for RETAIL: a configured install, a packer that records its calls, and a
# file_exists that answers for the retail exe as well as the mission.
func _make_retail_session(
	spawned: Array,
	packs: Array,
	retail_dir: String = "C:/retail",
	pack_ok: bool = true
) -> ShellGameSession:
	var next_pid := [7000]
	var session := Session.new()
	session.setup(
		func() -> String: return "C:/assets",
		func() -> String: return "",
		func() -> String: return "jo",
		func() -> Dictionary: return {"path": "C:/assets/alpha.bms"},
		func() -> PackedStringArray: return PackedStringArray(),
		func(path: String, args: PackedStringArray, cwd: String) -> int:
			var pid: int = int(next_pid[0])
			next_pid[0] += 1
			spawned.append({"path": path, "args": args, "pid": pid, "cwd": cwd})
			return pid,
		func(path: String) -> bool:
			var lower := path.replace("\\", "/").to_lower()
			return lower.ends_with("/alpha.bms") or lower.ends_with("/jointops.exe"),
		func(_text: String, _duration: float, _kind: StringName) -> void: pass,
		func(_pid: int) -> bool: return true,
		func(_pid: int) -> int: return OK,
		func() -> int: return 100,
		func() -> String: return retail_dir,
		func(out_dir: String, staged_from: String) -> Dictionary:
			packs.append({"out_dir": out_dir, "retail_dir": staged_from})
			if not pack_ok:
				return {"ok": false, "error": "packing blew up"}
			return {"ok": true, "exe": out_dir.path_join("Jointops.exe")})
	return session


func test_retail_packs_once_then_launches_the_packed_exe() -> void:
	var spawned: Array = []
	var packs: Array = []
	var session := _make_retail_session(spawned, packs)

	assert_true(session.start_mode("retail"), "retail starts when an install is configured")
	assert_eq(packs.size(), 1, "the assets are packed exactly once per launch")
	assert_eq(String(packs[0]["retail_dir"]), "C:/retail",
			"the configured install is what gets staged")
	assert_eq(spawned.size(), 1)
	assert_true(String(spawned[0]["path"]).to_lower().ends_with("/jointops.exe"),
			"retail launches the exe the pack step staged, not our runtime")
	assert_eq(Array(spawned[0]["args"] as PackedStringArray), ["/w", "/d", "/FRISK"],
			"retail gets retail's flags and none of ours")
	# Retail opens its boot archives CWD-relative, so launching it from anywhere but the
	# packed dir finds no archives and dies on the zero-archives gate with no /FRISK line
	# to explain why. This is the assertion that catches that regression.
	assert_eq(String(spawned[0]["cwd"]),
			String(spawned[0]["path"]).get_base_dir(),
			"retail launches with the packed dir as its working directory")
	assert_eq(String(session.get_state()["mode"]), "retail")


func test_retail_packs_into_the_editors_own_data_dir() -> void:
	# The pack output is ours, so it goes in user://, never beside the mounted assets. The
	# sibling-dir derivation this replaces broke the distributed exe: it littered whatever
	# directory the assets sat in, and an existing unmarked sibling made the packer refuse.
	var packs: Array = []
	var session := _make_retail_session([], packs)

	assert_true(session.start_mode("retail"))
	var out_dir := String(packs[0]["out_dir"]).replace("\\", "/")
	assert_eq(out_dir, ProjectSettings.globalize_path("user://packed").replace("\\", "/"),
			"F7 packs into the editor's own data dir")
	assert_false(out_dir.begins_with("C:/assets") or out_dir == "C:/packed",
			"and never into or beside the mounted resource dir")


func test_retail_carries_no_opennova_or_debug_arguments() -> void:
	var spawned: Array = []
	var session := _make_retail_session(spawned, [])
	assert_true(session.start_mode("retail"))

	var args := spawned[0]["args"] as PackedStringArray
	for flag in ["--resource-dir", "--loose-root", "--loose-mission",
			"--oned-run-id", "--oned-run-descriptor", "--oned-run-log", "--log-file"]:
		assert_false(args.has(flag), "retail understands none of %s" % flag)
	var state: Dictionary = session.get_state()
	assert_eq(state["run_id"], "", "retail has no debug identity to stamp")
	assert_eq(state["descriptor_path"], "")


func test_retail_without_a_configured_install_is_unavailable_and_never_packs() -> void:
	var spawned: Array = []
	var packs: Array = []
	var session := _make_retail_session(spawned, packs, "")

	assert_false(session.retail_available(), "no install configured means not available")
	assert_false(session.start_mode("retail"))
	assert_eq(packs.size(), 0, "a rejected launch must not pack anything")
	assert_eq(spawned.size(), 0)
	assert_true(session.get_last_error().contains("retail install"),
			"the error names the missing setting: %s" % session.get_last_error())


func test_a_failed_pack_does_not_spawn() -> void:
	var spawned: Array = []
	var packs: Array = []
	var session := _make_retail_session(spawned, packs, "C:/retail", false)

	assert_false(session.start_mode("retail"))
	assert_eq(packs.size(), 1, "the pack was attempted")
	assert_eq(spawned.size(), 0, "but nothing launched")
	assert_eq(session.get_last_error(), "packing blew up",
			"the packer's own reason reaches the user")


func test_capability_poll_never_packs() -> void:
	# _build_request runs on every toolbar refresh to ask whether a mode is available.
	# Packing there would rebuild an archive on UI events.
	var packs: Array = []
	var session := _make_retail_session([], packs)
	for _i in range(5):
		session.retail_available()
		session.get_current_mission_unavailable_reason()
	assert_eq(packs.size(), 0, "polling availability writes nothing")


func test_sessions_without_retail_seams_cannot_start_retail() -> void:
	var spawned: Array = []
	var session := _make_session(spawned, [], {}, [], [100])
	assert_false(session.retail_available())
	assert_false(session.start_mode("retail"), "unconfigured sessions reject the mode")
	assert_eq(spawned.size(), 0)
