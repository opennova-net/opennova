extends GutTest

# The render-fixture capture contract (RenderFixtureContract) and its probe's
# transactions. The contract's live legs are typed to their production owners
# (GameShell, GameWorld, Simulation, Terrain, Weather, LocalPlayerPresenter;
# ADR 0043 d12), so this file drives them over REAL fixtures (the packaged
# world, a bare Terrain, the null GameShell) and pins the rule they refuse
# with; the positive comparison legs (a spawned local player carrying the
# retail kit) run in the live render_fixture_capture probe over the retail
# fixture, never against a double.

const Probe := preload("res://probes/render/render_fixture_capture_probe.gd")


func test_mission_camera_pose_converts_to_godot_basis() -> void:
	var position: Vector3 = RenderFixtureContract.mission_to_godot(
			Vector3(277.129364, -363.799469, 28.8))
	assert_true(position.is_equal_approx(Vector3(277.129364, 28.8, 363.799469)))

	var forward: Vector3 = -RenderFixtureContract.camera_basis(0.0, 0.0).z
	assert_true(forward.is_equal_approx(Vector3(0.0, 0.0, -1.0)))
	var turned: Vector3 = -RenderFixtureContract.camera_basis(-45.0, 0.0).z
	var half_sqrt := sqrt(0.5)
	assert_true(turned.is_equal_approx(Vector3(-half_sqrt, 0.0, -half_sqrt)))
	var down: Vector3 = -RenderFixtureContract.camera_basis(0.0, -30.0).z
	assert_lt(down.y, 0.0)


func test_capture_resolution_comes_from_the_catalog() -> void:
	var parsed := RenderFixtureContract.parse_capture_resolution({"resolution": [2000, 1200]})
	assert_eq(parsed.get("size"), Vector2i(2000, 1200))
	assert_false(parsed.has("error"))

	for bad: Variant in [
		{},
		{"resolution": [2000]},
		{"resolution": [2000, "tall"]},
		{"resolution": [2000, 1200.5]},
		{"resolution": [2000, 128]},
		{"resolution": [2000, 8192]},
	]:
		var rejected := RenderFixtureContract.parse_capture_resolution(bad)
		assert_true(rejected.has("error"), "expected rejection for %s" % [bad])


func test_capture_mode_is_declared_by_the_fixture_and_env_only_verifies_it() -> void:
	assert_eq(RenderFixtureContract.select_capture_mode(
			{"capture_mode": "world_only"}, ""), {
		"mode": "world_only",
		"world_only": true,
	})
	assert_eq(RenderFixtureContract.select_capture_mode(
			{"capture_mode": "full_frame"}, "full_frame"), {
		"mode": "full_frame",
		"world_only": false,
	})
	assert_eq(RenderFixtureContract.select_capture_mode(
			{"capture_mode": "hud_hidden"}, "hud_hidden"), {
		"mode": "hud_hidden",
		"world_only": false,
	})
	assert_true(RenderFixtureContract.select_capture_mode(
			{"capture_mode": "world_only"}, "full_frame").has("error"),
			"the environment may not relabel a catalog fixture")
	assert_eq(RenderFixtureContract.select_capture_mode({}, "").get("mode"), "world_only",
			"the pre-mode diagnostic catalog retains its world-only contract")
	assert_true(RenderFixtureContract.select_capture_mode(
			{"capture_mode": "hud_only"}, "").has("error"))


func test_hud_hidden_contract_requires_the_exact_matched_presentation() -> void:
	var fixture := {
		"capture_mode": "hud_hidden",
		"retail_player_bms": {
			"applied": [283.129364, -357.799469, 27.0],
		},
		"camera_bms": {
			"yaw_deg": -90.0,
			"pitch_deg": -6.5,
		},
	}
	var contract := {
		"capture_mode": "hud_hidden",
		"equipped_weapon": "WPN_M16BURST",
		"weapon_clip": 30,
		"weapon_reserve": 270,
		"character_id": 0x0402,
		"arms_graphic": "IndoArms.3di",
		"arms_camo": [1, 0, 0],
		"gameplay_hud_visible": false,
		"hud_canvas_layer_active": true,
		"hud_detail_level": 3,
		"player_view_effects_active": true,
		"viewmodel_enabled": true,
		"terrain_enabled": true,
		"ads_active": false,
		"big_map_active": false,
		"player_pose_source": "retail_player_bms.applied",
	}
	var parsed: Dictionary = RenderFixtureContract.parse_comparison_contract(fixture, contract)
	assert_false(parsed.has("error"))
	assert_eq(parsed.equipped_weapon, "WPN_M16BURST")
	assert_eq(parsed.character_id, 0x0402)
	assert_eq(parsed.arms_graphic, "IndoArms.3di")
	assert_eq(parsed.arms_camo, [1, 0, 0])
	assert_eq(parsed.player_position_bms,
			Vector3(283.129364, -357.799469, 27.0))
	assert_eq(String(RenderFixtureContract.comparison_spawn_profile(parsed).get("primary", "")),
			"WPN_M16BURST",
			"the canonical contract equips through the production profile seam")

	for mutation: Dictionary in [
		{"capture_mode": "full_frame"},
		{"equipped_weapon": "WPN_M4AUTO"},
		{"weapon_clip": 29},
		{"weapon_reserve": 300},
		{"character_id": 0x0200},
		{"arms_graphic": "ArmsG.3di"},
		{"arms_camo": [0, 0, 0]},
		{"gameplay_hud_visible": true},
		{"hud_canvas_layer_active": false},
		{"hud_detail_level": 2},
		{"player_view_effects_active": false},
		{"viewmodel_enabled": false},
		{"terrain_enabled": false},
		{"ads_active": true},
		{"big_map_active": true},
		{"player_pose_source": "camera_bms.position"},
	]:
		var invalid_contract := contract.duplicate(true)
		for key: Variant in mutation:
			invalid_contract[key] = mutation[key]
		assert_true(RenderFixtureContract.parse_comparison_contract(
				fixture, invalid_contract).has("error"),
				"the HUD-hidden contract must reject %s" % [mutation])
	var extra := contract.duplicate(true)
	extra["profile_slot"] = 0
	assert_true(RenderFixtureContract.parse_comparison_contract(fixture, extra).has("error"),
			"profile setup fields do not belong in the comparison contract")


func test_comparison_contract_requires_full_frame_m16_and_exact_player_pose() -> void:
	var fixture := {
		"capture_mode": "full_frame",
		"retail_player_bms": {
			"applied": [283.129364, -357.799469, 27.0],
		},
		"camera_bms": {
			"yaw_deg": -90.0,
			"pitch_deg": -6.5,
		},
	}
	var contract := {
		"capture_mode": "full_frame",
		"equipped_weapon": "WPN_M16BURST",
		"retail_hud_weapon_label": "M16 - Burst",
		"hud_enabled": true,
		"viewmodel_enabled": true,
		"terrain_enabled": true,
		"player_pose_source": "retail_player_bms.applied",
	}
	var parsed: Dictionary = RenderFixtureContract.parse_comparison_contract(fixture, contract)
	assert_false(parsed.has("error"))
	assert_eq(parsed.weapon, "WPN_M16BURST")
	assert_eq(parsed.retail_hud_weapon_label, "M16 - Burst")
	assert_eq(parsed.player_position_bms,
			Vector3(283.129364, -357.799469, 27.0))
	assert_eq(parsed.yaw_deg, -90.0)
	assert_eq(parsed.pitch_deg, -6.5)

	var wrong_mode := fixture.duplicate(true)
	wrong_mode.capture_mode = "world_only"
	assert_true(RenderFixtureContract.parse_comparison_contract(
			wrong_mode, contract).has("error"))
	var wrong_pose := fixture.duplicate(true)
	wrong_pose.retail_player_bms.applied = [1.0, 2.0]
	assert_true(RenderFixtureContract.parse_comparison_contract(
			wrong_pose, contract).has("error"))
	for mutation: Dictionary in [
		{"capture_mode": "world_only"},
		{"retail_hud_weapon_label": ""},
		{"hud_enabled": false},
		{"viewmodel_enabled": false},
		{"terrain_enabled": false},
		{"player_pose_source": "camera_bms.position"},
	]:
		var invalid_contract := contract.duplicate(true)
		for key: Variant in mutation:
			invalid_contract[key] = mutation[key]
		assert_true(RenderFixtureContract.parse_comparison_contract(
				fixture, invalid_contract).has("error"),
				"the comparison contract must reject %s" % [mutation])


