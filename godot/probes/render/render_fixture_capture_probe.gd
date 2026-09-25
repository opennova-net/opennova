extends GameProbe

## render_fixture_capture: the exact-pose, production-world capture harness
## for docs/render/render-fixtures-v1.json. Boots the fixture's saved mission
## through the shell (the packed runtime root and expansion are the launch's),
## realizes the catalog camera/TOD with the shell frozen, captures every
## variant of the capture profile through the adapter's render bundle, and
## publishes the manifest sibling set through the fixture publication
## transaction. The contract rules live in RenderFixtureContract, the
## transaction in FixturePublication; this file orchestrates. Needs a window
## (the capture resolution is the window's).

const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")
const ShadowAttributionCaptureSession := preload(
		"res://probes/render/shadow_attribution_capture_session.gd")

const DEFAULT_CATALOG := "res://../docs/render/render-fixtures-v1.json"
const DEFAULT_OUTPUT_ROOT := "res://../.scratch/golden/render/fixtures"
const DEFAULT_SHADOW_ATTRIBUTION_OUTPUT_ROOT := \
		"res://../.scratch/golden/render/shadow-attribution"
# Upper bound on the frames spent waiting for the first-person weapon's idle
# clip to reach its hold before the pose is frozen (see _settle_viewmodel_hold).
const VIEWMODEL_HOLD_MAX_FRAMES := 240


class StaticTerrainShadowWarmupSuspension:
	extends RefCounted

	var _terrain: Terrain = null
	var _original_enabled := false
	var _active := false


	func begin(terrain: Terrain) -> Error:
		if _active:
			return ERR_ALREADY_IN_USE
		if terrain == null or not is_instance_valid(terrain):
			return ERR_UNCONFIGURED
		_terrain = terrain
		_original_enabled = terrain.is_static_terrain_shadow_enabled()
		_active = true
		if _original_enabled:
			terrain.set_static_terrain_shadow_enabled(false)
		return OK


	func finish() -> void:
		if not _active:
			return
		var terrain := _terrain
		var original_enabled := _original_enabled
		_terrain = null
		_original_enabled = false
		_active = false
		if terrain != null and is_instance_valid(terrain) \
				and terrain.is_static_terrain_shadow_enabled() != original_enabled:
			terrain.set_static_terrain_shadow_enabled(original_enabled)


var _game: MainGame
var _world: GameWorld
var _capture_size := RenderFixtureContract.DEFAULT_CAPTURE_SIZE
var _expected_mission_time_fixed24 := -1
var _failed := false
var _shadow_capture_session
var _static_shadow_warmup_suspension: StaticTerrainShadowWarmupSuspension
var _fixture_publication_staging_abs := ""
var _ctx: ProbeContext
var _failure := ""
var _result: Dictionary = {}


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	await _capture(ctx)
	_teardown()
	if _failed:
		return ProbeVerdict.failed(_failure, _result)
	return ProbeVerdict.passed("fixture %s: %d capture(s) published" % [
			String(_result.get("fixture", "")), int(_result.get("captures", 0))], _result)


