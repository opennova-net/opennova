extends GutTest

const WirePresentPass := preload("res://engine/world/wire_present_pass.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const PresentHeldWeapon := preload("res://engine/world/present_held_weapon.gd")


class FakeModel:
	extends Node3D
	var overlay_calls: Array = []
	var right_hand_collapse_calls: Array[bool] = []
	var weapon_channel_calls: Array = []
	var body_calls: Array = []
	var remote_tick_calls: Array[int] = []
	var remote_tick_results: Array[bool] = []
	var remote_body_calls: Array = []
	var remote_apply_results: Array[bool] = []
	var reset_remote_body_calls := 0
	var part_calls: Array = []
	var cleared_part_channels: Array[int] = []
	var pose_call_order: Array[String] = []
	var ctrl_values: Dictionary = {}
	var cleared_controls: Array[String] = []
	var shadow_caster_enabled := false
	func play_body_clip_at(key: String, phase_ticks: int) -> void:
		body_calls.append(["at", key, phase_ticks])
		pose_call_order.append("body")
	func play_body_blend_at(source_key: String, source_phase_ticks: int,
			target_key: String, target_phase_ticks: int, weight: float) -> void:
		body_calls.append([
			"blend", source_key, source_phase_ticks,
			target_key, target_phase_ticks, weight])
		pose_call_order.append("body")
	func play_body_clip(key: String) -> void:
		body_calls.append(["free", key])
		pose_call_order.append("body")
	func apply_remote_body_state(state_id: int, key: String, flags: int,
			phase_ticks: int = -1) -> bool:
		remote_body_calls.append([state_id, key, flags, phase_ticks])
		pose_call_order.append("body")
		return remote_apply_results.pop_front() \
				if not remote_apply_results.is_empty() else false
	func reset_remote_body_state() -> void:
		reset_remote_body_calls += 1
		pose_call_order.append("reset")
	func advance_remote_body_blend_tick(state_id: int) -> bool:
		remote_tick_calls.append(state_id)
		return remote_tick_results.pop_front() \
				if not remote_tick_results.is_empty() else false
	func set_part_phase(channel: int, phase: int) -> void:
		part_calls.append([channel, phase])
		ctrl_values["VEHICLE_SPECIAL%d" % channel] = phase
	func clear_part_phase(channel: int) -> void:
		cleared_part_channels.append(channel)
		ctrl_values.erase("VEHICLE_SPECIAL%d" % channel)
	func set_aim_overlay(deltas: Array) -> void:
		overlay_calls.append(deltas)
		pose_call_order.append("overlay")
	func set_right_hand_collapsed(collapsed: bool) -> void:
		right_hand_collapse_calls.append(collapsed)
		pose_call_order.append("right_hand")
	func set_weapon_channel(key: String, phase_ticks: int) -> void:
		weapon_channel_calls.append([key, phase_ticks])
	func set_ctrl_value(name: String, value: int) -> void:
		ctrl_values[name] = value
	func clear_ctrl_value(name: String) -> void:
		ctrl_values.erase(name)
		cleared_controls.append(name)
	func set_shadow_caster_enabled(enabled: bool) -> void:
		shadow_caster_enabled = enabled


class FakeSim:
	extends RefCounted
	var entities: Array = []
	var local_player_present := true
	var local_player_handle := 1

	func get_present_stride() -> int:
		return NovaSimulation.PF_STRIDE

	# The ADM-indexed third-person model lookup the present pass asks the sim for —
	# deliberately by INDEX, since that is what the wire row carries.
	func get_weapon_third_person_model(adm_index: int) -> String:
		return "WPN%d_3rd" % adm_index if adm_index > 0 else ""

	func has_local_player() -> bool:
		return local_player_present

	func get_local_player_wire_handle() -> int:
		return local_player_handle

	func _write_phase(out: PackedFloat32Array, base: int,
			channel: int, phase: int, active: bool) -> void:
		var phase_field := NovaSimulation.PF_PHASE1 + (channel - 1) * 2
		var active_field := NovaSimulation.PF_ACTIVE1 + (channel - 1) * 2
		var bits := phase & 0xFFFFFFFF
		out[base + phase_field] = float(bits & 0xFFFF)
		out[base + active_field] = (
				float(((bits >> 16) & 0xFFFF) + 1) if active else 0.0)

	func get_present_snapshot() -> PackedFloat32Array:
		var stride := NovaSimulation.PF_STRIDE
		var out := PackedFloat32Array()
		out.resize(entities.size() * stride)
		for i in range(entities.size()):
			var entity: Dictionary = entities[i]
			var base := i * stride
			out[base + NovaSimulation.PF_TYPE_ID] = float(entity.get("type_id", 0))
			out[base + NovaSimulation.PF_WIRE_HANDLE] = float(entity.get("handle", 0))
			out[base + NovaSimulation.PF_KIND] = float(entity.get("kind", -1))
			out[base + NovaSimulation.PF_INDEX] = float(entity.get("index", -1))
			out[base + NovaSimulation.PF_BMS_ID] = float(entity.get("bms_id", 0))
			out[base + NovaSimulation.PF_POS_X] = float(entity.get("x", 0.0))
			out[base + NovaSimulation.PF_POS_Y] = float(entity.get("y", 0.0))
			out[base + NovaSimulation.PF_POS_Z] = float(entity.get("z", 0.0))
			out[base + NovaSimulation.PF_YAW_DEG] = float(entity.get("yaw", 0.0))
			out[base + NovaSimulation.PF_PITCH_DEG] = float(entity.get("pitch", 0.0))
			out[base + NovaSimulation.PF_ROLL_DEG] = float(entity.get("roll", 0.0))
			out[base + NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED] = float(
					entity.get("local_view_suppressed", 0))
			out[base + NovaSimulation.PF_HIDDEN] = float(entity.get("hidden", 0))
			out[base + NovaSimulation.PF_ALIVE] = float(entity.get("alive", 1))
			out[base + NovaSimulation.PF_RESPAWN_REVISION] = float(
					entity.get("respawn_revision", 0))
			_write_phase(out, base, 1, int(entity.get("phase1", 0)),
					int(entity.get("active1", 0)) != 0)
			_write_phase(out, base, 2, int(entity.get("phase2", 0)),
					int(entity.get("active2", 0)) != 0)
			out[base + NovaSimulation.PF_ANIM_STATE] = float(entity.get("anim_state", -1))
			out[base + NovaSimulation.PF_ANIM_PHASE_TICKS] = float(
					entity.get("anim_phase", -1))
			out[base + NovaSimulation.PF_ANIM_SOURCE_STATE] = float(
					entity.get("anim_source_state", -1))
			out[base + NovaSimulation.PF_ANIM_SOURCE_PHASE_TICKS] = float(
					entity.get("anim_source_phase", -1))
			out[base + NovaSimulation.PF_ANIM_BLEND_WEIGHT] = float(
					entity.get("anim_blend_weight", 1.0))
			out[base + NovaSimulation.PF_ANIM_REMOTE_REQUEST] = float(
					entity.get("anim_remote_request", 1))
			out[base + NovaSimulation.PF_ANIM_STATE_PULSE] = float(
					entity.get("anim_pulse", -1))
			out[base + NovaSimulation.PF_ANIM_PULSE_TICKS] = float(
					entity.get("anim_pulse_ticks", -1))
			out[base + NovaSimulation.PF_AIM_OVERLAY_VALID] = float(
					entity.get("aim_overlay_valid", 0))
			var body: Vector3 = entity.get("aim_body", Vector3.ZERO)
			out[base + NovaSimulation.PF_AIM_BODY_PITCH_DEG] = body.x
			out[base + NovaSimulation.PF_AIM_BODY_YAW_DEG] = body.y
			out[base + NovaSimulation.PF_AIM_BODY_ROLL_DEG] = body.z
			out[base + NovaSimulation.PF_EMPLACED_CONTROLS_VALID] = float(
					entity.get("emplaced_controls_valid", 0))
			out[base + NovaSimulation.PF_EWEAP_GUNYAW] = float(
					entity.get("emplaced_gun_yaw", 0))
			out[base + NovaSimulation.PF_EWEAP_GUNPITCH] = float(
					entity.get("emplaced_gun_pitch", 0))
			out[base + NovaSimulation.PF_TEX_TEAM_VALID] = float(
					entity.get("tex_team_valid", 0))
			out[base + NovaSimulation.PF_TEX_TEAM] = float(
					entity.get("tex_team", 0))
			out[base + NovaSimulation.PF_ZONE_CTRL_VALID] = float(
					entity.get("zone_ctrl_valid", 0))
			out[base + NovaSimulation.PF_TEAMSWING] = float(
					entity.get("team_swing", 0))
			out[base + NovaSimulation.PF_LFP_CAMPPERCENT_VALID] = float(
					entity.get("lfp_camp_percent_valid", 0))
			out[base + NovaSimulation.PF_LFP_CAMPPERCENT] = float(
					entity.get("lfp_camp_percent", 0))
			out[base + NovaSimulation.PF_WORLD_HEAT_GLOW_VALID] = float(
					entity.get("world_heat_glow_valid", 0))
			out[base + NovaSimulation.PF_WORLD_HEAT_GLOW] = float(
					entity.get("world_heat_glow", 0))
			out[base + NovaSimulation.PF_RIGHT_HAND_COLLAPSED] = float(
					entity.get("right_hand_collapsed", 0))
			out[base + NovaSimulation.PF_HELD_WEAPON_ADM] = float(
					entity.get("held_weapon_adm", 0))
			out[base + NovaSimulation.PF_WPN_ANIM_STATE] = float(
					entity.get("wpn_anim_state", -1))
			out[base + NovaSimulation.PF_WPN_PHASE_TICKS] = float(
					entity.get("wpn_phase_ticks", -1))
			var angles: PackedVector3Array = entity.get(
					"aim_angles", PackedVector3Array())
			for cls in range(mini(angles.size(), 9)):
				var ob := (base + NovaSimulation.PF_AIM_ANGLES
						+ cls * NovaSimulation.PF_AIM_CLASS_STRIDE)
				out[ob] = angles[cls].x
				out[ob + 1] = angles[cls].y
				out[ob + 2] = angles[cls].z
		return out


class RevisionFakeSim:
	extends FakeSim
	var layout_revision := 1
	func get_present_layout_revision() -> int:
		return layout_revision


class ClockedRevisionFakeSim:
	extends RevisionFakeSim
	var logic_tick := 100
	func get_logic_tick() -> int:
		return logic_tick


class FakePlacer:
	extends RefCounted
	var built: Array[Node3D] = []
	var graphic_builds: Array[String] = []

	func build_player_animated_model(_type_id: int, parent: Node3D,
			_env_node: Node = null) -> Node3D:
		var node := FakeModel.new()
		parent.add_child(node)
		built.append(node)
		return node

	# The held-weapon path: a plain rigid model built by graphic NAME, no adm/clip.
	func build_model_from_graphic(graphic: String, _adm_name: String, parent: Node3D,
			_clip_key: String = "", _env_node: Node = null,
			_rig_graphic: String = "") -> Node3D:
		var node := FakeModel.new()
		parent.add_child(node)
		graphic_builds.append(graphic)
		return node


class ResolvingFakePlacer:
	extends FakePlacer
	func resolve_player_visual_item_id(type_id: int) -> int:
		return type_id + 100000 if type_id < 100000 else type_id


class SelectiveFakePlacer:
	extends FakePlacer
	var failed_types: Dictionary = {}
	var attempts: Array[int] = []
	func build_player_animated_model(type_id: int, parent: Node3D,
			_env_node: Node = null) -> Node3D:
		attempts.append(type_id)
		if bool(failed_types.get(type_id, false)):
			return null
		return super.build_player_animated_model(type_id, parent, _env_node)


class EmptyIndex:
	extends RefCounted
	func resolve(_bms_id: int, _kind: int, _index: int):
		return null


class CountingDeferIndex:
	extends RefCounted
	var by_bms_id: Dictionary = {}
	var resolve_calls := 0
	var generation := 1
	func resolve(bms_id: int, _kind: int, _index: int):
		resolve_calls += 1
		return by_bms_id.get(bms_id)
	func get_generation() -> int:
		return generation


class SpawnObserver:
	extends RefCounted
	var calls: Array = []

	func on_spawned(node: Node3D, kind: int, item_id: int) -> void:
		calls.append({
			"node": node,
			"kind": kind,
			"item_id": item_id,
			"position": node.position,
		})


func test_present_snapshot_rejects_a_legacy_short_stride() -> void:
	var sim := FakeSim.new()
	sim.local_player_present = false
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, EmptyIndex.new())
	var legacy_stride := NovaSimulation.PF_STRIDE - 1
	var snapshot := PackedFloat32Array()
	snapshot.resize(legacy_stride)

	presenter.present_snapshot(snapshot, legacy_stride)

	assert_true(placer.built.is_empty(),
			"a row that predates the blend tuple cannot be cross-read")


