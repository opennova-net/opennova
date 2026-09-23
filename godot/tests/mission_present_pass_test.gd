extends GutTest

# The native EntityPresenter's PLACED walk (the mission present pass) applies each
# entity's transform + PANM part channels + visibility onto its placed node every
# tick, from ONE batched sim snapshot. Real native components end to end: ObjectModel
# nodes (their CTRL store, body clips, Node3D state and the two visibility-owner
# bits are the observables), a real EntityIndex, and PF-layout snapshots built as
# pure data and fed through the public present_snapshot API. Change-gating claims
# read the presenter's stats counters — never instrumentation subclasses.
#
# The old aliased-register dismount cases are gone by design: production
# ObjectModel maps PLAYPARTANIM channels 1/2 to fixed VEHICLE_SPECIAL1/2
# [orig: HUD_CacheEntityDisplayInfo @ 0x4A3E18..0x4A3E38], so a part channel
# can never alias EWEAP registers; the register-independence case pins the
# real layout.

const RIGGED_3DI := "res://../fixtures/threedi/synth/shed.3di"
const MUZZLE_3DI := "res://../fixtures/threedi/synth/gun.3di"
const SECTIONED_3DI := "res://../fixtures/threedi/synth/pump.3di"


# Builds the flat PF-layout snapshot Simulation.get_present_snapshot()
# emits. Each entity is a Dictionary of overrides; unset fields default sanely
# (alive, not hidden, identity). Pure data — nothing here is a sim double.
class Snapshot:
	extends RefCounted
	var entities: Array = []
	func build() -> PackedFloat32Array:
		var stride: int = Simulation.PF_STRIDE
		var out := PackedFloat32Array()
		out.resize(entities.size() * stride)
		for i in range(entities.size()):
			var e: Dictionary = entities[i]
			var b := i * stride
			out[b + Simulation.PF_KIND] = float(e.get("kind", -1))
			out[b + Simulation.PF_INDEX] = float(e.get("index", -1))
			out[b + Simulation.PF_BMS_ID] = float(e.get("bms_id", 0))
			out[b + Simulation.PF_TYPE_ID] = float(e.get("type_id", 0))
			out[b + Simulation.PF_WIRE_HANDLE] = float(e.get("handle", 0))
			out[b + Simulation.PF_NET_ID] = float(e.get("net_id", 0))
			out[b + Simulation.PF_POS_X] = float(e.get("pos_x", 0.0))
			out[b + Simulation.PF_POS_Y] = float(e.get("pos_y", 0.0))
			out[b + Simulation.PF_POS_Z] = float(e.get("pos_z", 0.0))
			out[b + Simulation.PF_YAW_DEG] = float(e.get("yaw_deg", 0.0))
			PresentPassFixture.write_phase(out, b, 1, int(e.get("phase1", 0)),
					int(e.get("active1", 0)) != 0)
			PresentPassFixture.write_phase(out, b, 2, int(e.get("phase2", 0)),
					int(e.get("active2", 0)) != 0)
			var doors: Array = e.get("doors", [])
			out[b + Simulation.PF_DOOR_COUNT] = float(doors.size())
			var destroy: Array = e.get("destroy", [0, 0, 0, 0, 0, 0])
			for channel in range(6):
				out[b + Simulation.PF_OBJECT_DESTROY + channel] = float(destroy[channel])
			out[b + Simulation.PF_BODY_ANIM_SLOT] = float(e.get("body_anim_slot", -1))
			out[b + Simulation.PF_ANIM_STATE] = float(e.get("anim_state", -1))
			out[b + Simulation.PF_ANIM_PHASE_TICKS] = float(e.get("anim_phase", 0))
			out[b + Simulation.PF_ANIM_SOURCE_STATE] = float(
					e.get("anim_source_state", -1))
			out[b + Simulation.PF_ANIM_SOURCE_PHASE_TICKS] = float(
					e.get("anim_source_phase", -1))
			out[b + Simulation.PF_ANIM_BLEND_WEIGHT] = float(
					e.get("anim_blend_weight", 1.0))
			out[b + Simulation.PF_ANIM_VARIANT] = float(e.get("anim_variant", 0))
			out[b + Simulation.PF_ANIM_SOURCE_VARIANT] = float(
					e.get("anim_source_variant", 0))
			out[b + Simulation.PF_HIDDEN] = float(e.get("hidden", 0))
			out[b + Simulation.PF_LOCAL_VIEW_SUPPRESSED] = float(
					e.get("local_view_suppressed", 0))
			out[b + Simulation.PF_ALIVE] = float(e.get("alive", 1))
			out[b + Simulation.PF_AIM_OVERLAY_VALID] = float(
					e.get("aim_overlay_valid", 0))
			var body: Vector3 = e.get("aim_body", Vector3.ZERO)
			out[b + Simulation.PF_AIM_BODY_PITCH_DEG] = body.x
			out[b + Simulation.PF_AIM_BODY_YAW_DEG] = body.y
			out[b + Simulation.PF_AIM_BODY_ROLL_DEG] = body.z
			out[b + Simulation.PF_EMPLACED_CONTROLS_VALID] = float(
					e.get("emplaced_controls_valid", 0))
			out[b + Simulation.PF_EWEAP_GUNYAW] = float(
					e.get("emplaced_gun_yaw", 0))
			out[b + Simulation.PF_EWEAP_GUNPITCH] = float(
					e.get("emplaced_gun_pitch", 0))
			out[b + Simulation.PF_WEAP_SPIN] = float(e.get("weap_spin", 0))
			out[b + Simulation.PF_VEHICLE_MOTION_VALID] = float(
					e.get("vehicle_motion_valid", 0))
			out[b + Simulation.PF_VEHICLE_CTRL_MASK] = float(
					e.get("vehicle_ctrl_mask", 31))
			out[b + Simulation.PF_VEHICLE_TRACK_LEFT] = float(
					e.get("vehicle_track_left", 0))
			out[b + Simulation.PF_VEHICLE_TRACK_RIGHT] = float(
					e.get("vehicle_track_right", 0))
			out[b + Simulation.PF_VEHICLE_GUN_YAW] = float(
					e.get("vehicle_gun_yaw", 0))
			out[b + Simulation.PF_VEHICLE_GUN_PITCH] = float(
					e.get("vehicle_gun_pitch", 0))
			out[b + Simulation.PF_VEHICLE_STEERING] = float(
					e.get("vehicle_steering", 0))
			out[b + Simulation.PF_VEHICLE_SPEED] = float(
					e.get("vehicle_speed", 0))
			out[b + Simulation.PF_VEHICLE_ROTOR] = float(
					e.get("vehicle_rotor", 0))
			out[b + Simulation.PF_VEHICLE_TAIL_ROTOR] = float(
					e.get("vehicle_tail_rotor", 0))
			out[b + Simulation.PF_VEHICLE_WHEELS] = float(
					e.get("vehicle_wheels", 0))
			for tire in range(6):
				out[b + Simulation.PF_VEHICLE_TIRE00 + tire] = float(
						e.get("vehicle_tires", [0, 0, 0, 0, 0, 0])[tire])
			out[b + Simulation.PF_TEX_TEAM_VALID] = float(
					e.get("tex_team_valid", 0))
			out[b + Simulation.PF_TEX_TEAM] = float(
					e.get("tex_team", 0))
			out[b + Simulation.PF_ZONE_CTRL_VALID] = float(
					e.get("zone_ctrl_valid", 0))
			out[b + Simulation.PF_TEAMSWING] = float(
					e.get("team_swing", 0))
			out[b + Simulation.PF_LFP_CAMPPERCENT_VALID] = float(
					e.get("lfp_camp_percent_valid", 0))
			out[b + Simulation.PF_LFP_CAMPPERCENT] = float(
					e.get("lfp_camp_percent", 0))
			out[b + Simulation.PF_WORLD_HEAT_GLOW_VALID] = float(
					e.get("world_heat_glow_valid", 0))
			out[b + Simulation.PF_WORLD_HEAT_GLOW] = float(
					e.get("world_heat_glow", 0))
			out[b + Simulation.PF_RIGHT_HAND_COLLAPSED] = float(
					e.get("right_hand_collapsed", 0))
			var section_mask := int(e.get("section_mask", 0)) & 0xFFFFFFFF
			out[b + Simulation.PF_SECTION_MASK_VALID] = float(
					e.get("section_mask_valid", 0))
			out[b + Simulation.PF_SECTION_MASK_LO] = float(section_mask & 0xFFFF)
			out[b + Simulation.PF_SECTION_MASK_HI] = float(
					(section_mask >> 16) & 0xFFFF)
			var angles: PackedVector3Array = e.get(
					"aim_angles", PackedVector3Array())
			for cls in range(mini(angles.size(), 9)):
				var a := angles[cls]
				var ob := (b + Simulation.PF_AIM_ANGLES
						+ cls * Simulation.PF_AIM_CLASS_STRIDE)
				out[ob] = a.x
				out[ob + 1] = a.y
				out[ob + 2] = a.z
		return out
	# The door side table Simulation.get_present_door_phases() emits beside the
	# rows: (row index, count, phase[count]) entries for the rows whose "doors"
	# list is non-empty, in row order.
	func build_doors() -> PackedInt32Array:
		var out := PackedInt32Array()
		for i in range(entities.size()):
			var doors: Array = entities[i].get("doors", [])
			if doors.is_empty():
				continue
			out.append(i)
			out.append(doors.size())
			for door in doors:
				out.append(int(door))
		return out