func _capture(ctx: ProbeContext) -> void:
	var fixture_id := String(ctx.args.get("id", "")).strip_edges()
	if fixture_id.is_empty():
		_fail("id must name one catalog fixture")
		return
	var catalog_path := String(ctx.args.get("catalog", "")).strip_edges()
	if catalog_path.is_empty():
		catalog_path = DEFAULT_CATALOG
	var catalog := RenderFixtureContract.read_json(catalog_path)
	if catalog.is_empty():
		_fail("could not parse fixture catalog: %s" % catalog_path)
		return
	if not RenderFixtureContract.diagnostic_variant_contract_matches(
			catalog.get("diagnostic_variants")):
		_fail("catalog diagnostic_variants does not exactly match the capture driver")
		return
	var capture_profile := RenderFixtureContract.select_capture_profile(
			String(ctx.args.get("profile", "")),
			String(ctx.args.get("suppress_static_bms_ids", "")))
	if capture_profile.has("error"):
		_fail(String(capture_profile.error))
		return
	var fixture := RenderFixtureContract.fixture_by_id(catalog, fixture_id)
	if fixture.is_empty():
		_fail("fixture '%s' is not present in %s" % [fixture_id, catalog_path])
		return
	var capture_mode := RenderFixtureContract.select_capture_mode(
			fixture, String(ctx.args.get("mode", "")))
	if capture_mode.has("error"):
		_fail("fixture %s: %s" % [fixture_id, String(capture_mode.error)])
		return
	var world_only := bool(capture_mode.world_only)
	var catalog_contract: Variant = catalog.get("comparison_contract")
	var comparison_contract: Dictionary = {}
	if catalog.has("comparison_contract") or not world_only:
		comparison_contract = RenderFixtureContract.parse_comparison_contract(fixture, catalog_contract)
		if comparison_contract.has("error"):
			_fail("fixture %s: %s" % [
				fixture_id, String(comparison_contract.error)])
			return
	var gdextension_path := String(ctx.args.get("gdextension_binary", "")).strip_edges()
	if gdextension_path.is_empty():
		_fail("gdextension_binary must name the exact loaded extension binary")
		return
	var provenance := RenderFixtureContract.build_capture_provenance(
			String(ctx.args.get("source_commit", "")).strip_edges(),
			OS.get_executable_path(), gdextension_path)
	if provenance.has("error"):
		_fail(String(provenance.error))
		return
	var mission_name := String(fixture.get("mission", ""))
	var mission_root := String(ctx.args.get("mission_resource_dir", "")).strip_edges()
	if mission_root.is_empty():
		mission_root = LaunchFlags.resource_dir()
	# The packed runtime root and its expansion are the launch's (--resource-dir,
	# /exp); the manifest records the mounted expansion.
	var expansion := ResourceDirSettings.get_expansion().strip_edges()
	if not ResourceRoot.is_valid_root(mission_root):
		_fail("invalid loose mission root: %s" % mission_root)
		return
	if ctx.resource_root() == null:
		_fail("the shell has no mounted packed runtime root")
		return
	var mission_path := Paths.resolve_file(mission_root, mission_name)
	if mission_path.is_empty():
		_fail("%s is absent from loose mission root %s" % [mission_name, mission_root])
		return
	var expected_hash := String(catalog.get("missions", {}).get(mission_name, ""))
	var actual_hash := FileAccess.get_sha256(mission_path).to_lower()
	if expected_hash.is_empty() or actual_hash != expected_hash.to_lower():
		_fail("mission hash mismatch for %s: expected=%s actual=%s" % [
			mission_name, expected_hash, actual_hash])
		return

	var output_root := String(ctx.args.get("output_dir", "")).strip_edges()
	if output_root.is_empty():
		output_root = (DEFAULT_SHADOW_ATTRIBUTION_OUTPUT_ROOT \
				if String(capture_profile.id) \
						== RenderFixtureContract.CAPTURE_PROFILE_SHADOW_ATTRIBUTION \
				else DEFAULT_OUTPUT_ROOT).path_join(fixture_id)
	var output_abs := ProjectSettings.globalize_path(output_root).simplify_path()
	var scratch_abs := ProjectSettings.globalize_path(
			"res://../.scratch").simplify_path()
	var scratch_error := DirAccess.make_dir_recursive_absolute(scratch_abs)
	if scratch_error != OK:
		_fail("cannot create trusted capture scratch root: %s" \
				% error_string(scratch_error))
		return
	if not RenderFixtureContract.capture_output_is_allowed(capture_profile, output_abs, scratch_abs):
		_fail("capture profile %s output must be a strict descendant of %s" % [
				String(capture_profile.id), scratch_abs])
		return
	var resolution := RenderFixtureContract.parse_capture_resolution(catalog.get("capture", {}))
	if resolution.has("error"):
		_fail(String(resolution.error))
		return
	_capture_size = resolution.size
	# The window-size precondition comes before the publication transaction:
	# a window that cannot take the capture size leaves no staging behind.
	if not ctx.set_window_size(_capture_size):
		_fail("the shell has no window to size for the capture")
		return
	await ctx.wait_frames(3)
	var shell_viewport := ctx.viewport()
	if shell_viewport == null \
			or Vector2i(shell_viewport.get_visible_rect().size) != _capture_size:
		_fail("game window did not take the capture size %s" % str(_capture_size))
		return
	var publication := FixturePublication.begin_fixture_publication(output_abs, scratch_abs)
	if publication.has("error"):
		_fail(String(publication.error))
		return
	var publication_abs := String(publication.staging_path)
	_fixture_publication_staging_abs = publication_abs

	var local_player_profile := RenderFixtureContract.comparison_spawn_profile(comparison_contract) \
			if not comparison_contract.is_empty() else {}
	var boot_error := await ctx.load_saved_mission(
			mission_path, mission_name, local_player_profile)
	if not boot_error.is_empty():
		_fail(boot_error)
		return
	_game = ctx.game()
	_world = ctx.world()
	var camera := ctx.camera()
	var viewport := ctx.viewport()
	if camera == null or viewport == null or _world == null or not _world.is_loaded():
		_fail("production world/camera/viewport did not become capturable")
		return
	var probe_terrain: Terrain = _world.get_terrain_node()
	if probe_terrain != null:
		# Captures assert on the byte-level page hash/diff diagnostics that
		# steady-state play leaves off.
		probe_terrain.set_tile_cache_capture_diagnostics(true)
	_static_shadow_warmup_suspension = \
			StaticTerrainShadowWarmupSuspension.new()
	var warmup_shadow_error: Error = \
			_static_shadow_warmup_suspension.begin(probe_terrain)
	if warmup_shadow_error != OK:
		_fail("could not suspend static terrain shadow projection during live " \
				+ "capture warmup: %s" % error_string(warmup_shadow_error))
		return
	var capture_settings: Dictionary = catalog.get("capture", {})
	await ctx.wait_frames(int(capture_settings.get("load_settle_frames", 132)))
	if Vector2i(viewport.get_visible_rect().size) != _capture_size:
		_fail("game viewport is %s, expected %s" % [
			str(Vector2i(viewport.get_visible_rect().size)), str(_capture_size)])
		return
	var runtime := _world.get_runtime()
	var gameplay_input_active := bool(_game.is_gameplay_input_active())
	var post_spawn_state := {
		"entry": "single_player_auto_spawn_after_splash",
		"observed_at": "before_fixture_freeze",
		"world_loaded": _world.is_loaded(),
		"world_loading": bool(_game.is_world_loading()),
		"runtime_playing": runtime != null and runtime.is_playing(),
		"local_player_spawned": runtime != null and runtime.has_player(),
		"gameplay_input_active": gameplay_input_active,
		"gameplay_camera_current": camera.is_current()
				and viewport.get_camera_3d() == camera,
		# SP has no deploy-choice screen: its production-equivalent proof is the
		# shell's WORLD input gate plus the auto-spawned local player above.
		"spawn_or_menu_active": not gameplay_input_active,
	}
	if not RenderFixtureContract.post_spawn_capture_state_matches(post_spawn_state):
		_fail("production shell did not reach the post-spawn gameplay state: %s" \
				% JSON.stringify(post_spawn_state))
		return
	if not comparison_contract.is_empty():
		var spawn_witness := RenderFixtureContract.verify_comparison_spawn(_world, comparison_contract)
		if spawn_witness.has("error"):
			# Some missions carry an explicit spawn kit which correctly outranks the
			# PLAYER_INFO profile. For matched evidence, run the same public Armory
			# ACCEPT chain used in play, then require all sim and presentation
			# witnesses to converge before capture.
			var fallback := RenderFixtureContract.apply_comparison_weapon_fallback(
					_game, _world, comparison_contract)
			if fallback.has("error"):
				_fail("fixture %s: %s; fallback failed: %s" % [
						fixture_id, String(spawn_witness.error),
						String(fallback.error)])
				return
			await ctx.wait_frames(int(capture_settings.get(
					"visibility_settle_frames", 3)))
			spawn_witness = RenderFixtureContract.verify_comparison_spawn(_world, comparison_contract)
			if spawn_witness.has("error"):
				_fail("fixture %s: Armory fallback did not converge: %s" % [
						fixture_id, String(spawn_witness.error)])
				return
		# The Armory re-mount restarts the first-person weapon's idle clip. Retail
		# holds a non-looping idle's LAST frame once it has played out (the FP
		# channel plays the clip once and parks; the hip pose the registered retail
		# frames settle into is that hold), so capturing mid-clip bakes a random
		# phase -- up to ~1 cm of gun travel on the M4 clip set -- into the
		# evidence. Let the hold establish before the pose is frozen.
		await _settle_viewmodel_hold(VIEWMODEL_HOLD_MAX_FRAMES)

	var camera_spec: Dictionary = fixture.get("camera_bms", {})
	var raw_position: Array = camera_spec.get("position", [])
	if raw_position.size() != 3:
		_fail("fixture %s has no three-component camera_bms.position" % fixture_id)
		return
	var desired_position := RenderFixtureContract.mission_to_godot(Vector3(
			float(raw_position[0]), float(raw_position[1]), float(raw_position[2])))
	var yaw_deg := float(camera_spec.get("yaw_deg", 0.0))
	var pitch_deg := float(camera_spec.get("pitch_deg", 0.0))
	var fov_deg := float(camera_spec.get(
			"vertical_fov_deg", capture_settings.get("vertical_fov_deg", 50.534)))
	var minute_selection := RenderFixtureContract.select_fixture_minutes(
			fixture.get("minutes_of_day", []),
			String(ctx.args.get("minute", "")))
	if minute_selection.has("error"):
		_fail("fixture %s: %s" % [fixture_id, String(minute_selection.error)])
		return
	var minute_values: Array = minute_selection.values

	var manifest := {
		"schema": "opennova.render-fixture-captures.v1",
		"catalog_schema": String(catalog.get("schema", "")),
		"catalog_sha256": String(catalog.get("catalog_sha256", "")),
		"fixture": fixture,
		"mission": {
			"file": mission_name,
			"sha256": actual_hash,
			"source_kind": "loose_authoring_file",
			"runtime_mount": "packed_game_install",
			"expansion": expansion,
		},
		"capture": {
			"resolution": [_capture_size.x, _capture_size.y],
			"mode": String(capture_mode.mode),
			"world_only": world_only,
			# A world-only capture hides the FP gun through the presenter's
			# capture latch (mcp_begin_world_only_capture).
			"viewmodel_hidden": world_only,
			"post_spawn": post_spawn_state,
			"phase_contract": RenderFixtureContract.capture_phase_contract(),
			"comparison_contract": (catalog_contract as Dictionary).duplicate(true) \
					if catalog_contract is Dictionary else {},
			"camera_bms": {
				"position": raw_position.duplicate(),
				"yaw_deg": yaw_deg,
				"pitch_deg": pitch_deg,
				"vertical_fov_deg": fov_deg,
			},
			"camera_position_godot": desired_position,
			"camera_basis": RenderFixtureContract.camera_basis(yaw_deg, pitch_deg),
			"fov_deg": fov_deg,
		},
		"provenance": provenance,
		"artifacts": [],
	}
	if bool(capture_profile.scratch_only):
		(manifest["capture"] as Dictionary)["diagnostic_profile"] = \
				String(capture_profile.id)
	var adapter := ctx.adapter()
	for minute_value: Variant in minute_values:
		if not await _prepare_pose(
				camera, desired_position, yaw_deg, pitch_deg, fov_deg,
					float(minute_value), int(capture_settings.get(
							"visibility_settle_frames", 3)), comparison_contract):
			return
		_shadow_capture_session = ShadowAttributionCaptureSession.new()
		var shadow_session_error: Error = _shadow_capture_session.begin(
				_world, viewport)
		if shadow_session_error != OK:
			_fail("fixture %s: could not begin shadow capture session: %s" % [
					fixture_id, error_string(shadow_session_error)])
			return
		var presentation_mode := String(capture_mode.mode)
		if bool(capture_profile.scratch_only) \
				and not (manifest["capture"] as Dictionary).has(
						"shadow_attribution"):
			var dynamic_caster_inventory: Array = []
			for caster in _shadow_capture_session.get_dynamic_caster_inventory():
				dynamic_caster_inventory.append(caster.to_json_value())
			var static_caster_inventory: Array = []
			for caster in _shadow_capture_session.get_static_caster_inventory():
				static_caster_inventory.append(caster.to_json_value())
			(manifest["capture"] as Dictionary)["shadow_attribution"] = {
				"scratch_only": true,
				"dynamic_caster_inventory": dynamic_caster_inventory,
				"static_caster_inventory": static_caster_inventory,
			}
		var capture_rows: Array = manifest["artifacts"]
		for variant in capture_profile.variants:
			var variant_error: Error = _shadow_capture_session.apply_variant(variant)
			if variant_error != OK:
				_fail("fixture %s: could not apply capture variant %s: %s" % [
						fixture_id, String(variant.id), error_string(variant_error)])
				return
			# _prepare_pose freezes GameWorld after its exact-camera refresh. Process
			# frames alone cannot run Terrain while that owner is disabled, so every
			# shadow control transition needs the same explicitly non-time-owning
			# refresh before the adapter snapshots renderer state or pixels.
			var realization: Dictionary = await _realize_capture_variant_cache(
					variant, camera)
			if realization.has("error"):
				_fail("fixture %s: capture variant %s: %s" % [fixture_id,
						String(variant.id), String(realization.error)])
				return
			var label :="%s-m%04d-%s" % [
				fixture_id, int(minute_value), String(variant.id)]
			var result: Variant = await adapter.capture_mcp_render_bundle({
				"label": label,
				"settle_frames": int(capture_settings.get(
						"visibility_settle_frames", 3)),
				"world_only": world_only,
				"presentation_mode": presentation_mode,
			})
			if not (result is Dictionary) or result.has("error"):
				_fail("%s capture failed: %s" % [
					label, String(result.get("error", "invalid capture result")) \
					if result is Dictionary else "invalid capture result"])
				return
			var result_dict := result as Dictionary
			var comparison_witness: Dictionary = {}
			if not comparison_contract.is_empty():
				var diagnostics: Dictionary = result_dict.get("diagnostics", {})
				var presentation: Dictionary = diagnostics.get("presentation", {})
				var captured_hud: Dictionary = presentation.get(
						"hud_hidden_capture", {})
				comparison_witness = RenderFixtureContract.observe_comparison_contract(
						_game, _world, viewport, comparison_contract, captured_hud)
				if comparison_witness.has("error"):
					_fail("fixture %s: %s" % [
							fixture_id, String(comparison_witness.error)])
					return
				(manifest["capture"] as Dictionary)[
						"comparison_contract_witness"] = comparison_witness
			if not comparison_witness.is_empty():
				result_dict["comparison_contract_witness"] = comparison_witness
			var realized_variant_record: RenderCaptureVariant = \
					_shadow_capture_session.get_variant_diagnostics()
			var realized_variant: Dictionary = {}
			if realized_variant_record != null:
				var realized_value: Variant = \
						realized_variant_record.to_json_value()
				if realized_value is Dictionary:
					realized_variant = realized_value as Dictionary
			if not _valid_realized_camera(
					result_dict, desired_position, RenderFixtureContract.camera_basis(yaw_deg, pitch_deg),
					fov_deg, float(minute_value), _expected_mission_time_fixed24,
					variant, realized_variant):
				_fail("%s diagnostics did not preserve the requested pose/TOD" % label)
				return
			var row := publish_bundle(result_dict, publication_abs, label)
			if row.has("error"):
				_fail(String(row.error))
				return
			row["variant"] = String(variant.id)
			if bool(capture_profile.scratch_only):
				row["shadow_attribution"] = realized_variant
			capture_rows.append(row)
		_shadow_capture_session.finish()
		_shadow_capture_session = null

	var manifest_path := FixturePublication.publication_child_path(
			publication_abs, "%s-manifest.json" % fixture_id)
	if manifest_path.is_empty():
		_fail("fixture %s produced an unsafe manifest path" % fixture_id)
		return
	var manifest_bytes := JSON.stringify(McpJson.sanitize(manifest), "\t").to_utf8_buffer()
	var write_error := RenderFixtureContract.write_bytes(manifest_path, manifest_bytes)
	if write_error != OK:
		_fail("could not write fixture manifest: %s" % error_string(write_error))
		return
	var commit_error := FixturePublication.commit_fixture_publication(publication_abs, output_abs)
	if commit_error != OK:
		_fail("could not commit complete fixture publication: %s" \
				% error_string(commit_error))
		return
	_fixture_publication_staging_abs = ""
	manifest_path = FixturePublication.publication_child_path(
			output_abs, "%s-manifest.json" % fixture_id)
	if manifest_path.is_empty():
		_fail("fixture %s produced an unsafe committed manifest path" % fixture_id)
		return
	ctx.artifact("manifest", manifest_path, "json")
	_result = {
		"fixture": fixture_id,
		"captures": (manifest["artifacts"] as Array).size(),
		"manifest": manifest_path,
		"output": output_abs,
	}
	ctx.log("PASS fixture=%s captures=%d manifest=%s" % [
			fixture_id, int(_result["captures"]), manifest_path])


