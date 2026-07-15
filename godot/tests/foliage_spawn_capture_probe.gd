extends Node

## Deterministic retail-comparison capture at the real local-player spawn.
## This uses ONED's real Play Mission path and never searches foliage-painted
## cells, teleports the camera, or synthesizes input.
##
## NOVA_RESOURCE_DIR=<asset-dir> NOVA_MISSION_BMS=00TRe.bms \
##   "$GODOT_BIN" --path godot res://tests/foliage_spawn_capture_probe.tscn
## Optional output override: NOVA_SPAWN_CAPTURE_DIR=<absolute-or-res://-path>

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const EditorScene := preload("res://modtools/editor/editor_main.tscn")

const DEFAULT_MISSION := "00TRe.bms"
const DEFAULT_OUT_DIR := "res://../.scratch/00tre-spawn"
const EDITOR_BOOT_FRAMES := 9
const MISSION_OPEN_FRAMES := 30
const PLAY_SETTLE_FRAMES := 132
const VISIBILITY_SETTLE_FRAMES := 3
const CAPTURE_VIEWPORT_SIZE := Vector2i(1600, 900)
const FLICKER_CAPTURE_COUNT := 6
const FLICKER_MASK_DELTA := 6
const FLICKER_FADE_STEP := 0.25 / 22.0

var _mission_workspace
var _play_controller
var _input_router: Node
var _out_abs := ""
var _capture_stem := "00TRe"


