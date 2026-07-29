extends GutTest

# NovaDebugOverlay: the shared F3 mission inspector. Most legacy tests drive
# its runtime-free panes; the player tests use a small public-contract fake so
# no listen-server auto-spawn can make the pose/no-player cases nondeterministic.
# MissionRuntime metadata and game/ONED host wiring are covered separately.

const OverlayScript := preload("res://engine/debug/nova_debug_overlay.gd")
const DebugViewContext := preload("res://engine/debug/nova_debug_view_context.gd")
# Pages mount under the sidebar shell's page host; option checkboxes are
# named after their registry id.
const PAGES := "DebugPanel/DebugFrame/DebugContent/DebugBody/PageHost"
const COLLISION_TOGGLE_PATH := NodePath(PAGES + "/Rounds/show_collision")
const PLAYER_POSITION_PATH := NodePath(PAGES + "/Player/PlayerPosition")
const PLAYER_ORIENTATION_PATH := NodePath(PAGES + "/Player/PlayerOrientation")
const PLAYER_DUMP_PATH := NodePath(PAGES + "/Player/DumpSnapshot")
const PLAYER_DUMP_STATUS_PATH := NodePath(PAGES + "/Player/PlayerDumpStatus")
const USER_POINTS_TOGGLE_PATH := NodePath(PAGES + "/Animation/show_user_points")
const OCCLUSION_TOGGLE_PATH := NodePath(PAGES + "/Occlusion/show_portal_faces")
const OCCLUSION_STATUS_PATH := NodePath(PAGES + "/Occlusion/OcclusionStatus")
const OCCLUSION_LIST_PATH := NodePath(PAGES + "/Occlusion/OcclusionBuildings")
const ROUNDS_STATUS_PATH := NodePath(PAGES + "/Rounds/RoundsStatus")
const ROUNDS_LIST_PATH := NodePath(PAGES + "/Rounds/RoundEvents")
const SKELETON_TOGGLE_PATH := NodePath(PAGES + "/Animation/show_skeletons")
const FOLIAGE_TOGGLE_PATH := NodePath(PAGES + "/Terrain/hide_foliage")


class FakePoseSim:
	extends Node

	var _has_player := false
	var _position := Vector3.ZERO
	var _yaw_deg := 0.0
	var _pitch_deg := 0.0
	var _view_roll_deg := 0.0
	var round_debug := { "events": [] }

	func set_player_pose(
			position: Vector3, yaw_deg: float, pitch_deg: float, view_roll_deg: float) -> void:
		_has_player = true
		_position = position
		_yaw_deg = yaw_deg
		_pitch_deg = pitch_deg
		_view_roll_deg = view_roll_deg

	func set_yaw_deg(value: float) -> void:
		_yaw_deg = value

	func has_local_player() -> bool:
		return _has_player

	func get_local_player_position() -> Vector3:
		return _position

	func get_local_player_yaw_deg() -> float:
		return _yaw_deg

	func get_local_player_pitch_deg() -> float:
		return _pitch_deg

	func get_local_player_view() -> Dictionary:
		return {
			"fov_h_deg": 82.0,
			"scope_engaged": false,
			"mounted": false,
			"fp_roll_deg": _view_roll_deg,
		}

	func get_logic_tick() -> int:
		return 4242

	func get_round_debug() -> Dictionary:
		return round_debug

	func get_present_snapshot() -> PackedFloat32Array:
		return PackedFloat32Array()

	func get_present_stride() -> int:
		return 1

	func get_entity_count() -> int:
		return 0

	func get_fired_events_snapshot() -> PackedByteArray:
		return PackedByteArray()

	func get_wac_state() -> Dictionary:
		return {}

	func get_mission_variables_snapshot() -> PackedInt32Array:
		return PackedInt32Array()

	func get_global_variables_snapshot() -> PackedInt32Array:
		return PackedInt32Array()

	func get_music_variables_snapshot() -> PackedInt32Array:
		return PackedInt32Array()


class FakePoseRuntime:
	extends Node

	var _sim := FakePoseSim.new()
	var _mission_file := "00TRe.bms"
	var _mission_name := "Training Grounds"

	func _init() -> void:
		add_child(_sim)

	func set_mission_identity(mission_file: String, mission_name: String) -> void:
		_mission_file = mission_file
		_mission_name = mission_name

	func get_sim() -> FakePoseSim:
		return _sim

	func is_playing() -> bool:
		return true

	func get_mission_file() -> String:
		return _mission_file

	func get_mission_name() -> String:
		return _mission_name