func _model() -> ObjectModel:
	var m := ObjectModel.new()
	add_child_autofree(m)
	m.set_process(false)
	return m


func _sectioned_model() -> ObjectModel:
	var m := _model()
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(SECTIONED_3DI)), OK)
	m.set_object_data(data)
	return m


# A rigged model whose skeletal set registers the exact semantic keys the
# applier's infantry map produces (anim_idle = state 43, anim_walk_forward =
# state 1, anim_reset = state 0).
func _rigged_model() -> ObjectModel:
	var m := ObjectModel.new()
	add_child_autofree(m)
	m.set_process(false)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(RIGGED_3DI)), OK)
	var anim_root := ResourceRoot.new()
	assert_eq(anim_root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/anim")), OK)
	var sk := SkeletalAnim.new()
	assert_true(sk.load_from_bad_files(anim_root, "idle.bad", {
		"anim_reset": "idle.bad",
		"anim_idle": "idle.bad",
		"anim_walk_forward": "walk.bad",
	}))
	m.set_object_data(data)
	m.set_skeletal_anim(sk)
	return m


# A model with the def-named AI muzzle (the D-AI-6 seam): the def's
# launchups_* name (pushed by the placer in production) resolves against the
# model's userpoint table case-insensitively — gun authors Bullet01 and the
# committed rig gives it a skeleton.
func _muzzle_model() -> ObjectModel:
	var m := ObjectModel.new()
	add_child_autofree(m)
	m.set_process(false)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(MUZZLE_3DI)), OK)
	var anim_root := ResourceRoot.new()
	assert_eq(anim_root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/anim")), OK)
	var sk := SkeletalAnim.new()
	assert_true(sk.load_from_resource_root(anim_root, "soldier.adm"))
	m.set_muzzle_point_name("bullet01")  # lower-case: pins the stricmp match
	m.set_object_data(data)
	m.set_skeletal_anim(sk)
	assert_true(m.has_muzzle(), "the def-named userpoint resolves on the model")
	return m


func _index_of(by_bms_id: Dictionary) -> EntityIndex:
	var models: Array[ObjectModel] = []
	for bms_id in by_bms_id.keys():
		var model: ObjectModel = by_bms_id[bms_id]
		model.entity_ref = EntityRef.make(1, int(bms_id), int(bms_id))
		models.append(model)
	var index := EntityIndex.new()
	index.build(models, null)
	return index


func _make_pass(index: EntityIndex, sim: Simulation = null,
		options: Dictionary = {}) -> EntityPresenter:
	var p := EntityPresenter.new()
	add_child_autofree(p)
	p.setup(sim, index, options.get("placer"))
	var channels := int(EntityPresenter.OUTPUT_ALL)
	if not bool(options.get("drive_part_anim", true)):
		channels &= ~EntityPresenter.OUTPUT_PART_ANIM
	p.set_output_channels(channels)
	return p


func _present(p: Object, snap: Snapshot, revision: int = 1) -> void:
	p.present_snapshot(snap.build(), Simulation.PF_STRIDE, revision, snap.build_doors())


func _ctrl(model: ObjectModel, name: String) -> int:
	return int(model.get_ctrl_values().get(name, -1))


func _stat(p: Object, key: String) -> int:
	return int(p.get_stats_record().get(key))


func _clip_time(model: ObjectModel, key: String, phase_ticks: int) -> float:
	var fps: float = model.get_skeletal_anim().get_clip_fps(key)
	assert_gt(fps, 0.0, "fixture clip %s carries a frame rate" % key)
	return model.get_skeletal_anim().get_clip_phase_seconds(key, phase_ticks)


func test_door_phases_preserve_endpoints_and_release_only_their_owner() -> void:
	var model := _model()
	model.set_ctrl_value("DOOR_00", 77)
	var p := _make_pass(_index_of({ 1: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 1, "doors": [0, 65536, -1] }]
	_present(p, snap)
	assert_eq(_ctrl(model, "DOOR_00"), 0, "a closed door actively publishes zero")
	assert_eq(_ctrl(model, "DOOR_01"), 65536, "fully open retains the high word")
	assert_eq(_ctrl(model, "DOOR_02"), -1, "signed phase words survive the float row")
	snap.entities[0]["doors"] = [32768]
	_present(p, snap)
	assert_eq(_ctrl(model, "DOOR_00"), 32768)
	assert_false(model.get_ctrl_values().has("DOOR_01"))
	assert_false(model.get_ctrl_values().has("DOOR_02"))
	snap.entities[0]["doors"] = []
	_present(p, snap)
	assert_false(model.get_ctrl_values().has("DOOR_00"),
			"retail CTRL is one value; overwritten values are never restored")
	snap.entities[0]["doors"] = [123]
	_present(p, snap)
	model.set_ctrl_value("DOOR_00", 77)
	snap.entities[0]["doors"] = []
	_present(p, snap)
	assert_eq(_ctrl(model, "DOOR_00"), 77, "stale door teardown preserves a later writer")
	snap.entities[0]["doors"] = [456]
	_present(p, snap)
	p.set_output_channels(int(p.get_output_channels()) & ~EntityPresenter.OUTPUT_PART_ANIM)
	assert_false(model.get_ctrl_values().has("DOOR_00"), "disabling the output releases doors")


func test_cold_door_row_releases_only_what_the_door_writer_owned() -> void:
	# A cold row cannot enumerate the door registers it published before, so
	# the cold present releases by OWNER instead of probing all 30 names: a
	# stale present:doors value goes, a foreign writer's DOOR register stays,
	# and a zero-door cold row issues no per-register work at all.
	var model := _model()
	model.set_ctrl_override("present:doors", "DOOR_05", 1)
	model.set_ctrl_override("foreign", "DOOR_00", 77)
	var p := _make_pass(_index_of({ 1: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 1, "doors": [] }]
	var before := _stat(p, "control_dispatches")
	_present(p, snap)
	assert_false(model.get_ctrl_values().has("DOOR_05"),
			"the cold present releases the stale door this writer owned")
	assert_eq(_ctrl(model, "DOOR_00"), 77, "a foreign DOOR register survives the cold release")
	assert_eq(_stat(p, "control_dispatches") - before, 9,
			"a zero-door cold row pays only the sibling writers' cold clears " +
			"(3 emplaced + 2 vehicle + 3 zone + 1 heat), no door probes")
	snap.entities[0]["doors"] = [0, 65536]
	_present(p, snap)
	assert_eq(_ctrl(model, "DOOR_00"), 0, "a door row overwrites the foreign value (one CTRL value)")
	assert_eq(_ctrl(model, "DOOR_01"), 65536, "the side table carries the exact signed dword")


func test_retail_door_visible_part_uses_the_presented_phase() -> void:
	var assets := RetailData.assets()
	if assets.is_empty():
		pending("OPENNOVA_JO_ASSETS is required for the Iblock01 door")
		return
	var data := ObjectData.new()
	assert_eq(data.open_file(assets.path_join("IBlock01.3di")), OK)
	var model := _model()
	model.set_object_data(data)
	var clock := PanmClock.new()
	clock.set_time_ms_for_test(0)
	model.set_panm_clock(clock)
	var parts := model.get_render_part_nodes()
	assert_true(parts.has(0) and parts.has(1))
	if not parts.has(0) or not parts.has(1):
		return
	var building := parts[0] as Node3D
	var door := parts[1] as Node3D
	var p := _make_pass(_index_of({ 1: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 1, "doors": [0] }]
	_present(p, snap)
	model.advance_runtime_frame(0.0)
	var closed := door.transform
	var fixed := building.transform
	snap.entities[0]["doors"] = [65536]
	_present(p, snap)
	model.advance_runtime_frame(0.0)
	assert_ne(door.transform, closed, "the retail door visibly opens at the endpoint")
	assert_eq(building.transform, fixed, "the building stays fixed")
	snap.entities[0]["doors"] = [0]
	_present(p, snap)
	model.advance_runtime_frame(0.0)
	assert_eq(door.transform, closed, "closing returns to the exact initial transform")


func test_active_channel_poses_to_phase() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 1001: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 1001, "active1": 1, "phase1": 32768 }]
	_present(p, snap)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 32768,
			"engine-computed phase passes through to the PANM register")
	# 2 = the posed channel + the cold defensive release of the never-active
	# channel 2 (a fresh plan clears controls a prior owner may have left).
	assert_eq(_stat(p, "part_dispatches"), 2, "one channel posed, one cold release")


func test_transform_presentation_advances_the_static_shadow_registry_once() -> void:
	var model := _model()
	var index := EntityIndex.new()
	model.entity_ref = EntityRef.make(MissionData.KIND_BUILDING, 7, 501)
	index.build([model], null)
	var placer := MissionObjectPlacer.new()
	placer.register_static_instance(501, "Caster", 7,
			Transform3D(Basis.IDENTITY, Vector3(-9, -9, -9)), true)
	var p := _make_pass(index, null, { "placer": placer })
	var snap := Snapshot.new()
	snap.entities = [{
		"kind": MissionData.KIND_BUILDING, "index": 7, "bms_id": 501,
		"pos_x": 4.0, "pos_y": 5.0, "pos_z": 6.0, "yaw_deg": 30.0,
	}]
	var revision := placer.get_static_terrain_shadow_source_revision()
	_present(p, snap)
	assert_gt(placer.get_static_terrain_shadow_source_revision(), revision,
			"the first live transform repairs the placement-time source snapshot")
	assert_eq(placer.hide_static_instance(501), model.transform,
			"the carve consumes the exact pose applied by presentation")
	assert_true(placer.show_static_instance(501))
	revision = placer.get_static_terrain_shadow_source_revision()
	_present(p, snap)
	assert_eq(placer.get_static_terrain_shadow_source_revision(), revision,
			"an unchanged packed present row is a no-op for the terrain cache")
	snap.entities[0]["pos_x"] = 8.0
	_present(p, snap)
	assert_gt(placer.get_static_terrain_shadow_source_revision(), revision,
			"a later real movement invalidates the source exactly once")
	assert_eq(placer.hide_static_instance(501), model.transform)
	assert_true(placer.show_static_instance(501))


func test_publication_ownership_writes_zero_and_releases_suppressed_channel() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 1: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 1, "active1": 1, "phase1": 1234, "active2": 1, "phase2": 5678,
	}]
	_present(p, snap)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 1234)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL2"), 5678)

	snap.entities[0]["phase2"] = 0
	snap.entities[0]["active1"] = 0
	_present(p, snap)
	assert_false(model.get_ctrl_values().has("VEHICLE_SPECIAL1"),
			"a suppressed SPECIAL1 releases its prior override")
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL2"), 0,
			"an owned zero endpoint is still published")