func _prepare_pose(
		camera: Camera3D,
		position: Vector3,
		yaw_deg: float,
		pitch_deg: float,
		fov_deg: float,
		minute_of_day: float,
		settle_frames: int,
		comparison_contract: Dictionary = {}) -> bool:
	_game.process_mode = Node.PROCESS_MODE_INHERIT
	_world.process_mode = Node.PROCESS_MODE_INHERIT
	var runtime := _world.get_runtime()
	if runtime == null:
		_fail("mission runtime is unavailable while preparing an exact fixture pose")
		return false
	# Let render presentation settle while the mission clock is pinned. A paused
	# runtime keeps GameWorld's environment/material publication live but prevents
	# Weather.advance_world_driven() from advancing the selected fixed24 value.
	runtime.pause()
	if runtime.is_playing():
		_fail("mission runtime could not pause for an exact fixture pose")
		return false
	if not comparison_contract.is_empty():
		var pose := RenderFixtureContract.teleport_comparison_player(_world, comparison_contract)
		if pose.has("error"):
			_fail(String(pose.error))
			return false
	var minute_error: Error = _world.debug_set_mission_minute_of_day(minute_of_day)
	if minute_error != OK:
		_fail("could not set minute %.3f: %s" % [
			minute_of_day, error_string(minute_error)])
		return false
	var phase_error := RenderFixtureContract.pin_weather_phase(_world)
	if phase_error != OK:
		_fail("could not pin canonical weather phase: %s" % error_string(phase_error))
		return false
	var environment := _world.get_environment_node()
	if environment == null:
		_fail("mission environment is unavailable while preparing an exact fixture pose")
		return false
	_expected_mission_time_fixed24 = environment.get_mission_time_fixed24()
	await _ctx.wait_frames(settle_frames)
	if environment.get_mission_minute_of_day() != minute_of_day \
			or environment.get_mission_time_fixed24() \
			!= _expected_mission_time_fixed24:
		_fail("mission clock advanced while settling exact fixture minute %.3f" \
				% minute_of_day)
		return false
	# Freeze the production shell after the environment has published the chosen
	# minute. Rendering and frame_post_draw continue, while sim, weather, camera
	# presenter, particles, and cloud offsets retain one exact state.
	_game.process_mode = Node.PROCESS_MODE_DISABLED
	_world.process_mode = Node.PROCESS_MODE_DISABLED
	camera.projection = Camera3D.PROJECTION_PERSPECTIVE
	camera.keep_aspect = Camera3D.KEEP_HEIGHT
	camera.fov = fov_deg
	# No near write: the world's scene-environment leg pins the retail world
	# near plane (0.2) on the render camera every frame, the frozen-pose replay
	# included.
	camera.h_offset = 0.0
	camera.v_offset = 0.0
	camera.global_transform = Transform3D(RenderFixtureContract.camera_basis(yaw_deg, pitch_deg), position)
	camera.make_current()
	# The FP gun draws inside the beauty pass at its WORLD pose; with the shell
	# frozen nothing re-places it at the moved camera, so the shell does that
	# here (the hud_hidden variants capture WITH the gun).
	_game.mcp_restamp_viewmodel_for_capture()
	_finish_static_shadow_warmup_suspension()
	var refresh_error := _world.debug_refresh_render_pose(camera)
	if refresh_error != OK:
		_fail("could not refresh production state at the exact fixture pose: %s" \
				% error_string(refresh_error))
		return false
	if environment.get_mission_minute_of_day() != minute_of_day \
			or environment.get_mission_time_fixed24() \
			!= _expected_mission_time_fixed24:
		_fail("mission clock moved while refreshing exact fixture render state")
		return false
	return true