func _ready() -> void:
	var resource_dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if resource_dir.is_empty():
		resource_dir = ResourceDirSettings.get_resource_dir()
	if not ResourceDirSettings.is_valid_root(resource_dir):
		_fail("no valid resource dir; set NOVA_RESOURCE_DIR")
		return
	var configured_out := OS.get_environment("NOVA_SPAWN_CAPTURE_DIR").strip_edges()
	_out_abs = ProjectSettings.globalize_path(
		DEFAULT_OUT_DIR if configured_out.is_empty() else configured_out)
	var mkdir_err := DirAccess.make_dir_recursive_absolute(_out_abs)
	if mkdir_err != OK:
		_fail("cannot create output directory (%d): %s" % [mkdir_err, _out_abs])
		return

	var mission_name := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if mission_name.is_empty():
		mission_name = DEFAULT_MISSION
	_capture_stem = mission_name.get_file().get_basename()
	var mission_path := NovaPaths.resolve_file(resource_dir, mission_name)
	if mission_path.is_empty():
		_fail("%s not found in %s" % [mission_name, resource_dir])
		return
	print("[spawn-capture] resource dir: ", resource_dir)
	print("[spawn-capture] mission: ", mission_name, " -> ", mission_path)
	print("[spawn-capture] input: disabled before first played frame; none synthesized")

	var app = EditorScene.instantiate()
	add_child(app)
	await _settle(EDITOR_BOOT_FRAMES)
	var workstation = app.workstation
	if workstation == null:
		_fail("ONED workstation unavailable")
		return
	workstation.set_resource_root_dir(resource_dir, false)
	_mission_workspace = workstation.get_workspace_adapter(EditorWorkstation.Workspace.MISSION)
	if _mission_workspace == null or not _mission_workspace.has_method("play_mission"):
		_fail("mission workspace unavailable")
		return
	var open_err := int(_mission_workspace.open_file(mission_path))
	if open_err != OK:
		_fail("mission open failed (%d): %s" % [open_err, mission_path])
		return
	workstation.set_active_workspace(EditorWorkstation.Workspace.MISSION)
	await _settle(MISSION_OPEN_FRAMES)
	var play_err := int(_mission_workspace.play_mission())
	if play_err != OK:
		_fail("play_mission failed (%d): %s" % [play_err, mission_name])
		return

	# Bind only through the workspace's public live-play seam. Input is disabled
	# immediately, before the first played frame can move or rotate the player.
	_play_controller = _mission_workspace.play_controller()
	if _play_controller == null or not _play_controller.is_playing():
		_fail("live play controller unavailable")
		return
	var world = _play_controller.get_world()
	var camera: Camera3D = _play_controller.get_play_camera()
	if world == null or not world.is_loaded() or not world.has_local_player():
		_fail("GameWorld has no loaded local-player spawn anchor")
		return
	if camera == null or not camera.is_inside_tree():
		_fail("play Camera3D unavailable")
		return
	var play_viewport := camera.get_viewport() as SubViewport
	var play_container := _play_controller.get_node_or_null(
		"PlayViewportContainer") as SubViewportContainer
	if play_viewport == null or play_container == null:
		_fail("clean play SubViewport host unavailable")
		return
	# The editor workspace is intentionally dock-shaped, but the retail oracle is
	# a 16:9 game viewport. Detach the clean play target from the dock's stretch
	# before settling so camera FOV, terrain coverage, and both A/B images use the
	# same 1600x900 aspect as the standalone game.
	play_container.stretch = false
	play_viewport.size = CAPTURE_VIEWPORT_SIZE
	_play_controller.set_capture_suspended(true)
	_play_controller.set_process_input(false)
	_play_controller.set_process_unhandled_input(false)
	_input_router = _play_controller.find_child("PlayInputRouter", true, false)
	if _input_router != null:
		_input_router.set_process_unhandled_input(false)
	world.set_local_player_input(false, false, false, false, false, false, false)
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	await _settle(PLAY_SETTLE_FRAMES)

	var environment = world.get_node_or_null("NovaEnvironment")
	if environment == null or environment.get("time_of_day") == null:
		_fail("played mission environment/TOD unavailable")
		return
	if play_viewport == null or play_viewport == get_viewport():
		_fail("camera is not in the clean play SubViewport")
		return

	# Freeze the real game-loop host. Rendering stays live, while the player,
	# camera, mission clock, and foliage dispatch stay bit-identical for the A/B.
	_play_controller.set_process(false)
	var dispatcher = world.get_node_or_null("NovaTerrain/FoliageDispatcher")
	var foliage_error := runtime_foliage_validation_error(
		mission_name,
		dispatcher.get_frame_stats() if dispatcher != null else {},
		dispatcher.get_total_instances() if dispatcher != null else 0)
	if not foliage_error.is_empty():
		_fail(foliage_error)
		return
	var spawn_state := _snapshot(world, camera, environment, play_viewport)
	if not _valid_snapshot(spawn_state):
		_fail("spawn state failed validation")
		return
	_print_snapshot(spawn_state)
	_print_runtime_metadata(world, environment)
	_print_foliage_material_state(world)
	if OS.get_environment("NOVA_FOLIAGE_FLICKER_PROBE") == "1":
		await _run_foliage_flicker_probe(world, dispatcher, camera, play_viewport, spawn_state)
		return

	world.set_foliage_hidden(false)
	await _settle(VISIBILITY_SETTLE_FRAMES)
	if world.is_foliage_hidden():
		_fail("GameWorld refused foliage-visible state")
		return
	var default_path := _out_abs.path_join(_capture_stem + "_spawn_default.png")
	if await _capture(play_viewport, default_path) != OK:
		_fail("default capture failed")
		return
	if not _same_snapshot(spawn_state, _snapshot(world, camera, environment, play_viewport)):
		_fail("spawn state changed during default capture")
		return

	# This is the public API used by the game and ONED debug overlay.
	world.set_foliage_hidden(true)
	await _settle(VISIBILITY_SETTLE_FRAMES)
	if not world.is_foliage_hidden():
		_fail("GameWorld refused foliage-hidden state")
		return
	var hidden_path := _out_abs.path_join(_capture_stem + "_spawn_foliage_hidden.png")
	if await _capture(play_viewport, hidden_path) != OK:
		_fail("foliage-hidden capture failed")
		return
	if not _same_snapshot(spawn_state, _snapshot(world, camera, environment, play_viewport)):
		_fail("spawn state changed between A/B captures")
		return
	world.set_foliage_hidden(false)

	print("[spawn-capture] wrote: ", default_path)
	print("[spawn-capture] wrote: ", hidden_path)
	print("[spawn-capture] PASS: exact frozen player-spawn state in both images")
	_shutdown(0)


static func runtime_foliage_validation_error(
		mission_name: String, frame_stats: Dictionary, total_instances: int) -> String:
	if mission_name.get_file().get_basename().to_lower() != "00tre":
		return ""
	if int(frame_stats.get("runtime_detail_intents", 0)) <= 0:
		return "00TRe exact spawn produced no runtime detail foliage intents"
	var detail_instances := int(frame_stats.get("detail_high_instances", 0)) \
		+ int(frame_stats.get("detail_low_instances", 0))
	if detail_instances <= 0:
		return "00TRe exact spawn produced no hosted detail foliage instances"
	if total_instances <= 0:
		return "00TRe exact spawn produced no dispatcher foliage instances"
	return ""


