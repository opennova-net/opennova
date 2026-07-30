extends GutTest

# The unified MissionPresentPass applies each entity's transform + PANM part channels + visibility onto
# its placed node every tick, from ONE batched sim snapshot. It consolidates the old game path
# (MissionCommandHost: PANM only, by bms_id) and editor path (MissionSimDriver._apply: transform only,
# by (kind,index)). Asset-free: a fake sim emitting a PF-layout snapshot, the real MissionEntityRegistry
# resolver behaviour faked by a tiny index, and fake models capturing transform/phase/visible.

const PresentPass := preload("res://engine/world/mission_present_pass.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")


class FakeModel:
	extends Node3D
	var phases: Array = []        # [channel, phase]
	var body_calls: Array = []    # [key_or_slot, phase]
	var overlay_calls: Array = []
	var right_hand_collapse_calls: Array[bool] = []
	var ctrl_values: Dictionary = {}
	var set_controls: Array = []
	var cleared_controls: Array[String] = []
	var cleared_part_channels: Array[int] = []
	var part_control_names: Dictionary = {
		1: "VEHICLE_SPECIAL1",
		2: "VEHICLE_SPECIAL2",
	}
	func set_part_phase(channel: int, phase: int) -> void:
		var control_name := String(part_control_names.get(channel, ""))
		if not control_name.is_empty() and ctrl_values.has(control_name) and \
				int(ctrl_values[control_name]) == phase:
			return
		phases.append([channel, phase])
		if not control_name.is_empty():
			ctrl_values[control_name] = phase
	func clear_part_phase(channel: int) -> void:
		var control_name := String(part_control_names.get(channel, ""))
		if control_name.is_empty() or not ctrl_values.has(control_name):
			return
		cleared_part_channels.append(channel)
		ctrl_values.erase(control_name)
	func play_body_clip_at(key: String, phase_ticks: int) -> void:
		body_calls.append([key, phase_ticks])
	func play_body_blend_at(source_key: String, source_phase_ticks: int,
			target_key: String, target_phase_ticks: int, weight: float) -> void:
		body_calls.append([
			"blend", source_key, source_phase_ticks,
			target_key, target_phase_ticks, weight])
	func play_body_anim_at(slot: int, phase_ticks: int) -> void:
		body_calls.append([slot, phase_ticks])
	func play_body_anim(slot: int) -> void:
		body_calls.append([slot, -1])
	func set_aim_overlay(deltas: Array) -> void:
		overlay_calls.append(deltas)
	func set_right_hand_collapsed(collapsed: bool) -> void:
		right_hand_collapse_calls.append(collapsed)
	func set_ctrl_value(name: String, value: int) -> void:
		if ctrl_values.has(name) and int(ctrl_values[name]) == value:
			return
		ctrl_values[name] = value
		set_controls.append([name, value])
	func clear_ctrl_value(name: String) -> void:
		if not ctrl_values.has(name):
			return
		ctrl_values.erase(name)
		cleared_controls.append(name)


class OwnedBatchModel:
	extends FakeModel
	var ctrl_owners: Dictionary = {}
	var begin_batch_calls := 0
	var end_batch_calls := 0
	func set_ctrl_override(owner: String, name: String, value: int) -> void:
		if String(ctrl_owners.get(name, "")) == owner and \
				ctrl_values.has(name) and int(ctrl_values[name]) == value:
			return
		ctrl_values[name] = value
		ctrl_owners[name] = owner
	func clear_ctrl_override(owner: String, name: String) -> void:
		if String(ctrl_owners.get(name, "")) != owner:
			return
		ctrl_owners.erase(name)
		ctrl_values.erase(name)
		cleared_controls.append(name)
	func begin_ctrl_update() -> void:
		begin_batch_calls += 1
	func end_ctrl_update() -> void:
		end_batch_calls += 1


class MuzzleFakeModel:
	extends FakeModel
	var muzzle_queries := 0
	var muzzle_position := Vector3(4.0, 5.0, 6.0)
	func has_muzzle() -> bool:
		return true
	func get_muzzle_world_position() -> Vector3:
		muzzle_queries += 1
		return muzzle_position


# resolve(bms_id, kind, index) like MissionEntityRegistry: bms_id primary, (kind,index) fallback.
class FakeIndex:
	extends RefCounted
	var by_bms_id: Dictionary = {}
	var by_kind_index: Dictionary = {}  # "kind:index" -> Node
	func resolve(bms_id: int, kind: int, index: int) -> Node:
		var n: Variant = null
		if bms_id != 0:
			n = by_bms_id.get(bms_id, null)
		if (n == null or not is_instance_valid(n)) and kind >= 0 and index >= 0:
			n = by_kind_index.get("%d:%d" % [kind, index], null)
		return n if (n != null and is_instance_valid(n)) else null


class CountingIndex:
	extends FakeIndex
	var resolve_calls := 0
	var generation := 1
	func resolve(bms_id: int, kind: int, index: int) -> Node:
		resolve_calls += 1
		return super.resolve(bms_id, kind, index)
	func get_generation() -> int:
		return generation