func test_part_channel_releases_while_inactive_and_catches_up_when_reactivated() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 1: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 1, "active1": 1, "phase1": 100 }]
	_present(p, snap)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 100)
	assert_eq(_stat(p, "part_dispatches"), 2, "pose + the cold channel-2 release")

	snap.entities[0]["active1"] = 0
	snap.entities[0]["phase1"] = 200
	_present(p, snap)
	assert_false(model.get_ctrl_values().has("VEHICLE_SPECIAL1"),
			"an inactive channel releases its prior register publication")
	assert_eq(_stat(p, "part_dispatches"), 3, "the release dispatches once")
	_present(p, snap)
	assert_eq(_stat(p, "part_dispatches"), 3,
			"an unchanged inactive stamp does not redispatch the release")

	snap.entities[0]["active1"] = 1
	_present(p, snap)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 200,
			"reactivation catches the model up to the current phase")
	assert_eq(_stat(p, "part_dispatches"), 4, "exactly one catch-up dispatch")


# Retail evaluates the presentation writers per SUBMITTED model
# [orig: Terrain_RenderSectorModels @ 0x5c5d30]: a row whose bounds notifier
# reports off-screen skips the part/CTRL/aim dispatches and catches up with
# the live state at its next submission. set_on_screen is the production seam
# the bounds notifier drives.
func test_offscreen_row_skips_presentation_writers_until_reentry() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 7: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 7, "active1": 1, "phase1": 100,
			"emplaced_controls_valid": 1, "emplaced_gun_yaw": 40,
			"emplaced_gun_pitch": 8 }]
	_present(p, snap)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 100, "the submitted row poses")
	assert_eq(_ctrl(model, "EWEAP_GUNYAW"), 40)

	model.set_on_screen(false)
	snap.entities[0]["phase1"] = 250
	snap.entities[0]["emplaced_gun_yaw"] = 90
	_present(p, snap)
	_present(p, snap)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 100,
			"an off-screen row receives no part dispatch")
	assert_eq(_ctrl(model, "EWEAP_GUNYAW"), 40,
			"an off-screen row's emplaced controls hold the last submitted state")

	model.set_on_screen(true)
	snap.entities[0]["phase1"] = 400
	_present(p, snap)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 400,
			"re-entry re-poses to the live phase, not the missed ones")
	assert_eq(_ctrl(model, "EWEAP_GUNYAW"), 90,
			"re-entry reasserts the emplaced writer with current values")


func test_offscreen_falling_edge_releases_at_the_next_submission() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 3: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 3, "active1": 1, "phase1": 100 }]
	_present(p, snap)
	assert_true(model.get_ctrl_values().has("VEHICLE_SPECIAL1"))

	model.set_on_screen(false)
	snap.entities[0]["active1"] = 0
	_present(p, snap)
	assert_true(model.get_ctrl_values().has("VEHICLE_SPECIAL1"),
			"no release dispatch while the row is not submitted")
	model.set_on_screen(true)
	_present(p, snap)
	assert_false(model.get_ctrl_values().has("VEHICLE_SPECIAL1"),
			"the falling edge latched while off-screen releases on re-entry")


func test_both_channels_posed() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 7: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 7, "active1": 1, "phase1": 100,
			"active2": 1, "phase2": 200 }]
	_present(p, snap)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 100)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL2"), 200, "both active channels posed")


