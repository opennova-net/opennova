extends SceneTree

# Re-ground timing probe — the capture protocol behind
# docs/perf/reground-baseline.md. NOT a GUT test (no _test suffix, needs retail
# assets): it opens a retail mission through the exact interactive path, raises
# the whole heightmap underneath (the bulk-drift worst case: every entity's
# ground moves), then drives the activate-time drift scan and the bulk
# re-ground and dumps both PerfTimelines.
#
# Run (second invocation of a session is the recorded OS-warm number):
#   Godot_v4.6.1-stable_win64_console.exe --headless --path godot \
#       --script res://tests/mission_reground_perf_probe.gd
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
	ws._set_resource_root_dir(asset_dir, false, true)
	for m in missions:
		var path: String = asset_dir + "/" + String(m)
		ws.open_in_workspace("mission", path)
		await process_frame
		var workspace = ws._workspaces.get(ws.Workspace.MISSION)
		var controller = workspace._controller if workspace != null else null
		if controller == null or not controller.is_loaded():
			print("REGROUND %s <mission did not load>" % m)
			continue
		var ted = controller.terrain_editor
		# Raise the entire editable surface 5 world units through the same seam
		# the height brushes commit through (bumps the height revision, so the
		# next activate-time reconcile sees the edit).
		var img: Image = ted.terrain_mesh.get_heightmap_image()
		var floats := img.get_data().to_float32_array()
		for i in floats.size():
			floats[i] += 5.0
		var raised := Image.create_from_data(img.get_width(), img.get_height(), false,
			Image.FORMAT_RF, floats.to_byte_array())
		ted._set_heightmap_image(raised)
		var drift: int = controller.reconcile_with_terrain()
		var scan: PerfTimeline = PerfTimeline.latest()
		var moved: int = controller.reground_drifted()
		var apply: PerfTimeline = PerfTimeline.latest()
		print("REGROUND %s drift=%d moved=%d" % [m, drift, moved])
		if scan != null:
			_dump(scan)
		if apply != null and apply != scan:
			_dump(apply)
	quit()


func _dump(t: PerfTimeline) -> void:
	print("SPANS %s total=%.0fms" % [t.label, t.total_ms()])
	for s in t.spans():
		var span: Dictionary = s
		var ms := float(int(span["end_us"]) - int(span["start_us"])) / 1000.0
		print("SPANS   %s%s %.0fms" % ["  ".repeat(int(span["depth"])), span["name"], ms])