func test_sp_synthetic_filter_materializes_only_attachment_origin_rows() -> void:
	var sim := FakeSim.new()
	sim.entities = [
		{
			"type_id": 164,
			"handle": 0x1004,
			"kind": 1,
			"index": 0,
			"bms_id": 11,
		},
		{
			"type_id": 166,
			"handle": 0x1005,
			"kind": 255,
			"index": 0xFFFFFF,
			"bms_id": 0,
		},
	]
	var placer := ResolvingFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, EmptyIndex.new(), {
		"synthetic_origin_only": true,
	})
	presenter.present()
	assert_eq(placer.built.size(), 1,
			"ordinary SP rows stay with MissionPresentPass; synthetic children materialize")
	var ref: Dictionary = placer.built[0].get_meta("entity_ref", {})
	assert_eq(int(ref.get("item_id", 0)), 100166)
	assert_eq(int(ref.get("runtime_type_id", 0)), 166)
	assert_eq(int(ref.get("origin_kind", 0)), 255)


func test_wire_handle_resolver_keeps_synthetic_siblings_distinct() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		'type_id': 166,
		'handle': 0x1004,
		'kind': 255,
		'index': 0xffffff,
	}, {
		'type_id': 166,
		'handle': 0x1005,
		'kind': 255,
		'index': 0xffffff,
	}]
	var placer := ResolvingFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, EmptyIndex.new(), {
		'synthetic_origin_only': true,
	})

	presenter.present()

	assert_eq(presenter.resolve_wire_handle(0x1004), placer.built[0])
	assert_eq(presenter.resolve_wire_handle(0x1005), placer.built[1],
			'each attachment sibling resolves to its own live node')
	sim.entities = []
	presenter.present()
	assert_null(presenter.resolve_wire_handle(0x1004),
			'a retired wire row no longer resolves through its reused pool slot')