func test_comparison_contract_uses_production_spawn_profile_and_presentation_witness() -> void:
	var contract := {
		"weapon": "WPN_M16BURST",
		"retail_hud_weapon_label": "M16 - Burst",
		"hud_enabled": true,
		"viewmodel_enabled": true,
		"terrain_enabled": true,
		"player_position_bms": Vector3(283.129364, -357.799469, 27.0),
		"yaw_deg": -90.0,
		"pitch_deg": -6.5,
	}

	assert_eq(RenderFixtureContract.comparison_spawn_profile(contract), {
		"player_class": 9,
		"primary": "WPN_M16BURST",
		"primary_clips": 9,
		"secondary": "",
		"secondary_clips": -1,
		"accessory": "",
		"accessory_clips": -1,
	}, "legacy full_frame keeps its committed spawn-profile shape")
	var hidden_profile_contract := {
		"capture_mode": "hud_hidden",
		"equipped_weapon": "WPN_M16BURST",
	}
	assert_eq(RenderFixtureContract.comparison_spawn_profile(hidden_profile_contract), {
		"team": 0,
		"player_class": 9,
		"primary": "WPN_M16BURST",
		"primary_clips": 9,
		"secondary": "",
		"secondary_clips": -1,
		"accessory": "",
		"accessory_clips": -1,
		"side_profiles": [
			{
				"team": 0,
				"nationality": 2,
				"division": 0,
				"combo": 1,
				"player_class": 9,
			},
			{
				"team": 1,
				"nationality": 7,
				"division": 0,
				"combo": 0,
				"player_class": 9,
			},
		],
	})
	var avatar_db := AvatarDatabase.new()
	assert_eq(avatar_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/avatars/synth_avatars.def")), OK)
	var join_profile := avatar_db.character_join_profile(
			RenderFixtureContract.comparison_spawn_profile(hidden_profile_contract))
	assert_eq([join_profile.get_character_id(0), join_profile.get_character_id(1)],
			[0x0402, 0x8207],
			"the staged tree selections resolve to retail slot 0's character IDs")
	assert_eq([join_profile.get_player_class(0), join_profile.get_player_class(1)], [9, 9])
	# The reference leg: in the shipped table blue's 0x0402 wears the arms the retail
	# capture shows (IndoArms.3di, camo 1).
	var retail_avatars := RetailData.fixture("avatars/Avatars.def")
	if retail_avatars.is_empty():
		pending(RetailData.fixture_pending_text("avatars/Avatars.def"))
	else:
		var retail_db := AvatarDatabase.new()
		assert_eq(retail_db.load(retail_avatars), OK)
		var combo := retail_db.resolve_character_id(0x0402, 0)
		assert_not_null(combo)
		var arms := combo.get_arms()
		assert_not_null(arms, "blue 0x0402 authors an arms part")
		assert_eq(arms.graphic, "IndoArms.3di")
		assert_eq(arms.camo, Vector3i(1, 0, 0))


# The typed comparison legs over real owners that carry no matched spawn: the
# unloaded packaged world (no Simulation), the null shell (no presenter, no
# HUD witness) and a loaded minimal world whose spawn is not the retail kit.
# Every leg names what is missing instead of passing; the positive path (a
# spawned local player carrying WPN_M16BURST at 30/270, the retail arms) is
# the live probe's over the retail fixture.
func test_comparison_legs_refuse_real_owners_without_the_matched_spawn() -> void:
	var contract := {
		"weapon": "WPN_M16BURST",
		"retail_hud_weapon_label": "M16 - Burst",
		"hud_enabled": true,
		"viewmodel_enabled": true,
		"terrain_enabled": true,
		"player_position_bms": Vector3(283.129364, -357.799469, 27.0),
		"yaw_deg": -90.0,
		"pitch_deg": -6.5,
	}
	var shell: GameShell = add_child_autofree(GameShell.new())
	var unloaded := WorldFixture.make_world(self)
	assert_false(unloaded.is_loaded())
	var viewport := get_viewport()

	assert_true(RenderFixtureContract.verify_comparison_spawn(null, contract).has("error"),
			"no world: the spawn witness names the missing world")
	assert_eq(String(RenderFixtureContract.verify_comparison_spawn(
			unloaded, contract).get("error", "")),
			"comparison simulation is unavailable",
			"an unloaded world carries no simulation to witness")
	assert_eq(String(RenderFixtureContract.teleport_comparison_player(
			unloaded, contract).get("error", "")),
			"comparison simulation cannot teleport the local player")
	assert_eq(String(RenderFixtureContract.apply_comparison_weapon_fallback(
			shell, unloaded, contract).get("error", "")),
			"comparison Armory fallback has no live simulation")
	assert_true(RenderFixtureContract.apply_comparison_weapon_fallback(
			null, unloaded, contract).has("error"), "no shell: no fallback")
	assert_true(RenderFixtureContract.observe_comparison_contract(
			null, unloaded, viewport, contract).has("error"),
			"no shell: the presentation witness is unavailable")
	assert_true(RenderFixtureContract.observe_comparison_contract(
			shell, unloaded, null, contract).has("error"),
			"no viewport: the presentation witness is unavailable")
	assert_eq(String(RenderFixtureContract.observe_comparison_contract(
			shell, unloaded, viewport, contract).get("error", "")),
			"comparison simulation is unavailable",
			"the spawn witness gates the presentation witness")

	# A loaded minimal world spawns its own kit, never the retail M16: the
	# spawn witness refuses on the weapon, and the null shell's Armory
	# fallback has no presenter to refresh.
	var world := WorldFixture.boot_minimal(self)
	var refused := RenderFixtureContract.verify_comparison_spawn(world, contract)
	assert_true(refused.has("error"),
			"the mission's own spawn must not pass as matched evidence")
	assert_eq(String(RenderFixtureContract.apply_comparison_weapon_fallback(
			shell, world, contract).get("error", "")),
			"comparison game has no viewmodel refresh seam")
	assert_true(RenderFixtureContract.observe_comparison_contract(
			shell, world, viewport, contract).has("error"),
			"the presentation witness inherits the spawn refusal")


func test_capture_provenance_requires_a_frozen_source_and_exact_binaries() -> void:
	var godot_path := "user://render-fixture-provenance-godot.bin"
	var extension_path := "user://render-fixture-provenance-extension.bin"
	var godot_file := FileAccess.open(godot_path, FileAccess.WRITE)
	assert_not_null(godot_file)
	godot_file.store_string("godot-test")
	godot_file.close()
	var extension_file := FileAccess.open(extension_path, FileAccess.WRITE)
	assert_not_null(extension_file)
	extension_file.store_string("extension-test")
	extension_file.close()

	var provenance := RenderFixtureContract.build_capture_provenance(
			"a".repeat(40), godot_path, extension_path)
	assert_false(provenance.has("error"))
	assert_eq(provenance.source_commit, "a".repeat(40))
	assert_eq(provenance.godot_executable_sha256,
			"59cb4fdf868a41f5dd4d2fbeedc45517b0636dec738b69de646fd08df0c97d4f")
	assert_eq(provenance.gdextension_sha256,
			"294c60be27332e39c6f029b7d98414e3a82221eb17a7ac86bb9b1b8767e436de")
	assert_true(RenderFixtureContract.build_capture_provenance(
			"short", godot_path, extension_path).has("error"))
	assert_true(RenderFixtureContract.build_capture_provenance(
			"A".repeat(40), godot_path, extension_path).has("error"))
	assert_true(RenderFixtureContract.build_capture_provenance(
			"a".repeat(40), godot_path, "user://missing-extension.bin").has("error"))


func test_published_capture_artifacts_are_relocatable_with_the_manifest() -> void:
	var source_png := "user://render-fixture-portable-source.png"
	var source_state := "user://render-fixture-portable-source.state.json"
	var output_dir := "user://render-fixture-portable-output"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(output_dir))
	var png_file := FileAccess.open(source_png, FileAccess.WRITE)
	assert_not_null(png_file)
	png_file.store_buffer(PackedByteArray([1, 2, 3]))
	png_file.close()
	var png_sha256 := FileAccess.get_sha256(source_png)
	var diagnostics := {
		"schema": "OpenNovaRenderDiagnosticsV1",
		"frame": {"process": 77},
	}
	var state_file := FileAccess.open(source_state, FileAccess.WRITE)
	assert_not_null(state_file)
	state_file.store_string(JSON.stringify({
		"schema": "OpenNovaRenderCaptureV1",
		"capture": {
			"id": "portable-source-77",
			"label": "transport-truncated",
			"process_frame": 77,
			"captured_at_ticks_usec": 123456,
			"png_path": source_png,
			"png_sha256": png_sha256,
			"width": 2000,
			"height": 1200,
			"mime": "image/png",
		},
		"diagnostics": diagnostics,
	}))
	state_file.close()
	var source_state_sha256 := FileAccess.get_sha256(source_state)

	var probe := Probe.new()
	var row: Dictionary = probe.publish_bundle({
		"schema": "OpenNovaRenderCaptureV1",
		"capture_id": "portable-source-77",
		"artifact": {
			"png_path": source_png,
			"state_path": source_state,
			"sha256": png_sha256,
			"state_sha256": source_state_sha256,
			"width": 2000,
			"height": 1200,
			"mime": "image/png",
		},
		"diagnostics": diagnostics,
		"comparison_contract_witness": {
			"observed_at": "after_pose_settle_before_fixture_freeze",
			"equipped_weapon": "WPN_M16BURST",
			"player_class": 9,
			"weapon_clip": 30,
			"weapon_reserve": 270,
			"player_position_bms": Vector3(1.0, 2.0, 3.0),
			"requested_player_pose_bms": {
				"position": Vector3(1.0, 2.0, 3.0),
				"yaw_deg": -90.0,
				"pitch_deg": -6.5,
			},
			"hud_canvas_layer_visible": true,
			"viewmodel_canvas_layer_visible": true,
			"terrain_data_available": true,
			"terrain_node_visible": true,
		},
	}, ProjectSettings.globalize_path(output_dir), "portable")

	assert_false(row.has("error"), String(row.get("error", "")))
	assert_eq(row.png_path, "portable.png")
	assert_eq(row.state_path, "portable.state.json")
	assert_false(String(row.png_path).is_absolute_path())
	assert_false(String(row.state_path).is_absolute_path())
	assert_true(FileAccess.file_exists(output_dir.path_join(String(row.png_path))))
	assert_true(FileAccess.file_exists(output_dir.path_join(String(row.state_path))))
	var published_state := JSON.parse_string(FileAccess.get_file_as_string(
			output_dir.path_join(String(row.state_path)))) as Dictionary
	assert_eq(published_state.capture.label, "portable",
			"the published state and manifest must share the publication label")
	assert_eq(published_state.capture.png_path, "portable.png")
	assert_eq(published_state.publication, {
		"transform": "rewrite_capture_label_and_png_path_to_publication_siblings",
		"source_state_sha256": source_state_sha256,
	})
	assert_eq(published_state.comparison_contract_witness.equipped_weapon,
			"WPN_M16BURST")
	assert_eq(int(published_state.comparison_contract_witness.weapon_clip), 30)
	assert_eq(int(published_state.comparison_contract_witness.weapon_reserve), 270)
	assert_true(published_state.comparison_contract_witness.hud_canvas_layer_visible)
	assert_eq(row.png_sha256, png_sha256)
	assert_eq(row.source_state_sha256, source_state_sha256)
	assert_eq(row.state_sha256, FileAccess.get_sha256(
			output_dir.path_join(String(row.state_path))))