# Emits a flat PF-layout snapshot, just like NovaSimulation.get_present_snapshot(). Each entity is a
# Dictionary of overrides; unset fields default sanely (alive, not hidden, identity).
class FakeSim:
	extends RefCounted
	var entities: Array = []
	var muzzle_pushes: Array = []
	func set_ai_muzzle_world(net_id: int, position: Vector3) -> void:
		muzzle_pushes.append([net_id, position])
	func _write_phase(out: PackedFloat32Array, base: int,
			channel: int, phase: int, active: bool) -> void:
		var phase_field := NovaSimulation.PF_PHASE1 + (channel - 1) * 2
		var active_field := NovaSimulation.PF_ACTIVE1 + (channel - 1) * 2
		var bits := phase & 0xFFFFFFFF
		out[base + phase_field] = float(bits & 0xFFFF)
		out[base + active_field] = (
				float(((bits >> 16) & 0xFFFF) + 1) if active else 0.0)
	func get_present_stride() -> int:
		return NovaSimulation.PF_STRIDE
	func get_present_snapshot() -> PackedFloat32Array:
		var stride: int = NovaSimulation.PF_STRIDE
		var out := PackedFloat32Array()
		out.resize(entities.size() * stride)
		for i in range(entities.size()):
			var e: Dictionary = entities[i]
			var b := i * stride
			out[b + NovaSimulation.PF_KIND] = float(e.get("kind", -1))
			out[b + NovaSimulation.PF_INDEX] = float(e.get("index", -1))
			out[b + NovaSimulation.PF_BMS_ID] = float(e.get("bms_id", 0))
			out[b + NovaSimulation.PF_TYPE_ID] = float(e.get("type_id", 0))
			out[b + NovaSimulation.PF_WIRE_HANDLE] = float(e.get("handle", 0))
			out[b + NovaSimulation.PF_NET_ID] = float(e.get("net_id", 0))
			out[b + NovaSimulation.PF_POS_X] = float(e.get("pos_x", 0.0))
			out[b + NovaSimulation.PF_POS_Y] = float(e.get("pos_y", 0.0))
			out[b + NovaSimulation.PF_POS_Z] = float(e.get("pos_z", 0.0))
			out[b + NovaSimulation.PF_YAW_DEG] = float(e.get("yaw_deg", 0.0))
			_write_phase(out, b, 1, int(e.get("phase1", 0)),
					int(e.get("active1", 0)) != 0)
			_write_phase(out, b, 2, int(e.get("phase2", 0)),
					int(e.get("active2", 0)) != 0)
			out[b + NovaSimulation.PF_BODY_ANIM_SLOT] = float(e.get("body_anim_slot", -1))
			out[b + NovaSimulation.PF_ANIM_STATE] = float(e.get("anim_state", -1))
			out[b + NovaSimulation.PF_ANIM_PHASE_TICKS] = float(e.get("anim_phase", 0))
			out[b + NovaSimulation.PF_ANIM_SOURCE_STATE] = float(
					e.get("anim_source_state", -1))
			out[b + NovaSimulation.PF_ANIM_SOURCE_PHASE_TICKS] = float(
					e.get("anim_source_phase", -1))
			out[b + NovaSimulation.PF_ANIM_BLEND_WEIGHT] = float(
					e.get("anim_blend_weight", 1.0))
			out[b + NovaSimulation.PF_HIDDEN] = float(e.get("hidden", 0))
			out[b + NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED] = float(
					e.get("local_view_suppressed", 0))
			out[b + NovaSimulation.PF_ALIVE] = float(e.get("alive", 1))
			out[b + NovaSimulation.PF_AIM_OVERLAY_VALID] = float(
					e.get("aim_overlay_valid", 0))
			var body: Vector3 = e.get("aim_body", Vector3.ZERO)
			out[b + NovaSimulation.PF_AIM_BODY_PITCH_DEG] = body.x
			out[b + NovaSimulation.PF_AIM_BODY_YAW_DEG] = body.y
			out[b + NovaSimulation.PF_AIM_BODY_ROLL_DEG] = body.z
			out[b + NovaSimulation.PF_EMPLACED_CONTROLS_VALID] = float(
					e.get("emplaced_controls_valid", 0))
			out[b + NovaSimulation.PF_EWEAP_GUNYAW] = float(
					e.get("emplaced_gun_yaw", 0))
			out[b + NovaSimulation.PF_EWEAP_GUNPITCH] = float(
					e.get("emplaced_gun_pitch", 0))
			out[b + NovaSimulation.PF_VEHICLE_MOTION_VALID] = float(
					e.get("vehicle_motion_valid", 0))
			out[b + NovaSimulation.PF_VEHICLE_STEERING] = float(
					e.get("vehicle_steering", 0))
			out[b + NovaSimulation.PF_VEHICLE_SPEED] = float(
					e.get("vehicle_speed", 0))
			out[b + NovaSimulation.PF_TEX_TEAM_VALID] = float(
					e.get("tex_team_valid", 0))
			out[b + NovaSimulation.PF_TEX_TEAM] = float(
					e.get("tex_team", 0))
			out[b + NovaSimulation.PF_ZONE_CTRL_VALID] = float(
					e.get("zone_ctrl_valid", 0))
			out[b + NovaSimulation.PF_TEAMSWING] = float(
					e.get("team_swing", 0))
			out[b + NovaSimulation.PF_LFP_CAMPPERCENT_VALID] = float(
					e.get("lfp_camp_percent_valid", 0))
			out[b + NovaSimulation.PF_LFP_CAMPPERCENT] = float(
					e.get("lfp_camp_percent", 0))
			out[b + NovaSimulation.PF_WORLD_HEAT_GLOW_VALID] = float(
					e.get("world_heat_glow_valid", 0))
			out[b + NovaSimulation.PF_WORLD_HEAT_GLOW] = float(
					e.get("world_heat_glow", 0))
			out[b + NovaSimulation.PF_RIGHT_HAND_COLLAPSED] = float(
					e.get("right_hand_collapsed", 0))
			var angles: PackedVector3Array = e.get(
					"aim_angles", PackedVector3Array())
			for cls in range(mini(angles.size(), 9)):
				var a := angles[cls]
				var ob := (b + NovaSimulation.PF_AIM_ANGLES
						+ cls * NovaSimulation.PF_AIM_CLASS_STRIDE)
				out[ob] = a.x
				out[ob + 1] = a.y
				out[ob + 2] = a.z
		return out


class RevisionFakeSim:
	extends FakeSim
	var layout_revision := 1
	func get_present_layout_revision() -> int:
		return layout_revision


func _make_pass(index, sim, options: Dictionary = {}) -> Object:
	var p := PresentPass.new()
	p.setup(sim, index, options)
	return p


func test_active_channel_poses_to_phase() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 1001: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 1001, "active1": 1, "phase1": 32768 }]
	_make_pass(index, sim).present()
	assert_eq(model.phases.size(), 1, "one channel posed")
	assert_eq(int((model.phases[0] as Array)[0]), 1, "channel 1")
	assert_eq(int((model.phases[0] as Array)[1]), 32768, "engine-computed phase passes through")


func test_publication_ownership_writes_zero_and_releases_suppressed_channel() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 1: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 1,
		"active1": 1,
		"phase1": 1234,
		"active2": 1,
		"phase2": 5678,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.ctrl_values.get("VEHICLE_SPECIAL1"), 1234)
	assert_eq(model.ctrl_values.get("VEHICLE_SPECIAL2"), 5678)

	sim.entities[0]["phase2"] = 0
	sim.entities[0]["active1"] = 0
	presenter.present()
	assert_false(model.ctrl_values.has("VEHICLE_SPECIAL1"),
			"a suppressed SPECIAL1 releases its prior override")
	assert_eq(model.ctrl_values.get("VEHICLE_SPECIAL2"), 0,
			"an owned zero endpoint is still published")
	assert_has(model.cleared_part_channels, 1)


