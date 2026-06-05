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
##   NOVA_RESOURCE_DIR=<asset-dir> \
##   Godot --path godot --resolution 1600x900 \
##         res://modtools/tools/screenshot_capture.tscn
##
## Assets are opened by name from one external resource directory (the same
## resource-root model the runtime and editor use), NOT from bundled fixtures.
## The directory is taken from $NOVA_RESOURCE_DIR, falling back to the editor's
## persisted resource dir. It must hold Dvxi5.trn (with its sibling textures),
## full_00.env, the font, credits, the strings table, and the object below.
##
## Must run with a real rendering window: --headless does not render, so the
## captured viewport texture would be blank.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
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

# Asset names, resolved case-insensitively against the resource dir at runtime.
# Dvxi5.trn / full_00.env are the runtime's hardcoded world files (NovaWorld).
const TERRAIN_NAME := "Dvxi5.trn"
const ENV_NAME := "full_00.env"
const FONT_NAME := "Serpen36"  # bare basename; the fonts workspace appends .fnt
const CREDITS_NAME := "nlist.kda"
const STRINGS_NAME := "GAMETEXT.bin"
# A textured object that frames well from the 3/4 vantage _frame_object() uses.
const OBJECT_NAME := "MH53.3di"
# The Particles workspace edits .ptl files, which the game ships inside PFFs
# rather than as loose files in the resource dir, so (unlike the other shots)
# this one opens a bundled repo fixture by its res:// path. buildup.ptl is a
# small, self-contained effect whose first particle emits a steady stream.
const PARTICLE_FIXTURE := "res://../fixtures/particle/buildup.ptl"

# [workspace_id, asset_name, out_filename]. The asset is resolved from the
# resource dir and opened before the workspace is activated (PARTICLE is the
# exception — its asset_name is a res:// fixture path opened directly).
var _shots: Array = [
	[EditorWorkstation.Workspace.TERRAIN, TERRAIN_NAME, "overview.png"],
	[EditorWorkstation.Workspace.OBJECT, OBJECT_NAME, "object.png"],
	[EditorWorkstation.Workspace.PARTICLE, PARTICLE_FIXTURE, "particles.png"],
	[EditorWorkstation.Workspace.FONTS, FONT_NAME, "fonts.png"],
	[EditorWorkstation.Workspace.CREDITS, CREDITS_NAME, "credits.png"],
	[EditorWorkstation.Workspace.STRINGS, STRINGS_NAME, "strings.png"],
]