func test_zero_wire_handle_is_a_valid_remote_pool_slot() -> void:
	var sim := FakeSim.new()
	sim.local_player_present = false
	sim.entities = [{"type_id": 0x14B9, "handle": 0, "x": 3.0}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)

	presenter.present()

	assert_eq(placer.built.size(), 1,
			"packed handle zero is a real remote pool-0 slot")
	assert_eq(presenter.resolve_wire_handle(0), placer.built[0])
	assert_almost_eq(placer.built[0].position.x, 3.0, 0.001)


func test_zero_wire_handle_is_filtered_when_it_is_the_explicit_local_player() -> void:
	var sim := FakeSim.new()
	sim.local_player_handle = 0
	sim.entities = [{"type_id": 0x14B9, "handle": 0}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)

	presenter.present()

	assert_eq(placer.built.size(), 0,
			"packed handle zero is hidden only when explicit local-player validity says it is self")


func test_unresolved_slot_retries_after_disappearance_and_reuse() -> void:
	var sim := FakeSim.new()
	sim.entities = [{"type_id": 166, "handle": 0x1004}]
	var placer := SelectiveFakePlacer.new()
	placer.failed_types[166] = true
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	assert_eq(placer.attempts, [166])
	assert_eq(presenter.entity_count(), 0)

	sim.entities = []
	presenter.present()
	placer.failed_types[166] = false
	sim.entities = [{"type_id": 166, "handle": 0x1004}]
	presenter.present()
	assert_eq(placer.attempts, [166, 166],
			"retired failure cache cannot poison slot reuse")
	assert_eq(presenter.entity_count(), 1)


func test_unresolved_slot_retries_immediately_when_type_changes() -> void:
	var sim := FakeSim.new()
	sim.entities = [{"type_id": 166, "handle": 0x1004}]
	var placer := SelectiveFakePlacer.new()
	placer.failed_types[166] = true
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()

	sim.entities[0]["type_id"] = 167
	presenter.present()
	assert_eq(placer.attempts, [166, 167])
	assert_eq(presenter.entity_count(), 1)