func test_publish_bundle_rejects_claims_that_do_not_match_source_bytes() -> void:
	var source_png := "user://render-fixture-hash-source.png"
	var source_state := "user://render-fixture-hash-source.state.json"
	var output_dir := "user://render-fixture-hash-output"
	var output_png := output_dir.path_join("hash-mismatch.png")
	var output_state := output_dir.path_join("hash-mismatch.state.json")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(output_dir))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(output_png))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(output_state))
	var png_file := FileAccess.open(source_png, FileAccess.WRITE)
	assert_not_null(png_file)
	png_file.store_buffer(PackedByteArray([4, 5, 6]))
	png_file.close()
	var false_png_sha256 := "a".repeat(64)
	var diagnostics := {
		"schema": "OpenNovaRenderDiagnosticsV1",
		"frame": {"process": 78},
	}
	var state_file := FileAccess.open(source_state, FileAccess.WRITE)
	assert_not_null(state_file)
	state_file.store_string(JSON.stringify({
		"schema": "OpenNovaRenderCaptureV1",
		"capture": {
			"id": "hash-source-78",
			"label": "raw",
			"process_frame": 78,
			"captured_at_ticks_usec": 123457,
			"png_path": source_png,
			"png_sha256": false_png_sha256,
			"width": 2000,
			"height": 1200,
			"mime": "image/png",
		},
		"diagnostics": diagnostics,
	}))
	state_file.close()

	var probe := Probe.new()
	var row: Dictionary = probe.publish_bundle({
		"schema": "OpenNovaRenderCaptureV1",
		"capture_id": "hash-source-78",
		"artifact": {
			"png_path": source_png,
			"state_path": source_state,
			"sha256": false_png_sha256,
			"state_sha256": FileAccess.get_sha256(source_state),
			"width": 2000,
			"height": 1200,
			"mime": "image/png",
		},
		"diagnostics": diagnostics,
	}, ProjectSettings.globalize_path(output_dir), "hash-mismatch")

	assert_true(row.has("error"),
			"artifact and sidecar claims cannot substitute for hashing the PNG bytes")
	assert_false(FileAccess.file_exists(output_png),
			"a rejected bundle must not publish one half of the sibling set")
	assert_false(FileAccess.file_exists(output_state))


func test_publish_bundle_rejects_a_stale_source_state_hash_before_writing() -> void:
	var source_png := "user://render-fixture-state-hash-source.png"
	var source_state := "user://render-fixture-state-hash-source.state.json"
	var output_dir := "user://render-fixture-state-hash-output"
	var output_png := output_dir.path_join("state-hash-mismatch.png")
	var output_state := output_dir.path_join("state-hash-mismatch.state.json")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(output_dir))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(output_png))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(output_state))
	var png_file := FileAccess.open(source_png, FileAccess.WRITE)
	assert_not_null(png_file)
	png_file.store_buffer(PackedByteArray([7, 8, 9]))
	png_file.close()
	var png_sha256 := FileAccess.get_sha256(source_png)
	var diagnostics := {
		"schema": "OpenNovaRenderDiagnosticsV1",
		"frame": {"process": 79},
	}
	var state_file := FileAccess.open(source_state, FileAccess.WRITE)
	assert_not_null(state_file)
	state_file.store_string(JSON.stringify({
		"schema": "OpenNovaRenderCaptureV1",
		"capture": {
			"id": "state-hash-source-79",
			"label": "raw",
			"process_frame": 79,
			"captured_at_ticks_usec": 123458,
			"png_path": source_png,
			"png_sha256": png_sha256,
			"width": 2000,
			"height": 1200,
			"mime": "image/png",
		},
		"diagnostics": diagnostics,
	}))
	state_file.close()

	var probe := Probe.new()
	var row: Dictionary = probe.publish_bundle({
		"schema": "OpenNovaRenderCaptureV1",
		"capture_id": "state-hash-source-79",
		"artifact": {
			"png_path": source_png,
			"state_path": source_state,
			"sha256": png_sha256,
			"state_sha256": "b".repeat(64),
			"width": 2000,
			"height": 1200,
			"mime": "image/png",
		},
		"diagnostics": diagnostics,
	}, ProjectSettings.globalize_path(output_dir), "state-hash-mismatch")

	assert_true(row.has("error"),
			"the source state claim must be verified against its bytes")
	assert_false(FileAccess.file_exists(output_png))
	assert_false(FileAccess.file_exists(output_state))


func test_publish_bundle_rejects_a_hashed_but_incomplete_raw_sidecar() -> void:
	var source_png := "user://render-fixture-incomplete-state-source.png"
	var source_state := \
			"user://render-fixture-incomplete-state-source.state.json"
	var output_dir := "user://render-fixture-incomplete-state-output"
	var output_png := output_dir.path_join("incomplete-state.png")
	var output_state := output_dir.path_join("incomplete-state.state.json")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(output_dir))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(output_png))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(output_state))
	var png_file := FileAccess.open(source_png, FileAccess.WRITE)
	assert_not_null(png_file)
	png_file.store_buffer(PackedByteArray([10, 11, 12]))
	png_file.close()
	var png_sha256 := FileAccess.get_sha256(source_png)
	var state_file := FileAccess.open(source_state, FileAccess.WRITE)
	assert_not_null(state_file)
	state_file.store_string(JSON.stringify({
		"capture": {"png_sha256": png_sha256},
	}))
	state_file.close()

	var probe := Probe.new()
	var row: Dictionary = probe.publish_bundle({
		"schema": "OpenNovaRenderCaptureV1",
		"capture_id": "incomplete-source-80",
		"artifact": {
			"png_path": source_png,
			"state_path": source_state,
			"sha256": png_sha256,
			"state_sha256": FileAccess.get_sha256(source_state),
			"width": 2000,
			"height": 1200,
			"mime": "image/png",
		},
		"diagnostics": {
			"schema": "OpenNovaRenderDiagnosticsV1",
			"frame": {"process": 80},
		},
	}, ProjectSettings.globalize_path(output_dir), "incomplete-state")

	assert_true(row.has("error"),
			"hashes alone do not make an incomplete raw sidecar valid evidence")
	assert_false(FileAccess.file_exists(output_png))
	assert_false(FileAccess.file_exists(output_state))


func test_fixture_publication_replaces_the_destination_only_after_commit() -> void:
	var nonce := "%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()]
	var trusted_root := ProjectSettings.globalize_path(
			"user://render-fixture-transaction-" + nonce)
	var final_dir := trusted_root.path_join("fixture")
	assert_eq(DirAccess.make_dir_recursive_absolute(final_dir), OK)
	var old_path := final_dir.path_join("old-evidence.txt")
	var old_file := FileAccess.open(old_path, FileAccess.WRITE)
	assert_not_null(old_file)
	old_file.store_string("old")
	old_file.close()

	var transaction: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root)
	assert_false(transaction.has("error"))
	var staging_dir := String(transaction.get("staging_path", ""))
	assert_ne(staging_dir, final_dir)
	assert_true(DirAccess.dir_exists_absolute(staging_dir))
	var staged_png := staging_dir.path_join("fixture-beauty.png")
	var staged_manifest := staging_dir.path_join("fixture-manifest.json")
	var png_file := FileAccess.open(staged_png, FileAccess.WRITE)
	assert_not_null(png_file)
	png_file.store_buffer(PackedByteArray([1, 2, 3]))
	png_file.close()
	var manifest_file := FileAccess.open(staged_manifest, FileAccess.WRITE)
	assert_not_null(manifest_file)
	manifest_file.store_string("{}")
	manifest_file.close()

	assert_true(FileAccess.file_exists(old_path),
			"the prior evidence remains visible while the fixture is incomplete")
	assert_false(FileAccess.file_exists(
			final_dir.path_join("fixture-manifest.json")))
	assert_eq(FixturePublication.commit_fixture_publication(staging_dir, final_dir), OK)
	assert_false(DirAccess.dir_exists_absolute(staging_dir))
	assert_false(FileAccess.file_exists(old_path))
	assert_true(FileAccess.file_exists(
			final_dir.path_join("fixture-beauty.png")))
	assert_true(FileAccess.file_exists(
			final_dir.path_join("fixture-manifest.json")))

	var failed_transaction: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root)
	assert_false(failed_transaction.has("error"))
	var failed_stage := String(failed_transaction.staging_path)
	var incomplete := FileAccess.open(
			failed_stage.path_join("incomplete.png"), FileAccess.WRITE)
	assert_not_null(incomplete)
	incomplete.store_buffer(PackedByteArray([9]))
	incomplete.close()
	assert_eq(FixturePublication.abort_fixture_publication(failed_stage), OK)
	assert_false(DirAccess.dir_exists_absolute(failed_stage))
	assert_true(FileAccess.file_exists(
			final_dir.path_join("fixture-manifest.json")),
			"aborting an incomplete fixture must preserve the last complete fixture")
	assert_false(FileAccess.file_exists(final_dir.path_join("incomplete.png")))

	var source := FileAccess.get_file_as_string(
			"res://probes/render/render_fixture_capture_probe.gd")
	var begin := source.find(
			"begin_fixture_publication(output_abs, scratch_abs)")
	var publish := source.find(
			"publish_bundle(result_dict, publication_abs, label)", begin)
	var manifest_write := source.find("write_bytes(manifest_path", publish)
	var commit := source.find(
			"commit_fixture_publication(publication_abs, output_abs)",
			manifest_write)
	assert_gt(begin, -1)
	assert_gt(publish, begin)
	assert_gt(manifest_write, publish)
	assert_gt(commit, manifest_write,
			"only a complete variant set plus manifest may replace the destination")