var _editor: TerrainEditor
var _workstation: EditorWorkstation
var _root := ""
var _out_abs := ""


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)

	# Resolve the resource dir: $NOVA_RESOURCE_DIR, else the editor's persisted
	# dir. Everything (terrain, textures, foliage, env, font, credits, strings,
	# object) is opened by name from this one root.
	_root = OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if _root.is_empty():
		_root = ResourceDirSettings.get_resource_dir()
	if not ResourceDirSettings.is_valid_root(_root):
		push_error("[capture] no valid resource dir; set NOVA_RESOURCE_DIR")
		get_tree().quit(1)
		return
	# Persist before instancing so the workstation seeds VegAssets + scans the
	# root on its own _ready (the fonts/credits workspaces read it from there).
	ResourceDirSettings.set_resource_dir(_root)
	print("[capture] resource dir: ", _root)

	_editor = EditorScene.instantiate()
	add_child(_editor)
	# Let the editor's _ready run (window config, new_terrain, workstation bind).
	await get_tree().process_frame
	_force_window_size()
	_workstation = _editor.workstation
	# Re-apply the root explicitly: re-seeds VegAssets search roots and rescans,
	# so foliage .3di resolve from the dir regardless of _ready ordering.
	_workstation.set_resource_root_dir(_root)

	# Light the world: open an environment so the terrain shader gets sun/fog,
	# the sky dome renders, and the object preview (shared environment_editor)
	# is lit too. Without this the 3D views are flat grey on black.
	if _editor.environment_editor != null:
		var env_path := NovaPaths.resolve_file(_root, ENV_NAME)
		if env_path.is_empty():
			push_error("[capture] %s not found in %s" % [ENV_NAME, _root])
		else:
			var env_err: int = _editor.environment_editor.open_env(env_path)
			if env_err != OK:
				push_error("[capture] open_env failed (%d): %s" % [env_err, env_path])

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
		var asset_name: String = shot[1]
		var out_name: String = shot[2]
		var is_3d: bool = ws_id == EditorWorkstation.Workspace.TERRAIN \
			or ws_id == EditorWorkstation.Workspace.OBJECT \
			or ws_id == EditorWorkstation.Workspace.PARTICLE

		# Open the asset BEFORE activating the workspace: set_active_workspace()
		# rebuilds the workflow inspectors, so opening first means they're built
		# against already-loaded data (otherwise summaries render stale/empty).
		var ws: EditorWorkspace = _workstation._workspaces.get(ws_id)
		if ws == null:
			push_error("[capture] no workspace for %s" % out_name)
		elif ws_id == EditorWorkstation.Workspace.FONTS:
			# Fonts has a by-name opener that resolves <name>.fnt from the root.
			var ferr: int = int(ws.call("open_font_name", asset_name))
			if ferr != OK:
				push_error("[capture] open_font_name failed (%d): %s" % [ferr, asset_name])
		elif ws_id == EditorWorkstation.Workspace.PARTICLE:
			# .ptl assets ship inside PFFs, not as loose resource-dir files, so the
			# shot's asset_name is a bundled fixture's res:// path; open it by its
			# absolute path (the workspace resolves sibling textures from that dir).
			var ppath := ProjectSettings.globalize_path(asset_name)
			var perr: int = ws.open_file(ppath)
			if perr != OK:
				push_error("[capture] open_file failed (%d): %s" % [perr, ppath])
		else:
			# Resolve the asset case-insensitively against the resource dir and
			# hand the workspace a real OS path (open_file accepts absolute paths).
			var path := NovaPaths.resolve_file(_root, asset_name)
			if path.is_empty():
				push_error("[capture] %s not found in %s" % [asset_name, _root])
			else:
				var err: int = ws.open_file(path)
				if err != OK:
					push_error("[capture] open_file failed (%d): %s" % [err, path])

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
		elif ws_id == EditorWorkstation.Workspace.PARTICLE:
			_prime_particles(ws)
		elif ws_id == EditorWorkstation.Workspace.CREDITS:
			_balance_credits_split(ws)

		if ws_id == EditorWorkstation.Workspace.PARTICLE:
			# The restart in _prime_particles re-seeds the emit from t=0; give the
			# stream time to spawn and rise so the capture shows live particles over
			# the preview grid rather than an empty frame.
			for _i in 24:
				await get_tree().process_frame
		elif is_3d:
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
	# Hide the editor-only guides (reference grid + origin axes) for a clean hero
	# shot, via the workspace's first-class toggle interface (the same one the
	# editor's View settings drive). This doesn't persist the user's preference.
	ws.set_grid_visible(false)
	ws.set_axes_visible(false)
	var center := b.position + b.size * 0.5
	var radius := maxf(b.size.length() * 0.5, 1.0)
	cam.near = clampf(radius * 0.01, 0.02, 1.0)
	cam.far = maxf(radius * 30.0, 200.0)
	# Stand back ~2 radii on a 3/4 angle (front-right, slightly above) so the
	# whole structure fills the frame with a bit of headroom.
	var dir := Vector3(0.85, 0.45, 1.0).normalized()
	cam.position = center + dir * radius * 2.0
	cam.look_at(center)


# Replay the selected particle from t=0 so the capture lands during active
# emission. open_file pre-selects the first particle def; some defs are
# short-lived one-shots that would otherwise have expired by capture time after
# the 3D settle budget.
func _prime_particles(ws: EditorWorkspace) -> void:
	var preview = ws.get("_preview")
	if preview == null:
		return
	if preview.has_method("restart"):
		preview.restart()


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