# A pose sim that also carries the occlusion debug surface, so the Occlusion
# tab has state to render while every other pane keeps its pose-fake behavior.
class FakeOcclusionSim:
	extends FakePoseSim
	var occlusion: Dictionary = {}
	func get_occlusion_debug() -> Dictionary:
		return occlusion


class FakeOcclusionRuntime:
	extends Node
	var sim := FakeOcclusionSim.new()
	func _init() -> void:
		add_child(sim)
	func get_sim() -> FakeOcclusionSim:
		return sim
	func is_playing() -> bool:
		return true
	func get_mission_file() -> String:
		return "00TRe.bms"
	func get_mission_name() -> String:
		return "Training Grounds"


func _make_pose_runtime() -> FakePoseRuntime:
	var runtime := FakePoseRuntime.new()
	add_child_autofree(runtime)
	return runtime


func _make_overlay() -> CanvasLayer:
	# A unique scratch config per overlay: page/width persistence must never
	# leak between tests through the shared user:// store.
	var config_path := "user://test_debug_overlay_%d.cfg" % Time.get_ticks_usec()
	_dumped_paths.append(ProjectSettings.globalize_path(config_path))
	var overlay: CanvasLayer = OverlayScript.new(config_path)
	add_child_autofree(overlay)
	return overlay


func _dictionary_vector3(value: Variant) -> Vector3:
	var record: Dictionary = value if value is Dictionary else {}
	return Vector3(
			float(record.get("x", 0.0)),
			float(record.get("y", 0.0)),
			float(record.get("z", 0.0)))


func test_rounds_tab_exposes_both_person_bone_sections() -> void:
	var runtime := _make_pose_runtime()
	runtime.get_sim().round_debug = {
		"events": [{
			"tick": 42,
			"kind": 0,
			"kind_name": "organic",
			"entity_handle": 7,
			"entity_name": "Target",
			"husk": false,
			"section": 14,
			"secondary_section": 3,
			"material": 19,
			"effect_tag_name": "flesh",
		}],
	}
	var overlay := _make_overlay()
	overlay.set_runtime(runtime)
	overlay.toggle()
	overlay.select_page(&"Rounds")

	var status := overlay.get_node(ROUNDS_STATUS_PATH) as Label
	assert_string_contains(status.text, "1 person bone hits")
	var list := overlay.get_node(ROUNDS_LIST_PATH) as ItemList
	assert_eq(list.item_count, 1)
	var row := list.get_item_text(0)
	assert_string_contains(row, "reaction bone 14")
	assert_string_contains(row, "damage zone 3")
	assert_string_contains(row, "mat 19 -> flesh")


func test_rounds_tab_names_unresolved_person_fallback() -> void:
	var runtime := _make_pose_runtime()
	runtime.get_sim().round_debug = {
		"events": [{
			"tick": 43,
			"kind": 0,
			"kind_name": "organic",
			"entity_handle": 8,
			"section": 1,
			"secondary_section": 1,
			"fallback": true,
			"material": 19,
			"effect_tag_name": "player",
		}],
	}
	var overlay := _make_overlay()
	overlay.set_runtime(runtime)
	overlay.toggle()
	overlay.select_page(&"Rounds")

	var status := overlay.get_node(ROUNDS_STATUS_PATH) as Label
	assert_string_contains(status.text, "0 person bone hits")
	assert_string_contains(status.text, "1 organic fallbacks")
	var list := overlay.get_node(ROUNDS_LIST_PATH) as ItemList
	var row := list.get_item_text(0)
	assert_string_contains(row, "neutral fallback sphere")
	assert_string_contains(row, "reaction stand-in 1")
	assert_false(row.contains("damage zone 1"))




func test_without_runtime_reports_no_mission() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	assert_true(overlay._status_label.visible, "no source - the overlay says so")
	var page_list := overlay.find_child("PageList", true, false) as ItemList
	assert_true(page_list.visible,
		"the page list stays usable (the perf page works from host-wide state, no sim needed)")
	var entity_list := overlay.find_child("EntityList", true, false) as ItemList
	assert_eq(entity_list.item_count, 0, "the sim-fed pages sit empty")

	overlay.set_runtime_source(func(): return null)
	overlay.refresh_now()
	assert_true(overlay._status_label.visible, "a null-returning source reads as no mission")








func test_transport_signal_stays_quiet_without_a_runtime() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	watch_signals(overlay)
	(overlay.find_child("SimPlay", true, false) as Button).pressed.emit()
	(overlay.find_child("SimStop", true, false) as Button).pressed.emit()
	assert_signal_not_emitted(overlay, "transport_used",
		"a press with nothing to act on announces nothing")