func test_fixture_publication_recovers_the_prior_fixture_after_an_interrupted_install() -> void:
	var nonce := "%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()]
	var trusted_root := ProjectSettings.globalize_path(
			"user://render-fixture-recovery-" + nonce)
	var final_dir := trusted_root.path_join("fixture")
	assert_eq(DirAccess.make_dir_recursive_absolute(final_dir), OK)
	var old_path := final_dir.path_join("old-evidence.txt")
	var old_file := FileAccess.open(old_path, FileAccess.WRITE)
	assert_not_null(old_file)
	old_file.store_string("old")
	old_file.close()

	var transaction: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root)
	assert_false(transaction.has("error"))
	var staging_dir := String(transaction.get("staging_path", ""))
	var new_path := staging_dir.path_join("new-evidence.txt")
	var new_file := FileAccess.open(new_path, FileAccess.WRITE)
	assert_not_null(new_file)
	new_file.store_string("new")
	new_file.close()

	var install_failure := func(_source: String, _target: String) -> Error:
		return ERR_CANT_CREATE
	var restore_failure := func(_source: String, _target: String) -> Error:
		return ERR_BUSY
	assert_eq(FixturePublication.commit_fixture_publication(
			staging_dir, final_dir, install_failure, restore_failure), ERR_BUSY)
	assert_false(DirAccess.dir_exists_absolute(final_dir),
			"the injected interruption leaves recovery to the durable transaction")

	var owner_is_dead := func(_pid: int) -> bool:
		return false
	var recovered: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root, owner_is_dead)
	assert_false(recovered.has("error"),
			"the next publication must recover an interrupted prior transaction")
	assert_true(FileAccess.file_exists(old_path),
			"recovery restores the last complete fixture before accepting new work")
	assert_false(FileAccess.file_exists(final_dir.path_join("new-evidence.txt")))
	assert_eq(FixturePublication.abort_fixture_publication(
			String(recovered.get("staging_path", ""))), OK)


func test_fixture_publication_rejects_a_linked_output_root_without_touching_its_target() -> void:
	var nonce := "%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()]
	var parent := ProjectSettings.globalize_path(
			"user://render-fixture-link-root-" + nonce)
	var target := parent.path_join("external-target")
	var linked_output := parent.path_join("fixture-output")
	assert_eq(DirAccess.make_dir_recursive_absolute(target), OK)
	var sentinel := target.path_join("must-survive.txt")
	var sentinel_file := FileAccess.open(sentinel, FileAccess.WRITE)
	assert_not_null(sentinel_file)
	sentinel_file.store_string("do not delete")
	sentinel_file.close()
	var parent_dir := DirAccess.open(parent)
	assert_not_null(parent_dir)
	var link_error := parent_dir.create_link(target, linked_output)
	if link_error != OK and OS.get_name() == "Windows":
		var command_output: Array = []
		var junction_command := "mklink /J \"%s\" \"%s\"" % [
			linked_output.replace("/", "\\"), target.replace("/", "\\"),
		]
		var junction_exit := OS.execute("cmd.exe", PackedStringArray([
			"/d", "/c", junction_command,
		]), command_output, true)
		assert_eq(junction_exit, 0,
				"the regression fixture requires a real link/junction root: %s" \
				% "\n".join(command_output))
	else:
		assert_eq(link_error, OK,
				"the regression fixture requires a real link/junction root")
	assert_true(parent_dir.is_link(linked_output.get_file()))

	var transaction: Dictionary = FixturePublication.begin_fixture_publication(
			linked_output, parent)
	assert_true(transaction.has("error"),
			"publication must reject a linked root before any rename or traversal")
	if not transaction.has("error"):
		FixturePublication.abort_fixture_publication(String(transaction.get("staging_path", "")))
	assert_true(FileAccess.file_exists(sentinel),
			"rejecting the root must never traverse or delete the external target")
	assert_eq(DirAccess.remove_absolute(linked_output), OK)
	assert_true(FileAccess.file_exists(sentinel))
	assert_eq(DirAccess.remove_absolute(sentinel), OK)
	assert_eq(DirAccess.remove_absolute(target), OK)
	assert_eq(DirAccess.remove_absolute(parent), OK)


func test_fixture_publication_rejects_the_trusted_root_and_any_linked_ancestor() -> void:
	var nonce := "%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()]
	var container := ProjectSettings.globalize_path(
			"user://render-fixture-ancestor-link-" + nonce)
	var trusted_root := container.path_join("trusted-scratch")
	var external := container.path_join("external-target")
	assert_eq(DirAccess.make_dir_recursive_absolute(trusted_root), OK)
	assert_eq(DirAccess.make_dir_recursive_absolute(external), OK)

	var root_transaction: Dictionary = FixturePublication.begin_fixture_publication(
			trusted_root, trusted_root)
	assert_true(root_transaction.has("error"),
			"a fixture must be a dedicated strict descendant, never the scratch root")
	if not root_transaction.has("error"):
		FixturePublication.abort_fixture_publication(String(
				root_transaction.get("staging_path", "")))

	var sentinel := external.path_join("must-survive.txt")
	var sentinel_file := FileAccess.open(sentinel, FileAccess.WRITE)
	assert_not_null(sentinel_file)
	sentinel_file.store_string("do not delete")
	sentinel_file.close()
	var pivot := trusted_root.path_join("pivot")
	var trusted_dir := DirAccess.open(trusted_root)
	assert_not_null(trusted_dir)
	var link_error := trusted_dir.create_link(external, pivot)
	if link_error != OK and OS.get_name() == "Windows":
		var command_output: Array = []
		var junction_command := "mklink /J \"%s\" \"%s\"" % [
			pivot.replace("/", "\\"), external.replace("/", "\\"),
		]
		var junction_exit := OS.execute("cmd.exe", PackedStringArray([
			"/d", "/c", junction_command,
		]), command_output, true)
		assert_eq(junction_exit, 0,
				"the regression fixture requires an ancestor junction: %s" \
				% "\n".join(command_output))
	else:
		assert_eq(link_error, OK,
				"the regression fixture requires an ancestor directory link")
	assert_true(trusted_dir.is_link(pivot.get_file()))

	var output_inside_link := pivot.path_join("fixture")
	var transaction: Dictionary = FixturePublication.begin_fixture_publication(
			output_inside_link, trusted_root)
	assert_true(transaction.has("error"),
			"every existing component below the trusted root must be link-free")
	if not transaction.has("error"):
		FixturePublication.abort_fixture_publication(String(
				transaction.get("staging_path", "")))
	assert_true(FileAccess.file_exists(sentinel),
			"ancestor rejection must not enter or mutate the external target")
	assert_false(DirAccess.dir_exists_absolute(external.path_join("fixture")))

	assert_eq(DirAccess.remove_absolute(sentinel), OK)
	assert_eq(DirAccess.remove_absolute(external), OK)
	assert_true(trusted_dir.is_link(pivot.get_file()),
			"removing the target must leave a dangling reparse entry")
	var dangling_transaction: Dictionary = FixturePublication.begin_fixture_publication(
			output_inside_link, trusted_root)
	assert_true(dangling_transaction.has("error"),
			"dangling links must be rejected even when exists() reports false")
	if not dangling_transaction.has("error"):
		FixturePublication.abort_fixture_publication(String(
				dangling_transaction.get("staging_path", "")))
	assert_eq(DirAccess.remove_absolute(pivot), OK)
	assert_eq(DirAccess.remove_absolute(trusted_root), OK)
	assert_eq(DirAccess.remove_absolute(container), OK)


