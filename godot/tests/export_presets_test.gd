extends GutTest


func test_only_the_game_is_exported_and_editor_workflows_stay_source_only() -> void:
	var cfg := ConfigFile.new()
	assert_eq(cfg.load("res://export_presets.cfg"), OK)
	assert_eq(cfg.get_sections(), PackedStringArray(["preset.0", "preset.0.options"]))
	assert_eq(cfg.get_value("preset.0", "name"), "OpenNova Runtime")
	assert_eq(cfg.get_value("preset.0", "export_path"), "exports/windows/opennova.exe")
	assert_eq(cfg.get_value("preset.0", "include_filter"), "", "native game data stays outside the PCK")
	var excluded := String(cfg.get_value("preset.0", "exclude_filter")).split(",")
	for pattern in ["tools/*", "addons/opennova_world/*", "examples/*", "probes/*", "tests/*", "addons/gut/*", "game/mcp/*"]:
		assert_true(pattern in excluded, "%s stays in the source project" % pattern)
	assert_false("game/probe/*" in excluded, "the game ships the probe catalog")
	assert_true(cfg.get_value("preset.0.options", "imgui/debug"))
	assert_false(cfg.get_value("preset.0.options", "imgui/release"))