func test_part_channel_releases_while_inactive_and_catches_up_when_reactivated() -> void:
	var model := FakeModel.new()
	model.part_control_names = { 1: "HOLD_PHASE" }
	add_child_autofree(model)
	var index := CountingIndex.new()
	index.by_bms_id = { 1: model }
	var sim := RevisionFakeSim.new()
	sim.entities = [{ "bms_id": 1, "active1": 1, "phase1": 100 }]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.phases, [[1, 100]])

	sim.entities[0]["active1"] = 0
	sim.entities[0]["phase1"] = 200
	presenter.present()
	presenter.present()
	assert_eq(model.phases, [[1, 100]],
			"an inactive channel does not dispatch another phase")
	assert_false(model.ctrl_values.has("HOLD_PHASE"),
			"an inactive channel releases its prior register publication")
	assert_eq(model.cleared_part_channels, [1],
			"an unchanged inactive stamp does not redispatch the release")

	sim.entities[0]["active1"] = 1
	presenter.present()
	assert_eq(model.phases, [[1, 100], [1, 200]],
			"reactivation catches the model up to the current phase exactly once")


func test_both_channels_posed() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 7: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 7, "active1": 1, "phase1": 100, "active2": 1, "phase2": 200 }]
	_make_pass(index, sim).present()
	assert_eq(model.phases.size(), 2, "both active channels posed")


func test_emplaced_weapon_uses_named_controls_and_clears_them() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 8: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 8,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x1234,
		"emplaced_gun_pitch": 0xFEDC,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.ctrl_values, {
		"EWEAP_GUNYAW": 0x1234,
		"EWEAP_GUNPITCH": 0xFEDC,
	}, "semantic controls do not alias model-order PLAYPARTANIM channels")
	assert_true(model.phases.is_empty())

	sim.entities[0]["emplaced_controls_valid"] = 0
	presenter.present()
	assert_true(model.ctrl_values.is_empty(),
			"dismount/death clears retained EWEAP controls")
	assert_has(model.cleared_controls, "EWEAP_GUNYAW")
	assert_has(model.cleared_controls, "EWEAP_GUNPITCH")


func test_world_heat_glow_owns_cold_zero_and_releases_unavailable_state() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = {10: model}
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 10,
		"world_heat_glow_valid": 1,
		"world_heat_glow": 0,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.ctrl_values.get("HEAT_GLOW", -1), 0,
			"the authoritative cold branch publishes literal zero")

	sim.entities[0]["world_heat_glow"] = 0xFFFF
	presenter.present()
	assert_eq(model.ctrl_values.get("HEAT_GLOW", -1), 0xFFFF,
			"the world-model endpoint is the unsigned-word ceiling")

	sim.entities[0]["world_heat_glow_valid"] = 0
	presenter.present()
	assert_false(model.ctrl_values.has("HEAT_GLOW"),
			"a row without authoritative MountSlot heat releases this writer")
	assert_has(model.cleared_controls, "HEAT_GLOW")


func test_vehicle_motion_controls_share_one_owned_batch_and_release() -> void:
	var model := OwnedBatchModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = {61: model}
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"bms_id": 61,
		"vehicle_motion_valid": 1,
		"vehicle_steering": 0xFEDC,
		"vehicle_speed": 0x10000,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.ctrl_values, {
		"VEHICLE_STEERING": 0xFEDC,
		"VEHICLE_SPEED": 0x10000,
	}, "the cveh pair publishes by semantic retail name")
	assert_eq(model.ctrl_owners, {
		"VEHICLE_STEERING": "present:vehicle_motion",
		"VEHICLE_SPEED": "present:vehicle_motion",
	}, "both stores carry one teardown owner")
	assert_eq(model.begin_batch_calls, 1)
	assert_eq(model.end_batch_calls, 1,
			"both stores are consumed in one CTRL evaluation batch")

	sim.entities[0]["vehicle_motion_valid"] = 0
	presenter.present()
	assert_true(model.ctrl_values.is_empty(),
			"an unavailable/non-authoritative row releases the cveh writer")
	assert_true(model.ctrl_owners.is_empty())
	assert_has(model.cleared_controls, "VEHICLE_STEERING")
	assert_has(model.cleared_controls, "VEHICLE_SPEED")
	assert_eq(model.begin_batch_calls, 2)
	assert_eq(model.end_batch_calls, 2)


func test_sector_and_zone_controls_preserve_write_validity_and_owners() -> void:
	var model := OwnedBatchModel.new()
	add_child_autofree(model)
	# This unrelated writer is the bounded-model stand-in for another semantic
	# producer. Zone teardown must never use clear_ctrl_values().
	model.set_ctrl_override("foreign", "FOREIGN_CTRL", 77)
	var index := FakeIndex.new()
	index.by_bms_id = {62: model}
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"bms_id": 62,
		"tex_team_valid": 1,
		"tex_team": -1,
		"zone_ctrl_valid": 1,
		"team_swing": 0,
		"lfp_camp_percent_valid": 1,
		"lfp_camp_percent": 0x8000,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.ctrl_values, {
		"FOREIGN_CTRL": 77,
		"TEX_TEAM": -1,
		"TEAMSWING": 0,
		"LFP_CAMPPERCENT": 0x8000,
	}, "literal zero TEAMSWING remains an owned retail write")
	assert_eq(model.ctrl_owners.get("TEX_TEAM"), "present:sector_team")
	assert_eq(model.ctrl_owners.get("TEAMSWING"), "present:zone")
	assert_eq(model.ctrl_owners.get("LFP_CAMPPERCENT"), "present:zone")

	# Retail executes each valid writer at model submission, even when its input
	# snapshot did not change. A retained snapshot cache must therefore recover
	# from an intervening producer instead of leaving the foreign value/owner.
	model.set_ctrl_override("foreign", "TEX_TEAM", 7)
	presenter.present()
	assert_eq(model.ctrl_values.get("TEX_TEAM"), -1,
			"an unchanged valid snapshot reasserts the retail writer")
	assert_eq(model.ctrl_owners.get("TEX_TEAM"), "present:sector_team")

	sim.entities[0]["lfp_camp_percent_valid"] = 0
	presenter.present()
	assert_false(model.ctrl_values.has("LFP_CAMPPERCENT"),
			"a zone without a timer-list entry omits LFP instead of writing zero")
	assert_eq(model.ctrl_values.get("TEAMSWING"), 0,
			"the unconditional zone writer remains owned independently")
	assert_eq(model.ctrl_values.get("TEX_TEAM"), -1)

	sim.entities[0]["tex_team_valid"] = 0
	sim.entities[0]["zone_ctrl_valid"] = 0
	presenter.present()
	assert_eq(model.ctrl_values, {"FOREIGN_CTRL": 77},
			"leaving both callbacks releases only their scoped writers")
	assert_eq(model.ctrl_owners, {"FOREIGN_CTRL": "foreign"})
	assert_has(model.cleared_controls, "TEX_TEAM")
	assert_has(model.cleared_controls, "TEAMSWING")
	assert_has(model.cleared_controls, "LFP_CAMPPERCENT")


