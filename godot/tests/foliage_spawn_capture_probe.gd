extends Node

## Deterministic retail-comparison capture at the real local-player spawn.
## This uses the real standalone game shell and never searches
## foliage-painted cells, teleports the camera, or synthesizes input.
##
## NOVA_MISSION_RESOURCE_DIR=<loose-authoring-dir> \
## NOVA_RUNTIME_RESOURCE_DIR=<packed-game-dir> NOVA_EXPANSION=revx02 \
## NOVA_MISSION_BMS=00TRe.bms \
##   "$GODOT_BIN" --path godot res://tests/foliage_spawn_capture_probe.tscn
## Optional output override: NOVA_SPAWN_CAPTURE_DIR=<absolute-or-res://-path>

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")

const DEFAULT_MISSION := "00TRe.bms"
const DEFAULT_EXPANSION := "revx02"
const DEFAULT_OUT_DIR := "res://../.scratch/00tre-spawn"
const PLAY_SETTLE_FRAMES := 132
const VISIBILITY_SETTLE_FRAMES := 3
const CAPTURE_VIEWPORT_SIZE := Vector2i(1600, 900)
const FLICKER_CAPTURE_COUNT := 6
const FLICKER_MASK_DELTA := 6
const FLICKER_FADE_STEP := 0.25 / 22.0

var _game: Node
var _world: GameWorld
var _out_abs := ""
var _capture_stem := "00TRe"


func _ready() -> void:
	var mission_resource_dir := OS.get_environment("NOVA_MISSION_RESOURCE_DIR").strip_edges()
	if mission_resource_dir.is_empty():
		mission_resource_dir = ResourceDirSettings.get_resource_dir()
	if not ResourceDirSettings.is_valid_root(mission_resource_dir):
		_fail("no valid loose authoring root; set NOVA_MISSION_RESOURCE_DIR")
		return
	var runtime_resource_dir := OS.get_environment("NOVA_RUNTIME_RESOURCE_DIR").strip_edges()
	if not NovaResourceRoot.is_valid_root(runtime_resource_dir):
		_fail("no valid packed runtime root; set NOVA_RUNTIME_RESOURCE_DIR")
		return
	var requested_expansion := OS.get_environment("NOVA_EXPANSION").strip_edges()
	if requested_expansion.is_empty():
		requested_expansion = DEFAULT_EXPANSION
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
	var mission_path := NovaPaths.resolve_file(mission_resource_dir, mission_name)
	if mission_path.is_empty():
		_fail("%s not found in loose authoring root %s" % [
			mission_name, mission_resource_dir])
		return
	print("[spawn-capture] mission authoring root: ", mission_resource_dir)
	print("[spawn-capture] packed runtime root: ", runtime_resource_dir)
	print("[spawn-capture] mission: ", mission_name, " -> ", mission_path)
	print("[spawn-capture] input: disabled after shell load; none synthesized")

	get_window().mode = Window.MODE_WINDOWED
	get_window().size = CAPTURE_VIEWPORT_SIZE
	var session: Dictionary = await StandaloneProbe.boot(
		self, runtime_resource_dir, mission_name, requested_expansion, mission_path)
	if not String(session.get("error", "")).is_empty():
		_fail(String(session.error))
		return
	_game = session.game
	_world = session.world
	var runtime_root: NovaResourceRoot = _game.current_resource_root()
	if runtime_root == null:
		_fail("standalone game did not retain its packed runtime root")
		return
	var mount_validation_error := runtime_mount_validation_error(
		requested_expansion, runtime_root.get_expansion(),
		runtime_root.is_runtime_mount())
	if not mount_validation_error.is_empty():
		_fail(mount_validation_error)
		return
	print("[spawn-capture] runtime expansion: requested=%s actual=%s mount=packed" % [
		requested_expansion, runtime_root.get_expansion()])
	var world: GameWorld = _world
	var camera: Camera3D = session.camera
	if world == null or not world.is_loaded() \
			or world.get_sim() == null or not world.get_sim().has_local_player():
		_fail("GameWorld has no loaded local-player spawn anchor")
		return
	if camera == null or not camera.is_inside_tree():
		_fail("game Camera3D unavailable")
		return
	var play_viewport: Viewport = camera.get_viewport()
	world.get_sim().set_player_input(false, false, false, false, false, false, false)
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	await _settle(PLAY_SETTLE_FRAMES)

	var environment = world.get_node_or_null("NovaEnvironment")
	if environment == null or environment.get("time_of_day") == null:
		_fail("played mission environment/TOD unavailable")
		return
	if play_viewport == null:
		_fail("game viewport unavailable")
		return

	# Freeze the real game loop. Rendering stays live, while the player,
	# camera, mission clock, and foliage dispatch stay bit-identical for the A/B.
	world.process_mode = Node.PROCESS_MODE_DISABLED
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
	var loaded_mission = world.get_loaded_mission()
	var winning_entries: Array = [{
		"logical_name": mission_name,
		"source_type": "loose",
		"source_path": mission_path,
		"archive_path": "",
	}]
	winning_entries.append_array(_winning_source_entries(runtime_root, PackedStringArray([
		String(loaded_mission.get_terrain_ref()) + ".trn",
		String(loaded_mission.get_environment_ref()) + ".env",
	])))
	var source_validation_error := runtime_source_validation_error(
		requested_expansion, mission_name, winning_entries, true)
	if not source_validation_error.is_empty():
		_fail(source_validation_error)
		return
	for entry in winning_entries:
		print("[spawn-capture] runtime source: logical_name=%s source_type=%s source=%s" % [
			entry.get("logical_name", ""),
			entry.get("source_type", ""),
			entry.get("source_path", entry.get("archive_path", "")),
		])
	_print_snapshot(spawn_state)
	_print_runtime_metadata(world, environment)
	_print_model_lighting_trace(world, camera, environment)
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
		return "00TRe exact spawn produced no live detail foliage instances"
	if total_instances <= 0:
		return "00TRe exact spawn produced no dispatcher foliage instances"
	return ""