func test_emplaced_weapon_uses_named_controls_holds_and_releases() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 8: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 8,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x1234,
		"emplaced_gun_pitch": 0xFEDC,
		"weap_spin": 0xFFFF,
	}]
	var held := {
		"EWEAP_GUNYAW": 0x1234,
		"EWEAP_GUNPITCH": 0xFEDC,
		"WEAP_SPIN": 0xFFFF,
	}
	_present(p, snap)
	assert_eq(model.get_ctrl_values(), held,
			"semantic controls do not alias model-order PLAYPARTANIM channels")

	# The ewep writer has no occupant test, so a gun its gunner left keeps
	# publishing the same words: the turret holds its last traverse.
	# [orig: HUD_CacheWeaponSlotInfo @0x440930 via def+0x144 of the 'ewep'
	#  render-class row @0x82CFA0]
	_present(p, snap)
	assert_eq(model.get_ctrl_values(), held,
			"a dismounted gun's held words stay applied")

	snap.entities[0]["emplaced_controls_valid"] = 0
	_present(p, snap)
	assert_true(model.get_ctrl_values().is_empty(),
			"a row that stops publishing releases the retained EWEAP controls")


func test_world_heat_glow_owns_cold_zero_and_releases_unavailable_state() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 10: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 10,
		"world_heat_glow_valid": 1,
		"world_heat_glow": 0,
	}]
	_present(p, snap)
	assert_eq(_ctrl(model, "HEAT_GLOW"), 0,
			"the authoritative cold branch publishes literal zero")

	snap.entities[0]["world_heat_glow"] = 0xFFFF
	_present(p, snap)
	assert_eq(_ctrl(model, "HEAT_GLOW"), 0xFFFF,
			"the world-model endpoint is the unsigned-word ceiling")

	snap.entities[0]["world_heat_glow_valid"] = 0
	_present(p, snap)
	assert_false(model.get_ctrl_values().has("HEAT_GLOW"),
			"a row without authoritative MountSlot heat releases this writer")


func test_vehicle_motion_controls_publish_and_release_as_one_owned_pair() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 61: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 61,
		"vehicle_motion_valid": 1,
		"vehicle_steering": 0xFEDC,
		"vehicle_speed": 0x10000,
		"vehicle_rotor": 0x1234,
		"vehicle_tail_rotor": 0x1234,
		"vehicle_wheels": 0xABCD,
		"vehicle_tires": [0, 100, 200, 300, 400, 65536],
	}]
	_present(p, snap)
	assert_eq(model.get_ctrl_values(), {
		"VEHICLE_STEERING": 0xFEDC,
		"VEHICLE_SPEED": 0x10000,
		"HELO_ROTOR": 0x1234,
		"HELO_TAILROTOR": 0x1234,
		"VEHICLE_WHEELS": 0xABCD,
		"VEHICLE_TIRE00": 0,
		"VEHICLE_TIRE01": 100,
		"VEHICLE_TIRE02": 200,
		"VEHICLE_TIRE03": 300,
		"VEHICLE_TIRE04": 400,
		"VEHICLE_TIRE05": 65536,
	}, "the cveh callback's eleven words publish by semantic retail name")

	# The part-animation words are owned at rest too: literal zero is a write,
	# exactly like the steer/speed pair (Entity_CacheVehicleHUDStats stores all
	# five before every model submission).
	snap.entities[0]["vehicle_rotor"] = 0
	snap.entities[0]["vehicle_tail_rotor"] = 0
	snap.entities[0]["vehicle_wheels"] = 0
	_present(p, snap)
	assert_eq(_ctrl(model, "HELO_ROTOR"), 0,
			"a resting rotor publishes literal zero rather than releasing")
	assert_eq(_ctrl(model, "VEHICLE_WHEELS"), 0)

	snap.entities[0]["vehicle_tires"][2] = 1234
	_present(p, snap)
	assert_eq(_ctrl(model, "VEHICLE_TIRE02"), 1234)

	# Tank owns two alternating track phases and signed turret controls.
	snap.entities[0]["vehicle_ctrl_mask"] = 1 | 2 | 4 | 8 | 32 | 64
	snap.entities[0]["vehicle_track_left"] = 0xFFFF
	snap.entities[0]["vehicle_track_right"] = 0x2345
	snap.entities[0]["vehicle_gun_yaw"] = -123
	snap.entities[0]["vehicle_gun_pitch"] = 456
	_present(p, snap)
	for tire in range(6):
		assert_false(model.get_ctrl_values().has("VEHICLE_TIRE%02d" % tire))
	for track in range(4):
		assert_eq(_ctrl(model, "VEHICLE_WHEELS%02d" % track),
				0xFFFF if track % 2 == 0 else 0x2345)
	assert_eq(_ctrl(model, "VEHICLE_GUNYAW"), -123)
	assert_eq(_ctrl(model, "VEHICLE_GUNPITCH"), 456)
	# Helo render ownership releases ground/tank channels on the same node.
	snap.entities[0]["vehicle_ctrl_mask"] = 4 | 128
	_present(p, snap)
	assert_eq(model.get_ctrl_values(), {
		"HELO_ROTOR": 0, "HELO_TAILROTOR": 0,
		"HELO_GUNYAW": -123, "HELO_GUNPITCH": 456,
	})

	snap.entities[0]["vehicle_motion_valid"] = 0
	_present(p, snap)
	assert_true(model.get_ctrl_values().is_empty(),
			"an unavailable/non-authoritative row releases all cveh writers")


func test_sector_and_zone_controls_preserve_write_validity_and_scoped_release() -> void:
	var model := _model()
	# This unrelated writer is the stand-in for another semantic producer
	# (DOOR_00 is a real retail register no presenter touches). Zone teardown
	# must never bulk-clear the CTRL registers.
	model.set_ctrl_override("foreign", "DOOR_00", 77)
	var p := _make_pass(_index_of({ 62: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 62,
		"tex_team_valid": 1,
		"tex_team": -1,
		"zone_ctrl_valid": 1,
		"team_swing": 0,
		"lfp_camp_percent_valid": 1,
		"lfp_camp_percent": 0x8000,
	}]
	_present(p, snap)
	assert_eq(model.get_ctrl_values(), {
		"DOOR_00": 77,
		"TEX_TEAM": -1,
		"TEAMSWING": 0,
		"LFP_CAMPPERCENT": 0x8000,
	}, "literal zero TEAMSWING remains an owned retail write")

	# Retail executes each valid writer at model submission, even when its input
	# snapshot did not change. A retained snapshot cache must therefore recover
	# from an intervening producer instead of leaving the foreign value.
	model.set_ctrl_override("foreign", "TEX_TEAM", 7)
	_present(p, snap)
	assert_eq(_ctrl(model, "TEX_TEAM"), -1,
			"an unchanged valid snapshot reasserts the retail writer")

	snap.entities[0]["lfp_camp_percent_valid"] = 0
	_present(p, snap)
	assert_false(model.get_ctrl_values().has("LFP_CAMPPERCENT"),
			"a zone without a timer-list entry omits LFP instead of writing zero")
	assert_eq(_ctrl(model, "TEAMSWING"), 0,
			"the unconditional zone writer remains owned independently")

	snap.entities[0]["tex_team_valid"] = 0
	snap.entities[0]["zone_ctrl_valid"] = 0
	_present(p, snap)
	assert_eq(model.get_ctrl_values(), { "DOOR_00": 77 },
			"leaving both callbacks releases only their scoped writers")


func test_part_anim_and_emplaced_controls_remain_independent() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 9: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 9,
		"active1": 1,
		"phase1": 0x1111,
		"active2": 1,
		"phase2": 0xEEEE,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x2222,
		"emplaced_gun_pitch": 0xDDDD,
	}]
	_present(p, snap)
	assert_eq(model.get_ctrl_values(), {
		"VEHICLE_SPECIAL1": 0x1111,
		"VEHICLE_SPECIAL2": 0xEEEE,
		"EWEAP_GUNYAW": 0x2222,
		"EWEAP_GUNPITCH": 0xDDDD,
		"WEAP_SPIN": 0,
	}, "retail publishes generic and emplaced systems on distinct semantic registers")

	snap.entities[0]["emplaced_controls_valid"] = 0
	snap.entities[0]["phase1"] = 0x3333
	snap.entities[0]["phase2"] = 0xCCCC
	_present(p, snap)
	assert_eq(model.get_ctrl_values(), {
		"VEHICLE_SPECIAL1": 0x3333,
		"VEHICLE_SPECIAL2": 0xCCCC,
	}, "a row that stops publishing releases only the EWEAP registers")