func test_part_anim_and_emplaced_controls_remain_independent() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 9: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 9,
		"active1": 1,
		"phase1": 0x1111,
		"active2": 1,
		"phase2": 0xEEEE,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x2222,
		"emplaced_gun_pitch": 0xDDDD,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.ctrl_values, {
		"VEHICLE_SPECIAL1": 0x1111,
		"VEHICLE_SPECIAL2": 0xEEEE,
		"EWEAP_GUNYAW": 0x2222,
		"EWEAP_GUNPITCH": 0xDDDD,
	}, "retail publishes generic and emplaced systems on distinct semantic registers")

	sim.entities[0]["emplaced_controls_valid"] = 0
	sim.entities[0]["phase1"] = 0x3333
	sim.entities[0]["phase2"] = 0xCCCC
	presenter.present()
	assert_eq(model.ctrl_values, {
		"VEHICLE_SPECIAL1": 0x3333,
		"VEHICLE_SPECIAL2": 0xCCCC,
	}, "dismount clears only stale EWEAP ownership")


func test_first_invalid_emplaced_state_clears_stale_node_controls() -> void:
	var model := FakeModel.new()
	model.ctrl_values = {
		"EWEAP_GUNYAW": 0x1234,
		"EWEAP_GUNPITCH": 0xFEDC,
	}
	add_child_autofree(model)
	var index := CountingIndex.new()
	index.by_bms_id = { 8: model }
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"bms_id": 8,
		"emplaced_controls_valid": 0,
	}]
	_make_pass(index, sim).present()
	assert_true(model.ctrl_values.is_empty(),
			"the cold applied-state cache cannot retain controls from an earlier owner")
	assert_eq(model.cleared_controls, [
		"EWEAP_GUNYAW",
		"EWEAP_GUNPITCH",
	])


func test_dismount_restores_generic_part_values_for_the_same_registers() -> void:
	var model := FakeModel.new()
	model.part_control_names = {
		1: "EWEAP_GUNYAW",
		2: "EWEAP_GUNPITCH",
	}
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 9: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 9,
		"active1": 1,
		"phase1": 0x1111,
		"active2": 1,
		"phase2": 0xEEEE,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x2222,
		"emplaced_gun_pitch": 0xDDDD,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.ctrl_values, {
		"EWEAP_GUNYAW": 0x2222,
		"EWEAP_GUNPITCH": 0xDDDD,
	}, "live gunner controls outrank generic model-order values")

	sim.entities[0]["emplaced_controls_valid"] = 0
	sim.entities[0]["phase1"] = 0x3333
	sim.entities[0]["phase2"] = 0xCCCC
	presenter.present()
	assert_eq(model.ctrl_values, {
		"EWEAP_GUNYAW": 0x3333,
		"EWEAP_GUNPITCH": 0xCCCC,
	}, "dismount clears stale semantic ownership before generic PLAYPARTANIM")


func test_dismount_restores_unchanged_generic_values_for_aliased_registers() -> void:
	var model := FakeModel.new()
	model.part_control_names = {
		1: "EWEAP_GUNYAW",
		2: "EWEAP_GUNPITCH",
	}
	add_child_autofree(model)
	var index := CountingIndex.new()
	index.by_bms_id = { 9: model }
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"bms_id": 9,
		"active1": 1,
		"phase1": 0x1111,
		"active2": 1,
		"phase2": 0xEEEE,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x2222,
		"emplaced_gun_pitch": 0xDDDD,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.ctrl_values, {
		"EWEAP_GUNYAW": 0x2222,
		"EWEAP_GUNPITCH": 0xDDDD,
	})

	# The generic phase did not change, but clearing semantic mount ownership
	# erased the aliased register. It therefore has to be replayed on dismount.
	sim.entities[0]["emplaced_controls_valid"] = 0
	presenter.present()
	assert_eq(model.ctrl_values, {
		"EWEAP_GUNYAW": 0x1111,
		"EWEAP_GUNPITCH": 0xEEEE,
	}, "dismount restores retained generic values even when their phases are unchanged")


func test_body_clip_poses_to_sim_anim_state_phase() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 11: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 11, "body_anim_slot": 1, "anim_state": 43, "anim_phase": 9 }]
	_make_pass(index, sim).present()
	assert_eq(model.body_calls.size(), 1, "one body clip posed")
	assert_eq(String((model.body_calls[0] as Array)[0]), "anim_idle", "infantry anim state resolves to .adm key")
	assert_eq(int((model.body_calls[0] as Array)[1]), 9, "sim clip phase passes through")


func test_aim_and_right_hand_changes_repose_stable_body_state() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := CountingIndex.new()
	index.by_bms_id = { 11: model }
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"bms_id": 11,
		"anim_state": 43,
		"anim_phase": 9,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(3.0, 27.0, -2.0),
		"aim_angles": PackedVector3Array([Vector3(5.0, 6.0, 7.0)]),
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.body_calls.size(), 1)

	sim.entities[0]["aim_angles"] = PackedVector3Array([Vector3(8.0, 9.0, 10.0)])
	presenter.present()
	assert_eq(model.overlay_calls.size(), 2, "changed aim reaches the model")
	assert_eq(model.body_calls.size(), 2,
			"changed aim reposes an otherwise-stable externally-phased body")

	sim.entities[0]["right_hand_collapsed"] = 1
	presenter.present()
	assert_eq(model.right_hand_collapse_calls, [false, true])
	assert_eq(model.body_calls.size(), 3,
			"changed collapse state also reposes the stable body")