func test_live_slot_type_change_rebuilds_the_visual() -> void:
	var sim := FakeSim.new()
	sim.entities = [{"type_id": 166, "handle": 0x1004}]
	var placer := SelectiveFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var first := presenter.resolve_wire_handle(0x1004)

	sim.entities[0]["type_id"] = 167
	presenter.present()
	var second := presenter.resolve_wire_handle(0x1004)
	assert_eq(placer.attempts, [166, 167])
	assert_ne(second, first, "recycled handle cannot keep the prior type model")
	assert_eq(int(second.get_meta("entity_ref", {}).get("runtime_type_id", 0)), 167)


func test_stable_host_layout_skips_repeat_defer_resolution() -> void:
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"type_id": 166,
		"handle": 0x1004,
		"bms_id": 11,
		"kind": 1,
		"index": 0,
	}]
	var placed := Node3D.new()
	add_child_autofree(placed)
	var index := CountingDeferIndex.new()
	index.by_bms_id = { 11: placed }
	var placer := SelectiveFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, index)
	presenter.present()
	assert_eq(index.resolve_calls, 1)
	assert_true(placer.attempts.is_empty(),
			"the placed row remains owned by MissionPresentPass")

	sim.entities[0]["x"] = 9.0
	presenter.present()
	assert_eq(index.resolve_calls, 1,
			"stable topology with no wire nodes takes the empty fast path")

	sim.entities[0] = {
		"type_id": 167,
		"handle": 0x1005,
		"bms_id": 12,
		"kind": -1,
		"index": -1,
	}
	sim.layout_revision += 1
	presenter.present()
	assert_eq(index.resolve_calls, 1,
			"identity-less wire rows never consult the defer index")
	assert_eq(placer.attempts, [167],
			"same-size placed-to-wire replacement rebuilds classification")


func test_placed_identity_rows_defer_even_without_a_resolvable_node() -> void:
	# A batched static (MultiMesh instance) deliberately has NO per-entity node,
	# so the registry resolves null — yet the row carries its placed .bms
	# identity, and the placed representation owns the render. The wire pass must
	# not spawn a duplicate (the joiner's pre-convergence double-render). Rows
	# without placed identity (the joiner's players/streamed AI) still spawn.
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 164,
		"handle": 0x1004,
		"bms_id": 11,
		"kind": 1,
		"index": 3,
	}, {
		"type_id": 166,
		"handle": 0x0010,
		"bms_id": 0,
		"kind": -1,
		"index": -1,
	}]
	var placer := ResolvingFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, EmptyIndex.new())
	presenter.present()
	assert_eq(placer.built.size(), 1,
			"the batched-static row defers; only the identity-less row materializes")
	assert_null(presenter.resolve_wire_handle(0x1004),
			"no wire duplicate exists for the placed identity")
	assert_not_null(presenter.resolve_wire_handle(0x0010))


func test_wire_plan_survives_reorder_then_prunes_and_rebuilds_reused_type() -> void:
	var sim := RevisionFakeSim.new()
	sim.entities = [
		{"type_id": 166, "handle": 0x1004, "x": 4.0},
		{"type_id": 167, "handle": 0x1005, "x": 5.0},
	]
	var placer := SelectiveFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var first := presenter.resolve_wire_handle(0x1004)
	var second := presenter.resolve_wire_handle(0x1005)

	sim.entities = [
		{"type_id": 167, "handle": 0x1005, "x": 50.0},
		{"type_id": 166, "handle": 0x1004, "x": 40.0},
	]
	sim.layout_revision += 1
	presenter.present()
	assert_eq(presenter.resolve_wire_handle(0x1004), first)
	assert_eq(presenter.resolve_wire_handle(0x1005), second)
	assert_almost_eq(first.position.x, 40.0, 0.001)
	assert_almost_eq(second.position.x, 50.0, 0.001)

	sim.entities = [
		{"type_id": 167, "handle": 0x1005, "x": 51.0},
	]
	sim.layout_revision += 1
	presenter.present()
	assert_null(presenter.resolve_wire_handle(0x1004),
			"despawn prunes the retired row plan and visual")
	assert_eq(presenter.entity_count(), 1)

	sim.entities[0] = {"type_id": 168, "handle": 0x1005, "x": 60.0}
	sim.layout_revision += 1
	presenter.present()
	var replacement := presenter.resolve_wire_handle(0x1005)
	assert_ne(replacement, second,
			"a recycled handle with a new type cannot retain the old visual")
	assert_almost_eq(replacement.position.x, 60.0, 0.001)
	assert_eq(placer.attempts, [166, 167, 168])


func test_runtime_reset_rematerializes_the_restored_same_type_slot() -> void:
	var sim := FakeSim.new()
	sim.entities = [{"type_id": 166, "handle": 0x1004}]
	var placer := SelectiveFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var first := presenter.resolve_wire_handle(0x1004)

	presenter.reset_runtime_state()
	assert_eq(presenter.entity_count(), 0)
	assert_null(presenter.resolve_wire_handle(0x1004))
	presenter.present()
	var restored := presenter.resolve_wire_handle(0x1004)
	assert_eq(placer.attempts, [166, 166])
	assert_ne(restored, first,
			"restart builds a fresh restored-incarnation visual")