func test_first_invalid_emplaced_state_clears_stale_node_controls() -> void:
	var model := _model()
	# Controls retained from an earlier pass instance over this node (same
	# presentation owner, torn down without releasing).
	model.set_ctrl_override("present:emplaced", "EWEAP_GUNYAW", 0x1234)
	model.set_ctrl_override("present:emplaced", "EWEAP_GUNPITCH", 0xFEDC)
	var p := _make_pass(_index_of({ 8: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 8, "emplaced_controls_valid": 0 }]
	_present(p, snap)
	assert_true(model.get_ctrl_values().is_empty(),
			"the cold applied-state cache cannot retain controls from an earlier owner")


func test_body_clip_poses_to_sim_anim_state_phase() -> void:
	var model := _rigged_model()
	var p := _make_pass(_index_of({ 11: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 11, "body_anim_slot": 1,
			"anim_state": 43, "anim_phase": 9 }]
	_present(p, snap)
	assert_eq(model.get_active_body_clip(), "anim_idle",
			"infantry anim state resolves to .adm key")
	assert_almost_eq(model.get_animation_time(),
			_clip_time(model, "anim_idle", 9), 0.0001,
			"sim clip phase passes through")
	assert_eq(_stat(p, "body_dispatches"), 1, "one body clip posed")


func test_aim_and_right_hand_changes_repose_stable_body_state() -> void:
	var model := _rigged_model()
	var p := _make_pass(_index_of({ 11: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 11,
		"anim_state": 43,
		"anim_phase": 9,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(3.0, 27.0, -2.0),
		"aim_angles": PackedVector3Array([Vector3(5.0, 6.0, 7.0)]),
	}]
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 1)

	snap.entities[0]["aim_angles"] = PackedVector3Array([Vector3(8.0, 9.0, 10.0)])
	_present(p, snap)
	assert_eq(_stat(p, "aim_dispatches"), 2, "changed aim reaches the model")
	assert_eq(_stat(p, "body_dispatches"), 2,
			"changed aim reposes an otherwise-stable externally-phased body")

	snap.entities[0]["right_hand_collapsed"] = 1
	_present(p, snap)
	assert_true(model.is_right_hand_collapsed())
	assert_eq(_stat(p, "body_dispatches"), 3,
			"changed collapse state also reposes the stable body")


func test_hidden_body_catches_up_when_it_becomes_presentable() -> void:
	var model := _rigged_model()
	var p := _make_pass(_index_of({ 11: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 11,
		"hidden": 1,
		"anim_state": 43,
		"anim_phase": 9,
		"aim_overlay_valid": 1,
		"aim_angles": PackedVector3Array([Vector3(5.0, 6.0, 7.0)]),
	}]
	_present(p, snap)
	assert_eq(model.get_active_body_clip(), "",
			"hidden non-muzzle bodies skip skeletal dispatch")

	snap.entities[0]["aim_angles"] = PackedVector3Array([Vector3(8.0, 9.0, 10.0)])
	snap.entities[0]["right_hand_collapsed"] = 1
	_present(p, snap)
	assert_eq(model.get_active_body_clip(), "",
			"pose dependencies may update while hidden without writing the body")

	snap.entities[0]["hidden"] = 0
	_present(p, snap)
	assert_eq(model.get_active_body_clip(), "anim_idle",
			"visibility eligibility catches the body up to its authoritative pose")
	assert_almost_eq(model.get_animation_time(),
			_clip_time(model, "anim_idle", 9), 0.0001)


func test_body_clip_poses_authoritative_two_channel_blend() -> void:
	var model := _rigged_model()
	var p := _make_pass(_index_of({ 11: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 11,
		"anim_source_state": 43,
		"anim_source_phase": 17,
		"anim_state": 1,
		"anim_phase": 3,
		"anim_blend_weight": 0.2,
		"aim_overlay_valid": 1,
		"aim_angles": PackedVector3Array([Vector3(1.0, 2.0, 3.0)]),
	}]
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 1)
	assert_eq(model.get_active_body_clip(), "anim_walk_forward")
	assert_true(model.has_body_blend())
	assert_eq(model.get_body_blend_source_key(), "anim_idle",
			"placed NPCs consume the authority's exact primary blend tuple")
	assert_almost_eq(model.get_body_blend_source_time(),
			_clip_time(model, "anim_idle", 17), 0.0001)
	assert_almost_eq(model.get_body_blend_weight(), 0.2, 0.000001)

	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 1,
			"an unchanged retained blend does not redispatch")

	snap.entities[0]["anim_source_phase"] = 18
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 2,
			"the outgoing playhead participates in the retained pose stamp")

	snap.entities[0]["anim_blend_weight"] = 0.3
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 3,
			"the float32 blend weight participates in the retained pose stamp")

	snap.entities[0]["anim_source_state"] = 0
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 4,
			"the outgoing semantic state participates in the retained pose stamp")
	assert_eq(model.get_body_blend_source_key(), "anim_reset")
	assert_almost_eq(model.get_body_blend_source_time(),
			_clip_time(model, "anim_reset", 18), 0.0001)

	snap.entities[0]["aim_angles"] = PackedVector3Array([Vector3(4.0, 5.0, 6.0)])
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 5,
			"an overlay change reposes the retained two-channel body")

	snap.entities[0]["right_hand_collapsed"] = 1
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 6,
			"a right-hand mask change reposes the retained two-channel body")

	var channels := int(p.get_output_channels())
	p.set_output_channels(channels & ~EntityPresenter.OUTPUT_BODY_ANIM)
	snap.entities[0]["anim_source_phase"] = 19
	snap.entities[0]["anim_blend_weight"] = 0.4
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 6,
			"a disabled body channel performs no blend dispatch")
	p.set_output_channels(channels)
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 7,
			"reenabling the body channel cold-applies the full latest tuple")
	assert_almost_eq(model.get_body_blend_source_time(),
			_clip_time(model, "anim_reset", 19), 0.0001)
	assert_almost_eq(model.get_body_blend_weight(),
			0.4, 0.000001)

	snap.entities[0]["hidden"] = 1
	_present(p, snap)
	snap.entities[0]["anim_source_phase"] = 20
	snap.entities[0]["anim_blend_weight"] = 0.5
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 7,
			"a hidden non-muzzle body does not write an updated blend")
	snap.entities[0]["hidden"] = 0
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 8,
			"becoming visible catches up with the full latest blend tuple")
	assert_almost_eq(model.get_body_blend_source_time(),
			_clip_time(model, "anim_reset", 20), 0.0001)
	assert_almost_eq(model.get_body_blend_weight(),
			0.5, 0.000001)


func test_body_stamp_carries_the_served_ring_entries() -> void:
	# PF_ANIM_VARIANT / PF_ANIM_SOURCE_VARIANT: each channel's served ring entry
	# reaches the model and joins the retained pose stamp.
	# [orig: AnimMap_UpdateEntity @0x40B737..0x40B778]
	var model := _rigged_model()
	var p := _make_pass(_index_of({ 11: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 11, "anim_state": 43, "anim_phase": 9,
			"anim_variant": 2 }]
	_present(p, snap)
	assert_eq(model.get_active_body_variant(), 2,
			"the single channel poses its served entry")
	assert_eq(_stat(p, "body_dispatches"), 1)
	snap.entities[0]["anim_variant"] = 1
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 2,
			"the served entry participates in the retained pose stamp")
	assert_eq(model.get_active_body_variant(), 1)

	snap.entities[0]["anim_source_state"] = 43
	snap.entities[0]["anim_source_phase"] = 17
	snap.entities[0]["anim_source_variant"] = 3
	snap.entities[0]["anim_state"] = 1
	snap.entities[0]["anim_phase"] = 2
	snap.entities[0]["anim_blend_weight"] = 0.2
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 3)
	assert_eq(model.get_body_blend_source_variant(), 3,
			"the outgoing channel poses its own served entry")
	assert_eq(model.get_active_body_variant(), 1)
	snap.entities[0]["anim_source_variant"] = 0
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 4,
			"the outgoing entry participates in the retained pose stamp")
	assert_eq(model.get_body_blend_source_variant(), 0)


