extends SceneTree

# Boot diagnostic for the EditorApp root swap: instances editor_main.tscn,
# waits for _ready to settle, and prints exactly which boot steps completed.
# Run: "$GODOT_BIN" --headless --path godot -s res://tests/editor_app_boot_probe.gd


func _init() -> void:
	call_deferred("_run")


func _run() -> void:
	var scene: PackedScene = load("res://modtools/editor/editor_main.tscn")
	var app = scene.instantiate()
	root.add_child(app)
	await process_frame
	await process_frame

	print("PROBE app class: ", app.get_script().get_global_name())
	var terrain = app.get_terrain_editor()
	print("PROBE terrain_editor: ", "ok" if terrain != null else "MISSING")
	print("PROBE terrain.workstation set: ", terrain != null and terrain.workstation != null)
	print("PROBE environment_editor injected: ", app.environment_editor != null
			and terrain != null and terrain.environment_editor == app.environment_editor)
	print("PROBE mcp_service (post _init_mcp_service): ", "ok" if app.mcp_service != null else "NULL")
	if app.mcp_service != null:
		print("PROBE mcp status: ", app.mcp_service.get_status_text())
		var err: int = app.mcp_service.start(18975)
		print("PROBE mcp manual start(18975): err=", err, " running=", app.mcp_service.is_running())
		app.mcp_service.stop()
	var ws = app.workstation
	print("PROBE shell editor bound: ", ws != null and ws.editor == app)
	print("PROBE terrain seeded: ", terrain != null and not String(terrain.get_terrain_name_value()).is_empty())
	app.queue_free()
	await process_frame
	quit(0)