func test_synthetic_attachment_uses_panm_and_hidden_visibility_contract() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 166,
		"handle": 0x1005,
		"kind": 255,
		"index": 0xFFFFFF,
		"alive": 0,
		"active1": 1,
		"phase1": 0x2345,
		"active2": 1,
		"phase2": 0x6789,
	}]
	var placer := ResolvingFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, EmptyIndex.new(), {
		"synthetic_origin_only": true,
	})
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.part_calls, [[1, 0x2345], [2, 0x6789]],
			"attached items consume the same two PANM channels as placed items")
	sim.entities[0]["active1"] = 0
	sim.entities[0]["phase2"] = 0
	presenter.present()
	assert_false(model.ctrl_values.has("VEHICLE_SPECIAL1"),
			"wire presentation releases a no-longer-owned SPECIAL1 value")
	assert_eq(model.ctrl_values.get("VEHICLE_SPECIAL2"), 0,
			"wire presentation submits an owned zero endpoint")
	assert_has(model.cleared_part_channels, 1)
	assert_true(model.visible,
			"a dead attached item retains its graphic or husk until explicitly hidden")
	sim.entities[0]["hidden"] = 1
	presenter.present()
	assert_false(model.visible, "PF_HIDDEN ends attached-item presentation")


func test_wire_model_spawn_registers_after_identity_and_transform_are_ready() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		# A real joiner's ClientState has no authoritative BMS origin, so PF_KIND
		# is -1. The callback must derive pool 1 -> KIND_ITEM from the wire handle.
		"kind": -1,
		"index": 9,
		"bms_id": 77,
		"x": 4.0,
		"y": 5.0,
		"z": 6.0,
		"yaw": 90.0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var observer := SpawnObserver.new()
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.set_node_spawned_callback(Callable(observer, "on_spawned"))
	presenter.present()

	assert_eq(observer.calls.size(), 1, "the new wire model is registered exactly once")
	var call: Dictionary = observer.calls[0]
	assert_eq(int(call.kind), NovaMissionData.KIND_ITEM)
	assert_eq(int(call.item_id), 4567)
	assert_eq(call.position, Vector3(4, 5, 6),
			"registration runs after the production transform is applied")
	var node := call.node as Node3D
	var ref: Dictionary = node.get_meta("entity_ref", {})
	assert_eq(int(ref.get("wire_handle", 0)), 0x1004)
	assert_eq(int(ref.get("item_id", 0)), 4567)
	assert_eq(int(ref.get("origin_kind", 0)), -1)

	presenter.present()
	assert_eq(observer.calls.size(), 1, "steady presentation never re-registers the model")
	var late := SpawnObserver.new()
	presenter.set_node_spawned_callback(Callable(late, "on_spawned"))
	assert_eq(late.calls.size(), 1, "late consumers receive every already-live wire node")


func test_wire_model_applies_the_same_packed_overlay_result() -> void:
	var angles := PackedVector3Array()
	for i in range(9):
		angles.append(Vector3(-4.0 + i, 70.0 + i, 1.0 + i))
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"anim_state": 67,
		"anim_phase": 11,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(6.0, 33.0, -2.0),
		"aim_angles": angles,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.remote_body_calls, [[67, "anim_emplaced",
			NovaSimulation.infantry_anim_flags(67), 11]],
			"presentation forwards state, retail flags, and player phase")
	assert_eq(model.pose_call_order, ["right_hand", "overlay", "body"],
			"the current packed overlay is installed before the body clip evaluates")
	assert_eq(model.overlay_calls.size(), 1)
	var deltas: Array = model.overlay_calls[0]
	assert_eq(deltas.size(), 9)
	var body_basis := MissionObjectPlacer.bms_to_godot_basis(
			Vector3(6.0, 33.0, -2.0))
	assert_true(model.basis.is_equal_approx(body_basis))
	assert_true((deltas[8] as Basis).is_equal_approx(
			body_basis.inverse() * MissionObjectPlacer.bms_to_godot_basis(angles[8])))

	# The model owns transition acceptance and completion. A repeated semantic
	# state preserves its free-running playhead even for a revisionless source;
	# only the next state edge is dispatched.
	sim.entities[0]["anim_phase"] = 22
	presenter.present()
	sim.entities[0]["anim_state"] = 68
	sim.entities[0]["anim_phase"] = 6
	presenter.present()
	assert_eq(model.remote_body_calls, [
		[67, "anim_emplaced", NovaSimulation.infantry_anim_flags(67), 11],
		[68, "anim_emplaced_2", NovaSimulation.infantry_anim_flags(68), 6],
	], "state edges reach the completion-aware remote animation channel")


func test_wire_model_receives_the_transition_pulse_before_the_current_state() -> void:
	# A tapped prone roll rides the wire as 41/42 for a single 0x0A sample (the
	# emitted byte is `pending ?: current`), and several datagrams fold per
	# render frame, so the sim surfaces the buried transition as
	# PF_ANIM_STATE_PULSE. Presentation dispatches it FIRST — retail applies the
	# anim byte per record [orig: @0x4c1153] — so the locked roll clip accepts
	# and the follow-up state queues behind it at the model. Revisioned sim: the
	# second leg pins the edge gate staying closed once the pulse is drained.
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"anim_state": 48,
		"anim_phase": 60,
		"anim_pulse": 41,
		"anim_pulse_ticks": 6,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.remote_body_calls, [
		[41, "anim_roll_left", NovaSimulation.infantry_anim_flags(41), 6],
		[48, "anim_idle_prone", NovaSimulation.infantry_anim_flags(48), 60],
	], "the buried pulse dispatches before the current state, carrying its own phase")

	# A steady frame with no pulse and an unchanged state stays on the edge-gated
	# fast path: no re-dispatch.
	sim.entities[0]["anim_pulse"] = -1
	sim.entities[0]["anim_pulse_ticks"] = -1
	presenter.present()
	assert_eq(model.remote_body_calls.size(), 2,
			"pulse-free same-state frames keep the edge-gate skip")


