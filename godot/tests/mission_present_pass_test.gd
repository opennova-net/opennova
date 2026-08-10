extends GutTest

# The unified MissionPresentPass applies each entity's transform + PANM part
# channels + visibility onto its placed node every tick, from ONE batched sim
# snapshot. Real native components end to end: ObjectModel nodes (their
# CTRL store, body clips, and Node3D state are the observables), a real
# EntityIndex, and PF-layout snapshots built as pure data and fed through
# the public present_snapshot API. Change-gating claims read the applier's
# stats counters — never instrumentation subclasses.
#
# The old aliased-register dismount cases are gone by design: production
# ObjectModel maps PLAYPARTANIM channels 1/2 to fixed VEHICLE_SPECIAL1/2
# [orig: HUD_CacheEntityDisplayInfo @ 0x4A3E18..0x4A3E38], so a part channel
# can never alias EWEAP registers; the register-independence case pins the
# real layout.

const PresentPass := preload("res://game/world/mission_present_pass.gd")

const RIGGED_3DI := "res://../fixtures/threedi/3di3/Shed.3di"
const MUZZLE_3DI := "res://../fixtures/3dp/dapche2/dapche2.3di"


# Builds the flat PF-layout snapshot Simulation.get_present_snapshot()
# emits. Each entity is a Dictionary of overrides; unset fields default sanely
# (alive, not hidden, identity). Pure data — nothing here is a sim double.
class Snapshot:
	extends RefCounted
	var entities: Array = []
	func _write_phase(out: PackedFloat32Array, base: int,
			channel: int, phase: int, active: bool) -> void:
		var phase_field := Simulation.PF_PHASE1 + (channel - 1) * 2
		var active_field := Simulation.PF_ACTIVE1 + (channel - 1) * 2
		var bits := phase & 0xFFFFFFFF
		out[base + phase_field] = float(bits & 0xFFFF)
		out[base + active_field] = (
				float(((bits >> 16) & 0xFFFF) + 1) if active else 0.0)
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
			_write_phase(out, b, 1, int(e.get("phase1", 0)),
					int(e.get("active1", 0)) != 0)
			_write_phase(out, b, 2, int(e.get("phase2", 0)),
					int(e.get("active2", 0)) != 0)
			out[b + Simulation.PF_BODY_ANIM_SLOT] = float(e.get("body_anim_slot", -1))
			out[b + Simulation.PF_ANIM_STATE] = float(e.get("anim_state", -1))
			out[b + Simulation.PF_ANIM_PHASE_TICKS] = float(e.get("anim_phase", 0))
			out[b + Simulation.PF_ANIM_SOURCE_STATE] = float(
					e.get("anim_source_state", -1))
			out[b + Simulation.PF_ANIM_SOURCE_PHASE_TICKS] = float(
					e.get("anim_source_phase", -1))
			out[b + Simulation.PF_ANIM_BLEND_WEIGHT] = float(
					e.get("anim_blend_weight", 1.0))
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
			out[b + Simulation.PF_VEHICLE_MOTION_VALID] = float(
					e.get("vehicle_motion_valid", 0))
			out[b + Simulation.PF_VEHICLE_STEERING] = float(
					e.get("vehicle_steering", 0))
			out[b + Simulation.PF_VEHICLE_SPEED] = float(
					e.get("vehicle_speed", 0))
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


func _model() -> ObjectModel:
	var m := ObjectModel.new()
	add_child_autofree(m)
	m.set_process(false)
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
# model's userpoint table case-insensitively — dapche2 authors Bullet01 and the
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
	var entries: Array = []
	for bms_id in by_bms_id.keys():
		entries.append({ "model": by_bms_id[bms_id], "ref": {
			"kind": 1, "index": int(bms_id), "bms_id": int(bms_id),
			"group": -1, "team": -1, "position": Vector3.ZERO,
		} })
	var index := EntityIndex.new()
	index.build(entries, [])
	return index


func _make_pass(index: EntityIndex, sim: Simulation = null,
		options: Dictionary = {}) -> Object:
	var p := PresentPass.new()
	p.setup(sim, index, options)
	return p