func _valid_realized_camera(
		bundle: Dictionary,
		position: Vector3,
		basis: Basis,
		fov_deg: float,
		minute_of_day: float,
		mission_time_fixed24: int,
		variant,
		realized_variant: Dictionary) -> bool:
	var artifact: Dictionary = bundle.get("artifact", {})
	if int(artifact.get("width", 0)) != _capture_size.x \
			or int(artifact.get("height", 0)) != _capture_size.y:
		_ctx.log("realized capture is %dx%d, expected %s" % [
				int(artifact.get("width", 0)), int(artifact.get("height", 0)), str(_capture_size)])
		return false
	var diagnostics: Dictionary = bundle.get("diagnostics", {})
	if not RenderFixtureContract.capture_variant_matches_diagnostics(
			variant, diagnostics, realized_variant):
		_ctx.log("realized variant %s does not match its diagnostics: %s; diagnostics %s" % [
				String(variant.id), JSON.stringify(realized_variant),
				JSON.stringify(McpJson.sanitize({
					"world": diagnostics.get("world"),
					"terrain": diagnostics.get("terrain"),
					"renderer": diagnostics.get("renderer"),
					"shadows": diagnostics.get("shadows"),
				}))])
		return false
	var camera_state: Dictionary = diagnostics.get("camera", {})
	var realized: Variant = camera_state.get("global_transform")
	if not (realized is Transform3D):
		_ctx.log("diagnostics carry no camera transform")
		return false
	var transform := realized as Transform3D
	if not transform.origin.is_equal_approx(position) \
			or not transform.basis.is_equal_approx(basis) \
			or not is_equal_approx(float(camera_state.get("fov_deg", -1.0)), fov_deg):
		_ctx.log("realized camera %s fov %.3f, requested origin %s basis %s fov %.3f" % [
				str(transform), float(camera_state.get("fov_deg", -1.0)), str(position),
				str(basis), fov_deg])
		return false
	if not RenderFixtureContract.realized_reflection_pose_matches(diagnostics):
		_ctx.log("realized reflection pose does not match: %s" % JSON.stringify(
				McpJson.sanitize(diagnostics.get("water", {}))))
		return false
	if not RenderFixtureContract.realized_tod_matches(
			diagnostics, minute_of_day, mission_time_fixed24):
		_ctx.log("realized TOD does not match minute %.3f / fixed24 %d: %s" % [
				minute_of_day, mission_time_fixed24,
				JSON.stringify(McpJson.sanitize(diagnostics.get("environment", {})))])
		return false
	return true