func test_orphan_recovery_is_atomically_claimed_before_any_mutation() -> void:
	var nonce := "%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()]
	var trusted_root := ProjectSettings.globalize_path(
			"user://render-fixture-recovery-race-" + nonce)
	var final_dir := trusted_root.path_join("fixture")
	assert_eq(DirAccess.make_dir_recursive_absolute(final_dir), OK)
	var old_path := final_dir.path_join("old-evidence.txt")
	var old_file := FileAccess.open(old_path, FileAccess.WRITE)
	assert_not_null(old_file)
	old_file.store_string("old")
	old_file.close()
	var orphan: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root)
	assert_false(orphan.has("error"))
	var orphan_staging := String(orphan.get("staging_path", ""))
	assert_true(DirAccess.dir_exists_absolute(orphan_staging))
	var orphan_marker := orphan_staging.path_join("orphan-marker.txt")
	var marker_file := FileAccess.open(orphan_marker, FileAccess.WRITE)
	assert_not_null(marker_file)
	marker_file.store_string("orphan")
	marker_file.close()

	var race := {
		"competitor": {},
		"claimed_path": "",
		"claim_count": 0,
	}
	var owner_is_dead := func(_pid: int) -> bool:
		return false
	var owner_is_live := func(_pid: int) -> bool:
		return true
	var compete_after_claim := func(claim: String) -> void:
		race.claim_count = int(race.claim_count) + 1
		race.claimed_path = claim
		assert_true(DirAccess.dir_exists_absolute(claim),
				"the old journal must be moved under the winner's unique claim")
		race.competitor = FixturePublication.begin_fixture_publication(
				final_dir, trusted_root, owner_is_live)

	var winner: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root, owner_is_dead, compete_after_claim)
	assert_eq(int(race.claim_count), 1)
	assert_true((race.competitor as Dictionary).has("error"),
			"a competitor must not mutate a journal owned by a live recovery claim")
	assert_false(winner.has("error"),
			"the atomic claim winner must recover before accepting new work")
	assert_true(FileAccess.file_exists(old_path))
	assert_false(FileAccess.file_exists(orphan_marker),
			"the winner must discard the old orphan staging before creating its own")
	assert_false(DirAccess.dir_exists_absolute(String(race.claimed_path)))
	var winner_staging := String(winner.get("staging_path", ""))
	assert_true(DirAccess.dir_exists_absolute(winner_staging))
	assert_eq(FixturePublication.abort_fixture_publication(winner_staging), OK)


func test_orphan_recovery_revalidates_owner_liveness_after_the_atomic_claim() -> void:
	var nonce := "%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()]
	var trusted_root := ProjectSettings.globalize_path(
			"user://render-fixture-recovery-liveness-" + nonce)
	var final_dir := trusted_root.path_join("fixture")
	assert_eq(DirAccess.make_dir_recursive_absolute(final_dir), OK)
	var transaction: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root)
	assert_false(transaction.has("error"))
	var staging := String(transaction.get("staging_path", ""))
	var marker := staging.path_join("owner-work.txt")
	var marker_file := FileAccess.open(marker, FileAccess.WRITE)
	assert_not_null(marker_file)
	marker_file.store_string("still owned")
	marker_file.close()

	var liveness := {"checks": 0}
	var becomes_live_after_claim := func(_pid: int) -> bool:
		liveness.checks = int(liveness.checks) + 1
		# One preflight happens in the caller and one in the atomic-claim seam.
		# The third observation is deliberately the post-rename revalidation.
		return int(liveness.checks) >= 3
	var contender: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root, becomes_live_after_claim)
	assert_true(contender.has("error"))
	assert_eq(int(liveness.checks), 3)
	assert_true(FileAccess.file_exists(marker),
			"a revived owner must regain its original journal without mutation")
	assert_true(DirAccess.dir_exists_absolute(staging))
	assert_eq(FixturePublication.abort_fixture_publication(staging), OK)


func test_empty_terminal_journals_are_recoverable_after_owner_release() -> void:
	var nonce := "%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()]
	var trusted_root := ProjectSettings.globalize_path(
			"user://render-fixture-empty-journal-" + nonce)
	var final_dir := trusted_root.path_join("fixture")
	assert_eq(DirAccess.make_dir_recursive_absolute(final_dir), OK)
	var transaction: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root)
	assert_false(transaction.has("error"))
	var staging := String(transaction.get("staging_path", ""))
	var journal := staging.get_base_dir()
	assert_eq(DirAccess.remove_absolute(staging), OK)
	assert_eq(DirAccess.remove_absolute(journal.path_join("owner.json")), OK)
	assert_true(DirAccess.dir_exists_absolute(journal))

	var recovered: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root)
	assert_false(recovered.has("error"),
			"an empty journal is the normal terminal cleanup crash state")
	assert_eq(FixturePublication.abort_fixture_publication(
			String(recovered.get("staging_path", ""))), OK)

	# The same owner-last cleanup rule applies after an orphan was renamed under
	# a recovery claim and completed there.
	transaction = FixturePublication.begin_fixture_publication(final_dir, trusted_root)
	assert_false(transaction.has("error"))
	staging = String(transaction.get("staging_path", ""))
	journal = staging.get_base_dir()
	assert_eq(DirAccess.remove_absolute(staging), OK)
	assert_eq(DirAccess.remove_absolute(journal.path_join("owner.json")), OK)
	var abandoned_claim := "%s.recovering.%d.%d" % [
		journal, OS.get_process_id(), Time.get_ticks_usec(),
	]
	assert_eq(DirAccess.rename_absolute(journal, abandoned_claim), OK)
	var owner_is_live := func(_pid: int) -> bool:
		return true
	recovered = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root, owner_is_live)
	assert_false(recovered.has("error"),
			"owner release makes an empty claim terminal even before process exit")
	assert_false(DirAccess.dir_exists_absolute(abandoned_claim))
	assert_eq(FixturePublication.abort_fixture_publication(
			String(recovered.get("staging_path", ""))), OK)


func test_marker_write_and_cleanup_failure_leave_a_recoverable_rollback() -> void:
	var nonce := "%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()]
	var trusted_root := ProjectSettings.globalize_path(
			"user://render-fixture-rollback-marker-" + nonce)
	var final_dir := trusted_root.path_join("fixture")
	assert_eq(DirAccess.make_dir_recursive_absolute(final_dir), OK)
	var old_path := final_dir.path_join("old-evidence.txt")
	var old_file := FileAccess.open(old_path, FileAccess.WRITE)
	assert_not_null(old_file)
	old_file.store_string("old")
	old_file.close()
	var transaction: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root)
	assert_false(transaction.has("error"))
	var staging := String(transaction.get("staging_path", ""))
	var new_file := FileAccess.open(
			staging.path_join("new-evidence.txt"), FileAccess.WRITE)
	assert_not_null(new_file)
	new_file.store_string("new")
	new_file.close()
	var marker_failure := func(_path: String, _value: Dictionary) -> Error:
		return ERR_CANT_CREATE
	var cleanup_interruption := func(
			_root: String, _output: String, _trusted: String) -> Error:
		return ERR_BUSY
	assert_eq(FixturePublication.commit_fixture_publication(
			staging, final_dir, Callable(), Callable(),
			marker_failure, cleanup_interruption), ERR_BUSY)
	assert_true(FileAccess.file_exists(old_path),
			"the prior fixture must already be restored before cleanup")
	assert_false(FileAccess.file_exists(final_dir.path_join("new-evidence.txt")))

	var owner_is_dead := func(_pid: int) -> bool:
		return false
	var recovered: Dictionary = FixturePublication.begin_fixture_publication(
			final_dir, trusted_root, owner_is_dead)
	assert_false(recovered.has("error"),
			"the durable rollback marker must make this crash state recoverable")
	assert_true(FileAccess.file_exists(old_path))
	assert_eq(FixturePublication.abort_fixture_publication(
			String(recovered.get("staging_path", ""))), OK)


func test_fixture_ids_and_publication_labels_cannot_escape_the_transaction_root() -> void:
	var nonce := "%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()]
	var malicious_id := "../escaped-" + nonce
	assert_true(RenderFixtureContract.fixture_by_id({
		"fixtures": [{"id": malicious_id, "mission": "CP12.bms"}],
	}, malicious_id).is_empty(),
			"catalog lookup must reject non-canonical fixture IDs")

	var parent := ProjectSettings.globalize_path(
			"user://render-fixture-confinement-" + nonce)
	var staging := parent.path_join("staging")
	assert_eq(DirAccess.make_dir_recursive_absolute(staging), OK)
	var escaped_label := "escaped-" + nonce
	var escaped_png := parent.path_join(escaped_label + ".png")
	var escaped_state := parent.path_join(escaped_label + ".state.json")
	DirAccess.remove_absolute(escaped_png)
	DirAccess.remove_absolute(escaped_state)
	assert_true(FixturePublication.publication_child_path(
			staging, "../%s-manifest.json" % escaped_label).is_empty(),
			"post-join confinement must reject a manifest path outside staging")

	var source_png := parent.path_join("source.png")
	var source_state := parent.path_join("source.state.json")
	var png_file := FileAccess.open(source_png, FileAccess.WRITE)
	assert_not_null(png_file)
	png_file.store_buffer(PackedByteArray([21, 22, 23]))
	png_file.close()
	var png_sha256 := FileAccess.get_sha256(source_png)
	var diagnostics := {
		"schema": "OpenNovaRenderDiagnosticsV1",
		"frame": {"process": 81},
	}
	var state_file := FileAccess.open(source_state, FileAccess.WRITE)
	assert_not_null(state_file)
	state_file.store_string(JSON.stringify({
		"schema": "OpenNovaRenderCaptureV1",
		"capture": {
			"id": "confinement-source-81",
			"label": "raw",
			"process_frame": 81,
			"captured_at_ticks_usec": 123459,
			"png_path": source_png,
			"png_sha256": png_sha256,
			"width": 2000,
			"height": 1200,
			"mime": "image/png",
		},
		"diagnostics": diagnostics,
	}))
	state_file.close()
	var probe := Probe.new()
	var row: Dictionary = probe.publish_bundle({
		"schema": "OpenNovaRenderCaptureV1",
		"capture_id": "confinement-source-81",
		"artifact": {
			"png_path": source_png,
			"state_path": source_state,
			"sha256": png_sha256,
			"state_sha256": FileAccess.get_sha256(source_state),
			"width": 2000,
			"height": 1200,
			"mime": "image/png",
		},
		"diagnostics": diagnostics,
	}, staging, "../" + escaped_label)
	assert_true(row.has("error"),
			"bundle publication must reject a non-canonical label before writing")
	assert_false(FileAccess.file_exists(escaped_png))
	assert_false(FileAccess.file_exists(escaped_state))