func test_hidden_body_catches_up_when_it_becomes_presentable() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := CountingIndex.new()
	index.by_bms_id = { 11: model }
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"bms_id": 11,
		"hidden": 1,
		"anim_state": 43,
		"anim_phase": 9,
		"aim_overlay_valid": 1,
		"aim_angles": PackedVector3Array([Vector3(5.0, 6.0, 7.0)]),
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_true(model.body_calls.is_empty(), "hidden non-muzzle bodies skip skeletal dispatch")

	sim.entities[0]["aim_angles"] = PackedVector3Array([Vector3(8.0, 9.0, 10.0)])
	sim.entities[0]["right_hand_collapsed"] = 1
	presenter.present()
	assert_true(model.body_calls.is_empty(),
			"pose dependencies may update while hidden without writing the body")

	sim.entities[0]["hidden"] = 0
	presenter.present()
	assert_eq(model.body_calls, [["anim_idle", 9]],
			"visibility eligibility catches the body up to its current authoritative pose")


func test_body_clip_poses_authoritative_two_channel_blend() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := CountingIndex.new()
	index.by_bms_id = {11: model}
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"bms_id": 11,
		"anim_source_state": 43,
		"anim_source_phase": 17,
		"anim_state": 1,
		"anim_phase": 3,
		"anim_blend_weight": 0.2,
		"aim_overlay_valid": 1,
		"aim_angles": PackedVector3Array([Vector3(1.0, 2.0, 3.0)]),
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.body_calls.size(), 1)
	var call: Array = model.body_calls[0]
	assert_eq(call.slice(0, 5), [
		"blend", "anim_idle", 17, "anim_walk_forward", 3])
	assert_almost_eq(float(call[5]), 0.2, 0.000001,
			"placed NPCs consume the authority's exact primary blend tuple")

	presenter.present()
	assert_eq(model.body_calls.size(), 1,
			"an unchanged retained blend does not redispatch")

	sim.entities[0]["anim_source_phase"] = 18
	presenter.present()
	assert_eq(model.body_calls.size(), 2,
			"the outgoing playhead participates in the retained pose stamp")

	sim.entities[0]["anim_blend_weight"] = 0.3
	presenter.present()
	assert_eq(model.body_calls.size(), 3,
			"the float32 blend weight participates in the retained pose stamp")

	sim.entities[0]["anim_source_state"] = 0
	presenter.present()
	assert_eq(model.body_calls.size(), 4,
			"the outgoing semantic state participates in the retained pose stamp")
	assert_eq((model.body_calls.back() as Array).slice(0, 3),
			["blend", "anim_reset", 18])

	sim.entities[0]["aim_angles"] = PackedVector3Array(
			[Vector3(4.0, 5.0, 6.0)])
	presenter.present()
	assert_eq(model.body_calls.size(), 5,
			"an overlay change reposes the retained two-channel body")

	sim.entities[0]["right_hand_collapsed"] = 1
	presenter.present()
	assert_eq(model.body_calls.size(), 6,
			"a right-hand mask change reposes the retained two-channel body")

	var channels := int(presenter.get_output_channels())
	presenter.set_output_channels(channels & ~PresentPass.OUTPUT_BODY_ANIM)
	sim.entities[0]["anim_source_phase"] = 19
	sim.entities[0]["anim_blend_weight"] = 0.4
	presenter.present()
	assert_eq(model.body_calls.size(), 6,
			"a disabled body channel performs no blend dispatch")
	presenter.set_output_channels(channels)
	presenter.present()
	assert_eq(model.body_calls.size(), 7,
			"reenabling the body channel cold-applies the full latest tuple")
	assert_eq((model.body_calls.back() as Array).slice(0, 5),
			["blend", "anim_reset", 19, "anim_walk_forward", 3])
	assert_almost_eq(float((model.body_calls.back() as Array)[5]),
			0.4, 0.000001)

	sim.entities[0]["hidden"] = 1
	presenter.present()
	sim.entities[0]["anim_source_phase"] = 20
	sim.entities[0]["anim_blend_weight"] = 0.5
	presenter.present()
	assert_eq(model.body_calls.size(), 7,
			"a hidden non-muzzle body does not write an updated blend")
	sim.entities[0]["hidden"] = 0
	presenter.present()
	assert_eq(model.body_calls.size(), 8,
			"becoming visible catches up with the full latest blend tuple")
	assert_eq((model.body_calls.back() as Array).slice(0, 5),
			["blend", "anim_reset", 20, "anim_walk_forward", 3])
	assert_almost_eq(float((model.body_calls.back() as Array)[5]),
			0.5, 0.000001)


func test_placed_model_applies_snapshot_overlay_in_body_frame() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 12: model }
	var angles := PackedVector3Array()
	for i in range(9):
		angles.append(Vector3(2.0 + i, 20.0 + i, -3.0 + i))
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 12,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(7.0, 41.0, -5.0),
		"aim_angles": angles,
	}]
	_make_pass(index, sim).present()
	assert_eq(model.overlay_calls.size(), 1)
	var deltas: Array = model.overlay_calls[0]
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
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 13: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 13, "aim_overlay_valid": 0 }]
	_make_pass(index, sim).present()
	assert_eq(model.overlay_calls, [[]],
			"an unknown selector clears any pose retained by the model")


func test_placed_model_applies_and_restores_mounted_right_hand_collapse() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 14: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 14,
		"aim_overlay_valid": 1,
		"right_hand_collapsed": 1,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	sim.entities[0]["right_hand_collapsed"] = 0
	presenter.present()
	assert_eq(model.right_hand_collapse_calls, [true, false],
			"the placed pose consumes the packed mount verdict and restores on dismount")


func test_resolves_by_kind_index_fallback() -> void:
	# Editor path: the in-memory mission has no stable bms_id (0). The pass must fall back to (kind,index).
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_kind_index = { "3:2": model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 0, "kind": 3, "index": 2, "active1": 1, "phase1": 77 }]
	_make_pass(index, sim).present()
	assert_eq(model.phases.size(), 1, "resolved by (kind,index) when bms_id is 0")
	assert_eq(int((model.phases[0] as Array)[1]), 77)


func test_transform_applied_from_snapshot() -> void:
	# The consolidated pass MOVES entities (the old game path did not). Position is Godot-space; yaw-only
	# builds RotY(180 - yaw) through the shared placer convention.
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 5: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 5, "pos_x": 10.0, "pos_y": 2.0, "pos_z": -4.0, "yaw_deg": 90.0 }]
	_make_pass(index, sim).present()
	assert_true(model.position.is_equal_approx(Vector3(10.0, 2.0, -4.0)), "position applied from snapshot")
	# yaw 90 -> RotY(180 - 90) = RotY(90deg); model +x maps toward -z.
	var fwd := model.transform.basis * Vector3(1, 0, 0)
	assert_almost_eq(fwd.z, -1.0, 0.001, "yaw drives the heading basis (RotY(180 - yaw))")