func test_skeleton_toggle_lives_on_the_animation_page() -> void:
	# Registry toggles need no runtime and only emit intent for the host to act
	# on (build/free the 3D view) — everything rides ONE generic channel now.
	# The View page is gone; the bone views live with the animation data.
	var overlay := _make_overlay()
	overlay.toggle()
	assert_null(overlay.find_child("View", true, false),
			"the View page is fully dissolved")
	var skeleton_check := overlay.get_node_or_null(SKELETON_TOGGLE_PATH) as CheckBox
	assert_not_null(skeleton_check, "the skeleton checkbox has a stable public node path")
	assert_false(skeleton_check.button_pressed, "it defaults off")

	watch_signals(overlay)
	skeleton_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "debug_option_changed",
			[&"show_skeletons", true])
	skeleton_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(overlay, "debug_option_changed",
			[&"show_skeletons", false])


func test_user_points_toggle_lives_on_the_animation_page() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	var user_points_check := overlay.get_node_or_null(USER_POINTS_TOGGLE_PATH) as CheckBox
	assert_not_null(user_points_check, "the user-point checkbox has a stable public node path")
	assert_false(user_points_check.button_pressed, "it defaults off")

	watch_signals(overlay)
	user_points_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "debug_option_changed",
			[&"show_user_points", true])


func test_collision_toggle_lives_on_the_rounds_page() -> void:
	# "Show collision" belongs with the other what-geometry-does-the-world-test
	# views on Rounds & collision, not on the dissolving View page.
	var overlay := _make_overlay()
	overlay.toggle()
	var collision_check := overlay.get_node_or_null(COLLISION_TOGGLE_PATH) as CheckBox
	assert_not_null(collision_check, "the collision checkbox has a stable public node path")
	assert_false(collision_check.button_pressed, "it defaults off")

	watch_signals(overlay)
	collision_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "debug_option_changed",
			[&"show_collision", true])


func test_occlusion_page_portal_toggle_rides_the_option_registry() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	var portals_check := overlay.get_node_or_null(OCCLUSION_TOGGLE_PATH) as CheckBox
	assert_not_null(portals_check, "the portal checkbox has a stable public node path")
	assert_false(portals_check.button_pressed, "it defaults off")

	watch_signals(overlay)
	portals_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "debug_option_changed",
			[&"show_portal_faces", true])


func test_set_option_syncs_the_owning_control_and_emits() -> void:
	# The programmatic write path is the SAME path a click takes: one emission,
	# and the page's control re-syncs without re-firing.
	var overlay := _make_overlay()
	watch_signals(overlay)
	overlay.set_option(&"hide_foliage", true)
	assert_signal_emitted_with_parameters(overlay, "debug_option_changed",
			[&"hide_foliage", true])
	assert_eq(get_signal_emit_count(overlay, "debug_option_changed"), 1)
	var foliage_check := overlay.get_node_or_null(FOLIAGE_TOGGLE_PATH) as CheckBox
	assert_true(foliage_check.button_pressed, "the page control re-synced")
	assert_eq(overlay.get_option_value(&"hide_foliage"), true)
	overlay.set_option(&"hide_foliage", true)
	assert_eq(get_signal_emit_count(overlay, "debug_option_changed"), 1,
			"a repeated value never re-fires")


func test_occlusion_tab_reports_frame_state() -> void:
	# The Occlusion tab renders the sim's get_occlusion_debug() snapshot: the
	# camera line, the frame counts, one row per building with its section mask
	# and culling stage, and the weld rows.
	var runtime := FakeOcclusionRuntime.new()
	add_child_autofree(runtime)
	runtime.sim.occlusion = {
		"active": true,
		"camera_indoors": true,
		"exterior_visible": false,
		"water_visible": false,
		"local_blink_flags": 0x2,
		"counts": {
			"instances": 2, "batched": 2, "visible": 1, "toc_culled": 1,
			"slots": 3, "window_groups": 1, "viewthru_groups": 0,
			"welds": 1, "culled_entities": 4,
		},
		"buildings": [
			{ "bms_id": 42, "batched": true, "visible": true, "open_flagged": false,
				"mask": 0xB, "has_open": false, "has_windows": true, "has_links": false,
				"records": 6, "windows": 2, "portals": 1, "links": 0 },
			{ "bms_id": 43, "batched": true, "visible": false, "open_flagged": false,
				"mask": 0, "has_open": false, "has_windows": false, "has_links": true,
				"records": 4, "windows": 0, "portals": 0, "links": 1 },
		],
		"welds": [
			{ "own_bms": 42, "own_section": 1, "other_bms": 43, "other_section": 2 },
		],
	}
	var overlay := _make_overlay()
	overlay.set_runtime(runtime)
	overlay.toggle()
	overlay.select_page(&"Occlusion")

	var status := overlay.get_node(OCCLUSION_STATUS_PATH) as Label
	assert_string_contains(status.text, "indoors", "the camera line reports the blink state")
	assert_string_contains(status.text, "1 drawn", "the batch split totals surface")
	assert_string_contains(status.text, "1 occluder-culled")
	assert_string_contains(status.text, "entities hidden 4")
	var list := overlay.get_node(OCCLUSION_LIST_PATH) as ItemList
	assert_eq(list.item_count, 3, "two building rows + one weld row")
	assert_string_contains(list.get_item_text(0), "mask 0000000B")
	assert_string_contains(list.get_item_text(0), "[W]")
	assert_string_contains(list.get_item_text(1), "occluder-culled")
	assert_string_contains(list.get_item_text(2), "weld  #42 s1 <-> #43 s2")


