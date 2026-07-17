extends GutTest

# NovaDebugOverlay: the shared F3 mission inspector. Most legacy tests drive
# its runtime-free panes; the player tests use a small public-contract fake so
# no listen-server auto-spawn can make the pose/no-player cases nondeterministic.
# MissionRuntime metadata and game/ONED host wiring are covered separately.

const OverlayScript := preload("res://engine/debug/nova_debug_overlay.gd")
const DebugViewContext := preload("res://engine/debug/nova_debug_view_context.gd")
const COLLISION_TOGGLE_PATH := NodePath(
	"DebugPanel/DebugContent/DebugTabs/View/ViewCollision")
const PLAYER_POSITION_PATH := NodePath(
	"DebugPanel/DebugContent/DebugTabs/Player/PlayerPosition")
const PLAYER_ORIENTATION_PATH := NodePath(
	"DebugPanel/DebugContent/DebugTabs/Player/PlayerOrientation")
const PLAYER_DUMP_PATH := NodePath(
	"DebugPanel/DebugContent/DebugTabs/Player/DumpPlayerPose")
const PLAYER_DUMP_STATUS_PATH := NodePath(
	"DebugPanel/DebugContent/DebugTabs/Player/PlayerDumpStatus")
const USER_POINTS_TOGGLE_PATH := NodePath(
	"DebugPanel/DebugContent/DebugTabs/View/ViewUserPoints")
const OCCLUSION_TOGGLE_PATH := NodePath(
	"DebugPanel/DebugContent/DebugTabs/Occlusion/OcclusionShowPortals")
const OCCLUSION_STATUS_PATH := NodePath(
	"DebugPanel/DebugContent/DebugTabs/Occlusion/OcclusionStatus")
const OCCLUSION_LIST_PATH := NodePath(
	"DebugPanel/DebugContent/DebugTabs/Occlusion/OcclusionBuildings")


class FakePoseSim:
	extends Node

	var _has_player := false
	var _position := Vector3.ZERO
	var _yaw_deg := 0.0
	var _pitch_deg := 0.0
	var _view_roll_deg := 0.0

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
	var overlay: CanvasLayer = OverlayScript.new()
	add_child_autofree(overlay)
	return overlay


func _dictionary_vector3(value: Variant) -> Vector3:
	var record: Dictionary = value if value is Dictionary else {}
	return Vector3(
			float(record.get("x", 0.0)),
			float(record.get("y", 0.0)),
			float(record.get("z", 0.0)))




func test_without_runtime_reports_no_mission() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	assert_true(overlay._status_label.visible, "no source - the overlay says so")
	assert_true(overlay._tabs.visible,
		"the tabs stay usable (the perf pane works from host-wide state, no sim needed)")
	assert_eq(overlay._entity_list.item_count, 0, "the sim-fed panes sit empty")

	overlay.set_runtime_source(func(): return null)
	overlay.refresh_now()
	assert_true(overlay._status_label.visible, "a null-returning source reads as no mission")








func test_transport_signal_stays_quiet_without_a_runtime() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	watch_signals(overlay)
	overlay._play_button.pressed.emit()
	overlay._stop_button.pressed.emit()
	assert_signal_not_emitted(overlay, "transport_used",
		"a press with nothing to act on announces nothing")












func test_view_tab_skeleton_toggle_emits() -> void:
	# The View tab's "Show skeletons" checkbox is a pure view toggle: it needs no
	# runtime and only emits intent for the host to act on (build/free the 3D view).
	var overlay := _make_overlay()
	overlay.toggle()
	assert_not_null(overlay._tabs.get_node_or_null("View"), "a View tab exists")
	assert_not_null(overlay._skeleton_check, "the skeleton checkbox is reachable as a member")
	assert_eq(overlay._skeleton_check.name, "ViewSkeletons")
	assert_false(overlay._skeleton_check.button_pressed, "it defaults off")

	watch_signals(overlay)
	overlay._skeleton_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "skeleton_debug_toggled", [true])
	overlay._skeleton_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(overlay, "skeleton_debug_toggled", [false])


func test_view_tab_user_points_toggle_emits() -> void:
	# Named user points are another host-owned 3D view; the overlay exposes a
	# stable path and emits intent without requiring a running mission.
	var overlay := _make_overlay()
	overlay.toggle()
	var user_points_check := overlay.get_node_or_null(USER_POINTS_TOGGLE_PATH) as CheckBox
	assert_not_null(user_points_check, "the user-point checkbox has a stable public node path")
	assert_false(user_points_check.button_pressed, "it defaults off")

	watch_signals(overlay)
	user_points_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "user_points_toggled", [true])
	user_points_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(overlay, "user_points_toggled", [false])


func test_view_tab_collision_toggle_emits() -> void:
	# The View tab's "Show collision" checkbox: same host-neutral, runtime-free
	# contract as the skeleton toggle -- it only emits intent; the host builds/frees
	# the collision debug view.
	var overlay := _make_overlay()
	overlay.toggle()
	var collision_check := overlay.get_node_or_null(COLLISION_TOGGLE_PATH) as CheckBox
	assert_not_null(collision_check, "the collision checkbox has a stable public node path")
	assert_false(collision_check.button_pressed, "it defaults off")

	watch_signals(overlay)
	collision_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "collision_debug_toggled", [true])
	collision_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(overlay, "collision_debug_toggled", [false])