func test_placed_model_applies_snapshot_overlay_in_body_frame() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 12: model }))
	var angles := PackedVector3Array()
	for i in range(9):
		angles.append(Vector3(2.0 + i, 20.0 + i, -3.0 + i))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 12,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(7.0, 41.0, -5.0),
		"aim_angles": angles,
	}]
	_present(p, snap)
	var deltas: Array = model.get_aim_overlay()
	assert_eq(deltas.size(), 9, "all overlay classes use the packed selector result")
	var body_basis := MissionObjectPlacer.bms_to_godot_basis(
			Vector3(7.0, 41.0, -5.0))
	assert_true(model.basis.is_equal_approx(body_basis),
			"placed body renders in the same frame used to form overlay deltas")
	var expected_head := (
			body_basis.inverse()
			* MissionObjectPlacer.bms_to_godot_basis(angles[8]))
	assert_true((deltas[8] as Basis).is_equal_approx(expected_head))


func test_placed_model_clears_overlay_when_snapshot_selector_is_invalid() -> void:
	var model := _model()
	model.set_aim_overlay([Basis()])  # retained from an earlier owner
	var p := _make_pass(_index_of({ 13: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 13, "aim_overlay_valid": 0 }]
	_present(p, snap)
	assert_eq(model.get_aim_overlay(), [],
			"an unknown selector clears any pose retained by the model")


func test_placed_model_applies_and_restores_mounted_right_hand_collapse() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 14: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 14,
		"aim_overlay_valid": 1,
		"right_hand_collapsed": 1,
	}]
	_present(p, snap)
	assert_true(model.is_right_hand_collapsed(),
			"the placed pose consumes the packed mount verdict")
	snap.entities[0]["right_hand_collapsed"] = 0
	_present(p, snap)
	assert_false(model.is_right_hand_collapsed(), "and restores on dismount")


func test_resolves_by_kind_index_fallback() -> void:
	# Editor path: the in-memory mission has no stable bms_id (0). The pass
	# must fall back to (kind,index) through the real index.
	var model := _model()
	var index := EntityIndex.new()
	model.entity_ref = EntityRef.make(3, 2, 0)
	index.build([model], null)
	var p := _make_pass(index)
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 0, "kind": 3, "index": 2,
			"active1": 1, "phase1": 77 }]
	_present(p, snap)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 77,
			"resolved by (kind,index) when bms_id is 0")


func test_transform_applied_from_snapshot() -> void:
	# The consolidated pass MOVES entities (the old game path did not).
	# Position is Godot-space; yaw-only builds RotY(180 - yaw) through the
	# shared placer convention.
	var model := _model()
	var p := _make_pass(_index_of({ 5: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 5, "pos_x": 10.0, "pos_y": 2.0, "pos_z": -4.0,
			"yaw_deg": 90.0 }]
	_present(p, snap)
	assert_true(model.position.is_equal_approx(Vector3(10.0, 2.0, -4.0)),
			"position applied from snapshot")
	# yaw 90 -> RotY(180 - 90) = RotY(90deg); model +x maps toward -z.
	var fwd := model.transform.basis * Vector3(1, 0, 0)
	assert_almost_eq(fwd.z, -1.0, 0.001,
			"yaw drives the heading basis (RotY(180 - yaw))")


func test_stable_snapshot_does_not_redirty_the_transform_tree() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 21: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 21,
		"pos_x": 12.0,
		"yaw_deg": 15.0,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(3.0, 27.0, -2.0),
	}]
	_present(p, snap)
	assert_eq(_stat(p, "moved"), 1)
	_present(p, snap)
	assert_eq(_stat(p, "moved"), 1,
			"an unchanged row cannot recursively dirty every model descendant again")
	assert_true(model.basis.is_equal_approx(MissionObjectPlacer.bms_to_godot_basis(
			Vector3(3.0, 27.0, -2.0))),
			"aim body rotation is composed into the one root transform write")


func test_changed_aim_body_updates_root_with_stable_entity_transform() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 21: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 21,
		"handle": 2,
		"type_id": 101,
		"pos_x": 12.0,
		"yaw_deg": 15.0,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(3.0, 27.0, -2.0),
	}]
	_present(p, snap)
	var moved := _stat(p, "moved")

	snap.entities[0]["aim_body"] = Vector3(-5.0, 61.0, 4.0)
	_present(p, snap)
	assert_eq(_stat(p, "moved"), moved + 1,
			"aim-owned root rotation participates in the transform change stamp")
	assert_true(model.basis.is_equal_approx(MissionObjectPlacer.bms_to_godot_basis(
			Vector3(-5.0, 61.0, 4.0))),
			"the changed aim body, not the stable entity Euler, owns the root")


func test_stable_revisioned_snapshot_caches_pose_and_reasserts_live_publishers() -> void:
	var model := _muzzle_model()
	var sim := Simulation.new()
	var p := _make_pass(_index_of({ 21: model }), sim)
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 21,
		"handle": 2,
		"type_id": 101,
		"net_id": 77,
		"pos_x": 12.0,
		"yaw_deg": 15.0,
		"active1": 1,
		"phase1": 0x1111,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x2222,
		"emplaced_gun_pitch": 0xDDDD,
		"anim_state": 43,
		"anim_phase": 9,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(3.0, 27.0, -2.0),
		"aim_angles": PackedVector3Array([Vector3(5.0, 6.0, 7.0)]),
		"right_hand_collapsed": 1,
	}]
	_present(p, snap)
	var stats: MissionPresentStats = p.get_stats_record()

	# Visibility is a live output, so a stable snapshot must not short-circuit
	# the entire row.
	model.visible = false
	_present(p, snap)
	var next_stats: MissionPresentStats = p.get_stats_record()
	assert_true(model.visible, "live visibility ownership is reconciled every frame")

	assert_eq(next_stats.moved, stats.moved,
			"stable transform inputs do not rebuild or write the root transform")
	for key in [
		"transform_builds",
		"aim_dispatches",
		"rhc_dispatches",
		"body_dispatches",
	]:
		assert_eq(int(next_stats.get(key)), int(stats.get(key)),
				"stable rows add no %s work" % key)
	assert_eq(next_stats.part_dispatches,
			stats.part_dispatches + 1,
			"an active PLAYPART writer is reasserted at every submission")
	assert_eq(int(next_stats["control_dispatches"]),
			int(stats["control_dispatches"]) + 3,
			"the three valid emplaced writers are reasserted at every submission")
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 0x1111,
			"the reasserted writers land on the same values")
	assert_eq(_ctrl(model, "EWEAP_GUNYAW"), 0x2222)


func test_stable_layout_reuses_resolution_but_reads_fresh_pose() -> void:
	var model := _model()
	var other := _model()
	var p := _make_pass(_index_of({ 21: model, 999: other }))
	var snap := Snapshot.new()
	snap.entities = [
		{ "bms_id": 21, "handle": 2, "type_id": 101, "pos_x": 1.0 },
		{ "bms_id": 999, "handle": 3, "type_id": 102, "pos_x": 50.0 },
	]
	_present(p, snap)
	assert_eq(_stat(p, "plan_rebuilds"), 1, "the cold layout resolves each row once")
	assert_almost_eq(model.position.x, 1.0, 0.001)

	snap.entities[0]["pos_x"] = 9.0
	_present(p, snap)
	assert_eq(_stat(p, "plan_rebuilds"), 1,
			"pose-only updates reuse the revision-bound row plan")
	assert_almost_eq(model.position.x, 9.0, 0.001,
			"cached routing still reads fresh row values")