func test_occlusion_tab_without_debug_surface_shows_empty_state() -> void:
	# Harness sims without get_occlusion_debug (every pose fake) leave the tab
	# in its empty state instead of erroring.
	var runtime := _make_pose_runtime()
	var overlay := _make_overlay()
	overlay.set_runtime(runtime)
	overlay.toggle()
	overlay.select_page(&"Occlusion")
	var status := overlay.get_node(OCCLUSION_STATUS_PATH) as Label
	assert_string_contains(status.text, "No occlusion data")
	assert_eq((overlay.get_node(OCCLUSION_LIST_PATH) as ItemList).item_count, 0)


func test_hide_foliage_toggle_lives_on_the_terrain_page() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	var foliage_check := overlay.get_node_or_null(FOLIAGE_TOGGLE_PATH) as CheckBox
	assert_not_null(foliage_check, "the foliage checkbox has a stable public node path")
	assert_false(foliage_check.button_pressed, "it defaults off (foliage shown)")

	watch_signals(overlay)
	foliage_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "debug_option_changed",
			[&"hide_foliage", true])
	foliage_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(overlay, "debug_option_changed",
			[&"hide_foliage", false])



func test_player_tab_disables_dump_without_a_local_player() -> void:
	var overlay := _make_overlay()
	overlay.set_runtime(_make_pose_runtime())
	overlay.toggle()
	overlay.select_page(&"Player")

	var position_label := overlay.get_node_or_null(PLAYER_POSITION_PATH) as Label
	var orientation_label := overlay.get_node_or_null(PLAYER_ORIENTATION_PATH) as Label
	var dump_button := overlay.get_node_or_null(PLAYER_DUMP_PATH) as Button
	assert_not_null(position_label)
	assert_not_null(orientation_label)
	assert_not_null(dump_button)
	assert_string_contains(position_label.text, "No local player")
	assert_eq(orientation_label.text, "")
	assert_true(dump_button.disabled)
	watch_signals(overlay)
	assert_eq(overlay.dump_debug_snapshot(), "")
	assert_signal_not_emitted(overlay, "debug_snapshot_dumped")


func test_player_dump_reports_an_unwritable_target_without_success_signal() -> void:
	var runtime := _make_pose_runtime()
	runtime.get_sim().set_player_pose(Vector3.ZERO, 0.0, 0.0, 0.0)
	var overlay := _make_overlay()
	overlay.set_runtime(runtime)
	overlay.toggle()
	var status := overlay.get_node_or_null(PLAYER_DUMP_STATUS_PATH) as Label
	assert_not_null(status)

	var blocker_path := OS.get_cache_dir().path_join(
			"opennova_pose_blocker_%d.tmp" % Time.get_ticks_usec())
	var blocker := FileAccess.open(blocker_path, FileAccess.WRITE)
	assert_not_null(blocker)
	if blocker == null:
		return
	blocker.store_string("regular file, not a directory")
	blocker.close()
	_dumped_paths.append(blocker_path)

	watch_signals(overlay)
	var result: String = overlay.dump_debug_snapshot(
			blocker_path.path_join("pose.json"))
	assert_eq(result, "")
	assert_signal_not_emitted(overlay, "debug_snapshot_dumped")
	assert_string_contains(status.text, "Could not")