func _run_foliage_flicker_probe(
		world, dispatcher: NovaFoliageDispatcher, camera: Camera3D,
		viewport: Viewport, spawn_state: Dictionary) -> void:
	# Freeze every scene update and drive only the real foliage dispatcher. The
	# renderer stays live, so any remaining pixel changes come from foliage draws.
	world.process_mode = Node.PROCESS_MODE_DISABLED
	var viewmodel_pass: Node = _play_controller.find_child("ViewmodelPass", true, false)
	if viewmodel_pass is CanvasItem:
		viewmodel_pass.visible = false
	var base_transform := camera.global_transform

	world.set_foliage_hidden(true)
	await _settle(2)
	var hidden: Image = await _grab_flicker_image(viewport)
	if hidden == null or hidden.is_empty():
		_fail("flicker probe could not capture the foliage-hidden baseline")
		return
	hidden.save_png(_out_abs.path_join(_capture_stem + "_flicker_hidden.png"))
	world.set_foliage_hidden(false)

	var failures: Array[String] = []
	var skipped: Array[String] = []
	for tier in ["detail_high", "detail_low_far", "detail_auto"]:
		var result: Dictionary = await _probe_flicker_tier(
			dispatcher, camera, viewport, hidden, base_transform, tier)
		if not bool(result.get("ok", false)):
			failures.append("%s: %s" % [tier, result.get("failure", "unstable")])
		elif result.has("skipped"):
			skipped.append("%s (%s)" % [tier, result.skipped])
	camera.set_global_transform(base_transform)
	if not _same_snapshot(spawn_state, _snapshot(
			world, camera, world.get_node_or_null("NovaEnvironment"), viewport)):
		failures.append("exact spawn state changed during the flicker probe")
	if not failures.is_empty():
		_fail("; ".join(failures))
		return
	print("[spawn-flicker] PASS: exercised exact 00TRe spawn tiers were frame-stable; ",
		"skipped=", skipped)
	_shutdown(0)


func _probe_flicker_tier(
		dispatcher: NovaFoliageDispatcher, camera: Camera3D, viewport: Viewport,
		hidden: Image, base_transform: Transform3D, tier: String) -> Dictionary:
	camera.set_global_transform(base_transform)
	for _warmup in 2:
		dispatcher.render_frame(base_transform)
		_configure_flicker_draws(dispatcher, tier, false, 0.0)
		await _settle(1)

	var images: Array[Image] = []
	var setup := {}
	for frame_index in range(FLICKER_CAPTURE_COUNT):
		dispatcher.render_frame(base_transform)
		setup = _configure_flicker_draws(dispatcher, tier, false, 0.0)
		var image: Image = await _grab_flicker_image(viewport)
		if image == null or image.is_empty():
			return {"ok": false, "failure": "capture %d was empty" % frame_index}
		images.append(image)
		image.save_png(_out_abs.path_join(
			"%s_flicker_%s_%02d.png" % [_capture_stem, tier, frame_index]))

	if int(setup.get("kept", 0)) <= 0:
		return {"ok": false, "failure": "no visible draws matched this tier"}

	var worst_xor := 0
	var worst_changed := 0
	var worst_delta := 0
	var frame_diffs: Array[Dictionary] = []
	for index in range(1, images.size()):
		var diff := _masked_flicker_diff(hidden, images[0], images[index])
		frame_diffs.append(diff)
		worst_xor = maxi(worst_xor, int(diff.coverage_xor_pixels))
		worst_changed = maxi(worst_changed, int(diff.changed_rgb_pixels))
		worst_delta = maxi(worst_delta, int(diff.max_channel_delta))

	if frame_diffs.is_empty() or int(frame_diffs[0].coverage_union_pixels) <= 0:
		print("[spawn-flicker] tier=", tier, " setup=", setup,
			" skipped: active draws have no coverage in the exact spawn view")
		return {"ok": true, "skipped": "outside exact spawn view"}

	# Positive control: dropping the isolated tier must make the same metric red.
	dispatcher.render_frame(base_transform)
	_configure_flicker_draws(dispatcher, tier, true, 0.0)
	var dropout: Image = await _grab_flicker_image(viewport)
	var control := _masked_flicker_diff(hidden, images[0], dropout)
	var control_detects := int(control.coverage_xor_pixels) > 0

	# Hold geometry/camera fixed and nudge only c6.a by the amount produced by
	# 0.25 world units in retail's 20..42 fade band.
	dispatcher.render_frame(base_transform)
	_configure_flicker_draws(dispatcher, tier, false, 0.0)
	var fade_a: Image = await _grab_flicker_image(viewport)
	dispatcher.render_frame(base_transform)
	var fade_setup := _configure_flicker_draws(
		dispatcher, tier, false, -FLICKER_FADE_STEP)
	var fade_b: Image = await _grab_flicker_image(viewport)
	var fade_diff := _masked_flicker_diff(hidden, fade_a, fade_b)
	var fade_responds := (
		int(fade_diff.coverage_xor_pixels) > 0
		or int(fade_diff.changed_rgb_pixels) > 0
	)

	print("[spawn-flicker] tier=", tier, " setup=", setup,
		" frame_diffs=", frame_diffs,
		" worst_xor=", worst_xor,
		" worst_changed=", worst_changed,
		" worst_delta=", worst_delta)
	print("[spawn-flicker] tier=", tier,
		" positive_control=", control,
		" fade_step=", FLICKER_FADE_STEP,
		" fade_setup=", fade_setup,
		" fade_diff=", fade_diff)

	if not control_detects:
		return {"ok": false, "failure": "positive control could not detect a dropped tier"}
	if not fade_responds:
		return {"ok": false, "failure": "retail fade nudge produced no raster change"}
	if worst_xor != 0 or worst_changed != 0:
		return {
			"ok": false,
			"failure": "fixed-input frames changed coverage=%d RGB=%d max_delta=%d" % [
				worst_xor, worst_changed, worst_delta],
		}
	return {"ok": true, "fade_diff": fade_diff}