func test_wire_model_free_runs_compact_infantry_when_phase_is_absent() -> void:
	# Production infantry compacts carry the state byte but no player-channel
	# phase byte. Re-presenting that snapshot must preserve local playback
	# instead of externally pinning the selected clip to tick zero.
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x0004,
		"anim_state": 67,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	presenter.present()

	var model := placer.built[0] as FakeModel
	assert_eq(model.remote_body_calls, [
		[67, "anim_emplaced", NovaSimulation.infantry_anim_flags(67), -1],
	], "phase-less compact infantry is accepted once then free-runs locally")


func test_host_current_body_state_is_posed_without_remote_rearbitration() -> void:
	# Host-loopback snapshots carry AiEntity's already-accepted current state and
	# phase, not a compact pending request. It must retain the direct pose path.
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x0004,
		"anim_state": 67,
		"anim_phase": 11,
		"anim_remote_request": 0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()

	var model := placer.built[0] as FakeModel
	assert_eq(model.remote_body_calls, [],
			"host current state is not submitted to the receive-side request channel")
	assert_eq(model.body_calls, [["at", "anim_emplaced", 11]],
			"host current state keeps the authoritative direct-phase pose path")


func test_host_current_body_state_uses_authoritative_blend_tuple() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x0004,
		"anim_source_state": 43,
		"anim_source_phase": 18,
		"anim_state": 1,
		"anim_phase": 4,
		"anim_blend_weight": 0.3,
		"anim_remote_request": 0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()

	var model := placer.built[0] as FakeModel
	assert_eq(model.remote_body_calls, [])
	assert_eq(model.body_calls[0].slice(0, 5), [
		"blend", "anim_idle", 18, "anim_walk_forward", 4])
	assert_almost_eq(float(model.body_calls[0][5]), 0.3, 0.000001,
			"host-loopback consumes authority rather than reconstructing a blend")


func test_remote_blend_advances_on_steady_fixed_tick_without_redispatch() -> void:
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x0004,
		"anim_state": 1,
		"anim_phase": 0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	model.remote_apply_results = [true]
	# Re-submit once to arm the transition latch, since the first spawn call
	# above used the fake's default false result.
	sim.entities[0]["anim_state"] = 2
	presenter.present()
	model.remote_tick_results = [false]
	sim.entities[0]["anim_state"] = 2
	presenter.present()
	presenter.present()

	assert_eq(model.remote_tick_calls, [2],
			"the receive-side fixed-tick seam runs only while its row latch is live")
	assert_eq(model.remote_body_calls.size(), 2,
			"an active blend advances locally without re-submitting the same request")


func test_revisionless_source_keeps_remote_blend_latch_across_cold_plans() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x0004,
		"anim_state": 1,
		"anim_phase": 0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel

	model.remote_apply_results = [true, true]
	sim.entities[0]["anim_state"] = 2
	presenter.present()
	model.remote_tick_results = [false]
	presenter.present()

	assert_eq(model.remote_tick_calls, [2],
			"a revisionless cold row plan restores its live transition latch")
	assert_eq(model.remote_body_calls.size(), 2,
			"the retained latch advances instead of re-submitting the same state")


func test_remote_blend_uses_logic_tick_delta_not_present_call_count() -> void:
	var sim := ClockedRevisionFakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x0004,
		"anim_state": 1,
		"anim_phase": 0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel

	# The state arrives after one logic tick and is staged at target weight zero.
	model.remote_apply_results = [true]
	sim.logic_tick = 101
	sim.entities[0]["anim_state"] = 2
	presenter.present()
	model.remote_tick_results = [true, true, true, true, true]

	# Render-only presents at the same fixed tick must not accelerate the blend.
	presenter.present()
	presenter.present()
	assert_eq(model.remote_tick_calls, [],
			"duplicate presents at one logic tick do not advance a fixed-tick blend")

	# A catch-up frame advances every omitted fixed tick, not just one render call.
	sim.logic_tick = 104
	presenter.present()
	assert_eq(model.remote_tick_calls, [2, 2, 2],
			"a three-tick catch-up advances both channels and weight three times")
	presenter.present()
	assert_eq(model.remote_tick_calls, [2, 2, 2],
			"a repeated presentation of the catch-up result is idempotent")


func test_wire_model_resets_remote_body_channel_on_respawn_revision_change() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x0004,
		"anim_state": 67,
		"anim_phase": 11,
		"respawn_revision": 0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.reset_remote_body_calls, 0,
			"the first lifecycle sample initializes rather than resets a new model")

	# A revision jump represents one or more dead->alive edges folded before this
	# render. Reset must precede the animation request so an identical state ID is
	# accepted into a fresh remote body-channel epoch.
	model.pose_call_order.clear()
	sim.entities[0]["respawn_revision"] = 2
	sim.entities[0]["anim_phase"] = 6
	presenter.present()
	assert_eq(model.reset_remote_body_calls, 1)
	assert_eq(model.pose_call_order, ["right_hand", "overlay", "reset", "body"],
			"respawn reset runs before the compact animation is applied")
	assert_eq(model.remote_body_calls[-1], [67, "anim_emplaced",
			NovaSimulation.infantry_anim_flags(67), 6])

	model.pose_call_order.clear()
	presenter.present()
	assert_eq(model.reset_remote_body_calls, 1,
			"a steady revision does not repeatedly reset local clip playback")
	assert_eq(model.pose_call_order, ["right_hand", "overlay"])