## Publish one captured bundle as a relocatable manifest sibling set. Public so
## the evidence-contract test exercises the same seam as the capture loop.
func publish_bundle(bundle: Dictionary, output_abs: String, label: String) -> Dictionary:
	if not FixturePublication.publication_name_is_canonical(label):
		return {"error": "capture publication label is not canonical: %s" % label}
	var artifact: Dictionary = bundle.get("artifact", {})
	var source_png := String(artifact.get("png_path", ""))
	var source_state := String(artifact.get("state_path", ""))
	var output_png := FixturePublication.publication_child_path(output_abs, label + ".png")
	var output_state := FixturePublication.publication_child_path(
			output_abs, label + ".state.json")
	if output_png.is_empty() or output_state.is_empty():
		return {"error": "capture publication paths escape their fixture root"}
	var png_bytes := FileAccess.get_file_as_bytes(source_png)
	if png_bytes.is_empty():
		return {"error": "capture artifact is empty: %s" % source_png}
	var source_state_bytes := FileAccess.get_file_as_bytes(source_state)
	if source_state_bytes.is_empty():
		return {"error": "capture state is empty: %s" % source_state}
	var png_sha256 := GameRenderCapture.sha256_hex(png_bytes)
	var source_state_sha256 := GameRenderCapture.sha256_hex(source_state_bytes)
	if png_sha256.is_empty() or source_state_sha256.is_empty():
		return {"error": "could not hash captured bundle bytes"}
	if String(artifact.get("sha256", "")).to_lower() != png_sha256:
		return {"error": "capture artifact PNG hash does not match its bytes"}
	if String(artifact.get("state_sha256", "")).to_lower() \
			!= source_state_sha256:
		return {"error": "capture artifact state hash does not match its bytes"}
	var parsed_state: Variant = JSON.parse_string(
			source_state_bytes.get_string_from_utf8())
	if not (parsed_state is Dictionary):
		return {"error": "capture state is not a JSON object"}
	var published_state := parsed_state as Dictionary
	var state_contract_error := RenderFixtureContract.capture_source_state_contract_error(
			bundle, published_state, source_png)
	if not state_contract_error.is_empty():
		return {"error": state_contract_error}
	var capture: Dictionary = published_state.get("capture", {})
	if capture.is_empty() or String(capture.get("png_sha256", "")).to_lower() \
			!= png_sha256:
		return {"error": "capture state does not bind the source PNG hash"}
	# capture_mcp_render_bundle writes its transient user:// path into the raw
	# sidecar. Preserve the raw sidecar hash below, but publish a deterministic
	# relocatable sidecar whose label and PNG reference match the manifest entry.
	# The transport label is filename-sanitized and capped at 64 characters;
	# fixture publication labels are canonical and may be longer.
	capture["label"] = label
	capture["png_path"] = output_png.get_file()
	published_state["capture"] = capture
	published_state["publication"] = {
		"transform": "rewrite_capture_label_and_png_path_to_publication_siblings",
		"source_state_sha256": source_state_sha256,
	}
	var comparison_witness: Variant = bundle.get("comparison_contract_witness")
	if comparison_witness is Dictionary \
			and not (comparison_witness as Dictionary).is_empty():
		published_state["comparison_contract_witness"] = \
				(comparison_witness as Dictionary).duplicate(true)
	var state_bytes := JSON.stringify(
			McpJson.sanitize(published_state), "\t").to_utf8_buffer()
	# Validate and transform the complete sibling set before the first publication
	# write. The fixture-level staging transaction owns final-directory atomicity.
	var png_error := RenderFixtureContract.write_bytes(output_png, png_bytes)
	if png_error != OK:
		return {"error": "could not publish %s: %s" % [
			output_png, error_string(png_error)]}
	var state_error := RenderFixtureContract.write_bytes(output_state, state_bytes)
	if state_error != OK:
		return {"error": "could not publish %s: %s" % [
			output_state, error_string(state_error)]}
	return {
		"label": label,
		# The manifest lives beside these artifacts. Filename-only references
		# keep the captured bundle relocatable and prevent a committed manifest
		# from silently resolving back into user:// or a scratch worktree.
		"png_path": output_png.get_file(),
		"state_path": output_state.get_file(),
		"png_sha256": png_sha256,
		"state_sha256": GameRenderCapture.sha256_hex(state_bytes),
		"source_state_sha256": source_state_sha256,
		"width": int(artifact.get("width", 0)),
		"height": int(artifact.get("height", 0)),
	}