func _configure_flicker_draws(
		dispatcher: NovaFoliageDispatcher, tier: String,
		hide_selected: bool, fade_adjust: float) -> Dictionary:
	var kept := 0
	var fade_min := INF
	var fade_max := -INF
	for child in dispatcher.get_children():
		var draw := child as MeshInstance3D
		if draw == null:
			continue
		var was_visible := draw.visible
		var name_text := String(draw.name)
		var material := draw.material_override as ShaderMaterial
		var shader_file := ""
		if material != null and material.shader != null:
			shader_file = material.shader.resource_path.get_file()
		var fade := 0.0
		var fade_value: Variant = draw.get_instance_shader_parameter("u_fade")
		if fade_value is float or fade_value is int:
			fade = float(fade_value)
		var matches := false
		if name_text.begins_with("FoliageDetailDraw"):
			if tier == "detail_high":
				matches = shader_file == "foliage_detail_high.gdshader"
			elif tier == "detail_low_far":
				matches = shader_file == "foliage_detail_low.gdshader" and fade > 0.100001
			elif tier == "detail_auto":
				matches = true
		var keep := was_visible and matches
		draw.visible = keep and not hide_selected
		if keep:
			kept += 1
			fade_min = minf(fade_min, fade)
			fade_max = maxf(fade_max, fade)
			draw.set_instance_shader_parameter(&"u_wind_phase", 0.0)
			if not is_zero_approx(fade_adjust):
				draw.set_instance_shader_parameter(&"u_fade", maxf(fade + fade_adjust, 0.0))
	return {
		"kept": kept,
		"fade_min": fade_min if kept > 0 else 0.0,
		"fade_max": fade_max if kept > 0 else 0.0,
	}


func _grab_flicker_image(viewport: Viewport) -> Image:
	await get_tree().process_frame
	await RenderingServer.frame_post_draw
	return viewport.get_texture().get_image()