func test_player_tab_displays_and_dumps_a_fresh_authoritative_pose() -> void:
	var runtime := _make_pose_runtime()
	var sim := runtime.get_sim()
	var position_godot := Vector3(123.25, 4.5, -67.75)
	sim.set_player_pose(position_godot, 270.0, -8.75, 1.25)
	var camera_rig := Node3D.new()
	camera_rig.rotation_degrees = Vector3(0.0, 31.0, 0.0)
	add_child_autofree(camera_rig)
	var camera := Camera3D.new()
	camera.rotation_degrees = Vector3(-12.0, 0.0, 7.0)
	camera.fov = 73.0
	camera_rig.add_child(camera)
	var view_context := DebugViewContext.new()
	view_context.camera = camera
	view_context.camera_mode_known = true
	view_context.third_person = true
	var overlay := _make_overlay()
	overlay.set_runtime(runtime)
	overlay.set_view_context_source(func(): return view_context)
	overlay.toggle()
	overlay.select_page(&"Player")

	var position_label := overlay.get_node_or_null(PLAYER_POSITION_PATH) as Label
	var orientation_label := overlay.get_node_or_null(PLAYER_ORIENTATION_PATH) as Label
	var dump_button := overlay.get_node_or_null(PLAYER_DUMP_PATH) as Button
	var dump_status := overlay.get_node_or_null(PLAYER_DUMP_STATUS_PATH) as Label
	assert_not_null(position_label)
	assert_not_null(orientation_label)
	assert_not_null(dump_button)
	assert_not_null(dump_status)
	assert_false(dump_button.disabled)
	assert_string_contains(position_label.text, "123.250")
	assert_string_contains(position_label.text, "67.750",
			"BMS y is the inverse of Godot z")
	assert_string_contains(position_label.text, "4.500",
			"BMS z is Godot's up axis")
	assert_string_contains(orientation_label.text, "270.000")
	assert_string_contains(orientation_label.text, "-8.750")
	assert_string_contains(orientation_label.text, "1.250")
	assert_string_contains(orientation_label.text, "third person")

	# The click must resample NOW rather than serialize the 0.25-Hz label cache.
	var displayed_yaw := sim.get_local_player_yaw_deg()
	sim.set_yaw_deg(278.5)
	var sampled_yaw := sim.get_local_player_yaw_deg()
	var camera_godot := Vector3(124.0, 6.0, -70.0)
	camera.global_position = camera_godot
	assert_ne(sampled_yaw, displayed_yaw)
	overlay.debug_snapshot_dumped.connect(
			func(path: String): _dumped_paths.append(path))
	watch_signals(overlay)
	dump_button.pressed.emit()
	assert_signal_emitted(overlay, "debug_snapshot_dumped")
	var dumped_path := String(
			get_signal_parameters(overlay, "debug_snapshot_dumped", 0)[0])
	assert_true(FileAccess.file_exists(dumped_path))
	assert_string_contains(dumped_path.get_file(), "00TRe")

	var file := FileAccess.open(dumped_path, FileAccess.READ)
	assert_not_null(file)
	var payload_variant: Variant = JSON.parse_string(file.get_as_text()) if file != null else null
	if file != null:
		file.close()
	assert_typeof(payload_variant, TYPE_DICTIONARY)
	var payload: Dictionary = payload_variant if payload_variant is Dictionary else {}
	assert_eq(String(payload.get("schema", "")), "opennova.debug_snapshot.v1")
	assert_true(String(payload.get("captured_at_utc", "")).ends_with("Z"))
	assert_eq(int(payload.get("logic_tick", -1)), 4242)
	assert_eq(payload.get("picks", null), [], "no pick list injected -> an empty picks array")
	assert_eq(int(payload.get("pick_count", -1)), 0)
	var mission: Dictionary = payload.get("mission", {})
	assert_eq(String(mission.get("file", "")), "00TRe.bms")
	assert_eq(String(mission.get("name", "")), "Training Grounds")
	var player: Dictionary = payload.get("player", {})
	var godot_position: Dictionary = player.get("position_godot", {})
	var bms_position: Dictionary = player.get("position_bms", {})
	var orientation: Dictionary = player.get("orientation_mission_deg", {})
	assert_almost_eq(float(godot_position.get("x", 0.0)), position_godot.x, 0.0001)
	assert_almost_eq(float(godot_position.get("y", 0.0)), position_godot.y, 0.0001)
	assert_almost_eq(float(godot_position.get("z", 0.0)), position_godot.z, 0.0001)
	assert_almost_eq(float(bms_position.get("x", 0.0)), 123.25, 0.0001)
	assert_almost_eq(float(bms_position.get("y", 0.0)), 67.75, 0.0001)
	assert_almost_eq(float(bms_position.get("z", 0.0)), 4.5, 0.0001)
	assert_almost_eq(float(orientation.get("yaw", 0.0)), sampled_yaw, 0.0001,
			"the disk snapshot uses the click-time yaw")
	assert_almost_eq(float(orientation.get("pitch", 0.0)), -8.75, 0.0001)
	assert_almost_eq(float(orientation.get("view_roll", 0.0)), 1.25, 0.0001)
	var player_forward := _dictionary_vector3(player.get("forward_godot", {}))
	assert_almost_eq(player_forward.length(), 1.0, 0.0001)

	var view: Dictionary = payload.get("view", {})
	assert_almost_eq(float(view.get("fov_horizontal_deg", 0.0)), 82.0, 0.0001)
	assert_false(bool(view.get("scope_engaged", true)))
	assert_false(bool(view.get("mounted", true)))
	var camera_snapshot: Dictionary = view.get("camera", {})
	assert_eq(String(camera_snapshot.get("mode", "")), "third_person")
	var dumped_camera_godot := _dictionary_vector3(
			camera_snapshot.get("position_godot", {}))
	var dumped_camera_bms := _dictionary_vector3(
			camera_snapshot.get("position_bms", {}))
	assert_true(dumped_camera_godot.is_equal_approx(camera_godot),
			"the camera transform is sampled at click time")
	assert_true(dumped_camera_bms.is_equal_approx(Vector3(124.0, 70.0, 6.0)))
	var expected_camera_basis := camera.global_transform.basis.orthonormalized()
	var camera_forward := _dictionary_vector3(
			camera_snapshot.get("forward_godot", {}))
	var camera_up := _dictionary_vector3(camera_snapshot.get("up_godot", {}))
	assert_true(camera_forward.is_equal_approx(-expected_camera_basis.z),
			"the dump uses the camera's rotated world basis")
	assert_true(camera_up.is_equal_approx(expected_camera_basis.y))
	var quaternion: Dictionary = camera_snapshot.get(
			"orientation_quaternion_godot", {})
	var dumped_quaternion := Quaternion(
			float(quaternion.get("x", 0.0)),
			float(quaternion.get("y", 0.0)),
			float(quaternion.get("z", 0.0)),
			float(quaternion.get("w", 0.0)))
	var expected_quaternion := expected_camera_basis.get_rotation_quaternion()
	assert_almost_eq(absf(dumped_quaternion.dot(expected_quaternion)), 1.0, 0.0001)
	assert_almost_eq(float(camera_snapshot.get("godot_fov_deg", 0.0)), 73.0, 0.0001)
	var viewport_size: Dictionary = camera_snapshot.get("viewport_size", {})
	var viewport_width := float(viewport_size.get("x", 0.0))
	var viewport_height := float(viewport_size.get("y", 0.0))
	assert_gt(viewport_width, 0.0)
	assert_gt(viewport_height, 0.0)
	assert_almost_eq(float(camera_snapshot.get("viewport_aspect", 0.0)),
			viewport_width / viewport_height, 0.0001)

	var second_path: String = overlay.dump_debug_snapshot()
	assert_ne(second_path, dumped_path, "rapid consecutive snapshots never overwrite")
	assert_true(FileAccess.file_exists(second_path))
	_dumped_paths.append(second_path)

	var next_runtime := _make_pose_runtime()
	next_runtime.set_mission_identity("00TRa.bms", "Second Training Area")
	next_runtime.get_sim().set_player_pose(Vector3.ZERO, 0.0, 0.0, 0.0)
	overlay.set_runtime(next_runtime)
	assert_string_contains(dump_status.text, "No snapshot saved",
			"a live mission swap clears the previous mission's Saved path")
	assert_false(dump_status.text.contains(dumped_path))