func test_stable_snapshot_does_not_redirty_the_transform_tree() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 21: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 21,
		"pos_x": 12.0,
		"yaw_deg": 15.0,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(3.0, 27.0, -2.0),
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(int(presenter.get_stats()["moved"]), 1)
	presenter.present()
	assert_eq(int(presenter.get_stats()["moved"]), 1,
			"an unchanged row cannot recursively dirty every model descendant again")
	assert_true(model.basis.is_equal_approx(MissionObjectPlacer.bms_to_godot_basis(
			Vector3(3.0, 27.0, -2.0))),
			"aim body rotation is composed into the one root transform write")


func test_changed_aim_body_updates_root_with_stable_entity_transform() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := CountingIndex.new()
	index.by_bms_id = { 21: model }
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"bms_id": 21,
		"handle": 2,
		"type_id": 101,
		"pos_x": 12.0,
		"yaw_deg": 15.0,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(3.0, 27.0, -2.0),
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	var moved := int(presenter.get_stats()["moved"])

	sim.entities[0]["aim_body"] = Vector3(-5.0, 61.0, 4.0)
	presenter.present()
	assert_eq(int(presenter.get_stats()["moved"]), moved + 1,
			"aim-owned root rotation participates in the transform change stamp")
	assert_true(model.basis.is_equal_approx(MissionObjectPlacer.bms_to_godot_basis(
			Vector3(-5.0, 61.0, 4.0))),
			"the changed aim body, not the stable entity Euler, owns the root")


func test_stable_revisioned_snapshot_caches_pose_and_reasserts_live_publishers() -> void:
	var model := MuzzleFakeModel.new()
	add_child_autofree(model)
	var index := CountingIndex.new()
	index.by_bms_id = { 21: model }
	var sim := RevisionFakeSim.new()
	sim.entities = [{
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
	var presenter := _make_pass(index, sim)
	presenter.present()
	var presenter_stats: Dictionary = presenter.get_stats()
	var stats_record: MissionPresentStats = presenter.get_stats_record()
	assert_eq(stats_record.transform_builds,
			int(presenter_stats["transform_builds"]))
	assert_eq(stats_record.muzzle_queries,
			int(presenter_stats["muzzle_queries"]))
	var moved := int(presenter.get_stats()["moved"])
	var phase_calls := model.phases.size()
	var overlay_calls := model.overlay_calls.size()
	var rhc_calls := model.right_hand_collapse_calls.size()
	var set_ctrl_calls := model.set_controls.size()
	var clear_ctrl_calls := model.cleared_controls.size()
	var body_calls := model.body_calls.size()
	var muzzle_queries := model.muzzle_queries
	var muzzle_pushes := sim.muzzle_pushes.size()

	# Visibility and the AI muzzle seam are live outputs, so a stable snapshot
	# must not short-circuit the entire row.
	model.visible = false
	presenter.present()
	var next_presenter_stats: Dictionary = presenter.get_stats()
	assert_true(model.visible, "live visibility ownership is reconciled every frame")
	assert_eq(model.muzzle_queries, muzzle_queries + 1,
			"the four-tick muzzle-freshness seam still samples every frame")
	assert_eq(sim.muzzle_pushes.size(), muzzle_pushes + 1,
			"the fresh muzzle sample still reaches the simulation")

	assert_eq(int(presenter.get_stats()["moved"]), moved,
			"stable transform inputs do not rebuild or write the root transform")
	assert_eq(model.phases.size(), phase_calls,
			"stable PANM inputs do not redispatch their phase")
	assert_eq(model.overlay_calls.size(), overlay_calls,
			"stable aim inputs do not rebuild or redispatch the overlay")
	assert_eq(model.right_hand_collapse_calls.size(), rhc_calls,
			"stable right-hand collapse does not redispatch")
	assert_eq(model.set_controls.size(), set_ctrl_calls,
			"idempotent model setters absorb unchanged semantic publications")
	assert_eq(model.cleared_controls.size(), clear_ctrl_calls,
			"omitted semantic writers are not repeatedly cleared")
	assert_eq(model.body_calls.size(), body_calls,
			"stable externally-phased body state does not redispatch")
	for key in [
		"transform_builds",
		"aim_dispatches",
		"rhc_dispatches",
		"body_dispatches",
	]:
		assert_eq(int(next_presenter_stats[key]), int(presenter_stats[key]),
				"stable rows add no %s work" % key)
	assert_eq(int(next_presenter_stats["part_dispatches"]),
			int(presenter_stats["part_dispatches"]) + 1,
			"an active PLAYPART writer is reasserted at every submission")
	assert_eq(int(next_presenter_stats["control_dispatches"]),
			int(presenter_stats["control_dispatches"]) + 2,
			"the two valid EWEAP writers are reasserted at every submission")
	assert_eq(int(next_presenter_stats["muzzle_queries"]),
			int(presenter_stats["muzzle_queries"]) + 1,
			"the presenter counter also records the mandatory live muzzle query")


func test_stable_layout_reuses_resolution_but_reads_fresh_pose() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := CountingIndex.new()
	index.by_bms_id = { 21: model }
	var sim := RevisionFakeSim.new()
	sim.entities = [
		{ "bms_id": 21, "handle": 2, "type_id": 101, "pos_x": 1.0 },
		{ "bms_id": 999, "handle": 3, "type_id": 102, "pos_x": 50.0 },
	]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(index.resolve_calls, 2, "the cold layout resolves each row once")
	assert_almost_eq(model.position.x, 1.0, 0.001)

	sim.entities[0]["pos_x"] = 9.0
	presenter.present()
	assert_eq(index.resolve_calls, 2,
			"pose-only updates reuse the revision-bound row plan")
	assert_almost_eq(model.position.x, 9.0, 0.001,
			"cached routing still reads fresh row values")


func test_layout_plan_rebinds_after_reorder_removal_and_replacement() -> void:
	var a := FakeModel.new()
	var b := FakeModel.new()
	var c := FakeModel.new()
	add_child_autofree(a)
	add_child_autofree(b)
	add_child_autofree(c)
	var index := CountingIndex.new()
	index.by_bms_id = { 11: a, 22: b, 33: c }
	var sim := RevisionFakeSim.new()
	sim.entities = [
		{ "bms_id": 11, "handle": 2, "type_id": 101, "pos_x": 1.0 },
		{ "bms_id": 22, "handle": 3, "type_id": 102, "pos_x": 2.0 },
	]
	var presenter := _make_pass(index, sim)
	presenter.present()

	sim.entities = [
		{ "bms_id": 22, "handle": 3, "type_id": 102, "pos_x": 20.0 },
		{ "bms_id": 11, "handle": 2, "type_id": 101, "pos_x": 10.0 },
	]
	sim.layout_revision += 1
	presenter.present()
	assert_eq(index.resolve_calls, 4, "row reorder rebuilds the plan")
	assert_almost_eq(a.position.x, 10.0, 0.001)
	assert_almost_eq(b.position.x, 20.0, 0.001)

	sim.entities = [
		{ "bms_id": 11, "handle": 2, "type_id": 101, "pos_x": 12.0 },
	]
	sim.layout_revision += 1
	presenter.present()
	assert_eq(index.resolve_calls, 5, "row removal cannot retain a stale base")
	assert_almost_eq(a.position.x, 12.0, 0.001)
	assert_almost_eq(b.position.x, 20.0, 0.001)

	sim.entities = [
		{ "bms_id": 33, "handle": 4, "type_id": 103, "pos_x": 30.0 },
	]
	sim.layout_revision += 1
	presenter.present()
	assert_eq(index.resolve_calls, 6,
			"same-size identity replacement is resolved against the new row")
	assert_almost_eq(a.position.x, 12.0, 0.001)
	assert_almost_eq(c.position.x, 30.0, 0.001)


func test_freed_cached_node_marks_revisioned_plan_for_rebind() -> void:
	var old_model := FakeModel.new()
	var index := CountingIndex.new()
	index.by_bms_id = { 21: old_model }
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"bms_id": 21,
		"handle": 2,
		"type_id": 101,
		"pos_x": 12.0,
	}]
	var visibility_intent := {}
	var presenter := _make_pass(index, sim, {
		"present_visibility": visibility_intent,
	})
	presenter.present()
	assert_almost_eq(old_model.position.x, 12.0, 0.001)
	assert_true(bool(visibility_intent.get(21, false)))

	old_model.free()
	var replacement := FakeModel.new()
	add_child_autofree(replacement)
	index.by_bms_id[21] = replacement
	sim.entities[0]["pos_x"] = 30.0

	# A trusted layout revision no longer scans every ObjectID before the walk.
	# The first null encounter may defer the rebind, but it must dirty the plan
	# so the replacement is resolved on the following public presentation call.
	presenter.present()
	assert_false(visibility_intent.has(21),
			"a freed cached node releases its visibility intent immediately")
	presenter.present()
	assert_eq(index.resolve_calls, 2, "the freed cached node triggers one plan rebind")
	assert_almost_eq(replacement.position.x, 30.0, 0.001,
			"the replacement receives the current row after the rebind")