func test_layout_plan_rebinds_after_reorder_removal_and_replacement() -> void:
	var a := _model()
	var b := _model()
	var c := _model()
	var p := _make_pass(_index_of({ 11: a, 22: b, 33: c }))
	var snap := Snapshot.new()
	snap.entities = [
		{ "bms_id": 11, "handle": 2, "type_id": 101, "pos_x": 1.0 },
		{ "bms_id": 22, "handle": 3, "type_id": 102, "pos_x": 2.0 },
	]
	_present(p, snap, 1)

	snap.entities = [
		{ "bms_id": 22, "handle": 3, "type_id": 102, "pos_x": 20.0 },
		{ "bms_id": 11, "handle": 2, "type_id": 101, "pos_x": 10.0 },
	]
	_present(p, snap, 2)
	assert_eq(_stat(p, "plan_rebuilds"), 2, "row reorder rebuilds the plan")
	assert_almost_eq(a.position.x, 10.0, 0.001)
	assert_almost_eq(b.position.x, 20.0, 0.001)

	snap.entities = [
		{ "bms_id": 11, "handle": 2, "type_id": 101, "pos_x": 12.0 },
	]
	_present(p, snap, 3)
	assert_eq(_stat(p, "plan_rebuilds"), 3, "row removal cannot retain a stale base")
	assert_almost_eq(a.position.x, 12.0, 0.001)
	assert_almost_eq(b.position.x, 20.0, 0.001)

	snap.entities = [
		{ "bms_id": 33, "handle": 4, "type_id": 103, "pos_x": 30.0 },
	]
	_present(p, snap, 4)
	assert_eq(_stat(p, "plan_rebuilds"), 4,
			"same-size identity replacement is resolved against the new row")
	assert_almost_eq(a.position.x, 12.0, 0.001)
	assert_almost_eq(c.position.x, 30.0, 0.001)


func test_freeing_an_unplanned_model_keeps_the_typed_plan() -> void:
	var planned := ObjectModel.new()
	add_child(planned)
	planned.set_process(false)
	var index := _index_of({ 21: planned })
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 21,
		"handle": 2,
		"type_id": 101,
		"pos_x": 12.0,
	}]
	var p := _make_pass(index)
	_present(p, snap)
	var rebuilds := _stat(p, "plan_rebuilds")

	# A throwable / viewmodel / preview model that no row plan retains dies
	# without touching the plan: the lifetime stamp is scoped to planned rows.
	var stray := ObjectModel.new()
	add_child(stray)
	stray.set_process(false)
	stray.free()
	snap.entities[0]["pos_x"] = 30.0
	_present(p, snap)
	assert_eq(_stat(p, "plan_rebuilds"), rebuilds,
			"an unplanned model's death leaves the typed row plan intact")
	assert_almost_eq(planned.position.x, 30.0, 0.001,
			"the retained row keeps presenting through the same plan")


func test_freed_cached_node_marks_revisioned_plan_for_rebind() -> void:
	var old_model := ObjectModel.new()
	add_child(old_model)
	old_model.set_process(false)
	var index := _index_of({ 21: old_model })
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 21,
		"handle": 2,
		"type_id": 101,
		"pos_x": 12.0,
	}]
	var p := _make_pass(index)
	_present(p, snap)
	assert_almost_eq(old_model.position.x, 12.0, 0.001)
	assert_true(old_model.is_present_visible(),
			"the presented row's visibility intent lives on its node")

	old_model.free()
	var replacement := _model()
	snap.entities[0]["pos_x"] = 30.0

	# A trusted layout revision retains typed row references, but the model
	# lifetime stamp invalidates that plan before a freed pointer can be read.
	_present(p, snap)
	assert_eq(_stat(p, "plan_rebuilds"), 2,
			"freeing any cached model invalidates the typed row plan immediately")
	replacement.entity_ref = EntityRef.make(1, 21, 21)
	index.build([replacement], null)
	_present(p, snap)
	assert_almost_eq(replacement.position.x, 30.0, 0.001,
			"the replacement receives the current row after the rebind")
	# The intent died with the freed node: the replacement carries none of it
	# and derives its own bit from the current row.
	snap.entities[0]["hidden"] = 1
	_present(p, snap)
	assert_false(replacement.is_present_visible(),
			"the rebound row writes its own visibility intent onto the replacement")
	snap.entities[0]["hidden"] = 0
	_present(p, snap)
	assert_true(replacement.is_present_visible())


func test_transform_ignores_body_clip_visual_offsets() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 6: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 6,
		"pos_x": 10.0,
		"pos_y": 2.0,
		"pos_z": 3.0,
		"yaw_deg": 180.0,
		"anim_state": 86,
	}]
	_present(p, snap)
	assert_true(model.position.is_equal_approx(Vector3(10.0, 2.0, 3.0)),
		"sim entity position remains authoritative even for seated infantry clips")


func test_visibility_from_hidden_and_alive() -> void:
	var shown := _model()
	var hidden := _model()
	var dead := _model()
	var corpse := _model()
	var despawned := _model()
	var p := _make_pass(_index_of({
		1: shown, 2: hidden, 3: dead, 4: corpse, 5: despawned }))
	var snap := Snapshot.new()
	snap.entities = [
		{ "bms_id": 1, "hidden": 0, "alive": 1 },
		{ "bms_id": 2, "hidden": 1, "alive": 1 },
		{ "bms_id": 3, "hidden": 0, "alive": 0 },
		# A dead ORGANIC keeps rendering as a corpse until the sim despawns it
		# via PF_HIDDEN (the corpse timer + watch rule; world-wac-ai-re §19.4).
		{ "bms_id": 4, "hidden": 0, "alive": 0, "kind": 3 },
		{ "bms_id": 5, "hidden": 1, "alive": 0, "kind": 3 },
	]
	_present(p, snap)
	assert_true(shown.visible, "visible entity stays visible")
	assert_false(hidden.visible, "hidden entity is hidden")
	# A dead non-organic RENDERS: the destruction pass swaps its model to the
	# husk, and a def with no husk keeps the graphic standing — the witnessed
	# render pick [orig: Flags&4 && huskModel ? husk : graphic @0x413086;
	# world-wac-ai-re §24.6].
	assert_true(dead.visible, "a dead non-organic renders (husk swap / graphic fallback)")
	assert_true(corpse.visible, "a dead organic renders as a corpse")
	assert_false(despawned.visible, "the sim ends the corpse via PF_HIDDEN")


func test_placed_model_applies_and_restores_dismemberment_sections() -> void:
	var model := _sectioned_model()
	var p := _make_pass(_index_of({ 6: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 6,
		"section_mask_valid": 1,
		"section_mask": 0b00010,
	}]
	_present(p, snap)
	var parts: Dictionary = model.get_render_part_nodes()
	assert_eq(parts.size(), 5, "the fixture exposes five dismemberable sections")
	assert_true((parts[0] as Node3D).visible)
	assert_false((parts[1] as Node3D).visible,
			"a set entity section bit hides the matching placed-model part")
	assert_true((parts[2] as Node3D).visible)

	snap.entities[0]["section_mask"] = 0
	_present(p, snap)
	assert_true((parts[1] as Node3D).visible,
			"clearing the entity section mask restores the part")


func test_local_first_person_usegun_parent_is_not_world_rendered() -> void:
	# Retail skips the local UseGun PARENT'S own vehicle-model submit after its
	# embedded MountSlot becomes EquippedSlot in first person. This is a local
	# render verdict, independent of authoritative Entity.hidden; attached actors
	# render through a separate child walk. [orig: Entity_RenderVehicleModel
	# @0x4407d0, cull @0x4407f6..0x44084c, submit @0x440918;
	# RenderSlot_RenderEntityAndChildren child walk @0x5d7938+]
	var mount := _rigged_model()
	var unrelated := _rigged_model()
	var p := _make_pass(_index_of({ 41: mount, 42: unrelated }))
	var snap := Snapshot.new()
	snap.entities = [
		{ "bms_id": 41, "hidden": 0, "local_view_suppressed": 1,
				"anim_state": 43, "anim_phase": 7 },
		{ "bms_id": 42, "hidden": 0, "local_view_suppressed": 0,
				"anim_state": 43, "anim_phase": 7 },
	]
	_present(p, snap)
	assert_false(mount.visible,
			"the committed first-person UseGun parent skips its own world model")
	assert_true(unrelated.visible,
			"local UseGun suppression cannot hide unrelated world entities")
	assert_eq(mount.get_active_body_clip(), "",
			"a hidden model without a muzzle provider skips skeletal writes")
	assert_eq(unrelated.get_active_body_clip(), "anim_idle",
			"the visible model still receives its absolute pose")

	# Camera-mode changes, detach, and pre-commit slot mismatch all clear the
	# transient verdict. PF_HIDDEN remains independently authoritative.
	snap.entities[0]["local_view_suppressed"] = 0
	_present(p, snap)
	assert_true(mount.visible, "clearing the render verdict restores the parent immediately")
	assert_eq(mount.get_active_body_clip(), "anim_idle",
			"the absolute phase catches up when the model becomes renderable")
	snap.entities[0]["hidden"] = 1
	_present(p, snap)
	assert_false(mount.visible, "authoritative PF_HIDDEN still wins independently")


func test_options_gate_each_channel() -> void:
	# The editor/game can disable a channel; e.g. transform-only leaves part
	# phases untouched.
	var model := _model()
	var p := _make_pass(_index_of({ 9: model }), null, { "drive_part_anim": false })
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 9, "active1": 1, "phase1": 50, "pos_x": 3.0 }]
	_present(p, snap)
	assert_false(model.get_ctrl_values().has("VEHICLE_SPECIAL1"),
			"part-anim channel disabled -> nothing posed")
	assert_almost_eq(model.position.x, 3.0, 0.001, "transform still applied")