func _masked_flicker_diff(hidden: Image, first: Image, current: Image) -> Dictionary:
	if hidden == null or first == null or current == null:
		return {"coverage_xor_pixels": -1, "changed_rgb_pixels": -1, "max_channel_delta": -1}
	if hidden.get_size() != first.get_size() or first.get_size() != current.get_size():
		return {"coverage_xor_pixels": -1, "changed_rgb_pixels": -1, "max_channel_delta": -1}
	var hidden_rgba := hidden.duplicate()
	var first_rgba := first.duplicate()
	var current_rgba := current.duplicate()
	hidden_rgba.convert(Image.FORMAT_RGBA8)
	first_rgba.convert(Image.FORMAT_RGBA8)
	current_rgba.convert(Image.FORMAT_RGBA8)
	var hidden_bytes: PackedByteArray = hidden_rgba.get_data()
	var first_bytes: PackedByteArray = first_rgba.get_data()
	var current_bytes: PackedByteArray = current_rgba.get_data()
	var coverage_union := 0
	var coverage_xor := 0
	var changed_rgb := 0
	var max_delta := 0
	for pixel in range(first_rgba.get_width() * first_rgba.get_height()):
		var offset := pixel * 4
		var first_hidden_delta := 0
		var current_hidden_delta := 0
		var frame_delta := 0
		for channel in 3:
			first_hidden_delta = maxi(first_hidden_delta,
				absi(int(first_bytes[offset + channel]) - int(hidden_bytes[offset + channel])))
			current_hidden_delta = maxi(current_hidden_delta,
				absi(int(current_bytes[offset + channel]) - int(hidden_bytes[offset + channel])))
			frame_delta = maxi(frame_delta,
				absi(int(current_bytes[offset + channel]) - int(first_bytes[offset + channel])))
		var first_covered := first_hidden_delta > FLICKER_MASK_DELTA
		var current_covered := current_hidden_delta > FLICKER_MASK_DELTA
		if first_covered or current_covered:
			coverage_union += 1
			if frame_delta > 0:
				changed_rgb += 1
				max_delta = maxi(max_delta, frame_delta)
		if first_covered != current_covered:
			coverage_xor += 1
	return {
		"coverage_union_pixels": coverage_union,
		"coverage_xor_pixels": coverage_xor,
		"coverage_xor_ratio": float(coverage_xor) / float(maxi(coverage_union, 1)),
		"changed_rgb_pixels": changed_rgb,
		"max_channel_delta": max_delta,
	}


func _snapshot(world, camera: Camera3D, environment, viewport: Viewport) -> Dictionary:
	return {
		"position": world.local_player_position(),
		"yaw": float(world.local_player_yaw_deg()),
		"pitch": float(world.local_player_pitch_deg()),
		"camera": camera.global_transform,
		"fov": camera.fov,
		"tod": float(environment.get("time_of_day")),
		"viewport": viewport.get_visible_rect().size,
	}


func _valid_snapshot(state: Dictionary) -> bool:
	var position: Vector3 = state["position"]
	var transform: Transform3D = state["camera"]
	var size: Vector2 = state["viewport"]
	if not position.is_finite() or not transform.origin.is_finite():
		return false
	for axis in [transform.basis.x, transform.basis.y, transform.basis.z]:
		if not axis.is_finite():
			return false
	for value in [state["yaw"], state["pitch"], state["fov"], state["tod"]]:
		if not is_finite(float(value)):
			return false
	return size.x > 0.0 and size.y > 0.0 and float(state["fov"]) > 0.0


func _same_snapshot(a: Dictionary, b: Dictionary) -> bool:
	return a["position"] == b["position"] and a["yaw"] == b["yaw"] \
		and a["pitch"] == b["pitch"] and a["camera"] == b["camera"] \
		and a["fov"] == b["fov"] and a["tod"] == b["tod"] \
		and a["viewport"] == b["viewport"]


func _print_snapshot(state: Dictionary) -> void:
	var p: Vector3 = state["position"]
	var t: Transform3D = state["camera"]
	var size: Vector2 = state["viewport"]
	print("[spawn-capture] player position: (%.9f, %.9f, %.9f)" % [p.x, p.y, p.z])
	print("[spawn-capture] player yaw/pitch deg: %.9f / %.9f" % [state["yaw"], state["pitch"]])
	print("[spawn-capture] camera transform: origin=%s basis_x=%s basis_y=%s basis_z=%s" % [
		str(t.origin), str(t.basis.x), str(t.basis.y), str(t.basis.z)])
	print("[spawn-capture] camera FOV deg: %.9f" % state["fov"])
	print("[spawn-capture] environment TOD: %.9f" % state["tod"])
	print("[spawn-capture] play viewport: %dx%d" % [int(size.x), int(size.y)])