func _present(p: Object, snap: Snapshot, revision: int = 1) -> void:
	p.present_snapshot(snap.build(), Simulation.PF_STRIDE, revision)


func _ctrl(model: ObjectModel, name: String) -> int:
	return int(model.get_ctrl_values().get(name, -1))


func _stat(p: Object, key: String) -> int:
	return int(p.get_stats()[key])


func _clip_time(model: ObjectModel, key: String, phase_ticks: int) -> float:
	var fps: float = model.get_skeletal_anim().get_clip_fps(key)
	assert_gt(fps, 0.0, "fixture clip %s carries a frame rate" % key)
	return float(phase_ticks) / (2.0 * fps)


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


func test_offscreen_muzzle_row_keeps_fire_origin_feedback_and_pose() -> void:
	var muzzle_model := _muzzle_model()
	var plain_model := _rigged_model()
	var sim := Simulation.new()
	var p := _make_pass(_index_of({ 5: muzzle_model, 6: plain_model }), sim)
	var snap := Snapshot.new()
	snap.entities = [
		{ "bms_id": 5, "net_id": 9, "anim_state": 1, "anim_phase": 12 },
		{ "bms_id": 6, "anim_state": 1, "anim_phase": 12 },
	]
	_present(p, snap)
	var initial_pushes := _stat(p, "muzzles")
	var initial_bodies := _stat(p, "body_dispatches")
	assert_gt(initial_pushes, 0, "the posed muzzle sample reaches the simulation")
	assert_eq(plain_model.get_active_body_clip(), "anim_walk_forward")

	muzzle_model.set_on_screen(false)
	plain_model.set_on_screen(false)
	# Phase 14 stays inside the 8-frame walk clip's length, so the re-entry
	# playhead assertion is wrap-free.
	snap.entities[0]["anim_phase"] = 14
	snap.entities[1]["anim_phase"] = 14
	_present(p, snap)
	assert_gt(_stat(p, "muzzles"), initial_pushes,
			"the D-AI-6 fire-origin feedback survives off-screen (AI still shoot)")
	assert_gt(_stat(p, "body_dispatches"), initial_bodies,
			"the authoritative muzzle owner keeps posing off-screen")
	assert_almost_eq(plain_model.get_animation_time(),
			_clip_time(plain_model, "anim_walk_forward", 12), 0.0001,
			"a non-muzzle row stops body dispatches while not submitted")

	plain_model.set_on_screen(true)
	_present(p, snap)
	assert_almost_eq(plain_model.get_animation_time(),
			_clip_time(plain_model, "anim_walk_forward", 14), 0.0001,
			"re-entry re-poses the body to the live phase, not the missed one")


func test_both_channels_posed() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 7: model }))
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 7, "active1": 1, "phase1": 100,
			"active2": 1, "phase2": 200 }]
	_present(p, snap)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 100)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL2"), 200, "both active channels posed")


func test_emplaced_weapon_uses_named_controls_and_clears_them() -> void:
	var model := _model()
	var p := _make_pass(_index_of({ 8: model }))
	var snap := Snapshot.new()
	snap.entities = [{
		"bms_id": 8,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x1234,
		"emplaced_gun_pitch": 0xFEDC,
	}]
	_present(p, snap)
	assert_eq(model.get_ctrl_values(), {
		"EWEAP_GUNYAW": 0x1234,
		"EWEAP_GUNPITCH": 0xFEDC,
	}, "semantic controls do not alias model-order PLAYPARTANIM channels")

	snap.entities[0]["emplaced_controls_valid"] = 0
	_present(p, snap)
	assert_true(model.get_ctrl_values().is_empty(),
			"dismount/death clears retained EWEAP controls")


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
	}]
	_present(p, snap)
	assert_eq(model.get_ctrl_values(), {
		"VEHICLE_STEERING": 0xFEDC,
		"VEHICLE_SPEED": 0x10000,
	}, "the cveh pair publishes by semantic retail name")

	snap.entities[0]["vehicle_motion_valid"] = 0
	_present(p, snap)
	assert_true(model.get_ctrl_values().is_empty(),
			"an unavailable/non-authoritative row releases the cveh writer")


