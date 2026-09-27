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
