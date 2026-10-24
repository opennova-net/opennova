extends GutTest


func test_export_presets_do_not_bundle_resource_root_data() -> void:
	var text := FileAccess.get_file_as_string("res://export_presets.cfg")
	# Runtime, Play Runtime and Editor on Windows and Linux. Original
	# resource-root data is never embedded in any product's PCK.
	assert_eq(
		text.count('include_filter=""'), 6, "Exports should not bundle original resource-root data."
	)
	assert_false(text.contains("*.kda") or text.contains("*.fnt"),
		"KDA and FNT files are loaded from the configured resource root, not exported in the app PCK.")


# Probes (godot/probes) and the GUT suite are source-only: a shipped build
# lists the probe catalog but reports every probe unavailable (ADR 0041). The
# MCP transport (godot/game/mcp) is a debug runtime capability (ADR 0043
# d12): the game's own export (the Runtime) excludes it, the Play Runtime (the
# editor's Play child, ADR 0046 d1) keeps it so the editor can drive the running
# game; the shipped probe model (godot/game/probe) stays in every product. The
# game products never carry the editor scene (ADR 0046 d1); the Editor keeps
# game/* for its previews and MCP and carries the ImGui addon in release too.
func test_export_presets_exclude_probes_and_tests() -> void:
	var cfg := ConfigFile.new()
	assert_eq(cfg.load("res://export_presets.cfg"), OK)
	var names: Array[String] = []
	for section in cfg.get_sections():
		if not section.begins_with("preset.") or section.ends_with(".options"):
			continue
		var name := String(cfg.get_value(section, "name", ""))
		names.append(name)
		var excluded := String(cfg.get_value(section, "exclude_filter", "")).split(",")
		for pattern in ["probes/*", "tests/*", "addons/gut/*"]:
			assert_true(pattern in excluded,
				"%s excludes %s (source-only; never in the PCK)" % [section, pattern])
		assert_eq(
			"game/mcp/*" in excluded,
			name.begins_with("OpenNova Runtime"),
			"%s: the MCP transport leaves the game's own export only" % section
		)
		assert_false("game/probe/*" in excluded,
			"%s ships the probe model (the catalog lists every probe)" % section)
		var is_editor := name.begins_with("OpenNova Editor")
		assert_eq("editor/*" in excluded, not is_editor,
			"%s: the editor scene ships in the Editor only" % section)
		assert_eq(String(cfg.get_value(section, "custom_features", "")),
			"opennova_editor" if is_editor else "opennova_runtime",
			"%s tags its product, which picks its GDExtension variant (ADR 0046 d2)" % section)
		assert_eq(bool(cfg.get_value(section + ".options", "imgui/release", false)), is_editor,
			"%s: only the Editor ships the ImGui addon in a release export" % section)
		assert_true(bool(cfg.get_value(section + ".options", "imgui/debug", false)),
			"%s: development exports keep the ImGui tools" % section)
		assert_true(bool(cfg.get_value(section + ".options", "binary_format/embed_pck", false)),
			"%s: the package ships an executable with its embedded PCK" % section)
		assert_eq(String(cfg.get_value(section + ".options", "binary_format/architecture", "")), "x86_64")
		var linux := name.ends_with(" Linux")
		assert_eq(String(cfg.get_value(section, "platform", "")), "Linux" if linux else "Windows Desktop")
		var output := String(cfg.get_value(section, "export_path", ""))
		assert_true(output.begins_with("exports/linux/" if linux else "exports/windows/"))
		assert_eq(output.ends_with(".exe"), not linux, "Linux runtime names have no Windows extension")
	assert_eq(
		names,
		["OpenNova Runtime", "OpenNova Play Runtime", "OpenNova Editor",
			"OpenNova Runtime Linux", "OpenNova Play Runtime Linux", "OpenNova Editor Linux"],
		"the game, the editor's Play child and the editor ship on both platforms"
	)