func test_fixture_lookup_and_capture_variants_are_bounded() -> void:
	var catalog := {
		"fixtures": [
			{"id": "first", "mission": "A.bms"},
			{"id": "second", "mission": "B.bms"},
		],
	}
	assert_eq(RenderFixtureContract.fixture_by_id(catalog, "second").get("mission"), "B.bms")
	assert_true(RenderFixtureContract.fixture_by_id(catalog, "missing").is_empty())
	assert_eq(RenderFixtureContract.capture_variants().map(func(row): return row.id), [
		"beauty", "shadows_off", "lighting_only", "unshaded",
		"directional_shadow_atlas",
	])


func test_live_warmup_suspends_only_static_projection_until_exact_refresh() -> void:
	# The transaction is typed to the real Terrain: it reads the static-shadow
	# provider's own flag and restores exactly what it found.
	var terrain: Terrain = autofree(Terrain.new())
	terrain.set_static_terrain_shadow_enabled(true)
	var suspension = Probe.StaticTerrainShadowWarmupSuspension.new()
	assert_eq(suspension.begin(terrain), OK)
	assert_false(terrain.is_static_terrain_shadow_enabled(),
			"the live load warmup runs without static page projection")
	assert_eq(suspension.begin(terrain), ERR_ALREADY_IN_USE,
			"one transaction at a time")
	suspension.finish()
	assert_true(terrain.is_static_terrain_shadow_enabled(),
			"finish restores the provider the transaction found enabled")
	suspension.finish()
	assert_true(terrain.is_static_terrain_shadow_enabled(),
			"failure cleanup may finish the transaction more than once")
	assert_eq(suspension.begin(null), ERR_UNCONFIGURED, "no terrain: nothing to suspend")

	var already_disabled: Terrain = autofree(Terrain.new())
	already_disabled.set_static_terrain_shadow_enabled(false)
	var disabled_suspension = Probe.StaticTerrainShadowWarmupSuspension.new()
	assert_eq(disabled_suspension.begin(already_disabled), OK)
	assert_false(already_disabled.is_static_terrain_shadow_enabled())
	disabled_suspension.finish()
	assert_false(already_disabled.is_static_terrain_shadow_enabled(),
			"an originally disabled provider is restored disabled, never enabled")

	var source := FileAccess.get_file_as_string(
			"res://probes/render/render_fixture_capture_probe.gd")
	var boot := source.find("ctx.load_saved_mission(")
	var begin := source.find("_static_shadow_warmup_suspension.begin(", boot)
	var load_settle := source.find("\"load_settle_frames\"", boot)
	assert_gt(boot, -1)
	assert_gt(begin, boot)
	assert_gt(load_settle, begin,
			"static page projection must be off only during the live load warmup")
	var prepare := source.find("func _prepare_pose(")
	var freeze := source.find(
			"_world.process_mode = Node.PROCESS_MODE_DISABLED", prepare)
	var exact_camera := source.find("camera.make_current()", freeze)
	var restore := source.find(
			"_finish_static_shadow_warmup_suspension()", exact_camera)
	var refresh := source.find("_world.debug_refresh_render_pose(camera)", restore)
	assert_gt(freeze, prepare)
	assert_gt(exact_camera, freeze)
	assert_gt(restore, exact_camera)
	assert_gt(refresh, restore,
			"restore must precede the exact non-time-owning terrain refresh")
	var shutdown := source.find("func _teardown(")
	assert_gt(shutdown, restore)
	assert_true(source.substr(shutdown).contains(
			"_finish_static_shadow_warmup_suspension()"),
			"every deferred failure teardown must restore the provider")


func test_frozen_capture_realizes_each_shadow_variant_before_state_capture() -> void:
	var source := FileAccess.get_file_as_string(
			"res://probes/render/render_fixture_capture_probe.gd")
	var apply := source.find("_shadow_capture_session.apply_variant(variant)")
	var capture := source.find("adapter.capture_mcp_render_bundle({", apply)
	assert_gt(apply, -1)
	assert_gt(capture, apply)
	var between := source.substr(apply, capture - apply)
	assert_true(between.contains("_realize_capture_variant_cache("),
			"The frozen GameWorld must await exact async terrain pages after every " \
			+ "shadow control change and before the adapter snapshots state.")

	var shadows_off: Variant = RenderFixtureContract.capture_variants()[1]
	var stale_beauty := {
		"shadow_provider_enabled": false,
		"shadow_provider_frame_plan_count": 49,
		"shadow_provider_frame_plan_failures": 0,
		"shadow_provider_frame_raster_count": 48,
		"frame_requests": 49,
		"frame_compose_jobs": 48,
		"frame_ready_hits": 1,
		"frame_selected_ready_pages": 48,
		"frame_capacity_fallbacks": 0,
		"frame_shadow_alpha_changed_bytes": 73602,
		"frame_shadow_rgb_changed_bytes": 0,
		"ready_pages": 0,
	}
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(
			shadows_off, stale_beauty),
			"Disabled controls plus beauty-frame counters are not a realized shadows_off frame.")
	var realized_off := {
		"available": true,
		"tile_overlay_required": true,
		"tile_overlay_available": true,
		"shadow_raster_available": true,
		"upload_failures": 0,
		"shadow_raster_failures": 0,
		"shadow_provider_enabled": false,
		"shadow_provider_frame_plan_count": 0,
		"shadow_provider_frame_plan_failures": 0,
		"shadow_provider_frame_raster_count": 0,
		"frame_requests": 49,
		"frame_compose_jobs": 0,
		"frame_output_pages": 0,
		"frame_ready_hits": 49,
		"pending_jobs": 0,
		"frame_selected_ready_pages": 48,
		"frame_capacity_fallbacks": 0,
		"frame_shadow_alpha_changed_bytes": 0,
		"frame_shadow_rgb_changed_bytes": 0,
		"ready_pages": 48,
		"shadow_epoch_raster_jobs": 0,
	}
	assert_true(RenderFixtureContract.capture_variant_tile_cache_is_realized(
			shadows_off, realized_off))
	var no_overlay_required := realized_off.duplicate()
	no_overlay_required.tile_overlay_required = false
	no_overlay_required.tile_overlay_available = false
	assert_true(RenderFixtureContract.capture_variant_tile_cache_is_realized(
			shadows_off, no_overlay_required),
			"A terrain that does not require .til data remains valid without an overlay.")

	var beauty: Variant = RenderFixtureContract.capture_variants()[0]
	var realized_on := {
		"available": true,
		"tile_overlay_required": true,
		"tile_overlay_available": true,
		"shadow_raster_available": true,
		"upload_failures": 0,
		"shadow_raster_failures": 0,
		"shadow_provider_enabled": true,
		"shadow_provider_snapshot_exact": true,
		"shadow_provider_admitted_count": 17,
		"shadow_provider_resolved_casters": 17,
		"shadow_provider_frame_plan_count": 0,
		"shadow_provider_frame_plan_failures": 0,
		"shadow_provider_frame_raster_count": 0,
		"shadow_provider_frame_unsupported_draw_count": 0,
		"shadow_provider_frame_unsupported_attribution_truncated": 0,
		"shadow_provider_frame_pages_with_draws": 0,
		"shadow_provider_frame_projection_draws": 0,
		"frame_requests": 49,
		"frame_compose_jobs": 0,
		"frame_output_pages": 0,
		"frame_ready_hits": 49,
		"pending_jobs": 0,
		"frame_selected_ready_pages": 48,
		"frame_capacity_fallbacks": 0,
		"frame_shadow_alpha_changed_bytes": 73602,
		"frame_shadow_rgb_changed_bytes": 0,
		"ready_pages": 48,
		"shadow_epoch_raster_jobs": 48,
		"shadow_epoch_pages_with_draws": 15,
		"shadow_epoch_projection_draws": 145,
		"shadow_epoch_plan_failures": 0,
		"shadow_epoch_unsupported_draw_count": 0,
		"shadow_epoch_unsupported_attribution_truncated": 0,
	}
	assert_true(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on))
	var cache_hit_beauty := realized_on.duplicate()
	assert_true(RenderFixtureContract.capture_variant_tile_cache_is_realized(
			beauty, cache_hit_beauty),
			"A fully cached frame is exact after its shadow epoch was compiled.")
	cache_hit_beauty.frame_ready_hits = 48
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(
			beauty, cache_hit_beauty),
			"Every current request must resolve through composition or a ready hit.")
	cache_hit_beauty.frame_ready_hits = 49
	cache_hit_beauty.frame_selected_ready_pages = 0
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(
			beauty, cache_hit_beauty),
			"Retained global pages are not proof of a current-frame selection.")
	cache_hit_beauty.frame_selected_ready_pages = 48
	cache_hit_beauty.frame_capacity_fallbacks = 1
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(
			beauty, cache_hit_beauty),
			"A capacity fallback makes the evidence frame incomplete.")
	cache_hit_beauty.frame_capacity_fallbacks = 0
	realized_on.pending_jobs = 1
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"Capture must wait until every requested compile/upload is drained.")
	realized_on.pending_jobs = 0
	realized_on.tile_overlay_available = false
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"A mission that requires .til data must prove that its overlay is available.")
	realized_on.tile_overlay_available = true
	realized_on.upload_failures = 1
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"Cumulative upload failures make the evidence run inexact.")
	realized_on.upload_failures = 0
	realized_on.shadow_raster_failures = 1
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"Cumulative static-raster failures make the evidence run inexact.")
	realized_on.shadow_raster_failures = 0
	realized_on.shadow_epoch_pages_with_draws = 0
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"Enabled static shadows require at least one projected page witness.")
	realized_on.shadow_epoch_pages_with_draws = 15
	realized_on.shadow_epoch_projection_draws = 0
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"Enabled static shadows require realized projection draws.")
	realized_on.shadow_epoch_projection_draws = 145
	realized_on.shadow_provider_snapshot_exact = false
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"An incomplete static-caster snapshot must fail closed.")
	realized_on.shadow_provider_snapshot_exact = true
	realized_on.shadow_provider_admitted_count = 0
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"Enabled static evidence requires an admitted-caster inventory witness.")
	realized_on.shadow_provider_admitted_count = 17
	realized_on.shadow_provider_resolved_casters = 0
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"Enabled static evidence requires resolved caster geometry.")
	realized_on.shadow_provider_resolved_casters = 17
	realized_on.ready_pages = 0
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"An enabled provider without a resident current cache cannot back the capture.")
	realized_on.ready_pages = 48
	realized_on.shadow_epoch_plan_failures = 1
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"A rejected current page must stop evidence publication instead of hiding in a screenshot.")
	realized_on.shadow_epoch_plan_failures = 0
	realized_on.shadow_epoch_unsupported_draw_count = 1
	assert_false(RenderFixtureContract.capture_variant_tile_cache_is_realized(beauty, realized_on),
			"A skipped unsupported draw preserves gameplay but is not exact capture evidence.")


