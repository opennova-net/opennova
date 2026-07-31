extends GutTest

# NovaDebugOverlay: the shared F3 mission inspector. Most legacy tests drive
# its runtime-free panes; the player tests use a small public-contract fake so
# no listen-server auto-spawn can make the pose/no-player cases nondeterministic.
# MissionRuntime metadata and game/ONED shell wiring are covered separately.

const OverlayScript := preload("res://engine/debug/nova_debug_overlay.gd")
const DebugViewContext := preload("res://engine/debug/nova_debug_view_context.gd")
# Pages mount under the sidebar shell's page mount; option checkboxes are
# named after their registry id.
const PAGES := "DebugPanel/DebugFrame/DebugContent/DebugBody/PageMount"
const COLLISION_TOGGLE_PATH := NodePath(PAGES + "/Rounds/show_collision")
const PLAYER_POSITION_PATH := NodePath(PAGES + "/Player/PlayerPosition")
const PLAYER_ORIENTATION_PATH := NodePath(PAGES + "/Player/PlayerOrientation")
const PLAYER_DUMP_PATH := NodePath(PAGES + "/Player/DumpSnapshot")
const PLAYER_DUMP_STATUS_PATH := NodePath(PAGES + "/Player/PlayerDumpStatus")
const PLAYER_TELEPORT_STATUS_PATH := NodePath(PAGES + "/Player/PlayerTeleportStatus")
const USER_POINTS_TOGGLE_PATH := NodePath(PAGES + "/Animation/show_user_points")
const OCCLUSION_TOGGLE_PATH := NodePath(PAGES + "/Occlusion/show_portal_faces")
const OCCLUSION_STATUS_PATH := NodePath(PAGES + "/Occlusion/OcclusionStatus")
const OCCLUSION_LIST_PATH := NodePath(PAGES + "/Occlusion/OcclusionBuildings")
const OCCLUSION_DETAIL_PATH := NodePath(PAGES + "/Occlusion/OcclusionBuildingDetail")
const ROUNDS_STATUS_PATH := NodePath(PAGES + "/Rounds/RoundsStatus")
const ROUNDS_LIST_PATH := NodePath(PAGES + "/Rounds/RoundEvents")
const SKELETON_TOGGLE_PATH := NodePath(PAGES + "/Animation/show_skeletons")
const FOLIAGE_TOGGLE_PATH := NodePath(PAGES + "/Terrain/hide_foliage")
const ENTITY_LIST_PATH := NodePath(PAGES + "/Entities/EntityList")
const ENTITY_HEALTH_PATH := NodePath(PAGES + "/Entities/EntityEditHealth/EntityHealthValue")
const ENTITY_SET_HEALTH_PATH := NodePath(PAGES + "/Entities/EntityEditHealth/SetEntityHealth")


class FakePoseSim:
	extends Node

	var _has_player := false
	var _position := Vector3.ZERO
	var _yaw_deg := 0.0
	var _pitch_deg := 0.0
	var _view_roll_deg := 0.0
	var joiner := false
	var teleported_to := {}
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

	func is_joiner() -> bool:
		return joiner

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

	func debug_teleport_local_player(
			position: Vector3, yaw_deg: float, pitch_deg: float) -> Error:
		teleported_to = {
			"position": position,
			"yaw": yaw_deg,
			"pitch": pitch_deg,
		}
		return OK


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


class RealPlayerRuntime:
	extends Node

	var sim := NovaSimulation.new()

	func _init() -> void:
		add_child(sim)

	func prepare_populated_player() -> Error:
		var mission := NovaMissionData.new()
		var err := mission.create_default()
		if err != OK:
			return err
		if not sim.load_from_mission_data(mission):
			return ERR_CANT_CREATE
		if not sim.spawn_local_player(Vector3.ZERO, 0.0, 2):
			return ERR_CANT_CREATE
		var root := NovaResourceRoot.new()
		err = root.set_root_dir(ProjectSettings.globalize_path(
				"res://../fixtures/def"))
		if err != OK:
			return err
		return sim.load_weapon_table(root, "weapon.def")

	func get_sim() -> NovaSimulation:
		return sim

	func is_playing() -> bool:
		return true

	func get_mission_file() -> String:
		return "player_loadout.bms"

	func get_mission_name() -> String:
		return "Player loadout"


class FakeEntitySim:
	extends FakePoseSim

	var cards := [
		{
			"name": "AI zero",
			"net_id": 111,
			"position": Vector3(10.0, 2.0, -30.0),
			"pool": 0,
			"wire_handle": 1001,
			"alive": true,
			"hidden": false,
			"health": 80,
			"ai_health": 80,
			"team": 1,
			"state_name": "guard",
		},
		{
			"name": "AI one",
			"net_id": 222,
			"position": Vector3(20.0, 3.0, -40.0),
			"pool": 1,
			"wire_handle": 1002,
			"alive": true,
			"hidden": false,
			"health": 60,
			"ai_health": 60,
			"team": 2,
			"state_name": "patrol",
		},
	]
	var present_snapshot_reads := 0
	var edited_health := {}

	func get_entity_count() -> int:
		return cards.size()

	func get_entity_debug(index: int) -> Dictionary:
		return cards[index].duplicate(true) \
				if index >= 0 and index < cards.size() else {}

	func get_present_stride() -> int:
		return NovaSimulation.PF_STRIDE

	func get_present_snapshot() -> PackedFloat32Array:
		present_snapshot_reads += 1
		var snapshot := PackedFloat32Array()
		# Deliberately reverse client-view order relative to the AI pool and
		# include one visible entity that has no AI edit record.
		snapshot.append_array(_present_row(
				222, 1002, 502, Vector3(22.0, 4.0, -44.0)))
		snapshot.append_array(_present_row(
				333, 2001, 703, Vector3(30.0, 5.0, -50.0)))
		snapshot.append_array(_present_row(
				111, 1001, 501, Vector3(11.0, 2.0, -33.0)))
		return snapshot

	func get_world_entity_debug(net_id: int) -> Dictionary:
		if net_id != 333:
			return {}
		return {
			"name": "Client vehicle",
			"state_name": "driving",
			"health": 400,
			"team": 3,
			"alive": true,
		}

	func debug_set_entity_health(index: int, health: int) -> Error:
		if index < 0 or index >= cards.size():
			return ERR_INVALID_PARAMETER
		edited_health = {"index": index, "health": health}
		cards[index]["health"] = health
		return OK

	func debug_set_entity_position(_index: int, _position: Vector3) -> Error:
		return OK

	func _present_row(
			net_id: int,
			wire_handle: int,
			type_id: int,
			position: Vector3) -> PackedFloat32Array:
		var row := PackedFloat32Array()
		row.resize(NovaSimulation.PF_STRIDE)
		row[NovaSimulation.PF_TYPE_ID] = type_id
		row[NovaSimulation.PF_NET_ID] = net_id
		row[NovaSimulation.PF_WIRE_HANDLE] = wire_handle
		row[NovaSimulation.PF_KIND] = 1
		row[NovaSimulation.PF_INDEX] = net_id
		row[NovaSimulation.PF_BMS_ID] = 1000 + net_id
		row[NovaSimulation.PF_POS_X] = position.x
		row[NovaSimulation.PF_POS_Y] = position.y
		row[NovaSimulation.PF_POS_Z] = position.z
		row[NovaSimulation.PF_ALIVE] = 1.0
		return row


