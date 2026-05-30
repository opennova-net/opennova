extends Node

## Headless-ish screenshot driver for the OpenNova Editor (ONED).
##
## Instances the editor scene, forces a deterministic window size, then for each
## target workspace: switches to it, opens a representative fixture, lets the
## frame settle, and writes a PNG of the composited window to <repo>/screenshots/.
## The README references those PNGs, so re-running this keeps the docs in sync
## with the editor.
##
## Run via scripts/capture_screenshots.ps1, or directly:
##   Godot --path godot --resolution 1600x900 \
##         res://modtools/tools/screenshot_capture.tscn
##
## Must run with a real rendering window: --headless does not render, so the
## captured viewport texture would be blank.

const EditorScene := preload("res://modtools/terrain/terrain_editor.tscn")

const WINDOW_SIZE := Vector2i(1600, 900)
# Generous settle budget: lets Control layout, the deferred split layout, and the
# UPDATE_ALWAYS sub-viewports redraw at their new size before we grab the frame.
const SETTLE_FRAMES := 16
# 3D previews (terrain, object) build their mesh and auto-frame the camera over a
# few deferred frames after load; give them longer so we don't grab a half-built,
# stale-bounds frame.
const SETTLE_FRAMES_3D := 48

# repo-root /screenshots (res:// maps to godot/, so one level up is the repo root).
const OUT_DIR := "res://../screenshots"
# Object fixtures live at repo-root fixtures/, outside the Godot project tree.
# US01 is a textured character whose textures sit alongside the .3dp, so the
# preview renders correctly (unlike the older GP-format box models).
const OBJECT_FIXTURE := "res://../fixtures/3dp/US01_onimport/US01.3dp"
# A daylight environment so the 3D previews are lit and the sky dome renders.
# Terrain and the object preview share this via the editor's environment_editor.
const ENV_FIXTURE := "res://game/assets/environments/Full_00/full_00.env"

# [workspace_id, fixture_path, out_filename]. Empty fixture = capture whatever the
# workspace already shows (terrain is loaded for the overview shot first).
var _shots: Array = [
	[EditorWorkstation.Workspace.TERRAIN, "res://game/assets/terrains/Dvxi5/Dvxi5.trn", "overview.png"],
	[EditorWorkstation.Workspace.OBJECT, OBJECT_FIXTURE, "object.png"],
	[EditorWorkstation.Workspace.FONTS, "res://assets/fonts/Serpen36.fnt", "fonts.png"],
	[EditorWorkstation.Workspace.CREDITS, "res://assets/credits/nlist.kda", "credits.png"],
	[EditorWorkstation.Workspace.STRINGS, "res://fixtures/strings/menu.bin", "strings.png"],
]

var _editor: TerrainEditor
var _workstation: EditorWorkstation
var _out_abs := ""


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)

	_editor = EditorScene.instantiate()
	add_child(_editor)
	# Let the editor's _ready run (window config, new_terrain, workstation bind).
	await get_tree().process_frame
	_force_window_size()
	_workstation = _editor.workstation

	# Light the world: open an environment so the terrain shader gets sun/fog,
	# the sky dome renders, and the object preview (shared environment_editor)
	# is lit too. Without this the 3D views are flat grey on black.
	if _editor.environment_editor != null:
		var env_err: int = _editor.environment_editor.open_env(ENV_FIXTURE)
		if env_err != OK:
			push_error("[capture] open_env failed (%d): %s" % [env_err, ENV_FIXTURE])

	await _run_all()

	print("[capture] done -> ", _out_abs)
	get_tree().quit()


func _force_window_size() -> void:
	var w := get_window()
	w.mode = Window.MODE_WINDOWED
	w.min_size = Vector2i.ZERO  # drop the editor's 1366x768 floor
	w.size = WINDOW_SIZE
	w.position = Vector2i(40, 40)


