extends GutTest

## The OpenNova Editor's shell (ADR 0046 d4/d10) booted headless from its scene: the
## editor-enabled GDExtension variant is what a source run loads, the typed seam
## creates a project, fills its checklist, builds it, and Play starts the game on the
## build (the Godot binary at this checkout, headless and self-quitting) through the
## real process seam, whose exit the session notices.

const EDITOR_SCENE := "res://editor/editor_root.tscn"

var _dirs: Array[String] = []
var _app: Node = null


func before_each() -> void:
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	assert_true(_app.has_method("get_loaded_variant"), "the root is an EditorApp")
	var settings_dir := OS.get_cache_dir().path_join("opennova editor app %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


func after_each() -> void:
	if is_instance_valid(_app):
		_app.stop_play()
		var deadline := Time.get_ticks_msec() + 10000
		while _app.get_play_state() != "stopped" and Time.get_ticks_msec() < deadline:
			await get_tree().create_timer(0.05).timeout
			_app.pump()
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()


func test_editor_variant_boots_headless() -> void:
	if _app == null:
		return
	assert_eq(_app.get_loaded_variant(), "editor")
	assert_false(_app.is_available(), "headless: no ImGui context, the seam still works")
	assert_false(_app.is_project_open())
	assert_true(_app.is_source_run(), "a GUT run is a source run: Play drives this Godot binary")
	assert_eq(_app.get_recent_projects(), PackedStringArray())


func test_new_project_fills_builds_and_plays() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	var root := dir.path_join("My Game")
	assert_true(_app.new_project(root, "My Game"))
	assert_true(_app.is_project_open())
	assert_eq(_app.get_project_title(), "My Game")
	assert_eq(_app.get_project_root(), root)
	assert_gt(_app.get_required_total(), 0)
	assert_eq(_app.get_required_missing(), _app.get_required_total(), "a new project has every required file missing")
	assert_eq(_app.get_recent_projects(), PackedStringArray([root]))

	assert_false(_app.build(), "a build is refused while required files are missing")
	assert_eq(_app.create_missing_files(), 0, "Create all missing leaves nothing missing")
	assert_true(_app.build())
	var build_dir: String = _app.get_last_build_dir()
	assert_true(FileAccess.file_exists(build_dir.path_join("localres.pff")), build_dir)
	assert_eq(_app.get_problem_count(), 0)

	# Play: the runtime is this Godot binary at the source project, headless and
	# self-quitting, so the real process seam spawns it and sees it leave.
	_app.set("play_engine_args", PackedStringArray(["--headless", "--disable-render-loop", "--quit-after", "10"]))
	assert_true(_app.play(), "\n".join(_app.get_output_lines()))
	assert_eq(_app.get_play_state(), "running")
	var waited_ms := 0
	while _app.get_play_state() != "stopped" and waited_ms < 60000:
		OS.delay_msec(100)
		waited_ms += 100
		_app.pump()
	assert_eq(_app.get_play_state(), "stopped", "\n".join(_app.get_output_lines()))
	assert_true(_app.did_game_exit_on_its_own(), "the child quit by itself (--quit-after)")
	var output := "\n".join(_app.get_output_lines())
	assert_string_contains(output, "Running: ")
	assert_string_contains(output, "The game exited.")

	_app.close_project()
	assert_false(_app.is_project_open())
	assert_true(_app.open_project(root))
	assert_eq(_app.get_required_missing(), 0)


func test_catalog_edits_reach_the_play_child() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova catalog play %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_app.new_project(dir, "Catalog acceptance"))
	assert_eq(_app.create_missing_files(), 0)
	assert_true(_app.create_catalog("ammo.def"))
	var ammo_id: int = _app.add_catalog_record("ammo")
	assert_gt(ammo_id, 0)
	assert_true(_app.set_catalog_text(ammo_id, "name", "AMMO_CATALOG_TEST"))
	assert_true(_app.set_catalog_integer(ammo_id, "velocity", 1250))
	assert_true(_app.open_catalog("weapon.def"))
	var weapon_id: int = _app.add_catalog_record("weapon")
	assert_true(_app.set_catalog_text(weapon_id, "weapon_name", "WPN_CATALOG_TEST"))
	assert_true(_app.set_catalog_text(weapon_id, "round_type", "AMMO_CATALOG_TEST"))
	assert_true(_app.set_catalog_integer(weapon_id, "clipsize", 37))
	assert_true(_app.set_catalog_real(weapon_id, "weaponweight", 2.5))
	assert_eq(_app.get_catalog_integer(weapon_id, "weaponweight_fp16"), 163840)
	var action_id: int = _app.add_catalog_record("action", weapon_id)
	assert_true(_app.set_catalog_text(action_id, "name", "Fire"))
	assert_true(_app.set_catalog_text(action_id, "function", "Shoot"))
	assert_true(_app.set_catalog_integer(action_id, "ctrl_increment", 2))
	_app.catalog_undo()
	assert_eq(_app.get_catalog_integer(action_id, "ctrl_increment"), 0)
	_app.catalog_redo()
	assert_eq(_app.get_catalog_integer(action_id, "ctrl_increment"), 2)
	assert_true(_app.open_catalog("items.def"))
	var row: int = _app.add_catalog_record("item")
	var item_id: int = _app.get_catalog_integer(row, "id")
	assert_true(_app.set_catalog_text(row, "display_name", "Catalog marker"))
	assert_true(_app.is_catalog_dirty())
	assert_false(_app.build(), "unsaved catalog edits block Build")
	assert_true(_app.save_catalogs(), "\n".join(_app.get_output_lines()))
	assert_false(_app.is_catalog_dirty())
	assert_true(_app.build(), "\n".join(_app.get_output_lines()))
	var built_root: String = _app.get_last_build_dir()
	_app.set("play_engine_args", PackedStringArray(["--headless", "--disable-render-loop", "--max-fps", "60"]))
	assert_true(_app.play(), "\n".join(_app.get_output_lines()))
	var client: RefCounted = null
	var deadline := Time.get_ticks_msec() + 30000
	while Time.get_ticks_msec() < deadline:
		client = preload("res://tests/mcp/mcp_test_client.gd").new()
		if await client.connect_to(get_tree(), _app.get_play_mcp_port()):
			break
		client.close()
		client = null
		await get_tree().create_timer(0.1).timeout
	assert_not_null(client, "\n".join(_app.get_output_lines()))
	if client != null:
		var initialized: Variant = await client.initialize(get_tree())
		assert_not_null(initialized)
		var envelope: Variant = await client.call_tool(get_tree(), "game_probe", {
			"op": "run", "name": "catalog_readback", "args": {
				"item_id": item_id, "item_name": "Catalog marker",
				"weapon_name": "WPN_CATALOG_TEST", "weapon_clipsize": 37,
				"ammo_name": "AMMO_CATALOG_TEST", "ammo_velocity": 1250,
			},
		})
		assert_not_null(envelope)
		var started: Dictionary = envelope.get("result", {}).get("structuredContent", {}) if envelope is Dictionary else {}
		assert_true(started.has("run_id"), str(envelope))
		if started.has("run_id"):
			var verdict: Variant = null
			deadline = Time.get_ticks_msec() + 30000
			while verdict == null and Time.get_ticks_msec() < deadline:
				envelope = await client.call_tool(get_tree(), "game_probe", {
					"op": "status", "run_id": started["run_id"], "wait_ms": 500,
				})
				if envelope is Dictionary:
					verdict = envelope.get("result", {}).get("structuredContent", {}).get("verdict")
			assert_not_null(verdict, str(envelope))
			if verdict is Dictionary:
				assert_true(bool(verdict.get("ok", false)), str(verdict))
				assert_eq(str(verdict.get("data", {}).get("root", "")).replace("\\", "/").trim_suffix("/"), built_root.trim_suffix("/"))
		client.close()
	_app.stop_play()
	deadline = Time.get_ticks_msec() + 10000
	while _app.get_play_state() != "stopped" and Time.get_ticks_msec() < deadline:
		await get_tree().create_timer(0.05).timeout
		_app.pump()
	assert_eq(_app.get_play_state(), "stopped")
	_app.close_project()
	assert_false(_app.is_project_open())
	assert_true(_app.open_project(dir))
	assert_true(_app.open_catalog("items.def"))
	assert_eq(_app.get_catalog_row_name(1), "Catalog marker")
	var reloaded_row: int = _app.get_catalog_row_id(1)
	assert_true(_app.set_catalog_integer(reloaded_row, "hp", 50))
	_app.close_project()
	assert_true(_app.has_unsaved_prompt())
	_app.resolve_unsaved(2) # Cancel
	assert_true(_app.is_project_open())
	_app.close_project()
	_app.resolve_unsaved(1) # Discard
	assert_false(_app.is_project_open())