func test_every_capture_variant_must_match_its_post_capture_renderer_state() -> void:
	for variant in RenderFixtureContract.capture_variants():
		var diagnostics := {
			"world": {
				"loaded": true,
				"visible": true,
			},
			"terrain": {
				"available": true,
				"visible": true,
				"visible_in_tree": true,
			},
			"renderer": {"debug_draw": int(variant.debug_draw)},
			"shadows": {
				"dynamic": {
					"available": true,
					"visible": true,
					"visible_in_tree": true,
					"processing": true,
					"shadow_enabled": bool(variant.dynamic_shadow_enabled),
				},
				"static_terrain": {
					"available": true,
					"enabled": bool(variant.static_terrain_shadow_enabled),
					"suppressed_bms_ids": [],
				},
			},
		}
		var realized_variant := {
			"id": String(variant.id),
			"debug_draw": int(variant.debug_draw),
			"dynamic_shadow_enabled": bool(variant.dynamic_shadow_enabled),
			"static_terrain_shadow_enabled": bool(
					variant.static_terrain_shadow_enabled),
			"suppressed_dynamic_caster_bms_ids": [],
			"suppressed_static_caster_bms_ids": [],
		}
		assert_true(RenderFixtureContract.capture_variant_matches_diagnostics(
				variant, diagnostics, realized_variant),
				"variant %s must accept its exact realized state" % variant.id)
		var stale_dynamic: Dictionary = diagnostics.duplicate(true)
		stale_dynamic.shadows.dynamic.shadow_enabled = \
				not bool(variant.dynamic_shadow_enabled)
		assert_false(RenderFixtureContract.capture_variant_matches_diagnostics(
				variant, stale_dynamic, realized_variant),
				"variant %s must reject stale dynamic-shadow state" % variant.id)
		var stale_static: Dictionary = diagnostics.duplicate(true)
		stale_static.shadows.static_terrain.enabled = \
				not bool(variant.static_terrain_shadow_enabled)
		assert_false(RenderFixtureContract.capture_variant_matches_diagnostics(
				variant, stale_static, realized_variant),
				"variant %s must reject stale static-shadow state" % variant.id)
		var stale_debug: Dictionary = diagnostics.duplicate(true)
		stale_debug.renderer.debug_draw = int(variant.debug_draw) + 1
		assert_false(RenderFixtureContract.capture_variant_matches_diagnostics(
				variant, stale_debug, realized_variant),
				"variant %s must reject stale debug-draw state" % variant.id)
		for hidden_path in [
			["world", "visible"],
			["terrain", "visible"],
			["terrain", "visible_in_tree"],
			["shadows", "dynamic", "visible"],
			["shadows", "dynamic", "visible_in_tree"],
			["shadows", "dynamic", "processing"],
		]:
			var hidden: Dictionary = diagnostics.duplicate(true)
			if hidden_path.size() == 2:
				hidden[hidden_path[0]][hidden_path[1]] = false
			else:
				hidden[hidden_path[0]][hidden_path[1]][hidden_path[2]] = false
			assert_false(RenderFixtureContract.capture_variant_matches_diagnostics(
					variant, hidden, realized_variant),
					"variant %s must reject hidden/non-processing %s" % [
							variant.id, ".".join(hidden_path)])


func test_selected_caster_variants_require_exact_independently_realized_suppression() -> void:
	var variant = RenderFixtureContract.shadow_attribution_variants(PackedInt32Array([58]))[3]
	var diagnostics := {
		"world": {"loaded": true, "visible": true},
		"terrain": {"available": true, "visible": true, "visible_in_tree": true},
		"renderer": {"debug_draw": int(variant.debug_draw)},
		"shadows": {
			"dynamic": {
				"available": true,
				"visible": true,
				"visible_in_tree": true,
				"processing": true,
				"shadow_enabled": bool(variant.dynamic_shadow_enabled),
			},
			"static_terrain": {
				"available": true,
				"enabled": bool(variant.static_terrain_shadow_enabled),
				"suppressed_bms_ids": [58],
			},
		},
	}
	var realized_variant := {
		"id": String(variant.id),
		"debug_draw": int(variant.debug_draw),
		"dynamic_shadow_enabled": bool(variant.dynamic_shadow_enabled),
		"static_terrain_shadow_enabled": bool(
				variant.static_terrain_shadow_enabled),
		"suppressed_dynamic_caster_bms_ids": [],
		"suppressed_static_caster_bms_ids": [58],
	}
	assert_true(RenderFixtureContract.capture_variant_matches_diagnostics(
			variant, diagnostics, realized_variant))
	var request_echo_only := realized_variant.duplicate(true)
	request_echo_only.suppressed_static_caster_bms_ids = []
	assert_false(RenderFixtureContract.capture_variant_matches_diagnostics(
			variant, diagnostics, request_echo_only),
			"the request cannot substitute for the realized static provider set")
	var stale_provider := diagnostics.duplicate(true)
	stale_provider.shadows.static_terrain.suppressed_bms_ids = []
	assert_false(RenderFixtureContract.capture_variant_matches_diagnostics(
			variant, stale_provider, realized_variant),
			"captured renderer diagnostics must independently carry the exact static set")

	var dynamic_variant = RenderFixtureContract.shadow_attribution_variants()[3]
	var dynamic_diagnostics: Dictionary = diagnostics.duplicate(true)
	dynamic_diagnostics.shadows.dynamic.shadow_enabled = \
			bool(dynamic_variant.dynamic_shadow_enabled)
	dynamic_diagnostics.shadows.static_terrain.enabled = \
			bool(dynamic_variant.static_terrain_shadow_enabled)
	dynamic_diagnostics.shadows.static_terrain.suppressed_bms_ids = []
	var dynamic_realized := {
		"id": String(dynamic_variant.id),
		"debug_draw": int(dynamic_variant.debug_draw),
		"dynamic_shadow_enabled": bool(dynamic_variant.dynamic_shadow_enabled),
		"static_terrain_shadow_enabled": bool(
				dynamic_variant.static_terrain_shadow_enabled),
		"suppressed_dynamic_caster_bms_ids": [58],
		"suppressed_static_caster_bms_ids": [],
	}
	assert_true(RenderFixtureContract.capture_variant_matches_diagnostics(
			dynamic_variant, dynamic_diagnostics, dynamic_realized))
	dynamic_realized.suppressed_dynamic_caster_bms_ids = []
	assert_false(RenderFixtureContract.capture_variant_matches_diagnostics(
			dynamic_variant, dynamic_diagnostics, dynamic_realized),
			"dynamic suppression must be read from the actual ObjectModel caster bit")

	var source := FileAccess.get_file_as_string(
			"res://probes/render/render_fixture_capture_probe.gd")
	var camera_validation := source.find("func _valid_realized_camera(")
	var publication := source.find("func publish_bundle", camera_validation)
	assert_gt(camera_validation, -1)
	assert_gt(publication, camera_validation)
	assert_true(source.substr(
			camera_validation, publication - camera_validation).contains(
					"variant, diagnostics, realized_variant"),
			"post-capture diagnostics must be matched before bundle publication")