func _run_all() -> void:
	for shot in _shots:
		var ws_id: int = shot[0]
		var fixture: String = shot[1]
		var out_name: String = shot[2]
		var is_3d: bool = ws_id == EditorWorkstation.Workspace.TERRAIN \
			or ws_id == EditorWorkstation.Workspace.OBJECT

		# Load the fixture BEFORE activating the workspace: set_active_workspace()
		# rebuilds the workflow inspectors, so opening first means they're built
		# against already-loaded data (otherwise summaries render stale/empty).
		var ws: EditorWorkspace = _workstation._workspaces.get(ws_id)
		if not fixture.is_empty():
			# Fixtures under repo-root (res://../...) live outside the Godot
			# project, where res:// can't reach; hand the loader a real OS path.
			if fixture.begins_with("res://../"):
				fixture = ProjectSettings.globalize_path(fixture)
			if ws == null:
				push_error("[capture] no workspace for %s" % out_name)
			else:
				var err: int = ws.open_file(fixture)
				if err != OK:
					push_error("[capture] open_file failed (%d): %s" % [err, fixture])

		_workstation.set_active_workspace(ws_id)
		# One frame for the viewport mount + activate() to take effect.
		await get_tree().process_frame

		# Settle first (mesh build, the workspace's own deferred auto-frame, and
		# layout), then override the camera deterministically so the framing
		# never depends on a half-built model's stale bounds.
		var settle: int = SETTLE_FRAMES_3D if is_3d else SETTLE_FRAMES
		for _i in settle:
			await get_tree().process_frame

		if ws_id == EditorWorkstation.Workspace.TERRAIN:
			_frame_terrain()
		elif ws_id == EditorWorkstation.Workspace.OBJECT:
			_frame_object(ws)
		elif ws_id == EditorWorkstation.Workspace.CREDITS:
			_balance_credits_split(ws)

		if is_3d:
			# Let the new camera transform and far plane take effect.
			for _i in 6:
				await get_tree().process_frame

		await _capture(out_name)


# Park the fly camera inside the sky dome, looking across the terrain toward the
# foggy horizon so the dome fills the upper frame and the surface recedes below.
#
# Constraints from the terrain/sky geometry: the sky dome is a shallow cap that
# tops out ~175 units above ground and follows the camera in XZ, so the eye must
# sit below that to stay "inside" it. The fly camera ships with a 1500-unit far
# plane; we push it out so terrain renders to the fog line rather than getting
# clipped to a small disc.
func _frame_terrain() -> void:
	var b: AABB = _editor.terrain_mesh.get_world_bounds()
	if b.size == Vector3.ZERO:
		return
	var center := b.position + b.size * 0.5
	var cam: Camera3D = _editor.camera
	cam.far = 3000.0
	# A low 3/4 vantage aimed at the central hills: the eye stays under the dome
	# cap, and aiming at the terrain (not past it) keeps the surface filling the
	# foreground so we don't look down into the unlit skybox floor.
	cam.position = Vector3(center.x + 450.0, 150.0, center.z + 450.0)
	cam.look_at(Vector3(center.x, 90.0, center.z))
	# Hide the foliage preview: in this non-interactive path it scatters as
	# untextured white placeholder boxes, which just clutters the hero shot.
	if _editor._foliage_preview != null:
		_editor._foliage_preview.visible = false


# Frame the loaded object from a clean 3/4 vantage outside its bounds. The
# preview auto-frames on load, but for a large hollow model that can leave the
# eye inside the walls, so we set the camera ourselves from the model bounds.
func _frame_object(ws: EditorWorkspace) -> void:
	var preview = ws.get("_preview")
	if preview == null:
		return
	var cam: Camera3D = preview.get_editor_camera()
	var model = preview.get_object_model()
	if cam == null or model == null:
		return
	var b: AABB = model.get_model_bounds()
	if b.size == Vector3.ZERO:
		return
	# Prefer the highest-detail LOD for the hero shot.
	if preview.has_method("set_active_lod"):
		preview.set_active_lod(0)
	var center := b.position + b.size * 0.5
	var radius := maxf(b.size.length() * 0.5, 1.0)
	cam.near = clampf(radius * 0.01, 0.02, 1.0)
	cam.far = maxf(radius * 30.0, 200.0)
	# Stand back ~2 radii on a 3/4 angle (front-right, slightly above) so the
	# whole structure fills the frame with a bit of headroom.
	var dir := Vector3(0.85, 0.45, 1.0).normalized()
	cam.position = center + dir * radius * 2.0
	cam.look_at(center)


# The credits editor defaults to a very wide preview pane (split_offset -480),
# which leaves the entry list cramped and the preview mostly empty. Rebalance it
# toward the entry cards for a more legible screenshot.
func _balance_credits_split(ws: EditorWorkspace) -> void:
	var editor = ws.get("_editor")
	if editor == null:
		return
	var split := editor.get_node_or_null("HSplit") as HSplitContainer
	if split != null:
		split.split_offset = -40


func _capture(out_name: String) -> void:
	# Grab the frame after the GPU has finished drawing it.
	await RenderingServer.frame_post_draw
	var img := get_tree().root.get_texture().get_image()
	if img == null:
		push_error("[capture] null viewport image for %s" % out_name)
		return
	var path := _out_abs.path_join(out_name)
	var err := img.save_png(path)
	if err != OK:
		push_error("[capture] save_png failed (%d): %s" % [err, path])
	else:
		print("[capture] wrote ", path)