func test_occlusion_tab_portal_toggle_emits() -> void:
	# The Occlusion tab's "Show portal faces" checkbox: the collision-toggle
	# contract — it only emits intent; the host builds/frees the 3D view.
	var overlay := _make_overlay()
	overlay.toggle()
	var portals_check := overlay.get_node_or_null(OCCLUSION_TOGGLE_PATH) as CheckBox
	assert_not_null(portals_check, "the portal checkbox has a stable public node path")
	assert_false(portals_check.button_pressed, "it defaults off")

	watch_signals(overlay)
	portals_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "occlusion_debug_toggled", [true])
	portals_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(overlay, "occlusion_debug_toggled", [false])


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
	var status := overlay.get_node(OCCLUSION_STATUS_PATH) as Label
	assert_string_contains(status.text, "No occlusion data")
	assert_eq((overlay.get_node(OCCLUSION_LIST_PATH) as ItemList).item_count, 0)


func test_view_tab_hide_foliage_toggle_emits() -> void:
	# The View tab's "Hide foliage" checkbox: same host-neutral, runtime-free contract as
	# the skeleton toggle -- it only emits intent for the host to act on.
	var overlay := _make_overlay()
	overlay.toggle()
	assert_not_null(overlay._foliage_check, "the foliage checkbox is reachable as a member")
	assert_eq(overlay._foliage_check.name, "ViewHideFoliage")
	assert_false(overlay._foliage_check.button_pressed, "it defaults off (foliage shown)")

	watch_signals(overlay)
	overlay._foliage_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "foliage_hidden_toggled", [true])
	overlay._foliage_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(overlay, "foliage_hidden_toggled", [false])



func test_player_tab_disables_dump_without_a_local_player() -> void:
	var overlay := _make_overlay()
	overlay.set_runtime(_make_pose_runtime())
	overlay.toggle()

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
	assert_eq(overlay.dump_local_player_pose(), "")
	assert_signal_not_emitted(overlay, "local_player_pose_dumped")


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
	var result: String = overlay.dump_local_player_pose(
			blocker_path.path_join("pose.json"))
	assert_eq(result, "")
	assert_signal_not_emitted(overlay, "local_player_pose_dumped")
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
	overlay.local_player_pose_dumped.connect(
			func(path: String): _dumped_paths.append(path))
	watch_signals(overlay)
	dump_button.pressed.emit()
	assert_signal_emitted(overlay, "local_player_pose_dumped")
	var dumped_path := String(
			get_signal_parameters(overlay, "local_player_pose_dumped", 0)[0])
	assert_true(FileAccess.file_exists(dumped_path))
	assert_string_contains(dumped_path.get_file(), "00TRe")

	var file := FileAccess.open(dumped_path, FileAccess.READ)
	assert_not_null(file)
	var payload_variant: Variant = JSON.parse_string(file.get_as_text()) if file != null else null
	if file != null:
		file.close()
	assert_typeof(payload_variant, TYPE_DICTIONARY)
	var payload: Dictionary = payload_variant if payload_variant is Dictionary else {}
	assert_eq(String(payload.get("schema", "")), "opennova.player_pose.v1")
	assert_true(String(payload.get("captured_at_utc", "")).ends_with("Z"))
	assert_eq(int(payload.get("logic_tick", -1)), 4242)
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

	var second_path: String = overlay.dump_local_player_pose()
	assert_ne(second_path, dumped_path, "rapid consecutive snapshots never overwrite")
	assert_true(FileAccess.file_exists(second_path))

	var next_runtime := _make_pose_runtime()
	next_runtime.set_mission_identity("00TRa.bms", "Second Training Area")
	next_runtime.get_sim().set_player_pose(Vector3.ZERO, 0.0, 0.0, 0.0)
	overlay.set_runtime(next_runtime)
	assert_string_contains(dump_status.text, "No pose snapshot saved",
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

func test_particles_tab_toggles_emit() -> void:
	# Same host-neutral contract as the View toggles: the checkboxes only emit
	# intent; the host hides the effect world / builds the box view. All access
	# rides stable node names (ADR 0018 — no private pokes).
	var overlay := _make_overlay()
	overlay.toggle()
	assert_not_null(overlay.find_child("Particles", true, false), "a Particles tab exists")
	var hide_check := overlay.find_child("ParticlesHide", true, false) as CheckBox
	var boxes_check := overlay.find_child("ParticlesBoxes", true, false) as CheckBox
	assert_not_null(hide_check, "the hide checkbox has a stable node name")
	assert_not_null(boxes_check, "the boxes checkbox has a stable node name")
	assert_false(hide_check.button_pressed, "hide defaults off")
	assert_false(boxes_check.button_pressed, "boxes default off")

	watch_signals(overlay)
	hide_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "particles_hidden_toggled", [true])
	boxes_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "particle_boxes_toggled", [true])


func test_particles_tab_reports_counts_and_peak_reset() -> void:
	# The counts header keeps retail's current/peak form INCLUDING the peak
	# reset when the current count hits zero [orig: Debug_DrawParticleStats
	# @ 0x44c840 — dword_A895E0 zeroes with the count].
	var overlay := _make_overlay()
	overlay.toggle()
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