class FakeEntityRuntime:
	extends Node

	var sim := FakeEntitySim.new()

	func _init() -> void:
		add_child(sim)

	func get_sim() -> FakeEntitySim:
		return sim

	func is_playing() -> bool:
		return true

	func get_mission_file() -> String:
		return "entities.bms"

	func get_mission_name() -> String:
		return "Entity Identity"


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


class AuthorityTarget:
	extends RefCounted
	var calls := 0
	func mutate() -> void:
		calls += 1


class FakeTransportAdapter:
	extends Node
	var actions: Array[String] = []

	func mcp_game_control(action: String) -> Error:
		actions.append(action)
		return OK

	func debug_return_to_menu() -> Error:
		return OK


class FakeTransportWorld:
	extends Node
	var networked := false

	func is_net_session() -> bool:
		return networked


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
	var compact_picker := overlay.find_child(
			"CompactPagePicker", true, false) as OptionButton
	assert_true(page_list.visible or compact_picker.visible,
		"the page list stays usable (the perf page works from process-wide state, no sim needed)")
	var entity_list := overlay.find_child("EntityList", true, false) as ItemList
	assert_eq(entity_list.item_count, 0, "the sim-fed pages sit empty")

	overlay.set_runtime_source(func(): return null)
	overlay.refresh_now()
	assert_true(overlay._status_label.visible, "a null-returning source reads as no mission")


func test_runtime_context_rejects_non_object_sources() -> void:
	var ctx := NovaDebugContext.new()
	ctx.runtime_source = func(): return 42
	assert_null(ctx.runtime(),
			"a malformed supplier degrades to no runtime instead of validating a scalar instance")


func test_entity_rows_follow_client_present_order_and_edits_use_ai_index() -> void:
	var runtime := FakeEntityRuntime.new()
	add_child_autofree(runtime)
	var overlay := _make_overlay()
	overlay.set_runtime(runtime)
	overlay.toggle()
	overlay.select_page(&"Entities")

	var entity_list := overlay.get_node(ENTITY_LIST_PATH) as ItemList
	assert_eq(entity_list.item_count, 3)
	assert_string_contains(entity_list.get_item_text(0), "ssn 222")
	assert_string_contains(entity_list.get_item_text(1), "ssn 333")
	assert_string_contains(entity_list.get_item_text(2), "ssn 111")
	assert_gt(runtime.sim.present_snapshot_reads, 0,
			"the visible list is the client-present view")

	overlay.set_edit_unlocked(true)
	entity_list.item_selected.emit(1)
	var set_health := overlay.get_node(ENTITY_SET_HEALTH_PATH) as Button
	assert_true(set_health.disabled,
			"a presented vehicle without an AI record is visible but read-only")

	entity_list.item_selected.emit(0)
	var health := overlay.get_node(ENTITY_HEALTH_PATH) as SpinBox
	health.value = 37
	assert_false(set_health.disabled)
	set_health.pressed.emit()
	assert_eq(runtime.sim.edited_health, {"index": 1, "health": 37},
			"client row zero maps its edit to AI pool entry one")


func test_shared_session_keeps_host_status_and_authority_sources() -> void:
	var session := NovaDebugSession.new()
	session.set_status_source(func():
		return {"label": "host status", "logic_tick": 77, "authority": false})
	session.set_authority_source(func(): return false)
	var target := AuthorityTarget.new()
	session.set_target_source(&"authority_test", func(): return target)
	var action := NovaDebugControlDef.action_control(
			&"authority_test", &"Test", "Mutate", "Host-only test.",
			&"authority_test", &"mutate")
	action.requires_unlock = true
	action.authority = NovaDebugControlDef.Authority.HOST_ONLY
	assert_true(session.register_control(action))
	session.set_edit_unlocked(true)

	var config_path := "user://test_shared_debug_overlay_%d.cfg" % Time.get_ticks_usec()
	_dumped_paths.append(ProjectSettings.globalize_path(config_path))
	var overlay: CanvasLayer = OverlayScript.new(config_path, session)
	add_child_autofree(overlay)
	var unlock_edits := overlay.find_child("UnlockEdits", true, false) as CheckButton
	assert_true(unlock_edits.button_pressed,
			"the overlay initializes Live edits from the already-unlocked shared session")
	session.set_edit_unlocked(false)
	assert_false(unlock_edits.button_pressed,
			"an external session lock immediately updates the overlay")
	session.set_edit_unlocked(true)
	assert_true(unlock_edits.button_pressed,
			"an external session unlock immediately updates the overlay")

	assert_eq(session.capture_snapshot()["runtime"], {
		"label": "host status", "logic_tick": 77, "authority": false,
	})
	assert_eq(int(session.invoke_control(
			&"authority_test", null, true)["error"]), ERR_UNAUTHORIZED)
	assert_eq(target.calls, 0)








func test_transport_controls_are_disabled_without_a_runtime() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	assert_true((overlay.find_child("SimPlay", true, false) as Button).disabled)
	assert_true((overlay.find_child("SimStop", true, false) as Button).disabled)