func _capture(viewport: Viewport, path: String) -> Error:
	await RenderingServer.frame_post_draw
	var image: Image = viewport.get_texture().get_image()
	if image == null:
		return ERR_UNAVAILABLE
	var expected := Vector2i(viewport.get_visible_rect().size)
	if image.get_size() != expected:
		push_error("[spawn-capture] viewport/image mismatch: %s vs %s" % [
			str(expected), str(image.get_size())])
		return ERR_INVALID_DATA
	return image.save_png(path)


func _settle(frames: int) -> void:
	for _i in frames:
		await get_tree().process_frame


func _fail(reason: String) -> void:
	push_error("[spawn-capture] FAIL: " + reason)
	_shutdown(1)


func _shutdown(exit_code: int) -> void:
	if _play_controller != null:
		_play_controller.set_process(true)
		_play_controller.set_process_input(true)
		_play_controller.set_process_unhandled_input(true)
	if _input_router != null:
		_input_router.set_process_unhandled_input(true)
	if _mission_workspace != null and _mission_workspace.has_method("stop_play_mission"):
		_mission_workspace.stop_play_mission()
	get_tree().quit(exit_code)

func _print_runtime_metadata(world, environment) -> void:
	var mission = world.get_loaded_mission()
	var mission_info: Dictionary = mission.get_info() if mission != null else {}
	print("[spawn-capture] mission metadata: ", {
		"environment_ref": mission.get_environment_ref() if mission != null else "",
		"terrain_ref": mission.get_terrain_ref() if mission != null else "",
		"start_time_raw_q8_8": int(mission_info.get("start_time", -1)),
		"minutes_per_day": int(mission_info.get("minutes_per_day", -1)),
	})
	print("[spawn-capture] environment lighting: ", {
		"sun_direction": environment.get_sun_direction(),
		"light_direction": environment.get_light_direction(),
		"sun_keyframe": environment.get_sun_color(),
		"sun_light_runtime": environment.get_sun_light(),
		"sky_ambient_runtime": environment.get_sky_ambient(),
		"fog_color_runtime": environment.get_fog_color(),
		"fog_color_target": environment.get_fog_color_target(),
		"sky_base": environment.get_sky_base(),
		"sky_bright": environment.get_sky_bright(),
		"sky_highlight": environment.get_sky_highlight(),
		"cloud_base": environment.get_cloud_base(),
		"cloud_highlight": environment.get_cloud_highlight(),
		"cloud_edge": environment.get_cloud_edge(),
		"color_src_gain": environment.get_color_src_gain(),
		"fog_start": environment.get_fog_start(),
		"fog_end": environment.get_fog_level(),
		"fog_end_target": environment.get_fog_level_target(),
		"fog_type": environment.get_fog_type(),
		"sky_height": environment.get_sky_height(),
		"sky_height_target": environment.get_sky_height_target(),
	})

	var sky = world.get_node_or_null("NovaSky")
	var sky_material: ShaderMaterial = sky.sky_material if sky != null else null
	var cloud1: Texture2D = environment.get_sky_map1_tex()
	var cloud2: Texture2D = environment.get_sky_map2_tex()
	print("[spawn-capture] sky material: ", {
		"node": sky != null,
		"material": sky_material != null,
		"u_flat_pass": sky_material.get_shader_parameter("u_flat_pass") if sky_material != null else null,
		"u_has_clouds": sky_material.get_shader_parameter("u_has_clouds") if sky_material != null else null,
		"u_fog_color": sky_material.get_shader_parameter("u_fog_color") if sky_material != null else null,
		"u_fog_end": sky_material.get_shader_parameter("u_fog_end") if sky_material != null else null,
		"u_sky_base": sky_material.get_shader_parameter("u_sky_base") if sky_material != null else null,
		"u_sky_bright": sky_material.get_shader_parameter("u_sky_bright") if sky_material != null else null,
		"u_sky_highlight": sky_material.get_shader_parameter("u_sky_highlight") if sky_material != null else null,
		"cloud_tex1_size": Vector2i(cloud1.get_width(), cloud1.get_height()) if cloud1 != null else Vector2i.ZERO,
		"cloud_tex2_size": Vector2i(cloud2.get_width(), cloud2.get_height()) if cloud2 != null else Vector2i.ZERO,
	})

	var dispatcher = world.get_node_or_null("NovaTerrain/FoliageDispatcher")
	print("[spawn-capture] dispatcher: ", {
		"present": dispatcher != null,
		"total_instances": dispatcher.get_total_instances() if dispatcher != null else -1,
		"frame_stats": dispatcher.get_frame_stats() if dispatcher != null else {},
	})
	var data = world.get_terrain_data()
	var terrain = world.get_node_or_null("NovaTerrain")
	var assigned_tile_info = terrain.get_tile_info_override() if terrain != null else null
	var tile_info_source := "override"
	if assigned_tile_info == null and data != null:
		assigned_tile_info = data.get_tileinfo_resource()
		tile_info_source = "terrain_data"
	var resource_root = world.get_resource_root()
	var mission_til_name: String = String(world.get_loaded_mission_file()).get_basename() + ".til"
	var mission_til_bytes: PackedByteArray = resource_root.read_file(mission_til_name) \
		if resource_root != null and resource_root.has_file(mission_til_name) else PackedByteArray()
	print("[spawn-capture] TIL metadata: ", {
		"mission_til_name": mission_til_name,
		"mission_til_bytes": mission_til_bytes.size(),
		"terrain_tileinfo_filename": data.get_tileinfo_filename() if data != null else "",
		"assigned_tile_info_source": tile_info_source,
		"assigned_tile_info_entries": assigned_tile_info.get_entry_count() if assigned_tile_info != null else -1,
		"terrain_tiles": data.get_tile_count() if data != null else -1,
	})


