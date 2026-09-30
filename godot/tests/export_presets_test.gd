extends GutTest


func test_export_presets_do_not_bundle_resource_root_data() -> void:
	var text := FileAccess.get_file_as_string("res://export_presets.cfg")
	# One empty include_filter per preset: the Windows Runtime (macOS delivery
	# removed 2026-08-11) and the Web build (ADR 0049). Neither should bundle
	# original resource-root data.
	assert_eq(text.count("include_filter=\"\""), 2,
		"Runtime exports should not bundle original resource-root data.")
	assert_false(text.contains("*.kda") or text.contains("*.fnt"),
		"KDA and FNT files are loaded from the configured resource root, not exported in the app PCK.")


# Probes (godot/probes) and the GUT suite are source-only: a shipped build
# lists the probe catalog but reports every probe unavailable (ADR 0041). The
# MCP transport (godot/game/mcp) is a debug runtime capability (ADR 0043
# d12): every shipped preset excludes it, and the shipped probe model
# (godot/game/probe) stays in the runtime.
func test_export_presets_exclude_probes_and_tests() -> void:
	var cfg := ConfigFile.new()
	assert_eq(cfg.load("res://export_presets.cfg"), OK)
	var names: Array[String] = []
	for section in cfg.get_sections():
		if not section.begins_with("preset.") or section.ends_with(".options"):
			continue
		names.append(String(cfg.get_value(section, "name", "")))
		var excluded := String(cfg.get_value(section, "exclude_filter", "")).split(",")
		for pattern in ["probes/*", "tests/*", "addons/gut/*"]:
			assert_true(pattern in excluded,
				"%s excludes %s (source-only; never in the PCK)" % [section, pattern])
		assert_true("game/mcp/*" in excluded,
			"%s: the MCP transport never ships" % section)
		assert_false("game/probe/*" in excluded,
			"%s ships the probe model (the catalog lists every probe)" % section)
	names.sort()
	assert_eq(names, ["OpenNova Runtime", "OpenNova Web"] as Array[String],
		"the Windows Runtime and the Web build are the shipped presets")


# The Web build (ADR 0049) is the dlink threads template the wasm GDExtension
# loads into, with the page shell that stages assets/ and serves PLAY RETAIL.
# The imgui addon ships no web binaries and the shell is a template, not a
# resource, so neither enters the PCK.
func test_web_preset_loads_the_extension_through_the_page_shell() -> void:
	var cfg := ConfigFile.new()
	assert_eq(cfg.load("res://export_presets.cfg"), OK)
	var web := ""
	for section in cfg.get_sections():
		if section.begins_with("preset.") and not section.ends_with(".options") \
				and cfg.get_value(section, "name", "") == "OpenNova Web":
			web = section
	assert_ne(web, "", "the Web preset exists")
	if web.is_empty():
		return
	var options := web + ".options"
	assert_eq(cfg.get_value(web, "platform"), "Web")
	assert_true(cfg.get_value(options, "variant/extensions_support"), "dlink template")
	assert_true(cfg.get_value(options, "variant/thread_support"), "threads template")
	assert_eq(cfg.get_value(options, "html/custom_html_shell"), "res://web/shell.html")
	assert_true(FileAccess.file_exists("res://web/shell.html"), "the shell is in the project")
	var excluded := String(cfg.get_value(web, "exclude_filter", "")).split(",")
	for pattern in ["addons/imgui-godot/*", "web/*"]:
		assert_true(pattern in excluded, "the Web preset excludes %s" % pattern)