func test_listen_host_can_resume_but_cannot_pause_or_step() -> void:
	var session := NovaDebugSession.new()
	NovaDebugCatalog.install(session)
	var game_adapter := FakeTransportAdapter.new()
	add_child_autofree(game_adapter)
	session.set_target_source(
			NovaDebugCatalog.TARGET_GAME_SHELL, func(): return game_adapter)
	session.set_authority_source(func(): return true)
	session.set_edit_unlocked(true)

	var config_path := "user://test_debug_overlay_%d.cfg" % Time.get_ticks_usec()
	_dumped_paths.append(ProjectSettings.globalize_path(config_path))
	var overlay: CanvasLayer = OverlayScript.new(config_path, session)
	add_child_autofree(overlay)
	var runtime := _make_pose_runtime()
	var world := FakeTransportWorld.new()
	world.networked = true
	add_child_autofree(world)
	overlay.set_runtime(runtime)
	overlay.set_world_source(func(): return world)
	overlay.toggle()
	overlay.select_page(&"Sim")

	var play := overlay.find_child("SimPlay", true, false) as Button
	var pause := overlay.find_child("SimPause", true, false) as Button
	var step := overlay.find_child("SimStep", true, false) as Button
	assert_false(play.disabled, "resume keeps the network pump recovery path")
	assert_true(pause.disabled, "a listen host cannot pause its network pump")
	assert_true(step.disabled, "a listen host cannot single-step its network pump")
	assert_string_contains(pause.tooltip_text, "multiplayer")
	assert_string_contains(step.tooltip_text, "multiplayer")

	play.pressed.emit()
	assert_eq(game_adapter.actions, ["resume"])












func test_skeleton_toggle_lives_on_the_animation_page() -> void:
	# Registry toggles need no runtime and only emit intent for the shell to act
	# on (build/free the 3D view) — everything rides ONE generic channel now.
	# The View page is gone; the bone views live with the animation data.
	var overlay := _make_overlay()
	overlay.toggle()
	assert_null(overlay.find_child("View", true, false),
			"the View page is fully dissolved")
	var skeleton_check := overlay.get_node_or_null(SKELETON_TOGGLE_PATH) as CheckBox
	assert_not_null(skeleton_check, "the skeleton checkbox has a stable public node path")
	assert_false(skeleton_check.button_pressed, "it defaults off")

	var session: NovaDebugSession = overlay.get_debug_session()
	watch_signals(session)
	skeleton_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(session, "control_invoked",
			[&"show_skeletons", true])
	skeleton_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(session, "control_invoked",
			[&"show_skeletons", false])


func test_user_points_toggle_lives_on_the_animation_page() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	var user_points_check := overlay.get_node_or_null(USER_POINTS_TOGGLE_PATH) as CheckBox
	assert_not_null(user_points_check, "the user-point checkbox has a stable public node path")
	assert_false(user_points_check.button_pressed, "it defaults off")

	var session: NovaDebugSession = overlay.get_debug_session()
	watch_signals(session)
	user_points_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(session, "control_invoked",
			[&"show_user_points", true])


func test_collision_toggle_lives_on_the_rounds_page() -> void:
	# "Show collision" belongs with the other what-geometry-does-the-world-test
	# views on Rounds & collision, not on the dissolving View page.
	var overlay := _make_overlay()
	overlay.toggle()
	var collision_check := overlay.get_node_or_null(COLLISION_TOGGLE_PATH) as CheckBox
	assert_not_null(collision_check, "the collision checkbox has a stable public node path")
	assert_false(collision_check.button_pressed, "it defaults off")

	var session: NovaDebugSession = overlay.get_debug_session()
	watch_signals(session)
	collision_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(session, "control_invoked",
			[&"show_collision", true])


func test_occlusion_page_portal_toggle_rides_the_option_registry() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	var portals_check := overlay.get_node_or_null(OCCLUSION_TOGGLE_PATH) as CheckBox
	assert_not_null(portals_check, "the portal checkbox has a stable public node path")
	assert_false(portals_check.button_pressed, "it defaults off")

	var session: NovaDebugSession = overlay.get_debug_session()
	watch_signals(session)
	portals_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(session, "control_invoked",
			[&"show_portal_faces", true])


func test_set_option_syncs_the_owning_control_and_emits() -> void:
	# The programmatic write path is the SAME path a click takes: one emission,
	# and the page's control re-syncs without re-firing.
	var overlay := _make_overlay()
	var session: NovaDebugSession = overlay.get_debug_session()
	watch_signals(session)
	overlay.set_option(&"hide_foliage", true)
	assert_signal_emitted_with_parameters(session, "control_invoked",
			[&"hide_foliage", true])
	assert_eq(get_signal_emit_count(session, "control_invoked"), 1)
	var foliage_check := overlay.get_node_or_null(FOLIAGE_TOGGLE_PATH) as CheckBox
	assert_true(foliage_check.button_pressed, "the page control re-synced")
	assert_eq(overlay.get_option_value(&"hide_foliage"), true)
	overlay.set_option(&"hide_foliage", true)
	assert_eq(get_signal_emit_count(session, "control_invoked"), 1,
			"a repeated value never re-fires")


