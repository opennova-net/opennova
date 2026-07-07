extends SceneTree

# Mission-load timing probe — the capture protocol behind
# docs/perf/mission-load-baseline.md. NOT a GUT test (no _test suffix, needs
# retail assets): it instantiates the real editor main scene, mounts a retail
# loose-asset root, opens each mission through the exact interactive path, and
# dumps the PerfTimeline spans.
#
# Run (second invocation of a session is the recorded OS-warm number):
#   Godot_v4.6.1-stable_win64_console.exe --headless --path godot \
#       --script res://tests/mission_load_perf_probe.gd
# with JO_ASSETS_DIR pointing at a retail JO loose-asset directory.

const DEFAULT_MISSIONS := ["CP15.bms", "ASH_I1gA.bms", "03TR.bms"]


func _init() -> void:
	var asset_dir := OS.get_environment("JO_ASSETS_DIR")
	if asset_dir.is_empty():
		push_error("Set JO_ASSETS_DIR to a retail JO loose-asset directory.")
		quit(1)
		return
	var missions := DEFAULT_MISSIONS
	var missions_env := OS.get_environment("JO_PROBE_MISSIONS")
	if not missions_env.is_empty():
		missions = missions_env.split(",", false)

	var scene := load("res://modtools/editor/editor_main.tscn") as PackedScene
	var editor = scene.instantiate()
	root.add_child(editor)
	await process_frame
	var ws = editor if editor.has_method("open_in_workspace") else editor.find_child("EditorWorkstation", true, false)
	if ws == null:
		for child in editor.get_children():
			if child.has_method("open_in_workspace"):
				ws = child
				break
	if ws == null:
		push_error("Editor main scene exposes no open_in_workspace shell.")
		quit(1)
		return
	ws.set_resource_root_dir(asset_dir, false, true)
	for m in missions:
		var path: String = asset_dir + "/" + String(m)
		ws.open_in_workspace("mission", path)
		await process_frame
		var t: PerfTimeline = PerfTimeline.latest()
		if t == null:
			print("SPANS %s <no timeline>" % m)
			continue
		print("SPANS %s total=%.0fms" % [m, t.total_ms()])
		for s in t.spans():
			var span: Dictionary = s
			var ms := float(int(span["end_us"]) - int(span["start_us"])) / 1000.0
			print("SPANS   %s%s %.0fms" % ["  ".repeat(int(span["depth"])), span["name"], ms])
	quit()