const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
var _saved_resource_dir := ""
var _dumped_paths: Array[String] = []


func before_all() -> void:
	_saved_resource_dir = ResourceDirSettings.get_resource_dir()
	ResourceDirSettings.set_resource_dir("")


func after_each() -> void:
	for path in _dumped_paths:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	_dumped_paths.clear()


func after_all() -> void:
	ResourceDirSettings.set_resource_dir(_saved_resource_dir)



# --- Particles tab (the retail particle debug pages, mimicked; ptl-format-re.md §11) ---

func test_particles_page_toggles_ride_the_option_registry() -> void:
	# Same host-neutral contract as every registry toggle: the checkboxes only
	# emit intent; the host hides the effect world / builds the box view. All
	# access rides stable node names (ADR 0018 — no private pokes).
	var overlay := _make_overlay()
	overlay.toggle()
	assert_not_null(overlay.find_child("Particles", true, false), "a Particles page exists")
	var hide_check := overlay.find_child("hide_particles", true, false) as CheckBox
	var boxes_check := overlay.find_child("show_effect_boxes", true, false) as CheckBox
	assert_not_null(hide_check, "the hide checkbox has a stable node name")
	assert_not_null(boxes_check, "the boxes checkbox has a stable node name")
	assert_false(hide_check.button_pressed, "hide defaults off")
	assert_false(boxes_check.button_pressed, "boxes default off")

	watch_signals(overlay)
	hide_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "debug_option_changed",
			[&"hide_particles", true])
	boxes_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "debug_option_changed",
			[&"show_effect_boxes", true])