func _texture_meta(value: Variant) -> Dictionary:
	var texture := value as Texture2D
	return {
		"valid": texture != null,
		"size": Vector2i(texture.get_width(), texture.get_height()) if texture != null else Vector2i.ZERO,
		"path": texture.resource_path if texture != null else "",
	}


func _print_foliage_material_state(world) -> void:
	var dispatcher: Node = world.get_node_or_null("NovaTerrain/FoliageDispatcher")
	if dispatcher == null:
		print("[spawn-capture] detail materials: dispatcher missing")
		return
	var seen := {}
	var tier_counts := {}
	var uv2_min := Vector2(INF, INF)
	var uv2_max := Vector2(-INF, -INF)
	var uv2_count := 0
	for child in dispatcher.get_children():
		var draw := child as MeshInstance3D
		if draw == null or not draw.visible or not String(draw.name).begins_with("FoliageDetailDraw"):
			continue
		var material := draw.material_override as ShaderMaterial
		var shader_path := material.shader.resource_path if material != null and material.shader != null else ""
		var tier := shader_path.get_file()
		tier_counts[tier] = int(tier_counts.get(tier, 0)) + 1
		if material != null and not seen.has(material.get_instance_id()):
			seen[material.get_instance_id()] = true
			print("[spawn-capture] detail material: ", {
				"tier": tier,
				"u_has_fd_texture": material.get_shader_parameter("u_has_fd_texture"),
				"u_fd_texture": _texture_meta(material.get_shader_parameter("u_fd_texture")),
				"u_has_colormap": material.get_shader_parameter("u_has_colormap"),
				"u_colormap": _texture_meta(material.get_shader_parameter("u_colormap")),
				"u_has_heightfield_normal": material.get_shader_parameter("u_has_heightfield_normal"),
				"u_heightfield_normal": _texture_meta(material.get_shader_parameter("u_heightfield_normal")),
				"u_has_tile_overlay": material.get_shader_parameter("u_has_tile_overlay"),
				"u_tile_overlay": _texture_meta(material.get_shader_parameter("u_tile_overlay")),
				"u_tile_overlay_tint": material.get_shader_parameter("u_tile_overlay_tint"),
				"u_emitter_color": material.get_shader_parameter("u_emitter_color"),
			})
		var mesh := draw.mesh
		if mesh == null:
			continue
		for surface in range(mesh.get_surface_count()):
			var arrays := mesh.surface_get_arrays(surface)
			if arrays.size() <= Mesh.ARRAY_TEX_UV2:
				continue
			var uv2s: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV2]
			for uv in uv2s:
				uv2_min = uv2_min.min(uv)
				uv2_max = uv2_max.max(uv)
				uv2_count += 1
	print("[spawn-capture] detail mesh state: ", {
		"visible_draws_by_tier": tier_counts,
		"unique_materials": seen.size(),
		"uv2_count": uv2_count,
		"uv2_min": uv2_min if uv2_count > 0 else Vector2.ZERO,
		"uv2_max": uv2_max if uv2_count > 0 else Vector2.ZERO,
	})