func test_sector_and_zone_controls_preserve_write_validity_and_scoped_release() -> void:
	var model := _model()
	# This unrelated writer is the stand-in for another semantic producer
	# (DOOR_00 is a real retail register no presenter touches). Zone teardown
	# must never use clear_ctrl_values().
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
	}, "retail publishes generic and emplaced systems on distinct semantic registers")

	snap.entities[0]["emplaced_controls_valid"] = 0
	snap.entities[0]["phase1"] = 0x3333
	snap.entities[0]["phase2"] = 0xCCCC
	_present(p, snap)
	assert_eq(model.get_ctrl_values(), {
		"VEHICLE_SPECIAL1": 0x3333,
		"VEHICLE_SPECIAL2": 0xCCCC,
	}, "dismount clears only stale EWEAP ownership")


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
	var blend: Dictionary = model.get_body_blend()
	assert_eq(String(blend.get("source_key", "")), "anim_idle",
			"placed NPCs consume the authority's exact primary blend tuple")
	assert_almost_eq(float(blend.get("source_time", -1.0)),
			_clip_time(model, "anim_idle", 17), 0.0001)
	assert_almost_eq(float(blend.get("weight", -1.0)), 0.2, 0.000001)

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
	assert_eq(String(model.get_body_blend().get("source_key", "")), "anim_reset")
	assert_almost_eq(float(model.get_body_blend().get("source_time", -1.0)),
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
	p.set_output_channels(channels & ~PresentPass.OUTPUT_BODY_ANIM)
	snap.entities[0]["anim_source_phase"] = 19
	snap.entities[0]["anim_blend_weight"] = 0.4
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 6,
			"a disabled body channel performs no blend dispatch")
	p.set_output_channels(channels)
	_present(p, snap)
	assert_eq(_stat(p, "body_dispatches"), 7,
			"reenabling the body channel cold-applies the full latest tuple")
	assert_almost_eq(float(model.get_body_blend().get("source_time", -1.0)),
			_clip_time(model, "anim_reset", 19), 0.0001)
	assert_almost_eq(float(model.get_body_blend().get("weight", -1.0)),
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
	assert_almost_eq(float(model.get_body_blend().get("source_time", -1.0)),
			_clip_time(model, "anim_reset", 20), 0.0001)
	assert_almost_eq(float(model.get_body_blend().get("weight", -1.0)),
			0.5, 0.000001)


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
	index.build([{ "model": model, "ref": {
		"kind": 3, "index": 2, "bms_id": 0, "group": -1, "team": -1,
		"position": Vector3.ZERO,
	} }], [])
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
	var stats: Dictionary = p.get_stats()
	var stats_record: MissionPresentStats = p.get_stats_record()
	assert_eq(stats_record.transform_builds, int(stats["transform_builds"]))
	assert_eq(stats_record.muzzle_queries, int(stats["muzzle_queries"]))

	# Visibility and the AI muzzle seam are live outputs, so a stable snapshot
	# must not short-circuit the entire row.
	model.visible = false
	_present(p, snap)
	var next_stats: Dictionary = p.get_stats()
	assert_true(model.visible, "live visibility ownership is reconciled every frame")
	assert_eq(int(next_stats["muzzle_queries"]), int(stats["muzzle_queries"]) + 1,
			"the four-tick muzzle-freshness seam still samples every frame")
	assert_eq(int(next_stats["muzzles"]), int(stats["muzzles"]) + 1,
			"the fresh muzzle sample still reaches the simulation")

	assert_eq(int(next_stats["moved"]), int(stats["moved"]),
			"stable transform inputs do not rebuild or write the root transform")
	for key in [
		"transform_builds",
		"aim_dispatches",
		"rhc_dispatches",
		"body_dispatches",
	]:
		assert_eq(int(next_stats[key]), int(stats[key]),
				"stable rows add no %s work" % key)
	assert_eq(int(next_stats["part_dispatches"]),
			int(stats["part_dispatches"]) + 1,
			"an active PLAYPART writer is reasserted at every submission")
	assert_eq(int(next_stats["control_dispatches"]),
			int(stats["control_dispatches"]) + 2,
			"the two valid EWEAP writers are reasserted at every submission")
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
	var visibility_intent := {}
	var p := _make_pass(index, null, {
		"present_visibility": visibility_intent,
	})
	_present(p, snap)
	assert_almost_eq(old_model.position.x, 12.0, 0.001)
	assert_true(bool(visibility_intent.get(21, false)))

	old_model.free()
	var replacement := _model()
	snap.entities[0]["pos_x"] = 30.0

	# A trusted layout revision no longer scans every ObjectID before the walk.
	# The first null encounter may defer the rebind, but it must dirty the plan
	# so the replacement is resolved on the following public presentation call.
	_present(p, snap)
	assert_false(visibility_intent.has(21),
			"a freed cached node releases its visibility intent immediately")
	index.build([{ "model": replacement, "ref": {
		"kind": 1, "index": 21, "bms_id": 21, "group": -1, "team": -1,
		"position": Vector3.ZERO,
	} }], [])
	_present(p, snap)
	assert_almost_eq(replacement.position.x, 30.0, 0.001,
			"the replacement receives the current row after the rebind")


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
			PresentPass.OUTPUT_TRANSFORM
			| PresentPass.OUTPUT_PART_ANIM
			| PresentPass.OUTPUT_BODY_ANIM)
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
	index.build([], [])
	var p := _make_pass(index)
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 1234, "active1": 1, "phase1": 1 }]
	_present(p, snap)  # must not crash
	assert_eq(_stat(p, "posed"), 0, "nothing posed when the target is unresolved")