static func runtime_mount_validation_error(
		requested_expansion: String, actual_expansion: String,
		is_runtime_mount: bool) -> String:
	var requested := requested_expansion.strip_edges().to_lower()
	var actual := actual_expansion.strip_edges().to_lower()
	if requested.is_empty():
		return "runtime comparison requires an explicit expansion"
	if not is_runtime_mount:
		return "comparison root is not a packed runtime mount"
	if actual != requested:
		return "requested expansion %s silently fell back to %s" % [
			requested, "base assets" if actual.is_empty() else actual]
	return ""


static func runtime_source_validation_error(
		requested_expansion: String, mission_name: String,
		winning_entries: Array, mission_is_saved_loose: bool = false) -> String:
	if winning_entries.is_empty():
		return "runtime comparison reported no winning source entries"
	var mission_key := mission_name.get_file().to_lower()
	var mission_found := false
	if requested_expansion.strip_edges().is_empty():
		return "runtime comparison source validation requires an explicit expansion"
	for value in winning_entries:
		var entry := value as Dictionary
		var logical_name := String(entry.get("logical_name", "")).get_file()
		var source_type := String(entry.get("source_type", "")).to_lower()
		var archive_path := String(entry.get("archive_path", "")).replace("\\", "/")
		if logical_name.is_empty():
			return "runtime comparison has a missing winning source entry"
		if logical_name.to_lower() == mission_key:
			mission_found = true
			if mission_is_saved_loose:
				var source_path := String(entry.get("source_path", "")).replace("\\", "/")
				if source_type != "loose" or source_path.is_empty():
					return "%s did not identify its saved loose source" % logical_name
				continue
		if source_type != "pff" or archive_path.is_empty():
			return "%s did not resolve from a packed archive" % logical_name
	if not mission_found:
		return "runtime comparison did not report the %s mission winner" % mission_name
	return ""


static func _winning_source_entries(
		root: NovaResourceRoot, logical_names: PackedStringArray) -> Array:
	var winners_by_name := {}
	for value in root.list_file_entries():
		var entry := value as Dictionary
		winners_by_name[String(entry.get("logical_name", "")).get_file().to_lower()] = entry
	var winners: Array = []
	for logical_name in logical_names:
		var key := String(logical_name).get_file().to_lower()
		winners.append(winners_by_name.get(key, {
			"logical_name": String(logical_name).get_file(),
			"source_type": "",
			"archive_path": "",
		}))
	return winners


func _run_foliage_flicker_probe(
		world, dispatcher: NovaFoliageDispatcher, camera: Camera3D,
		viewport: Viewport, spawn_state: Dictionary) -> void:
	# Freeze every scene update and drive only the real foliage dispatcher. The
	# renderer stays live, so any remaining pixel changes come from foliage draws.
	world.process_mode = Node.PROCESS_MODE_DISABLED
	var viewmodel_pass: Node = _game.find_child("ViewmodelPass", true, false) \
			if _game != null else null
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
		var cutoff := 0.0
		var cutoff_value: Variant = draw.get_instance_shader_parameter("u_high_pass_cutoff")
		if cutoff_value is float or cutoff_value is int:
			cutoff = float(cutoff_value)
		var matches := false
		if name_text.begins_with("FoliageDetailDraw"):
			if tier == "detail_high":
				matches = shader_file == "foliage_detail_high.gdshader"
			elif tier == "detail_low_far":
				# Primary LOW (>= 33u) carries no strict-LESS cutoff; the near
				# secondary does. Both share the unscaled distance fade.
				matches = shader_file == "foliage_detail_low.gdshader" and cutoff <= 0.0
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
		"position": world.get_sim().get_local_player_position(),
		"yaw": float(world.get_sim().get_local_player_yaw_deg()),
		"pitch": float(world.get_sim().get_local_player_pitch_deg()),
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
	if _world != null and is_instance_valid(_world):
		_world.process_mode = Node.PROCESS_MODE_INHERIT
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
		"ceiling_ambient_runtime": environment.get_ceiling_color(),
		"floor_ambient_runtime": environment.get_floor_color(),
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


func _print_model_lighting_trace(world, camera: Camera3D, environment) -> void:
	if OS.get_environment("NOVA_MODEL_LIGHTING_TRACE") != "1":
		return
	var sim = world.get_sim()
	var light_dir: Vector3 = environment.get_light_direction()
	var iris_samples := PackedInt32Array()
	if sim != null:
		iris_samples = sim.compute_iris_samples(
			camera.global_position, -camera.global_basis.z, light_dir)
	print("[spawn-capture] model lighting trace: ", {
		"local_player_indoors":
			sim.local_player_indoors() if sim != null else null,
		"local_player_blink_flags":
			sim.local_player_blink_flags() if sim != null else null,
		"local_player_interior_item_id":
			sim.local_player_interior_item_id() if sim != null else null,
		"iris_samples": iris_samples,
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