func test_particles_tab_reports_counts_and_peak_reset() -> void:
	# The counts header keeps retail's current/peak form INCLUDING the peak
	# reset when the current count hits zero [orig: Debug_DrawParticleStats
	# @ 0x44c840 — dword_A895E0 zeroes with the count].
	var overlay := _make_overlay()
	overlay.toggle()
	overlay.select_page(&"Particles")
	var stub := _StubEffectWorld.new()
	add_child_autofree(stub)
	overlay.set_effect_world_source(func(): return stub)
	var counts := overlay.find_child("ParticleCounts", true, false) as Label
	var groups := overlay.find_child("ParticleGroups", true, false) as ItemList
	assert_not_null(counts, "the counts label has a stable node name")
	assert_not_null(groups, "the group list has a stable node name")

	stub.alive = 7
	overlay.refresh_now()
	assert_string_contains(counts.text, "7 / 7", "current and peak track the live count")
	assert_eq(groups.item_count, 3, "group header + emitter row + missing-texture row")

	stub.alive = 3
	overlay.refresh_now()
	assert_string_contains(counts.text, "3 / 7", "the peak latches")

	stub.alive = 0
	overlay.refresh_now()
	assert_string_contains(counts.text, "0 / 0", "the peak resets at zero, like retail")


class _StubEffectWorld extends Node3D:
	var alive := 0

	func active_entry_count() -> int:
		return alive

	func effect_count() -> int:
		return 2

	func get_debug_group_report() -> Array:
		return [{
			"id": 1,
			"name": "puff",
			"source": "stock.ptl",
			"forever": false,
			"emitters": [{"name": "Fx0_0", "alive": alive, "rendered": alive, "node": self}],
		}]

	func get_unresolved_texture_names() -> PackedStringArray:
		return PackedStringArray(["SMOKE1.TGA"])


# --- The sidebar shell (page framework) --------------------------------------

func _page_list_texts(overlay: CanvasLayer) -> PackedStringArray:
	var list := overlay.find_child("PageList", true, false) as ItemList
	var texts := PackedStringArray()
	for i in range(list.item_count):
		texts.append(list.get_item_text(i).strip_edges())
	return texts


func test_sidebar_lists_every_page_under_its_category() -> void:
	var overlay := _make_overlay()
	var list := overlay.find_child("PageList", true, false) as ItemList
	assert_not_null(list, "the shell carries the page list")
	var texts := _page_list_texts(overlay)
	for header in ["SIMULATION", "WORLD", "PLAYER", "DIAGNOSTICS"]:
		assert_has(texts, header, "the %s section header is present" % header)
	for page_title in ["Entities", "Sim", "Vars", "Net", "Particles", "Occlusion",
			"Rounds & collision", "Terrain & foliage", "Animation & models",
			"Player", "Stats", "Perf"]:
		assert_has(texts, page_title, "the %s page is listed" % page_title)
	assert_false(texts.has("View"), "the dissolved View page is gone")
	var header_row := texts.find("SIMULATION")
	assert_false(list.is_item_selectable(header_row), "section headers are not rows")


func test_selection_and_width_persist_across_instances() -> void:
	var config_path := "user://test_debug_overlay_persist_%d.cfg" % Time.get_ticks_usec()
	_dumped_paths.append(ProjectSettings.globalize_path(config_path))
	var first: CanvasLayer = OverlayScript.new(config_path)
	add_child(first)
	assert_eq(String(first.get_active_page_id()), "Entities",
			"a fresh config lands on the first page")
	assert_true(first.select_page(&"Rounds"))
	# Resize through the public handle node: press, drag 80 px left, release.
	var handle := first.find_child("DebugResizeHandle", true, false) as Control
	var panel := first.find_child("DebugPanel", true, false) as Control
	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_LEFT
	press.pressed = true
	handle.gui_input.emit(press)
	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(-80, 0)
	handle.gui_input.emit(motion)
	var release := InputEventMouseButton.new()
	release.button_index = MOUSE_BUTTON_LEFT
	release.pressed = false
	handle.gui_input.emit(release)
	var widened := -panel.offset_left
	assert_gt(widened, 560.0, "dragging the handle left widens the panel")
	remove_child(first)
	first.free()

	var second: CanvasLayer = OverlayScript.new(config_path)
	add_child_autofree(second)
	assert_eq(String(second.get_active_page_id()), "Rounds",
			"the last-selected page survives a relaunch")
	var second_panel := second.find_child("DebugPanel", true, false) as Control
	assert_almost_eq(-second_panel.offset_left, widened, 0.01,
			"the panel width survives a relaunch")