func test_shadow_attribution_profile_is_opt_in_and_scratch_only() -> void:
	var canonical: Dictionary = RenderFixtureContract.select_capture_profile("")
	assert_false(canonical.has("error"))
	assert_eq(String(canonical.id), "canonical")
	assert_false(bool(canonical.scratch_only))
	assert_eq((canonical.variants as Array).map(func(row): return row.id), [
		"beauty", "shadows_off", "lighting_only", "unshaded",
		"directional_shadow_atlas",
	])

	var attribution: Dictionary = RenderFixtureContract.select_capture_profile(
			"shadow_attribution")
	assert_false(attribution.has("error"))
	assert_eq(String(attribution.id), "shadow_attribution")
	assert_true(bool(attribution.scratch_only))
	var attribution_variants := attribution.variants as Array
	assert_eq(attribution_variants.map(func(row): return row.id), [
		"both_shadow_systems",
		"dynamic_shadow_only",
		"static_terrain_shadow_only",
		"dynamic_without_bms58",
		"shadows_off",
	])
	assert_true(attribution_variants[0].dynamic_shadow_enabled)
	assert_true(attribution_variants[0].static_terrain_shadow_enabled)
	assert_true(attribution_variants[1].dynamic_shadow_enabled)
	assert_false(attribution_variants[1].static_terrain_shadow_enabled)
	assert_false(attribution_variants[2].dynamic_shadow_enabled)
	assert_true(attribution_variants[2].static_terrain_shadow_enabled)
	assert_eq(Array(attribution_variants[3].suppressed_dynamic_caster_bms_ids),
			[58])
	assert_eq(Array(attribution_variants[3].suppressed_static_caster_bms_ids), [])
	assert_false(attribution_variants[4].dynamic_shadow_enabled)
	assert_false(attribution_variants[4].static_terrain_shadow_enabled)
	assert_true(RenderFixtureContract.capture_output_is_allowed(
			attribution, "C:/repo/.scratch/shadow-attribution/cp12",
			"C:/repo/.scratch"))
	assert_false(RenderFixtureContract.capture_output_is_allowed(
			attribution, "C:/repo/screenshots/parity/cp12",
			"C:/repo/.scratch"),
			"the unconfirmed attribution profile may not publish evidence")
	assert_true(RenderFixtureContract.capture_output_is_allowed(
			canonical, "C:/repo/.scratch/golden/render/cp12",
			"C:/repo/.scratch"))
	assert_false(RenderFixtureContract.capture_output_is_allowed(
			canonical, "C:/repo/.scratch", "C:/repo/.scratch"),
			"a fixture may never replace the trusted scratch root itself")
	assert_true(RenderFixtureContract.select_capture_profile("projected_slots").has("error"),
			"unwitnessed shadow profiles fail closed")

	var selected: Dictionary = RenderFixtureContract.select_capture_profile(
			"shadow_attribution", "77, 58,77")
	assert_false(selected.has("error"))
	var selected_variants := selected.variants as Array
	assert_eq(selected_variants.map(func(row): return row.id), [
		"both_shadow_systems",
		"dynamic_shadow_only",
		"static_terrain_shadow_only",
		"static_without_selected",
		"dynamic_without_bms58",
		"shadows_off",
	])
	assert_eq(Array(selected_variants[3].suppressed_static_caster_bms_ids),
			[58, 77], "comma-separated scratch selection is sorted and deduplicated")
	assert_true(RenderFixtureContract.select_capture_profile(
			"shadow_attribution", "58,nope").has("error"),
			"malformed BMS ids fail before a capture mutates render state")


func test_realized_tod_must_preserve_the_exact_requested_clock() -> void:
	var exact := {
		"environment": {
			"mission_minute_of_day": 720.0,
			"mission_time_fixed24": 12 * 0x1000000,
		},
	}
	assert_true(RenderFixtureContract.realized_tod_matches(exact, 720.0, 12 * 0x1000000))

	var drifted := exact.duplicate(true)
	drifted.environment.mission_minute_of_day = 720.000536441803
	drifted.environment.mission_time_fixed24 += 1
	assert_false(RenderFixtureContract.realized_tod_matches(
			drifted, 720.0, 12 * 0x1000000),
			"one weather tick makes pairwise fixture evidence non-identical")


func test_minute_filter_only_selects_a_declared_integer_witness() -> void:
	assert_eq(RenderFixtureContract.select_fixture_minutes([350, 720, 1320], ""), {
		"values": [350, 720, 1320],
	})
	assert_eq(RenderFixtureContract.select_fixture_minutes([350, 720, 1320], "720"), {
		"values": [720],
	})
	assert_true(RenderFixtureContract.select_fixture_minutes(
			[350, 720, 1320], "721").has("error"))
	assert_true(RenderFixtureContract.select_fixture_minutes(
			[350, 720, 1320], "720.0").has("error"),
			"the environment override is an integer selector, not a TOD value")
	assert_eq(RenderFixtureContract.select_fixture_minutes(
			[350.0, 720.0, 1320.0], "720"), {"values": [720]},
			"Godot JSON numerics are floats even when the catalog literal is integral")
	assert_true(RenderFixtureContract.select_fixture_minutes(
			[350.0, 720.5, 1320.0], "").has("error"),
			"fractional catalog minutes are not canonical witnesses")


func test_catalog_variants_must_exactly_match_the_capture_driver() -> void:
	var declared := [
		"beauty", "shadows_off", "lighting_only", "unshaded",
		"directional_shadow_atlas",
	]
	assert_true(RenderFixtureContract.diagnostic_variant_contract_matches(declared))

	var reordered := declared.duplicate()
	reordered.reverse()
	assert_false(RenderFixtureContract.diagnostic_variant_contract_matches(reordered),
			"variant order is part of reproducible evidence identity")
	assert_false(RenderFixtureContract.diagnostic_variant_contract_matches(
			declared.slice(0, declared.size() - 1)),
			"the driver may not silently omit a catalog variant")


func test_exact_fixture_pose_reseeds_the_public_weather_phase_owner() -> void:
	# The packaged world owns its Weather node: the pin reaches its
	# prepare_world_driven (the cloud, sway and lightning phase reset) through
	# the typed seam; no world is the parameter refusal.
	var world := WorldFixture.make_world(self)
	assert_not_null(world.get_weather_node(), "the packaged world carries its weather owner")
	assert_eq(RenderFixtureContract.pin_weather_phase(world), OK)
	assert_eq(RenderFixtureContract.pin_weather_phase(null), ERR_INVALID_PARAMETER)
	var contract: Dictionary = RenderFixtureContract.capture_phase_contract()
	assert_eq(contract.weather, "canonical_reset_at_requested_tod")
	assert_eq(contract.water_noise,
			"one_pose_refresh_then_frozen_noncanonical")
	assert_eq(contract.particles, "frozen_noncanonical")
	assert_eq(contract.pixel_metrics, "within_run_variants_only")


func test_active_water_capture_requires_the_exact_mirrored_camera_pose() -> void:
	var diagnostics := {
		"camera": {
			"global_transform": Transform3D(Basis.IDENTITY,
					Vector3(1000.0, 24.0, 30.0)),
		},
		"water": {
			"render_active": true,
			"mesh_visible": true,
			"height": 20.0,
			"reflection": {
				"available": true,
				"camera": {
					"global_transform": Transform3D(Basis.IDENTITY,
							Vector3(1000.0, 16.0, 30.0)),
				},
			},
		},
	}
	assert_true(RenderFixtureContract.realized_reflection_pose_matches(diagnostics))

	var stale := diagnostics.duplicate(true)
	stale.water.reflection.camera.global_transform = Transform3D(
			Basis.IDENTITY, Vector3(1000.001, 16.0, 30.0))
	assert_false(RenderFixtureContract.realized_reflection_pose_matches(stale),
			"a stale mirror from the presenter pose must reject the capture")

	var missing := diagnostics.duplicate(true)
	missing.water.reflection.available = false
	assert_false(RenderFixtureContract.realized_reflection_pose_matches(missing),
			"active water may not claim a capture without a reflection camera")

	var inactive := diagnostics.duplicate(true)
	inactive.water.render_active = false
	assert_true(RenderFixtureContract.realized_reflection_pose_matches(inactive),
			"missions without rendered water have no mirror-pose precondition")

	var hidden := diagnostics.duplicate(true)
	hidden.water.mesh_visible = false
	hidden.water.reflection.camera.global_transform = Transform3D(
			Basis.IDENTITY, Vector3(999.0, 15.0, 31.0))
	assert_true(RenderFixtureContract.realized_reflection_pose_matches(hidden),
			"an occluded strip is never sampled, so a stale mirror is no defect")

	var unwitnessed := diagnostics.duplicate(true)
	(unwitnessed.water as Dictionary).erase("mesh_visible")
	assert_false(RenderFixtureContract.realized_reflection_pose_matches(unwitnessed),
			"diagnostics without the strip-visibility witness cannot claim a capture")


func test_capture_requires_the_single_player_post_spawn_equivalent() -> void:
	var state := {
		"entry": "single_player_auto_spawn_after_splash",
		"observed_at": "before_fixture_freeze",
		"world_loaded": true,
		"world_loading": false,
		"runtime_playing": true,
		"local_player_spawned": true,
		"gameplay_input_active": true,
		"gameplay_camera_current": true,
		"spawn_or_menu_active": false,
	}
	assert_true(RenderFixtureContract.post_spawn_capture_state_matches(state))
	var mislabeled := state.duplicate(true)
	mislabeled.observed_at = "capture"
	assert_false(RenderFixtureContract.post_spawn_capture_state_matches(mislabeled),
			"the runtime is paused at capture; the playing witness must be timestamped")

	for key in [
		"world_loaded", "runtime_playing", "local_player_spawned",
		"gameplay_input_active", "gameplay_camera_current",
	]:
		var invalid := state.duplicate(true)
		invalid[key] = false
		assert_false(RenderFixtureContract.post_spawn_capture_state_matches(invalid),
				"%s is a required post-spawn witness" % key)
	var still_loading := state.duplicate(true)
	still_loading.world_loading = true
	assert_false(RenderFixtureContract.post_spawn_capture_state_matches(still_loading))
	var spawn_screen := state.duplicate(true)
	spawn_screen.spawn_or_menu_active = true
	assert_false(RenderFixtureContract.post_spawn_capture_state_matches(spawn_screen))
