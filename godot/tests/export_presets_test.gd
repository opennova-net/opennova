extends GutTest


func test_export_presets_do_not_bundle_resource_root_data() -> void:
	var text := FileAccess.get_file_as_string("res://export_presets.cfg")
	# One empty include_filter per preset: Runtime, Windows only
	# (macOS delivery removed 2026-08-11). Neither should bundle original
	# resource-root data.
	assert_eq(text.count("include_filter=\"\""), 1,
		"Runtime exports should not bundle original resource-root data.")
	assert_false(text.contains("*.kda") or text.contains("*.fnt"),
		"KDA and FNT files are loaded from the configured resource root, not exported in the app PCK.")


# Probes (godot/probes) and the GUT suite are source-only: a shipped build
# lists the probe catalog but reports every probe unavailable (ADR 0041). The
# MCP transport (godot/game/mcp) is a debug runtime capability (ADR 0043
# d12): the Runtime preset excludes it,
# the shipped probe model (godot/game/probe) stays in the runtime.
func test_export_presets_exclude_probes_and_tests() -> void:
	var cfg := ConfigFile.new()
	assert_eq(cfg.load("res://export_presets.cfg"), OK)
	var presets := 0
	var runtime_presets := 0
	for section in cfg.get_sections():
		if not section.begins_with("preset.") or section.ends_with(".options"):
			continue
		presets += 1
		var excluded := String(cfg.get_value(section, "exclude_filter", "")).split(",")
		for pattern in ["probes/*", "tests/*", "addons/gut/*"]:
			assert_true(pattern in excluded,
				"%s excludes %s (source-only; never in the PCK)" % [section, pattern])
		var runtime_preset := String(cfg.get_value(section, "name", "")) == "OpenNova Runtime"
		if runtime_preset:
			runtime_presets += 1
		assert_eq("game/mcp/*" in excluded, runtime_preset,
			"%s: the MCP transport leaves the Runtime export only" % section)
		assert_false("game/probe/*" in excluded,
			"%s ships the probe model (the catalog lists every probe)" % section)
	assert_eq(presets, 1, "only the Runtime preset ships")
	assert_eq(runtime_presets, 1, "one Runtime preset carries the exclusion")