func test_wire_model_applies_and_restores_mounted_right_hand_collapse() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"aim_overlay_valid": 1,
		"right_hand_collapsed": 1,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	sim.entities[0]["right_hand_collapsed"] = 0
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.right_hand_collapse_calls, [true, false],
			"the wire pose consumes the same mount verdict and restores on dismount")


func test_wire_model_applies_and_clears_named_emplaced_controls() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x2000,
		"emplaced_gun_pitch": 0xE000,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.ctrl_values, {
		"EWEAP_GUNYAW": 0x2000,
		"EWEAP_GUNPITCH": 0xE000,
	})

	sim.entities[0]["emplaced_controls_valid"] = 0
	presenter.present()
	assert_true(model.ctrl_values.is_empty())
	assert_has(model.cleared_controls, "EWEAP_GUNYAW")
	assert_has(model.cleared_controls, "EWEAP_GUNPITCH")


func test_wire_direct_carrier_applies_and_releases_scoped_world_heat() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"world_heat_glow_valid": 1,
		"world_heat_glow": 0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.ctrl_values.get("HEAT_GLOW", -1), 0,
			"the live UseGun carrier scope owns retail's cold zero")

	sim.entities[0]["world_heat_glow"] = 0xFFFF
	presenter.present()
	assert_eq(model.ctrl_values.get("HEAT_GLOW", -1), 0xFFFF,
			"wire-direct carrier presentation keeps the world unsigned-word cap")

	sim.entities[0]["world_heat_glow_valid"] = 0
	presenter.present()
	assert_false(model.ctrl_values.has("HEAT_GLOW"),
			"an unscoped/joiner row releases rather than synthesizes heat")
	assert_has(model.cleared_controls, "HEAT_GLOW")


func test_wire_direct_numbered_zone_applies_and_releases_callback_controls() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"tex_team_valid": 1,
		"tex_team": 2,
		"zone_ctrl_valid": 1,
		"team_swing": 0x10000,
		"lfp_camp_percent_valid": 1,
		"lfp_camp_percent": 0x4000,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.ctrl_values, {
		"TEX_TEAM": 2,
		"TEAMSWING": 0x10000,
		"LFP_CAMPPERCENT": 0x4000,
	}, "an unresolved pool-1 zone still runs the generic-world CTRL callback")

	sim.entities[0]["lfp_camp_percent_valid"] = 0
	presenter.present()
	assert_false(model.ctrl_values.has("LFP_CAMPPERCENT"),
			"missing timer entry is an omitted write, not a fabricated zero")
	assert_true(model.ctrl_values.has("TEAMSWING"))
	sim.entities[0]["tex_team_valid"] = 0
	sim.entities[0]["zone_ctrl_valid"] = 0
	presenter.present()
	assert_true(model.ctrl_values.is_empty())
	assert_has(model.cleared_controls, "TEX_TEAM")
	assert_has(model.cleared_controls, "TEAMSWING")


func test_wire_model_honors_local_first_person_parent_cull() -> void:
	# Host-side dynamic/unplaced mount targets are owned by this pass rather than
	# MissionPresentPass. They consume the same transient retail render verdict.
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"alive": 1,
		"local_view_suppressed": 1,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, EmptyIndex.new())
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_false(model.visible,
			"the dynamic local UseGun parent skips its own world model")

	sim.entities[0]["local_view_suppressed"] = 0
	presenter.present()
	assert_true(model.visible, "clearing the transient verdict restores the parent")
	sim.entities[0]["alive"] = 0
	presenter.present()
	assert_true(model.visible,
			"a dead non-hidden organic remains visible as a corpse")
	sim.entities[0]["hidden"] = 1
	presenter.present()
	assert_false(model.visible,
			"the authoritative compact hidden bit suppresses the world model")


func test_wire_model_clears_overlay_when_snapshot_selector_is_invalid() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"aim_overlay_valid": 0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.overlay_calls, [[]],
			"a remote model cannot retain an overlay after selector invalidation")


# A remote player's upper-body weapon pose. The sim derives the state (nothing about the
# weapon channel crosses the wire — every observer re-derives it from the peer's equipped
# ADM index and Flags bit 0x10), and the wire pass drives it onto that peer's model. A -1
# state means "no channel this frame" and must CLEAR the pose, or a peer keeps the hold it
# had before it switched weapons.
# [orig: the selection Entity_UpdateInfantryPlayerBody @0x4b5dad, which retail runs for
#  every player body it draws rather than only the local one]
func test_wire_model_applies_and_clears_the_remote_weapon_channel() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"aim_overlay_valid": 1,
		"wpn_anim_state": 51,   # pistol hold
		"wpn_phase_ticks": -1,  # the secondary playhead is not replicated
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.weapon_channel_calls.size(), 1,
			"the remote row drove the weapon channel")
	if model.weapon_channel_calls.size() == 1:
		assert_eq(model.weapon_channel_calls[0][0],
				NovaSimulation.infantry_anim_key(51),
				"the state id resolves to the pistol hold clip key")
		assert_eq(model.weapon_channel_calls[0][1], -1)

	# The peer stows its weapon: the channel goes away and the pose must clear.
	sim.entities[0]["wpn_anim_state"] = -1
	presenter.present()
	assert_eq(model.weapon_channel_calls[-1], ["", -1],
			"a -1 state clears the hold pose rather than leaving the last one posed")