class TestHostPage:
	extends NovaDebugPage
	var refreshed := 0

	func page_id() -> StringName:
		return &"HostExtras"

	func page_title() -> String:
		return "Host extras"

	func page_category() -> StringName:
		return &"Host"

	func refresh() -> void:
		refreshed += 1


func test_register_page_appends_a_host_page() -> void:
	var overlay := _make_overlay()
	var page := TestHostPage.new()
	overlay.register_page(page)
	overlay.toggle()
	assert_true(overlay.select_page(&"HostExtras"), "the registered page selects by id")
	assert_gt(page.refreshed, 0, "selection refreshes the newly active page")
	var texts := _page_list_texts(overlay)
	assert_has(texts, "HOST", "a custom category grows its own section")
	assert_has(texts, "Host extras", "the page lists under it")


# --- The pick list + snapshot embedding ---------------------------------------

func _fabricated_pick(handle: int, pick_name: String) -> Dictionary:
	return {
		"hit": true, "entity_handle": handle, "pool": 2, "kind": 2,
		"index": handle, "bms_id": 1400 + handle, "net_id": 0, "item_id": 55,
		"name": pick_name, "position_godot": Vector3(10, 2, -30),
		"bound_radius": 4.0, "hit_position_godot": Vector3(10, 3, -30),
		"hit_normal_godot": Vector3.UP, "distance_units": 45.5,
		"hit_class": "static", "section": 1, "face": 17, "bone": -1,
		"hit_zone": -1, "surface_type": 3, "material_flags": 0, "tick": 777,
		"source": "crosshair", "ray_origin_godot": Vector3(0, 2, 0),
		"ray_dir_godot": Vector3(0, 0, -1),
	}


func test_snapshot_embeds_the_pick_list() -> void:
	var runtime := _make_pose_runtime()
	runtime.get_sim().set_player_pose(Vector3(1, 2, 3), 90.0, 0.0, 0.0)
	var overlay := _make_overlay()
	overlay.set_runtime(runtime)
	var picks := NovaDebugPickList.new()
	picks.add(_fabricated_pick(9, "RckS07"))
	overlay.set_pick_list(picks)

	var target := OS.get_cache_dir().path_join(
			"opennova_snapshot_%d.json" % Time.get_ticks_usec())
	_dumped_paths.append(target)
	var path: String = overlay.dump_debug_snapshot(target)
	assert_ne(path, "", "the dump succeeded")
	var file := FileAccess.open(path, FileAccess.READ)
	var payload: Dictionary = JSON.parse_string(file.get_as_text())
	file.close()

	assert_eq(int(payload.get("pick_count", -1)), 1)
	var entry: Dictionary = (payload.get("picks", []) as Array)[0]
	var identity: Dictionary = entry.get("identity", {})
	assert_eq(int(identity.get("bms_id", 0)), 1409)
	assert_eq(String(identity.get("name", "")), "RckS07")
	var pick_block: Dictionary = entry.get("pick", {})
	assert_eq(String(pick_block.get("source", "")), "crosshair",
			"the card keeps its input provenance")
	assert_almost_eq(float(pick_block.get("distance_units", 0.0)), 45.5, 0.001)
	var hit_bms: Dictionary = pick_block.get("hit_position_bms", {})
	assert_almost_eq(float(hit_bms.get("y", 0.0)), 30.0, 0.001,
			"the hit point converts to BMS space (y = -Godot z)")
	assert_true(bool(entry.get("stale", false)),
			"the pose fake exposes no live cards, so the pick reports stale")


func test_entities_page_renders_and_curates_the_pick_list() -> void:
	var overlay := _make_overlay()
	var picks := NovaDebugPickList.new()
	picks.add(_fabricated_pick(1, "crate_a"))
	picks.add(_fabricated_pick(2, "crate_b"))
	overlay.set_pick_list(picks)
	overlay.toggle()  # Entities is the default page; opening refreshes it

	var rows := overlay.find_child("PickRows", true, false)
	assert_eq(rows.get_child_count(), 2, "one row per pick")
	var header := overlay.find_child("PicksHeader", true, false) as Label
	assert_string_contains(header.text, "(2/8)")
	assert_string_contains(
			(rows.get_child(0).get_node("PickLabel") as Label).text, "crate_a")

	(rows.get_child(0).get_node("RemovePick") as Button).pressed.emit()
	assert_eq(picks.size(), 1, "the row's X removes exactly that pick")
	assert_eq(int(picks.get_picks()[0].get("entity_handle", -1)), 2)
	(overlay.find_child("ClearPicks", true, false) as Button).pressed.emit()
	assert_eq(picks.size(), 0, "Clear picks empties the set")
