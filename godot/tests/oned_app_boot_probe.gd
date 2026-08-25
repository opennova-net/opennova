extends SceneTree

# Minimal ONED boot diagnostic.
# Run: "$GODOT_BIN" --headless --path godot -s res://tests/oned_app_boot_probe.gd


func _init() -> void:
	call_deferred("_run")


func _run() -> void:
	var scene: PackedScene = load("res://modtools/oned_main.tscn")
	var app := scene.instantiate() as OnedApp
	root.add_child(app)
	await process_frame

	print("PROBE ONED: ", "ok" if app != null else "MISSING")
	print("PROBE session: ",
			"ok" if app != null and app.get_run_session() != null else "MISSING")
	print("PROBE resource setting: ",
			"ok" if app != null and app.get_node_or_null("%ResourceDirEdit") != null else "MISSING")
	print("PROBE OpenNova action: ",
			"ok" if app != null and app.get_node_or_null("%RunOpenNovaButton") != null else "MISSING")
	print("PROBE retail action: ",
			"ok" if app != null and app.get_node_or_null("%RunRetailButton") != null else "MISSING")
	print("PROBE stop action: ",
			"ok" if app != null and app.get_node_or_null("%StopButton") != null else "MISSING")

	app.queue_free()
	await process_frame
	quit(0)