# A remote player's HELD WEAPON. The sim folds the draw gate into the ADM field — a hidden
# or unarmed body reports 0, which is both our weapon table's null row and the original's
# own `if (entity->equippedAdmIndex)` precondition — so a zero row must build no model at
# all, and a nonzero one must resolve its gfx3 through the ADM-indexed table.
# [orig: BoneCallback_org0_World draw 5, precondition @0x4e3c97; gate
#  Entity_CanFireWeapon @0x4dcb10]
func test_wire_row_builds_a_held_weapon_only_when_it_is_armed() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"aim_overlay_valid": 1,
		"held_weapon_adm": 0,   # unarmed / hidden by the gate
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	assert_eq(placer.graphic_builds.size(), 0,
			"an ADM of 0 draws nothing — no model is built at all")
	var body := presenter.resolve_wire_handle(0x1004)
	var skeleton := Skeleton3D.new()
	for bone_index in range(PresentHeldWeapon.BONE_INDEX + 1):
		skeleton.add_bone("Bone%d" % bone_index)
	body.add_child(skeleton)

	# Now the peer is holding something the table can resolve.
	sim.entities[0]["held_weapon_adm"] = 16
	presenter.present()
	assert_eq(placer.graphic_builds, ["WPN16_3rd"],
			"the model is resolved from the ADM index the wire carries")
	var weapon := presenter.get("_weapon_nodes").get(0x1004) as Node3D
	assert_not_null(weapon)
	assert_true(weapon.visible)
	sim.entities[0]["hidden"] = 1
	presenter.present()
	assert_false(weapon.visible,
			"the weapon consumes this snapshot's body visibility without a one-tick lag")
	sim.entities[0]["hidden"] = 0
	presenter.present()
	assert_true(weapon.visible,
			"the caster and color model return on the same visible snapshot")

	# Stowing it again retires the node rather than leaving a gun floating.
	sim.entities[0]["held_weapon_adm"] = 0
	presenter.present()
	assert_eq(placer.graphic_builds.size(), 1,
			"going unarmed frees the weapon rather than rebuilding one")


# --- Native held-weapon parity (the PR that moves the walk native) ---------------

# The native applier carries its own port of the hand-frame calibration (the
# godot-cpp Basis(axis, angle) parity gotcha): pin the two implementations
# together across representative bone bases — including negative-component
# columns — so they can never drift.
func test_native_hand_frame_basis_matches_the_gdscript_origin() -> void:
	var bases: Array[Basis] = [
		Basis.IDENTITY,
		Basis(Vector3(0, 1, 0), 0.7) * Basis(Vector3(1, 0, 0), -0.4),
		Basis(Vector3(-1, 0, 0).normalized(), 1.2),
		Basis(Vector3(0.5, -0.5, 0.70710678).normalized(), 2.1),
		Basis(Vector3(-0.57735, -0.57735, -0.57735).normalized(), -2.8),
		Basis(Vector3(0, 0, -1), PI / 2.0) * Basis(Vector3(0, -1, 0), 0.3),
	]
	for b in bases:
		var expected := PresentHeldWeapon.hand_frame_basis(b)
		var got: Basis = NovaPresentApplier.held_weapon_hand_frame_basis(b)
		assert_true(got.is_equal_approx(expected),
				"hand-frame parity at %s: native %s vs gd %s" % [b, got, expected])


# Full attach-transform parity against a really posed Skeleton3D, both frames,
# across an entity-angle sweep — pins the joint/rest-inverse/nudge math.
func test_native_held_weapon_attach_matches_the_gdscript_origin() -> void:
	var body := Node3D.new()
	add_child_autofree(body)
	body.global_transform = Transform3D(
			Basis(Vector3(0, 1, 0), 0.9), Vector3(4.0, 1.5, -7.0))
	var skeleton := Skeleton3D.new()
	body.add_child(skeleton)
	skeleton.position = Vector3(0.1, 0.9, 0.0)
	for bone_index in range(PresentHeldWeapon.BONE_INDEX + 1):
		skeleton.add_bone("Bone%d" % bone_index)
		if bone_index > 0:
			skeleton.set_bone_parent(bone_index, bone_index - 1)
		# A non-trivial rest chain: the rest-inverse term must matter.
		skeleton.set_bone_rest(bone_index, Transform3D(
				Basis(Vector3(0, 0, 1), 0.11 * bone_index),
				Vector3(0.05 * bone_index, 0.1, 0.02)))
	skeleton.reset_bone_poses()
	# Pose the hand chain away from rest so pose != rest.
	skeleton.set_bone_pose_rotation(10,
			Quaternion(Vector3(1, 0, 0).normalized(), 0.6))
	skeleton.set_bone_pose_rotation(PresentHeldWeapon.BONE_INDEX,
			Quaternion(Vector3(0.3, -0.8, 0.52).normalized(), -1.1))
	for angles: Vector3 in [Vector3.ZERO, Vector3(15, -120, 40), Vector3(-80, 270, -30)]:
		for hand_frame in [false, true]:
			var expected: Variant = PresentHeldWeapon.attach_transform(
					body, angles, hand_frame)
			var got: Variant = NovaPresentApplier.held_weapon_attach_transform(
					skeleton, angles, hand_frame)
			assert_not_null(expected, "the reference places a transform")
			assert_true((got as Transform3D).is_equal_approx(expected as Transform3D),
					"attach parity (hand=%s, %s): native %s vs gd %s" % [
							hand_frame, angles, got, expected])
	# The cannot-place leg: too few bones -> null from both.
	var short_skel := Skeleton3D.new()
	add_child_autofree(short_skel)
	short_skel.add_bone("only")
	assert_null(NovaPresentApplier.held_weapon_attach_transform(
			short_skel, Vector3.ZERO, false))