func test_reenabled_output_channels_catch_up_to_current_state() -> void:
	var model := _rigged_model()
	var p := _make_pass(_index_of({ 9: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 9,
		"pos_x": 1.0,
		"active1": 1,
		"phase1": 100,
		"anim_state": 43,
		"anim_phase": 9,
	}]
	_present(p, snap)
	assert_almost_eq(model.position.x, 1.0, 0.001)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 100)
	assert_almost_eq(model.get_animation_time(),
			_clip_time(model, "anim_idle", 9), 0.0001)

	var channels := int(p.get_output_channels())
	var frozen := (
			EntityPresenter.OUTPUT_TRANSFORM
			| EntityPresenter.OUTPUT_PART_ANIM
			| EntityPresenter.OUTPUT_BODY_ANIM)
	p.set_output_channels(channels & ~frozen)
	snap.entities[0]["pos_x"] = 8.0
	snap.entities[0]["phase1"] = 200
	snap.entities[0]["anim_phase"] = 12
	_present(p, snap)
	assert_almost_eq(model.position.x, 1.0, 0.001)
	assert_false(model.get_ctrl_values().has("VEHICLE_SPECIAL1"),
			"the disabled part seam released its writers")
	assert_almost_eq(model.get_animation_time(),
			_clip_time(model, "anim_idle", 9), 0.0001)

	p.set_output_channels(channels)
	_present(p, snap)
	assert_almost_eq(model.position.x, 8.0, 0.001,
			"transform ownership catches up on its rising edge")
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 200,
			"part ownership catches up on its rising edge")
	assert_almost_eq(model.get_animation_time(),
			_clip_time(model, "anim_idle", 12), 0.0001,
			"body ownership catches up on its rising edge")


func test_unresolved_target_does_not_crash() -> void:
	var index := EntityIndex.new()
	index.build([], null)
	var p := _make_pass(index)
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 1234, "active1": 1, "phase1": 1 }]
	_present(p, snap)  # must not crash
	assert_eq(_stat(p, "posed"), 0, "nothing posed when the target is unresolved")


func test_occlusion_claim_blocks_the_show_but_never_the_hide() -> void:
	# Two-bit visibility ownership: the render-occlusion apply owns hides
	# through the model's occlusion-hidden bit (set_occlusion_hidden). A
	# sim-wants-visible write is withheld while the claim stands, a sim hide
	# always lands, and clearing the claim (as the occlusion release does)
	# returns sole ownership to this walk.
	var model := _model()
	var p := _make_pass(_index_of({ 1001: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 1001 }]

	model.set_occlusion_hidden(true)  # occlusion hid it; the sim wants it visible
	_present(p, snap)
	assert_false(model.visible, "a claimed node is not re-shown by the present drive")
	assert_true(model.is_present_visible(),
			"the release intent on the node records the complete present predicate")

	snap.entities = [{ "bms_id": 1001, "hidden": 1 }]
	_present(p, snap)
	assert_false(model.visible, "a sim hide lands regardless of the claim")
	assert_false(model.is_present_visible())

	snap.entities = [{ "bms_id": 1001, "local_view_suppressed": 1 }]
	_present(p, snap)
	assert_false(model.is_present_visible(),
			"first-person suppression is part of the same release predicate")

	snap.entities = [{ "bms_id": 1001 }]
	model.set_occlusion_hidden(false)
	_present(p, snap)
	assert_true(model.visible,
			"with the claim cleared the present drive owns visibility again")


func test_body_anim_dispatch_gates_on_the_ab_seam() -> void:
	# The public output-channel mask freezes pose dispatch so probes can isolate
	# the skeleton-update share without reaching into presenter fields.
	var model := _rigged_model()
	var p := _make_pass(_index_of({ 11: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 11, "body_anim_slot": 1,
			"anim_state": 43, "anim_phase": 9 }]
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 1, "body anim dispatches by default")
	var channels := int(p.get_output_channels())
	p.set_output_channels(channels & ~EntityPresenter.OUTPUT_BODY_ANIM)
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 1, "the frozen seam dispatches nothing new")
	p.set_output_channels(channels)
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 2, "restoring the seam resumes dispatch")


func test_disabling_part_anim_output_releases_all_retained_ctrl_writers() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 12: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 12,
		"active1": 1,
		"phase1": 0x1111,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x2222,
		"emplaced_gun_pitch": 0x3333,
		"world_heat_glow_valid": 1,
		"world_heat_glow": 0x4444,
		"tex_team_valid": 1,
		"tex_team": 2,
		"zone_ctrl_valid": 1,
		"team_swing": 0x10000,
		"lfp_camp_percent_valid": 1,
		"lfp_camp_percent": 0x8000,
	}]
	_present(p, snap)
	assert_false(model.get_ctrl_values().is_empty())
	var channels := int(p.get_output_channels())
	p.set_output_channels(channels & ~EntityPresenter.OUTPUT_PART_ANIM)
	assert_true(model.get_ctrl_values().is_empty(),
			"freezing the output seam cannot retain its last CTRL frame")


func test_destruction_publication_reclaims_owner_and_releases_only_its_slots() -> void:
	var model := _model()
	var pass_ := _make_pass(_index_of({1: model}))
	var snap := Snapshot.new()
	snap.entities = [{"bms_id": 1, "hidden": 1, "destroy": [65536, 32768, 1, 0, 0, 0]}]
	_present(pass_, snap)
	assert_eq(_ctrl(model, "OBJECT_DESTROY"), 65536, "hidden rows retain destruction publication")
	model.set_ctrl_override("other", "OBJECT_DESTROY", 65536)
	_present(pass_, snap)
	model.clear_ctrl_override("other", "OBJECT_DESTROY")
	assert_eq(_ctrl(model, "OBJECT_DESTROY"), 65536, "an unchanged phase reclaims equal-value ownership")
	model.set_ctrl_override("other", "OBJECT_DESTROY01", 99)
	snap.entities[0]["destroy"] = [0, 0, 0, 0, 0, 0]
	_present(pass_, snap)
	assert_false(model.get_ctrl_values().has("OBJECT_DESTROY"), "falling phase releases our slot")
	assert_eq(_ctrl(model, "OBJECT_DESTROY01"), 99, "stale release preserves a later writer")
	assert_false(model.get_ctrl_values().has("OBJECT_DESTROY02"))