func test_transform_ignores_body_clip_visual_offsets() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 6: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 6,
		"pos_x": 10.0,
		"pos_y": 2.0,
		"pos_z": 3.0,
		"yaw_deg": 180.0,
		"anim_state": 86,
	}]
	_make_pass(index, sim).present()
	assert_true(model.position.is_equal_approx(Vector3(10.0, 2.0, 3.0)),
		"sim entity position remains authoritative even for seated infantry clips")


func test_visibility_from_hidden_and_alive() -> void:
	var shown := FakeModel.new()
	var hidden := FakeModel.new()
	var dead := FakeModel.new()
	var corpse := FakeModel.new()
	var despawned := FakeModel.new()
	add_child_autofree(shown)
	add_child_autofree(hidden)
	add_child_autofree(dead)
	add_child_autofree(corpse)
	add_child_autofree(despawned)
	var index := FakeIndex.new()
	index.by_bms_id = { 1: shown, 2: hidden, 3: dead, 4: corpse, 5: despawned }
	var sim := FakeSim.new()
	sim.entities = [
		{ "bms_id": 1, "hidden": 0, "alive": 1 },
		{ "bms_id": 2, "hidden": 1, "alive": 1 },
		{ "bms_id": 3, "hidden": 0, "alive": 0 },
		# A dead ORGANIC keeps rendering as a corpse until the sim despawns it via
		# PF_HIDDEN (the corpse timer + watch rule; world-wac-ai-re §19.4).
		{ "bms_id": 4, "hidden": 0, "alive": 0, "kind": 3 },
		{ "bms_id": 5, "hidden": 1, "alive": 0, "kind": 3 },
	]
	_make_pass(index, sim).present()
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
	var mount := FakeModel.new()
	var unrelated := FakeModel.new()
	add_child_autofree(mount)
	add_child_autofree(unrelated)
	var index := FakeIndex.new()
	index.by_bms_id = { 41: mount, 42: unrelated }
	var sim := FakeSim.new()
	sim.entities = [
		{ "bms_id": 41, "hidden": 0, "local_view_suppressed": 1,
				"anim_state": 43, "anim_phase": 7 },
		{ "bms_id": 42, "hidden": 0, "local_view_suppressed": 0,
				"anim_state": 43, "anim_phase": 7 },
	]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_false(mount.visible,
			"the committed first-person UseGun parent skips its own world model")
	assert_true(unrelated.visible,
			"local UseGun suppression cannot hide unrelated world entities")
	assert_eq(mount.body_calls.size(), 0,
			"a hidden model without a muzzle provider skips skeletal writes")
	assert_eq(unrelated.body_calls.size(), 1,
			"the visible model still receives its absolute pose")

	# Camera-mode changes, detach, and pre-commit slot mismatch all clear the
	# transient verdict. PF_HIDDEN remains independently authoritative.
	sim.entities[0]["local_view_suppressed"] = 0
	presenter.present()
	assert_true(mount.visible, "clearing the render verdict restores the parent immediately")
	assert_eq(mount.body_calls.size(), 1,
			"the absolute phase catches up when the model becomes renderable")
	sim.entities[0]["hidden"] = 1
	presenter.present()
	assert_false(mount.visible, "authoritative PF_HIDDEN still wins independently")