## Wait until every first-person viewmodel part's active body clip has played
## to its end (a non-looping clip then holds its last frame, the state retail's
## settled hip idle shows), bounded by max_frames. Looping clips have no hold
## and are left alone: their phase is inherently unpinned on both engines.
func _settle_viewmodel_hold(max_frames: int) -> void:
	var presenter := _ctx.presenter()
	if presenter == null:
		return
	for _i in max_frames:
		var held := true
		for part in presenter.vm_parts():
			if not is_instance_valid(part):
				continue
			var model := part as ObjectModel
			if model == null:
				continue
			var skeletal: SkeletalAnim = model.get_skeletal_anim()
			var key: String = model.get_active_body_clip()
			if skeletal == null or key.is_empty() or skeletal.is_clip_looping(key):
				continue
			var length := skeletal.get_clip_length(key)
			if length <= 0.0:
				continue
			# get_animation_time() is the body-clip playhead (with the gated FP
			# channel it is the pinned advance position); get_animation_time_ms
			# is the PANM material clock and never measures the clip.
			if model.get_animation_time() < length:
				held = false
		if held:
			return
		await _ctx.tree.process_frame


func _realize_capture_variant_cache(variant, camera: Camera3D) -> Dictionary:
	var deadline_ms := Time.get_ticks_msec() + 30000
	var diagnostics: Dictionary = {}
	for _attempt in range(512):
		var refresh_error: Error = _world.debug_refresh_render_pose(camera)
		if refresh_error != OK:
			return {"error": "could not refresh renderer state: %s" %
					error_string(refresh_error)}
		var terrain := _world.get_terrain_node()
		diagnostics = terrain.get_tile_cache_diagnostics() \
				if terrain != null else {}
		if RenderFixtureContract.capture_variant_tile_cache_is_realized(variant, diagnostics):
			return {"diagnostics": diagnostics}
		if Time.get_ticks_msec() >= deadline_ms:
			break
		# GameWorld is frozen for exact-pose capture. Repeating this explicitly
		# non-time-owning refresh advances page demand/upload while the process
		# frame gives the two portable CPU workers scheduling time.
		await _ctx.tree.process_frame
	return {"error": "terrain page cache did not become capture-ready: %s" %
			JSON.stringify(diagnostics)}


func _finish_static_shadow_warmup_suspension() -> void:
	if _static_shadow_warmup_suspension == null:
		return
	_static_shadow_warmup_suspension.finish()
	_static_shadow_warmup_suspension = null


func _fail(reason: String) -> void:
	if _failed:
		return
	_failed = true
	_failure = reason
	_ctx.log("FAIL: " + reason)


## Every exit path: the warmup provider back, an unfinished publication
## aborted, the shadow session closed, the shell and world processing again.
func _teardown() -> void:
	_finish_static_shadow_warmup_suspension()
	if not _fixture_publication_staging_abs.is_empty():
		var abort_error := FixturePublication.abort_fixture_publication(
				_fixture_publication_staging_abs)
		if abort_error != OK:
			_ctx.log("could not clean fixture publication staging path %s: %s" % [
					_fixture_publication_staging_abs, error_string(abort_error)])
		_fixture_publication_staging_abs = ""
	if _shadow_capture_session != null:
		_shadow_capture_session.finish()
		_shadow_capture_session = null
	if _game != null and is_instance_valid(_game):
		_game.process_mode = Node.PROCESS_MODE_INHERIT
	if _world != null and is_instance_valid(_world):
		_world.process_mode = Node.PROCESS_MODE_INHERIT