func test_occlusion_claim_blocks_the_show_but_never_the_hide() -> void:
	# Two-bit visibility ownership: the render-occlusion apply owns hides
	# through a claim set shared by reference (the occlusion_hidden_ids setup
	# option). A sim-wants-visible write is withheld while the claim stands, a
	# sim hide always lands, and clearing the claim (as the occlusion release
	# does) returns sole ownership to this pass.
	var model := _model()
	var claims := { 1001: true }
	var visibility_intent := {}
	var p := _make_pass(_index_of({ 1001: model }), null, {
		"occlusion_hidden_ids": claims,
		"present_visibility": visibility_intent,
	})
	var snap := Snapshot.new()
	snap.entities = [{ "bms_id": 1001 }]

	model.visible = false  # occlusion hid it; the sim wants it visible
	_present(p, snap)
	assert_false(model.visible, "a claimed node is not re-shown by the present drive")
	assert_true(bool(visibility_intent.get(1001, false)),
			"the shared release intent records the complete present predicate")

	snap.entities = [{ "bms_id": 1001, "hidden": 1 }]
	_present(p, snap)
	assert_false(model.visible, "a sim hide lands regardless of the claim")
	assert_false(bool(visibility_intent.get(1001, true)))

	snap.entities = [{ "bms_id": 1001, "local_view_suppressed": 1 }]
	_present(p, snap)
	assert_false(bool(visibility_intent.get(1001, true)),
			"first-person suppression is part of the same release predicate")

	snap.entities = [{ "bms_id": 1001 }]
	claims.clear()
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
	p.set_output_channels(channels & ~PresentPass.OUTPUT_BODY_ANIM)
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
	p.set_output_channels(channels & ~PresentPass.OUTPUT_PART_ANIM)
	assert_true(model.get_ctrl_values().is_empty(),
			"freezing the output seam cannot retain its last CTRL frame")


func test_native_basis_matches_the_placement_convention() -> void:
	# The native walk carries its own port of bms_to_godot_basis (the godot-cpp
	# Basis(axis, angle) parity gotcha): pin the two implementations together
	# across the angle space so they can never drift.
	for pitch in [-90.0, -30.0, 0.0, 15.0, 90.0, 180.0]:
		for yaw in [-180.0, -45.0, 0.0, 90.0, 135.0, 270.0]:
			for roll in [-60.0, 0.0, 30.0, 180.0]:
				var rot := Vector3(pitch, yaw, roll)
				var expected := MissionObjectPlacer.bms_to_godot_basis(rot)
				var got: Basis = PresentApplier.bms_to_godot_basis(rot)
				assert_true(got.is_equal_approx(expected),
						"basis parity at %s: native %s vs placer %s" % [
								rot, got, expected])