func test_options_gate_each_channel() -> void:
	# The editor/game can disable a channel; e.g. transform-only leaves part phases untouched.
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 9: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 9, "active1": 1, "phase1": 50, "pos_x": 3.0 }]
	_make_pass(index, sim, { "drive_part_anim": false }).present()
	assert_eq(model.phases.size(), 0, "part-anim channel disabled -> nothing posed")
	assert_almost_eq(model.position.x, 3.0, 0.001, "transform still applied")


func test_reenabled_output_channels_catch_up_to_current_state() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := CountingIndex.new()
	index.by_bms_id = { 9: model }
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"bms_id": 9,
		"pos_x": 1.0,
		"active1": 1,
		"phase1": 100,
		"anim_state": 43,
		"anim_phase": 9,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_almost_eq(model.position.x, 1.0, 0.001)
	assert_eq(model.phases, [[1, 100]])
	assert_eq(model.body_calls, [["anim_idle", 9]])

	var channels := int(presenter.get_output_channels())
	var frozen := (
			PresentPass.OUTPUT_TRANSFORM
			| PresentPass.OUTPUT_PART_ANIM
			| PresentPass.OUTPUT_BODY_ANIM)
	presenter.set_output_channels(channels & ~frozen)
	sim.entities[0]["pos_x"] = 8.0
	sim.entities[0]["phase1"] = 200
	sim.entities[0]["anim_phase"] = 12
	presenter.present()
	assert_almost_eq(model.position.x, 1.0, 0.001)
	assert_eq(model.phases, [[1, 100]])
	assert_eq(model.body_calls, [["anim_idle", 9]])

	presenter.set_output_channels(channels)
	presenter.present()
	assert_almost_eq(model.position.x, 8.0, 0.001,
			"transform ownership catches up on its rising edge")
	assert_eq(model.phases, [[1, 100], [1, 200]],
			"part ownership catches up on its rising edge")
	assert_eq(model.body_calls, [["anim_idle", 9], ["anim_idle", 12]],
			"body ownership catches up on its rising edge")


func test_unresolved_target_does_not_crash() -> void:
	var index := FakeIndex.new()  # empty -> resolves nothing
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 1234, "active1": 1, "phase1": 1 }]
	_make_pass(index, sim).present()  # must not crash
	assert_eq(int(_make_pass(index, sim).get_stats()["posed"]), 0, "nothing posed when the target is unresolved")


func test_occlusion_claim_blocks_the_show_but_never_the_hide() -> void:
	# Two-bit visibility ownership: the render-occlusion apply owns hides
	# through a claim set shared by reference (the occlusion_hidden_ids setup
	# option). A sim-wants-visible write is withheld while the claim stands, a
	# sim hide always lands, and clearing the claim (as the occlusion release
	# does) returns sole ownership to this pass.
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 1001: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 1001 }]
	var claims := { 1001: true }
	var visibility_intent := {}
	var present_pass := _make_pass(index, sim, {
		"occlusion_hidden_ids": claims,
		"present_visibility": visibility_intent,
	})

	model.visible = false  # occlusion hid it; the sim wants it visible
	present_pass.present()
	assert_false(model.visible, "a claimed node is not re-shown by the present drive")
	assert_true(bool(visibility_intent.get(1001, false)),
			"the shared release intent records the complete present predicate")

	sim.entities = [{ "bms_id": 1001, "hidden": 1 }]
	present_pass.present()
	assert_false(model.visible, "a sim hide lands regardless of the claim")
	assert_false(bool(visibility_intent.get(1001, true)))

	sim.entities = [{ "bms_id": 1001, "local_view_suppressed": 1 }]
	present_pass.present()
	assert_false(bool(visibility_intent.get(1001, true)),
			"first-person suppression is part of the same release predicate")

	sim.entities = [{ "bms_id": 1001 }]
	claims.clear()
	present_pass.present()
	assert_true(model.visible,
			"with the claim cleared the present drive owns visibility again")


func test_body_anim_dispatch_gates_on_the_ab_seam() -> void:
	# The public output-channel mask freezes pose dispatch so probes can isolate
	# the skeleton-update share without reaching into presenter fields.
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 11: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 11, "body_anim_slot": 1, "anim_state": 43, "anim_phase": 9 }]
	var present_pass := _make_pass(index, sim)
	present_pass.present()
	assert_eq(model.body_calls.size(), 1, "body anim dispatches by default")
	var channels := int(present_pass.get_output_channels())
	present_pass.set_output_channels(channels & ~PresentPass.OUTPUT_BODY_ANIM)
	present_pass.present()
	assert_eq(model.body_calls.size(), 1, "the frozen seam dispatches nothing new")
	present_pass.set_output_channels(channels)
	present_pass.present()
	assert_eq(model.body_calls.size(), 2, "restoring the seam resumes dispatch")


func test_disabling_part_anim_output_releases_all_retained_ctrl_writers() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = {12: model}
	var sim := FakeSim.new()
	sim.entities = [{
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
	var present_pass := _make_pass(index, sim)
	present_pass.present()
	assert_false(model.ctrl_values.is_empty())
	var channels := int(present_pass.get_output_channels())
	present_pass.set_output_channels(channels & ~PresentPass.OUTPUT_PART_ANIM)
	assert_true(model.ctrl_values.is_empty(),
			"freezing the output seam cannot retain its last CTRL frame")
	assert_has(model.cleared_part_channels, 1)
	assert_has(model.cleared_controls, "EWEAP_GUNYAW")
	assert_has(model.cleared_controls, "EWEAP_GUNPITCH")
	assert_has(model.cleared_controls, "HEAT_GLOW")
	assert_has(model.cleared_controls, "TEX_TEAM")
	assert_has(model.cleared_controls, "TEAMSWING")
	assert_has(model.cleared_controls, "LFP_CAMPPERCENT")


func test_native_basis_matches_the_placement_convention() -> void:
	# The native walk carries its own port of bms_to_godot_basis (the godot-cpp
	# Basis(axis, angle) parity gotcha): pin the two implementations together
	# across the angle space so they can never drift.
	for pitch in [-90.0, -30.0, 0.0, 15.0, 90.0, 180.0]:
		for yaw in [-180.0, -45.0, 0.0, 90.0, 135.0, 270.0]:
			for roll in [-60.0, 0.0, 30.0, 180.0]:
				var rot := Vector3(pitch, yaw, roll)
				var expected := MissionObjectPlacer.bms_to_godot_basis(rot)
				var got: Basis = NovaPresentApplier.bms_to_godot_basis(rot)
				assert_true(got.is_equal_approx(expected),
						"basis parity at %s: native %s vs placer %s" % [
								rot, got, expected])