func test_occlusion_tab_reports_frame_state() -> void:
	# The Occlusion tab turns the sim's snapshot into decisions: exceptional
	# buildings lead, rows are selectable, and welds explain only the selected
	# building instead of masquerading as more table rows.
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
	assert_string_contains(status.text, "Camera: INDOORS",
			"the camera line reports the blink state")
	assert_string_contains(status.text, "1 drawn", "the batch split totals surface")
	assert_string_contains(status.text, "1 culled")
	assert_string_contains(status.text, "4 entities hidden")
	var list := overlay.get_node(OCCLUSION_LIST_PATH) as ItemList
	assert_eq(list.item_count, 2, "only the two inspectable buildings are rows")
	assert_string_contains(list.get_item_text(0), "CULLED")
	assert_string_contains(list.get_item_text(0), "#43")
	assert_string_contains(list.get_item_text(1), "DRAWN")
	assert_string_contains(list.get_item_text(1), "#42")
	var detail := overlay.get_node(OCCLUSION_DETAIL_PATH) as Label
	assert_string_contains(detail.text, "Building #43")
	assert_string_contains(detail.text, "Welded connections: 1")
	assert_string_contains(detail.text, "building #42 section 1")


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

	var session: NovaDebugSession = overlay.get_debug_session()
	watch_signals(session)
	foliage_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(session, "control_invoked",
			[&"hide_foliage", true])
	foliage_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(session, "control_invoked",
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
	assert_false(orientation_label.visible,
			"empty optional player details do not leave blank rows behind")
	assert_false((overlay.get_node(PAGES + "/Player/PlayerCombat") as Label).visible)
	assert_false((overlay.get_node(PAGES + "/Player/PlayerInventory") as Label).visible)
	assert_true(dump_button.disabled)
	watch_signals(overlay)
	assert_eq(overlay.dump_debug_snapshot(), "")
	assert_signal_not_emitted(overlay, "debug_snapshot_dumped")


func test_player_tab_stays_docked_and_sidebar_remains_clickable() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(560, 900)
	add_child_autofree(viewport)
	var config_path := "user://test_debug_overlay_%d.cfg" % Time.get_ticks_usec()
	_dumped_paths.append(ProjectSettings.globalize_path(config_path))
	var overlay: CanvasLayer = OverlayScript.new(config_path)
	viewport.add_child(overlay)
	overlay.toggle()
	assert_true(overlay.select_page(&"Player"))
	await wait_process_frames(2)

	var panel := overlay.find_child("DebugPanel", true, false) as Control
	var page_list := overlay.find_child("PageList", true, false) as ItemList
	var compact_picker := overlay.find_child(
			"CompactPagePicker", true, false) as OptionButton
	var page_help := overlay.find_child("ActivePageHelp", true, false) as Label
	var viewport_rect := viewport.get_visible_rect()
	assert_eq(viewport_rect.size, Vector2(560, 900),
			"the repro uses the minimum width that fits the configured dock")
	var panel_rect := panel.get_global_rect()
	assert_gte(panel_rect.position.x, viewport_rect.position.x + 7.0,
			"opening Player must not push the dock past the viewport's left edge")
	assert_almost_eq(panel_rect.size.x, 544.0, 1.0,
			"a constrained dock keeps an 8 px inset on both sides")
	var page_list_rect := page_list.get_global_rect()
	assert_gte(page_list_rect.position.x, viewport_rect.position.x,
			"opening Player must not push the sidebar off the left edge")

	# Shrinking the window applies a temporary fitted width without changing
	# the user's preferred dock width.
	viewport.size = Vector2i(520, 900)
	await wait_process_frames(2)
	viewport_rect = viewport.get_visible_rect()
	panel_rect = panel.get_global_rect()
	assert_gte(panel_rect.position.x, viewport_rect.position.x + 7.0)
	assert_lte(panel_rect.end.x, viewport_rect.end.x - 7.0)
	assert_almost_eq(panel_rect.size.x, 504.0, 1.0)
	var stats_row := -1
	for row in range(page_list.item_count):
		if page_list.get_item_text(row).strip_edges() == "Stats":
			stats_row = row
			break
	assert_gte(stats_row, 0, "the Stats destination row exists")
	if stats_row < 0:
		return
	var stats_rect := page_list.get_item_rect(stats_row)
	var click_position := page_list.get_global_transform_with_canvas() * stats_rect.get_center()
	assert_true(page_list.get_global_rect().has_point(click_position),
			"the destination tab is visibly inside the sidebar")
	assert_true(viewport_rect.has_point(click_position),
			"the destination tab's clickable center remains on screen")

	var motion := InputEventMouseMotion.new()
	motion.position = click_position
	motion.global_position = click_position
	viewport.push_input(motion, true)
	var press := InputEventMouseButton.new()
	press.position = click_position
	press.global_position = click_position
	press.button_index = MOUSE_BUTTON_LEFT
	press.button_mask = MOUSE_BUTTON_MASK_LEFT
	press.pressed = true
	viewport.push_input(press, true)
	var release := press.duplicate() as InputEventMouseButton
	release.button_mask = 0
	release.pressed = false
	viewport.push_input(release, true)
	await wait_process_frames(2)

	assert_eq(String(overlay.get_active_page_id()), "Stats",
			"a real sidebar click must still leave the Player page")

	viewport.size = Vector2i(360, 900)
	await wait_process_frames(2)
	viewport_rect = viewport.get_visible_rect()
	panel_rect = panel.get_global_rect()
	assert_gte(panel_rect.position.x, viewport_rect.position.x + 7.0,
			"the dock remains fully on-screen below its preferred width floor")
	assert_lte(panel_rect.end.x, viewport_rect.end.x - 7.0)
	assert_false(page_list.visible)
	assert_true(compact_picker.visible,
			"narrow docks replace the fixed sidebar with a usable page picker")
	assert_gte(page_help.size.x, 140.0,
			"active-page metadata remains readable instead of collapsing to a sliver")
	for index in range(compact_picker.item_count):
		if compact_picker.get_item_text(index) == "Player":
			compact_picker.item_selected.emit(index)
			break
	assert_eq(String(overlay.get_active_page_id()), "Player",
			"the compact picker can leave Stats at the narrowest supported layout")

	viewport.size = Vector2i(900, 900)
	await wait_process_frames(2)
	panel_rect = panel.get_global_rect()
	assert_almost_eq(panel_rect.size.x, 552.0, 1.0,
			"growing the viewport restores the preferred width")
	assert_true(page_list.visible)
	assert_false(compact_picker.visible)


func test_populated_player_loadout_cannot_expand_dock_or_hide_tabs() -> void:
	var runtime := RealPlayerRuntime.new()
	add_child_autofree(runtime)
	assert_eq(runtime.prepare_populated_player(), OK)
	var inventory: Dictionary = runtime.sim.get_local_player_inventory()
	assert_gt((inventory.get("pools", {}) as Dictionary).size(), 20,
			"the regression uses the wide production ammo-pool inventory")

	var viewport := SubViewport.new()
	viewport.size = Vector2i(1600, 900)
	add_child_autofree(viewport)
	var config_path := "user://test_debug_overlay_%d.cfg" % Time.get_ticks_usec()
	_dumped_paths.append(ProjectSettings.globalize_path(config_path))
	var overlay: CanvasLayer = OverlayScript.new(config_path)
	viewport.add_child(overlay)
	overlay.set_runtime(runtime)
	overlay.toggle()
	assert_true(overlay.select_page(&"Player"))
	await wait_process_frames(2)

	var panel := overlay.find_child("DebugPanel", true, false) as Control
	var page_list := overlay.find_child("PageList", true, false) as ItemList
	var page_mount := overlay.find_child("PageMount", true, false) as ScrollContainer
	var player_page := overlay.get_node(PAGES + "/Player") as Control
	var inventory_label := overlay.get_node(
			PAGES + "/Player/PlayerInventory") as Label
	var inventory_toggle := overlay.get_node(
			PAGES + "/Player/PlayerInventoryToggle") as Button
	var dump := overlay.get_node(PAGES + "/Player/DumpSnapshot") as Button
	var teleport := overlay.get_node(PAGES + "/Player/TeleportPlayer") as Button
	var viewport_rect := viewport.get_visible_rect()
	assert_true(inventory_toggle.visible,
			"the typed simulation exposes a concise inventory disclosure")
	assert_string_contains(inventory_toggle.text, "slot")
	assert_string_contains(inventory_toggle.text, "active pool")
	assert_false(inventory_toggle.button_pressed,
			"verbose inventory starts collapsed so actions stay above the fold")
	assert_false(inventory_label.visible)
	assert_true(page_mount.get_global_rect().encloses(dump.get_global_rect()),
			"snapshot action is visible without scrolling past inventory")
	assert_true(page_mount.get_global_rect().encloses(teleport.get_global_rect()),
			"teleport action is visible without scrolling past inventory")
	assert_false(page_mount.get_v_scroll_bar().visible,
			"the default populated Player summary fits the live 1600x900 dock")

	inventory_toggle.set_pressed_no_signal(true)
	inventory_toggle.toggled.emit(true)
	await wait_process_frames(2)
	assert_true(inventory_label.visible)
	assert_string_contains(inventory_label.text, "Ammo",
			"expanding reaches the ammo-pool content that caused the runaway")
	assert_false(inventory_label.text.contains("1 rounds"),
			"single-round loadout entries use readable grammar")
	assert_string_contains(inventory_label.tooltip_text, "All ammo pools",
			"the compact inventory retains the complete raw pool list")
	assert_gte(panel.get_global_rect().position.x,
			viewport_rect.position.x + 7.0,
			"live loadout content cannot expand the dock past the left inset")
	assert_lte(panel.get_global_rect().end.x,
			viewport_rect.end.x - 7.0,
			"live loadout content cannot expand the dock past the right inset")
	assert_gte(page_list.get_global_rect().position.x,
			viewport_rect.position.x,
			"the Player loadout cannot push the other page tabs off-screen")
	assert_lte(player_page.get_combined_minimum_size().x, page_mount.size.x,
			"the populated page itself fits rather than relying on hidden clipping")
	assert_lte(player_page.get_global_rect().end.x,
			page_mount.get_global_rect().end.x + 1.0,
			"expanded loadout controls remain inside the visible page column")

	viewport.size = Vector2i(360, 900)
	await wait_process_frames(2)
	viewport_rect = viewport.get_visible_rect()
	assert_gte(panel.get_global_rect().position.x,
			viewport_rect.position.x + 7.0)
	assert_lte(panel.get_global_rect().end.x,
			viewport_rect.end.x - 7.0)
	assert_lte(player_page.get_combined_minimum_size().x, page_mount.size.x,
			"the real loadout also fits the compact single-column dock")
	assert_lte(player_page.get_global_rect().end.x,
			page_mount.get_global_rect().end.x + 1.0,
			"compact loadout content wraps instead of being silently clipped")


func test_player_page_scrolls_without_moving_the_sidebar() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(520, 360)
	add_child_autofree(viewport)
	var config_path := "user://test_debug_overlay_%d.cfg" % Time.get_ticks_usec()
	_dumped_paths.append(ProjectSettings.globalize_path(config_path))
	var overlay: CanvasLayer = OverlayScript.new(config_path)
	viewport.add_child(overlay)
	overlay.toggle()
	assert_true(overlay.select_page(&"Player"))
	await wait_process_frames(2)

	var page_mount := overlay.find_child("PageMount", true, false) as ScrollContainer
	var page_list := overlay.find_child("PageList", true, false) as ItemList
	var teleport := overlay.get_node(PAGES + "/Player/TeleportPlayer") as Button
	assert_not_null(page_mount)
	assert_eq(page_mount.horizontal_scroll_mode,
			ScrollContainer.SCROLL_MODE_SHOW_NEVER)
	assert_false(page_mount.get_h_scroll_bar().visible,
			"page content never pushes the whole dock sideways")
	var vertical_bar := page_mount.get_v_scroll_bar()
	assert_true(vertical_bar.visible)
	assert_gt(vertical_bar.max_value, vertical_bar.page,
			"the shared mount makes the bottom of a tall page reachable")
	var sidebar_before := page_list.get_global_rect()
	vertical_bar.value = vertical_bar.max_value
	await wait_process_frames(2)
	assert_true(page_mount.get_global_rect().intersects(teleport.get_global_rect()),
			"scrolling reaches the Player page's final action")
	assert_eq(page_list.get_global_rect(), sidebar_before,
			"page scrolling leaves navigation fixed")
	assert_true(overlay.select_page(&"Stats"))
	await wait_process_frames(2)
	assert_eq(page_mount.scroll_vertical, 0,
			"a newly selected page always opens at its top")


func test_redesigned_diagnostic_pages_fit_the_compact_dock() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(360, 900)
	add_child_autofree(viewport)
	var config_path := "user://test_debug_overlay_%d.cfg" % Time.get_ticks_usec()
	_dumped_paths.append(ProjectSettings.globalize_path(config_path))
	var overlay: CanvasLayer = OverlayScript.new(config_path)
	viewport.add_child(overlay)
	overlay.toggle()
	await wait_process_frames(2)

	var page_mount := overlay.find_child("PageMount", true, false) as ScrollContainer
	assert_not_null(page_mount)
	assert_false(page_mount.get_h_scroll_bar().visible,
			"the compact dock never needs whole-page horizontal scrolling")
	for page_id in [&"Entities", &"Animation", &"Occlusion", &"Particles", &"Rendering"]:
		assert_true(overlay.select_page(page_id))
		await wait_process_frames(2)
		var page := overlay.get_node(NodePath(PAGES + "/" + String(page_id))) as Control
		assert_lte(page.get_combined_minimum_size().x, page_mount.size.x,
				"%s fits the compact page column" % page_id)
		assert_lte(page.get_global_rect().end.x,
				page_mount.get_global_rect().end.x + 1.0,
				"%s stays inside the visible dock" % page_id)


func test_player_teleport_has_separate_policy_and_result_feedback() -> void:
	var runtime := _make_pose_runtime()
	runtime.get_sim().set_player_pose(Vector3(1, 2, 3), 15.0, -4.0, 0.0)
	var overlay := _make_overlay()
	overlay.set_runtime(runtime)
	overlay.toggle()
	assert_true(overlay.select_page(&"Player"))

	var teleport := overlay.get_node(PAGES + "/Player/TeleportPlayer") as Button
	var teleport_status := overlay.get_node(PLAYER_TELEPORT_STATUS_PATH) as Label
	var dump_status := overlay.get_node(PLAYER_DUMP_STATUS_PATH) as Label
	assert_true(teleport.disabled)
	assert_string_contains(teleport_status.text, "Live edits")
	assert_string_contains(dump_status.text, "No snapshot saved")

	overlay.set_edit_unlocked(true)
	assert_false(teleport.disabled)
	(overlay.get_node(PAGES + "/Player/PlayerTeleportValues/TeleportXField/TeleportX")
			as SpinBox).value = 40.0
	(overlay.get_node(PAGES + "/Player/PlayerTeleportValues/TeleportYField/TeleportY")
			as SpinBox).value = 50.0
	(overlay.get_node(PAGES + "/Player/PlayerTeleportValues/TeleportZField/TeleportZ")
			as SpinBox).value = 60.0
	teleport.pressed.emit()

	assert_eq(runtime.get_sim().teleported_to.get("position"), Vector3(40, 50, 60))
	assert_eq(teleport_status.text, "Player moved.")
	assert_string_contains(dump_status.text, "No snapshot saved",
			"teleport feedback does not overwrite snapshot feedback")


func test_shell_keeps_long_runtime_identity_and_actions_inside_the_dock() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(520, 900)
	add_child_autofree(viewport)
	var runtime := _make_pose_runtime()
	var long_name := "Very Long Training Mission ".repeat(12)
	runtime.set_mission_identity("00TRe.bms", long_name)
	var config_path := "user://test_debug_overlay_%d.cfg" % Time.get_ticks_usec()
	_dumped_paths.append(ProjectSettings.globalize_path(config_path))
	var overlay: CanvasLayer = OverlayScript.new(config_path)
	viewport.add_child(overlay)
	overlay.set_runtime(runtime)
	overlay.toggle()
	await wait_process_frames(2)

	var panel := overlay.find_child("DebugPanel", true, false) as Control
	var title := overlay.find_child("DebugTitle", true, false) as Label
	var runtime_status := overlay.find_child("RuntimeStatus", true, false) as Label
	var live_edits := overlay.find_child("UnlockEdits", true, false) as CheckButton
	var copy := overlay.find_child("CopyDebugSnapshot", true, false) as Button
	var close_button := overlay.find_child("CloseDebug", true, false) as Button
	var edit_status := overlay.find_child("DebugStatus", true, false) as Label
	assert_eq(title.text, "F3")
	assert_true(runtime_status.clip_text)
	assert_eq(runtime_status.get_theme_font_size("font_size"), 13)
	assert_eq(runtime_status.text, "%s | local" % long_name,
			"runtime identity keeps natural case instead of shouting")
	assert_string_contains(runtime_status.tooltip_text, long_name)
	var panel_surface := panel.get_theme_stylebox("panel") as StyleBoxFlat
	assert_not_null(panel_surface)
	assert_gte(panel_surface.bg_color.a, 0.9,
			"the dock stays readable over bright and busy game scenes")
	assert_eq(live_edits.text, "Live edits")
	assert_string_contains(live_edits.tooltip_text, "nothing by itself")
	assert_string_contains(live_edits.tooltip_text, "not undoable")
	assert_string_contains(edit_status.text, "READ ONLY")
	for action in [live_edits, copy, close_button]:
		assert_true(panel.get_global_rect().encloses(action.get_global_rect()),
				"header actions remain inside the dock with an unbounded mission name")

	overlay.set_edit_unlocked(true)
	assert_string_contains(edit_status.text, "LIVE EDITS")
	assert_string_contains(edit_status.text, "immediately")

	runtime.get_sim().joiner = true
	overlay.refresh_now()
	assert_true(live_edits.disabled)
	assert_false(live_edits.button_pressed)
	assert_false(overlay.get_debug_session().is_edit_unlocked(),
			"losing host authority revokes the session unlock, not just its visual state")
	assert_string_contains(edit_status.text, "host-only",
			"joiners see why authoritative controls remain read-only")

	runtime.get_sim().joiner = false
	overlay.refresh_now()
	assert_false(live_edits.disabled)
	assert_false(live_edits.button_pressed,
			"returning to host authority never silently restores live edits")
	assert_string_contains(edit_status.text, "READ ONLY")

	overlay.set_edit_unlocked(true)
	var custom_authority := {"allowed": false}
	overlay.set_authority_source(func(): return custom_authority["allowed"])
	assert_true(live_edits.disabled)
	assert_false(overlay.get_debug_session().is_edit_unlocked(),
			"the UI and write path share a custom authority source")
	custom_authority["allowed"] = true
	overlay.refresh_now()
	assert_false(live_edits.disabled)
	assert_false(live_edits.button_pressed,
			"restored custom authority still requires an explicit unlock")


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
	# Same shell-neutral contract as every registry toggle: the checkboxes only
	# emit intent; the shell hides the effect world / builds the box view. All
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

	var session: NovaDebugSession = overlay.get_debug_session()
	watch_signals(session)
	hide_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(session, "control_invoked",
			[&"hide_particles", true])
	boxes_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(session, "control_invoked",
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
	assert_string_contains(counts.text, "Live effects: 7 (peak 7)",
			"the effect-world entry count is named as live effects")
	assert_string_contains(counts.text, "Alive particles: 7",
			"emitter particle instances are counted separately")
	assert_string_contains(counts.text, "Drawn quads: 7",
			"rendered particle quads are counted separately")
	assert_eq(groups.item_count, 1,
			"the live list has one selectable row per group, not raw emitter rows")

	stub.alive = 3
	overlay.refresh_now()
	assert_string_contains(counts.text, "Live effects: 3 (peak 7)", "the peak latches")

	stub.alive = 0
	overlay.refresh_now()
	assert_string_contains(counts.text, "Live effects: 0 (peak 0)",
			"the peak resets at zero, like retail")


func test_particles_tab_prioritizes_silent_groups_and_explains_the_selection() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	overlay.select_page(&"Particles")
	var stub := _StubEffectWorld.new()
	stub.groups = [
		{
			"id": 10,
			"name": "healthy sparks",
			"source": "sparks.ptl",
			"forever": false,
			"emitters": [{"name": "SparkFan", "alive": 6, "rendered": 6}],
		},
		{
			"id": 2,
			"name": "silent smoke",
			"source": "smoke.ptl",
			"forever": true,
			"emitters": [{"name": "SmokeColumn", "alive": 4, "rendered": 0}],
		},
	]
	add_child_autofree(stub)
	overlay.set_effect_world_source(func(): return stub)
	overlay.refresh_now()

	var groups := overlay.find_child("ParticleGroups", true, false) as ItemList
	var detail := overlay.find_child("ParticleGroupDetail", true, false) as Label
	assert_eq(groups.item_count, 2)
	assert_true(groups.is_item_selectable(0), "live groups can be inspected")
	assert_eq(int(groups.get_item_metadata(0)), 2,
			"a group with live particles but no drawn quads rises above healthy groups")
	assert_eq(groups.get_selected_items(), PackedInt32Array([0]),
			"the highest-attention group is inspected immediately")
	assert_string_contains(detail.text, "silent smoke")
	assert_string_contains(detail.text, "smoke.ptl")
	assert_string_contains(detail.text, "SmokeColumn")
	assert_string_contains(detail.text, "alive but no quads are drawn",
			"the detail explains why the group needs attention")


func test_particles_tab_keeps_catalog_issues_out_of_the_live_group_list() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	overlay.select_page(&"Particles")
	var stub := _StubEffectWorld.new()
	add_child_autofree(stub)
	overlay.set_effect_world_source(func(): return stub)
	overlay.refresh_now()

	var groups := overlay.find_child("ParticleGroups", true, false) as ItemList
	var catalog_status := overlay.find_child(
			"ParticleCatalogStatus", true, false) as Label
	var catalog_issues := overlay.find_child(
			"ParticleCatalogIssues", true, false) as ItemList
	assert_eq(groups.item_count, 1)
	assert_false(groups.get_item_text(0).contains("SMOKE1.TGA"),
			"asset problems do not masquerade as live particle groups")
	assert_string_contains(catalog_status.text, "2 definitions")
	assert_string_contains(catalog_status.text, "1 missing texture")
	assert_eq(catalog_issues.item_count, 1)
	assert_string_contains(catalog_issues.get_item_text(0), "SMOKE1.TGA")


func test_particles_tab_preserves_selection_by_group_id_when_attention_order_changes() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	overlay.select_page(&"Particles")
	var stub := _StubEffectWorld.new()
	stub.groups = [
		{
			"id": 2, "name": "smoke", "source": "smoke.ptl",
			"emitters": [{"name": "Smoke", "alive": 4, "rendered": 0}],
		},
		{
			"id": 10, "name": "sparks", "source": "sparks.ptl",
			"emitters": [{"name": "Sparks", "alive": 6, "rendered": 6}],
		},
	]
	add_child_autofree(stub)
	overlay.set_effect_world_source(func(): return stub)
	overlay.refresh_now()
	var groups := overlay.find_child("ParticleGroups", true, false) as ItemList
	var detail := overlay.find_child("ParticleGroupDetail", true, false) as Label

	groups.select(1)
	groups.item_selected.emit(1)
	assert_string_contains(detail.text, "sparks")
	stub.groups = [
		{
			"id": 2, "name": "smoke", "source": "smoke.ptl",
			"emitters": [{"name": "Smoke", "alive": 4, "rendered": 4}],
		},
		{
			"id": 10, "name": "sparks", "source": "sparks.ptl",
			"emitters": [{"name": "Sparks", "alive": 6, "rendered": 0}],
		},
	]
	overlay.refresh_now()

	var selected := groups.get_selected_items()
	assert_eq(selected.size(), 1)
	assert_eq(int(groups.get_item_metadata(selected[0])), 10,
			"refresh follows the selected group identity, not its former row index")
	assert_string_contains(detail.text, "sparks")


func test_particles_tab_explains_when_hide_toggle_suppresses_live_diagnostics() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	overlay.select_page(&"Particles")
	var stub := _StubEffectWorld.new()
	stub.alive = 4
	add_child_autofree(stub)
	overlay.set_effect_world_source(func(): return stub)
	overlay.set_option(&"hide_particles", true)
	overlay.refresh_now()

	var counts := overlay.find_child("ParticleCounts", true, false) as Label
	var groups := overlay.find_child("ParticleGroups", true, false) as ItemList
	var detail := overlay.find_child("ParticleGroupDetail", true, false) as Label
	assert_string_contains(counts.text, "PARTICLES HIDDEN")
	assert_eq(groups.item_count, 0,
			"a suppressed live report cannot masquerade as a real empty effect set")
	assert_string_contains(detail.text, "diagnostics are suppressed")


class _StubEffectWorld extends Node3D:
	var alive := 0
	var groups: Array = []

	func active_entry_count() -> int:
		return alive

	func effect_count() -> int:
		return 2

	func get_debug_group_report() -> Array:
		if not groups.is_empty():
			return groups
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
			"Rounds & collision", "Terrain & foliage", "Rendering & overlays",
			"Environment", "Audio",
			"Animation & models", "Player", "Stats", "Perf"]:
		assert_has(texts, page_title, "the %s page is listed" % page_title)
	assert_false(texts.has("View"), "the dissolved View page is gone")
	var header_row := texts.find("SIMULATION")
	assert_false(list.is_item_selectable(header_row), "section headers are not rows")
	assert_false(list.is_item_disabled(header_row),
			"section headers keep readable contrast without becoming selectable")


func test_shell_omits_search_and_page_count_chrome() -> void:
	var overlay := _make_overlay()
	assert_null(overlay.find_child("DebugSearch", true, false),
			"the categorized sidebar does not need another page/control search")
	assert_null(overlay.find_child("DebugSearchResults", true, false),
			"the shell does not spend space announcing how many pages it owns")
	var help := overlay.find_child("ActivePageHelp", true, false) as Label
	assert_false(help.text.to_lower().contains("control"),
			"page headings communicate location without catalog-count noise")


func test_copy_always_captures_every_control() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	var copy := overlay.find_child("CopyDebugSnapshot", true, false) as Button
	var feedback_timer := overlay.find_child(
			"CopyFeedbackTimer", true, false) as Timer
	var full_control_count: int = overlay.list_controls().size()

	var copied_payload: Dictionary = overlay.capture_clipboard_snapshot()
	assert_eq((copied_payload.get("controls", []) as Array).size(),
			full_control_count,
			"the shell copy captures the complete public control catalog")

	copy.pressed.emit()
	assert_eq(copy.text, "Copied")
	assert_true(feedback_timer.time_left > 0.0)
	feedback_timer.timeout.emit()
	assert_eq(copy.text, "Copy", "copy confirmation resets instead of sticking forever")


func test_page_navigation_is_focused_on_open_and_escape_closes() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	assert_true(overlay.select_page(&"Rounds"))
	var page_list := overlay.find_child("PageList", true, false) as ItemList
	var compact_picker := overlay.find_child(
			"CompactPagePicker", true, false) as OptionButton
	assert_eq(page_list.focus_mode, Control.FOCUS_ALL)
	assert_true(page_list.has_focus() or compact_picker.has_focus(),
			"opening F3 puts keyboard focus on the navigation that remains")

	var active_before: StringName = overlay.get_active_page_id()
	var next_page := InputEventKey.new()
	next_page.keycode = KEY_PAGEDOWN
	next_page.ctrl_pressed = true
	next_page.pressed = true
	assert_true(overlay.handle_key_input(next_page))
	assert_ne(overlay.get_active_page_id(), active_before)

	var find_shortcut := InputEventKey.new()
	find_shortcut.keycode = KEY_F
	find_shortcut.ctrl_pressed = true
	find_shortcut.pressed = true
	assert_false(overlay.handle_key_input(find_shortcut),
			"Ctrl+F is not captured after removing the redundant shell search")

	var escape := InputEventKey.new()
	escape.keycode = KEY_ESCAPE
	escape.pressed = true
	assert_true(overlay.handle_key_input(escape))
	assert_false(overlay.visible, "Escape closes the dock in one step")


func test_page_cycle_shortcut_precedes_focused_page_controls() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(900, 900)
	add_child_autofree(viewport)
	var config_path := "user://test_debug_overlay_%d.cfg" % Time.get_ticks_usec()
	_dumped_paths.append(ProjectSettings.globalize_path(config_path))
	var overlay: CanvasLayer = OverlayScript.new(config_path)
	viewport.add_child(overlay)
	overlay.toggle()
	assert_true(overlay.select_page(&"Stats"))
	await wait_process_frames(2)

	var stats_tree := overlay.get_node(PAGES + "/Stats/StatsRows") as Tree
	stats_tree.grab_focus()
	var next_page := InputEventKey.new()
	next_page.keycode = KEY_PAGEDOWN
	next_page.ctrl_pressed = true
	next_page.pressed = true
	viewport.push_input(next_page, true)
	await wait_process_frames(2)
	assert_eq(String(overlay.get_active_page_id()), "Perf",
			"Ctrl+PageDown cycles pages before a focused Tree consumes PageDown")


func test_selection_and_width_persist_across_instances() -> void:
	var config_path := "user://test_debug_overlay_persist_%d.cfg" % Time.get_ticks_usec()
	_dumped_paths.append(ProjectSettings.globalize_path(config_path))
	var viewport := SubViewport.new()
	viewport.size = Vector2i(900, 900)
	add_child_autofree(viewport)
	var first: CanvasLayer = OverlayScript.new(config_path)
	viewport.add_child(first)
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
	viewport.remove_child(first)
	first.free()

	var second: CanvasLayer = OverlayScript.new(config_path)
	viewport.add_child(second)
	assert_eq(String(second.get_active_page_id()), "Rounds",
			"the last-selected page survives a relaunch")
	var second_panel := second.find_child("DebugPanel", true, false) as Control
	assert_almost_eq(-second_panel.offset_left, widened, 0.01,
			"the panel width survives a relaunch")
	var second_handle := second.find_child("DebugResizeHandle", true, false) as Control
	assert_gte(second_handle.custom_minimum_size.x, 10.0,
			"the visible resize target is large enough to discover")
	var reset := InputEventMouseButton.new()
	reset.button_index = MOUSE_BUTTON_LEFT
	reset.pressed = true
	reset.double_click = true
	second_handle.gui_input.emit(reset)
	assert_almost_eq(-second_panel.offset_left, 560.0, 0.01,
			"double-clicking the resize divider restores the default width")


class TestShellPage:
	extends NovaDebugPage
	var refreshed := 0

	func page_id() -> StringName:
		return &"ShellExtras"

	func page_title() -> String:
		return "Shell extras"

	func page_category() -> StringName:
		return &"Shell"

	func refresh() -> void:
		refreshed += 1


func test_register_page_appends_a_shell_page() -> void:
	var overlay := _make_overlay()
	var page := TestShellPage.new()
	overlay.register_page(page)
	overlay.toggle()
	assert_true(overlay.select_page(&"ShellExtras"), "the registered page selects by id")
	assert_gt(page.refreshed, 0, "selection refreshes the newly active page")
	var texts := _page_list_texts(overlay)
	assert_has(texts, "SHELL", "a custom category grows its own section")
	assert_has(texts, "Shell extras", "the page lists under it")


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
			(rows.get_child(0).get_node("SelectPick0") as Button).text, "crate_a")

	(rows.get_child(0).get_node("RemovePick") as Button).pressed.emit()
	assert_eq(picks.size(), 1, "the row's X removes exactly that pick")
	assert_eq(int(picks.get_picks()[0].get("entity_handle", -1)), 2)
	(overlay.find_child("ClearPicks", true, false) as Button).pressed.emit()
	assert_eq(picks.size(), 0, "Clear picks empties the set")
	await wait_process_frames(2)


func test_replacing_pick_list_detaches_entities_even_while_page_is_inactive() -> void:
	var overlay := _make_overlay()
	var old_picks := NovaDebugPickList.new()
	var new_picks := NovaDebugPickList.new()
	old_picks.add(_fabricated_pick(8, "old_pick"))
	overlay.set_pick_list(old_picks)
	overlay.toggle()
	assert_true(overlay.select_page(&"Entities"))
	assert_eq(old_picks.picked.get_connections().size(), 1)
	var rows := overlay.find_child("PickRows", true, false)
	assert_eq(rows.get_child_count(), 1)

	assert_true(overlay.select_page(&"Particles"))
	overlay.close()
	overlay.set_pick_list(new_picks)
	assert_eq(old_picks.picked.get_connections().size(), 0,
			"the inactive page no longer listens to the retired pick list")
	assert_eq(new_picks.picked.get_connections().size(), 1,
			"the replacement binds without waiting for Entities to refresh")
	assert_eq(rows.get_child_count(), 0,
			"an empty replacement cannot leave stale pick buttons on the hidden page")

	overlay.set_pick_list(null)
	assert_eq(new_picks.picked.get_connections().size(), 0,
			"detaching the shell pick model removes the last page callback")
	await wait_process_frames(2)
