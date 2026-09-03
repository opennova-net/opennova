extends GutTest

# The EntityPresenter's WIRE walk (the joiner/wire present pass), end to end on
# REAL components: a real MissionObjectPlacer over a flat fixture root
# (assembled in before_all from committed fixtures), real ObjectModel wire
# avatars (their CTRL store, body clips, and Node3D state are the observables),
# a real EntityIndex defer gate, and real Simulation instances (an empty one
# for the static clock, a minimal-mission boot for logic-tick stepping).
# Snapshots are built as pure data and fed through the public
# present_wire_snapshot API.


# Fixture items.def wire-test ids (graphic -> committed model fixture).
const TYPE_PUMP := 6100      # -> item 106100, pump (static, PANM channels)
const TYPE_ARMORY := 6101    # -> item 106101, armory
const TYPE_RIFLEMAN := 6102  # -> item 106102, shed + soldier.adm (skeletal)
const TYPE_SCALED := 6103    # -> item 106103, pump at authored scale 1.5
const TYPE_UNRESOLVED := 555
const TYPE_UNRESOLVED_B := 666

static var _flat_dir := ""


func should_skip_script():
	if RetailData.def_root().is_empty():
		return RetailData.fixture_pending_text("def/weapon.def")
	return false


func before_all() -> void:
	# One flat resource root per run: ResourceRoot indexes flat filenames
	# only (and refuses user://), so committed fixtures are copied into the OS
	# temp dir. M9K_3rd.3di is the held-weapon gfx3 the fixture weapon.def's
	# first row names (adm index 1).
	_flat_dir = OS.get_temp_dir().replace("\\", "/") + "/opennova_wire_test_root"
	DirAccess.make_dir_recursive_absolute(_flat_dir)
	var copies := {
		"res://../fixtures/def/items.def": "items.def",
		"res://../fixtures/threedi/synth/pump.3di": "pump.3di",
		"res://../fixtures/threedi/synth/armory.3di": "armory.3di",
		"res://../fixtures/threedi/synth/shed.3di": "shed.3di",
		"res://../fixtures/threedi/synth/gun.3di": "M9K_3rd.3di",
		"res://../fixtures/anim/soldier.adm": "soldier.adm",
		"res://../fixtures/anim/idle.bad": "idle.bad",
		"res://../fixtures/anim/walk.bad": "walk.bad",
	}
	# The shipped weapon.def (its first row's gfx3 is the held-weapon witness)
	# and ammo.def come from the reference fixture set (should_skip_script).
	copies[RetailData.fixture("def/weapon.def")] = "weapon.def"
	copies[RetailData.fixture("def/ammo.def")] = "ammo.def"
	for src in copies.keys():
		var err := DirAccess.copy_absolute(
				ProjectSettings.globalize_path(src), _flat_dir + "/" + copies[src])
		assert(err == OK)


# Builds the wire PF-layout snapshot present_snapshot_from_client_replicas
# emits. Each entity is a Dictionary of overrides; pure data.
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
		var stride := Simulation.PF_STRIDE
		var out := PackedFloat32Array()
		out.resize(entities.size() * stride)
		for i in range(entities.size()):
			var entity: Dictionary = entities[i]
			var base := i * stride
			out[base + Simulation.PF_TYPE_ID] = float(entity.get("type_id", 0))
			out[base + Simulation.PF_WIRE_HANDLE] = float(entity.get("handle", 0))
			out[base + Simulation.PF_CHARACTER_ID] = float(
					entity.get("character_id", 0))
			out[base + Simulation.PF_KIND] = float(entity.get("kind", -1))
			out[base + Simulation.PF_INDEX] = float(entity.get("index", -1))
			out[base + Simulation.PF_BMS_ID] = float(entity.get("bms_id", 0))
			out[base + Simulation.PF_POS_X] = float(entity.get("x", 0.0))
			out[base + Simulation.PF_POS_Y] = float(entity.get("y", 0.0))
			out[base + Simulation.PF_POS_Z] = float(entity.get("z", 0.0))
			out[base + Simulation.PF_YAW_DEG] = float(entity.get("yaw", 0.0))
			out[base + Simulation.PF_PITCH_DEG] = float(entity.get("pitch", 0.0))
			out[base + Simulation.PF_ROLL_DEG] = float(entity.get("roll", 0.0))
			out[base + Simulation.PF_LOCAL_VIEW_SUPPRESSED] = float(
					entity.get("local_view_suppressed", 0))
			out[base + Simulation.PF_HIDDEN] = float(entity.get("hidden", 0))
			out[base + Simulation.PF_ALIVE] = float(entity.get("alive", 1))
			out[base + Simulation.PF_RESPAWN_REVISION] = float(
					entity.get("respawn_revision", 0))
			_write_phase(out, base, 1, int(entity.get("phase1", 0)),
					int(entity.get("active1", 0)) != 0)
			_write_phase(out, base, 2, int(entity.get("phase2", 0)),
					int(entity.get("active2", 0)) != 0)
			out[base + Simulation.PF_ANIM_STATE] = float(entity.get("anim_state", -1))
			out[base + Simulation.PF_ANIM_PHASE_TICKS] = float(
					entity.get("anim_phase", -1))
			out[base + Simulation.PF_ANIM_SOURCE_STATE] = float(
					entity.get("anim_source_state", -1))
			out[base + Simulation.PF_ANIM_SOURCE_PHASE_TICKS] = float(
					entity.get("anim_source_phase", -1))
			out[base + Simulation.PF_ANIM_BLEND_WEIGHT] = float(
					entity.get("anim_blend_weight", 1.0))
			out[base + Simulation.PF_ANIM_REMOTE_REQUEST] = float(
					entity.get("anim_remote_request", 1))
			out[base + Simulation.PF_ANIM_STATE_PULSE] = float(
					entity.get("anim_pulse", -1))
			out[base + Simulation.PF_ANIM_PULSE_TICKS] = float(
					entity.get("anim_pulse_ticks", -1))
			out[base + Simulation.PF_AIM_OVERLAY_VALID] = float(
					entity.get("aim_overlay_valid", 0))
			var body: Vector3 = entity.get("aim_body", Vector3.ZERO)
			out[base + Simulation.PF_AIM_BODY_PITCH_DEG] = body.x
			out[base + Simulation.PF_AIM_BODY_YAW_DEG] = body.y
			out[base + Simulation.PF_AIM_BODY_ROLL_DEG] = body.z
			out[base + Simulation.PF_EMPLACED_CONTROLS_VALID] = float(
					entity.get("emplaced_controls_valid", 0))
			out[base + Simulation.PF_EWEAP_GUNYAW] = float(
					entity.get("emplaced_gun_yaw", 0))
			out[base + Simulation.PF_EWEAP_GUNPITCH] = float(
					entity.get("emplaced_gun_pitch", 0))
			out[base + Simulation.PF_TEX_TEAM_VALID] = float(
					entity.get("tex_team_valid", 0))
			out[base + Simulation.PF_TEX_TEAM] = float(
					entity.get("tex_team", 0))
			out[base + Simulation.PF_ZONE_CTRL_VALID] = float(
					entity.get("zone_ctrl_valid", 0))
			out[base + Simulation.PF_TEAMSWING] = float(
					entity.get("team_swing", 0))
			out[base + Simulation.PF_LFP_CAMPPERCENT_VALID] = float(
					entity.get("lfp_camp_percent_valid", 0))
			out[base + Simulation.PF_LFP_CAMPPERCENT] = float(
					entity.get("lfp_camp_percent", 0))
			out[base + Simulation.PF_WORLD_HEAT_GLOW_VALID] = float(
					entity.get("world_heat_glow_valid", 0))
			out[base + Simulation.PF_WORLD_HEAT_GLOW] = float(
					entity.get("world_heat_glow", 0))
			out[base + Simulation.PF_RIGHT_HAND_COLLAPSED] = float(
					entity.get("right_hand_collapsed", 0))
			out[base + Simulation.PF_HELD_WEAPON_ADM] = float(
					entity.get("held_weapon_adm", 0))
			out[base + Simulation.PF_WPN_ANIM_STATE] = float(
					entity.get("wpn_anim_state", -1))
			out[base + Simulation.PF_WPN_PHASE_TICKS] = float(
					entity.get("wpn_phase_ticks", -1))
			out[base + Simulation.PF_WPN_SOURCE_STATE] = float(
					entity.get("wpn_source_state", -1))
			out[base + Simulation.PF_WPN_SOURCE_PHASE_TICKS] = float(
					entity.get("wpn_source_phase", -1))
			out[base + Simulation.PF_WPN_BLEND_WEIGHT] = float(
					entity.get("wpn_blend_weight", 1.0))
			out[base + Simulation.PF_WPN_VARIANT] = float(
					entity.get("wpn_variant", 0))
			out[base + Simulation.PF_WPN_SOURCE_VARIANT] = float(
					entity.get("wpn_source_variant", 0))
			var section_mask := int(entity.get("section_mask", 0)) & 0xFFFFFFFF
			out[base + Simulation.PF_SECTION_MASK_VALID] = float(
					entity.get("section_mask_valid", 0))
			out[base + Simulation.PF_SECTION_MASK_LO] = float(section_mask & 0xFFFF)
			out[base + Simulation.PF_SECTION_MASK_HI] = float(
					(section_mask >> 16) & 0xFFFF)
			var angles: PackedVector3Array = entity.get(
					"aim_angles", PackedVector3Array())
			for cls in range(mini(angles.size(), 9)):
				var ob := (base + Simulation.PF_AIM_ANGLES
						+ cls * Simulation.PF_AIM_CLASS_STRIDE)
				out[ob] = angles[cls].x
				out[ob + 1] = angles[cls].y
				out[ob + 2] = angles[cls].z
		return out


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


func _flat_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_flat_dir), OK, "the flat fixture root mounts")
	return root


func _placer() -> MissionObjectPlacer:
	var root := _flat_root()
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_flat_dir + "/items.def"), OK)
	return MissionObjectPlacer.create(root, item_db)


# The pass keeps only the sim's ObjectID; the RefCounted sim lives while the
# test holds it here (released after each case).
var _sims: Array[Simulation] = []


func after_each() -> void:
	_sims.clear()


func _sim() -> Simulation:
	var sim := Simulation.new()
	_sims.append(sim)
	return sim


# A booted minimal-mission sim: its logic tick steps under step(), which the
# facade's blend-tick delta consumption reads.
func _ticking_sim() -> Simulation:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../assets")), OK)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	var sim := Simulation.new()
	_sims.append(sim)
	assert_true(sim.load_from_mission_data(mission))
	return sim


func _container() -> Node3D:
	var container := Node3D.new()
	add_child_autofree(container)
	return container


# The per-entity lighting factors (x = effectScale, y = interior flag,
# z = interior daylight t) are one instance uniform stamped on the direct
# GeometryInstance3D children of every ROBJ part node and of the model's own
# Skeleton3D (skinned submeshes bind there instead of under a part).
func _entity_light_instances(model: ObjectModel) -> Array[GeometryInstance3D]:
	var parents: Array[Node] = []
	var parts: Dictionary = model.get_render_part_nodes()
	for key in parts.keys():
		parents.append(parts[key] as Node3D)
	if model.has_skeleton():
		parents.append(model.get_skeleton())
	var out: Array[GeometryInstance3D] = []
	for parent in parents:
		for child in parent.get_children():
			var instance := child as GeometryInstance3D
			if instance != null:
				out.append(instance)
	return out


func _assert_entity_light(model: ObjectModel, expected: Vector4,
		message: String) -> void:
	var checked := 0
	for instance in _entity_light_instances(model):
		var actual: Variant = instance.get_instance_shader_parameter("u_entity_light")
		assert_true(actual is Vector4 and (actual as Vector4).is_equal_approx(expected),
				"%s: %s carries %s, expected %s" % [
						message, instance.name, actual, expected])
		checked += 1
	assert_gt(checked, 0, message + " (at least one lit surface)")


func _wire_pass(sim: Simulation, placer: MissionObjectPlacer, container: Node3D,
		defer_index: EntityIndex = null, options: Dictionary = {}) -> EntityPresenter:
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup_wire(sim, placer, container, defer_index)
	presenter.set_synthetic_origin_only(
			bool(options.get("synthetic_origin_only", false)))
	if options.has("cold_spawn_budget"):
		presenter.set_cold_spawn_budget(int(options["cold_spawn_budget"]))
	if options.has("camera"):
		presenter.set_spectator_camera(options["camera"])
	return presenter


func _present(p: EntityPresenter, snap: Snapshot, revision: int = 1) -> void:
	p.present_wire_snapshot(snap.build(), Simulation.PF_STRIDE, revision)


func _ctrl(model: ObjectModel, name: String) -> int:
	return int(model.get_ctrl_values().get(name, -1))


func _index_with_placed(bms_id: int) -> Dictionary:
	# A real defer index whose registered placed node is a real model.
	var placed := ObjectModel.new()
	add_child_autofree(placed)
	placed.set_process(false)
	var index := EntityIndex.new()
	placed.entity_ref = EntityRef.make(1, 0, bms_id)
	index.build([placed], [])
	return { "index": index, "placed": placed }


func _empty_index() -> EntityIndex:
	var index := EntityIndex.new()
	index.build([], [])
	return index


func test_present_snapshot_rejects_a_short_stride() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container, _empty_index())
	var short_stride := Simulation.PF_STRIDE - 1
	var snapshot := PackedFloat32Array()
	snapshot.resize(short_stride)
	p.present_wire_snapshot(snapshot, short_stride, 1)
	assert_eq(container.get_child_count(), 0,
			"a row that predates the blend tuple cannot be cross-read")


func test_sp_synthetic_filter_materializes_only_attachment_origin_rows() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container, _empty_index(), {
		"synthetic_origin_only": true,
	})
	var snap := Snapshot.new()
	snap.entities = [
		{ "type_id": TYPE_PUMP, "handle": 0x1004, "kind": 1, "index": 0,
				"bms_id": 11 },
		{ "type_id": TYPE_RIFLEMAN, "handle": 0x1005, "kind": 255,
				"index": 0xFFFFFF, "bms_id": 0 },
	]
	_present(p, snap)
	assert_eq(p.wire_entity_count(), 1,
			"ordinary SP rows stay with MissionPresentPass; synthetic children materialize")
	var model: ObjectModel = p.resolve_wire_handle(0x1005)
	assert_not_null(model)
	var ref: EntityRef = model.entity_ref
	assert_eq(ref.item_id, 106102,
			"the wire type resolves through the ITEM_ID_OFFSET visual mapping")
	assert_eq(ref.runtime_type_id, TYPE_RIFLEMAN)
	assert_eq(ref.origin_kind, 255)


func test_wire_handle_resolver_keeps_synthetic_siblings_distinct() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container, _empty_index(), {
		"synthetic_origin_only": true,
	})
	var snap := Snapshot.new()
	snap.entities = [
		{ "type_id": TYPE_PUMP, "handle": 0x1004, "kind": 255, "index": 0xFFFFFF },
		{ "type_id": TYPE_PUMP, "handle": 0x1005, "kind": 255, "index": 0xFFFFFF },
	]
	_present(p, snap)
	var a: ObjectModel = p.resolve_wire_handle(0x1004)
	var b: ObjectModel = p.resolve_wire_handle(0x1005)
	assert_not_null(a)
	assert_not_null(b)
	assert_ne(a, b, "each attachment sibling resolves to its own live node")
	snap.entities = []
	_present(p, snap, 2)
	assert_null(p.resolve_wire_handle(0x1004),
			"a retired wire row no longer resolves through its reused pool slot")


func test_zero_wire_handle_is_a_valid_remote_pool_slot() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{ "type_id": TYPE_PUMP, "handle": 0, "x": 3.0 }]
	_present(p, snap)
	assert_eq(p.wire_entity_count(), 1, "packed handle zero is a real remote pool-0 slot")
	var model: ObjectModel = p.resolve_wire_handle(0)
	assert_not_null(model)
	assert_almost_eq(model.position.x, 3.0, 0.001)


func test_wire_pose_preserves_authored_model_scale() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_SCALED,
		"handle": 0x1004,
		"x": 7.0,
		"y": 2.0,
		"z": -3.0,
		"yaw": 37.0,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x1004)
	assert_not_null(model)
	assert_eq(model.get_entity_uniform_scale_q16(), 0x18000,
			"items.def scale reaches the typed ObjectModel state")
	var actual_scale := model.transform.basis.get_scale()
	assert_true(actual_scale.is_equal_approx(Vector3.ONE * 1.5),
			"wire presentation composes scale with the live pose")
	assert_true(model.position.is_equal_approx(Vector3(7.0, 2.0, -3.0)))
	# A subsequent transform write must retain scale; this is the overwrite bug
	# the old position/rotation-only presenter had.
	snap.entities[0]["x"] = 11.0
	snap.entities[0]["yaw"] = 91.0
	_present(p, snap, 2)
	assert_true(model.transform.basis.get_scale().is_equal_approx(
			Vector3.ONE * 1.5))
	assert_almost_eq(model.position.x, 11.0, 0.001)


func test_local_player_handle_is_filtered_from_the_wire_walk() -> void:
	# Packed handles are only hidden when the sim's explicit local-player
	# validity marks one as self — the joiner's H never builds a duplicate.
	var sim := _ticking_sim()
	sim.spawn_local_player(Vector3(100, 0, 100), 0.0, 0)
	assert_true(sim.has_local_player())
	var local_handle := int(sim.get_local_player_wire_handle())
	var container := _container()
	var p := _wire_pass(sim, _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [
		{ "type_id": TYPE_PUMP, "handle": local_handle },
		{ "type_id": TYPE_PUMP, "handle": local_handle + 1, "x": 7.0 },
	]
	_present(p, snap)
	assert_null(p.resolve_wire_handle(local_handle),
			"the local player's own row stays with LocalPlayerPresenter")
	assert_not_null(p.resolve_wire_handle(local_handle + 1),
			"unrelated remote rows still materialize")


func test_unresolved_slot_retries_after_disappearance_and_reuse() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{ "type_id": TYPE_UNRESOLVED, "handle": 0x1004 }]
	_present(p, snap)
	assert_eq(p.wire_entity_count(), 0)
	assert_eq(int(p.get_wire_stats_record().unresolved), 1)

	snap.entities = []
	_present(p, snap, 2)
	snap.entities = [{ "type_id": TYPE_PUMP, "handle": 0x1004 }]
	_present(p, snap, 3)
	assert_eq(p.wire_entity_count(), 1,
			"retired failure cache cannot poison slot reuse")


func test_unresolved_slot_retries_immediately_when_type_changes() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{ "type_id": TYPE_UNRESOLVED, "handle": 0x1004 }]
	_present(p, snap)
	assert_eq(p.wire_entity_count(), 0)

	# type_id is in the identity quintet the plan revision keys on, so the
	# producer bumps the revision with the change.
	snap.entities[0]["type_id"] = TYPE_PUMP
	_present(p, snap, 2)
	assert_eq(p.wire_entity_count(), 1,
			"a changed type retries immediately instead of holding the failure cache")


func test_live_slot_type_change_rebuilds_the_visual() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{ "type_id": TYPE_PUMP, "handle": 0x1004 }]
	_present(p, snap)
	var first: ObjectModel = p.resolve_wire_handle(0x1004)
	assert_not_null(first)

	snap.entities[0]["type_id"] = TYPE_ARMORY
	_present(p, snap, 2)
	var second: ObjectModel = p.resolve_wire_handle(0x1004)
	assert_ne(second, first, "recycled handle cannot keep the prior type model")
	assert_eq(second.entity_ref.runtime_type_id,
			TYPE_ARMORY)


func test_live_player_character_id_change_rebuilds_the_visual() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x0004,
		"character_id": 0x0400,
	}]
	_present(p, snap)
	var first: ObjectModel = p.resolve_wire_handle(0x0004)
	assert_not_null(first)
	assert_eq(first.entity_ref.character_id,
			0x0400)

	snap.entities[0]["character_id"] = 0x0600
	_present(p, snap, 2)
	var second: ObjectModel = p.resolve_wire_handle(0x0004)
	assert_ne(second, first,
			"a reused player slot cannot retain the prior selected character")
	assert_eq(second.entity_ref.character_id,
			0x0600, "the packed 0x0C identity keys remote presentation")


func test_placed_identity_rows_defer_even_without_a_resolvable_node() -> void:
	# A batched static (MultiMesh instance) deliberately has NO per-entity node,
	# so the registry resolves null — yet the row carries its placed .bms
	# identity, and the placed representation owns the render. The wire pass must
	# not spawn a duplicate. Rows without placed identity still spawn.
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container, _empty_index())
	var snap := Snapshot.new()
	snap.entities = [
		{ "type_id": TYPE_PUMP, "handle": 0x1004, "bms_id": 11, "kind": 1,
				"index": 3 },
		{ "type_id": TYPE_ARMORY, "handle": 0x0010, "bms_id": 0, "kind": -1,
				"index": -1 },
	]
	_present(p, snap)
	assert_eq(p.wire_entity_count(), 1,
			"the batched-static row defers; only the identity-less row materializes")
	assert_null(p.resolve_wire_handle(0x1004),
			"no wire duplicate exists for the placed identity")
	assert_not_null(p.resolve_wire_handle(0x0010))


func test_stable_host_layout_keeps_placed_rows_deferred() -> void:
	var placed := _index_with_placed(11)
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container, placed["index"])
	var snap := Snapshot.new()
	snap.entities = [{ "type_id": TYPE_PUMP, "handle": 0x1004, "bms_id": 11,
			"kind": 1, "index": 0 }]
	_present(p, snap)
	assert_eq(p.wire_entity_count(), 0,
			"the placed row remains owned by MissionPresentPass")

	snap.entities[0]["x"] = 9.0
	_present(p, snap)
	assert_eq(p.wire_entity_count(), 0,
			"stable topology with no wire nodes takes the empty fast path")

	snap.entities[0] = { "type_id": TYPE_ARMORY, "handle": 0x1005, "bms_id": 12,
			"kind": -1, "index": -1 }
	_present(p, snap, 2)
	assert_eq(p.wire_entity_count(), 1,
			"same-size placed-to-wire replacement rebuilds classification")


func test_admitted_player_row_with_synthetic_origin_builds_on_the_host() -> void:
	# The HOST runs this pass in full mode with the mission defer index. An
	# admitted player (spawn_player_entity) has NO authored .bms placement, so
	# its row carries the none/synthetic origin (kind 255, index 0xFFFFFF) and
	# must BUILD here — the defer gate only owns real placed identities.
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container, _empty_index())
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 2,
		"kind": 255,
		"index": 0xFFFFFF,
		"bms_id": 0xFFF1,
		"x": 7.0,
	}]
	_present(p, snap)
	var avatar: ObjectModel = p.resolve_wire_handle(2)
	assert_not_null(avatar, "the admitted player's avatar node exists on the host")
	assert_almost_eq(avatar.position.x, 7.0, 0.001)
	var ref: EntityRef = avatar.entity_ref
	assert_eq(ref.runtime_type_id, TYPE_RIFLEMAN)
	assert_eq(ref.item_id, 106102,
			"the runtime type resolves through the placer's visual mapping")


func test_render_culled_row_hides_and_skips_legs_until_released() -> void:
	# The occlusion frame's collector gate over a wire row: a culled row is not
	# drawn, so no presentation leg runs for it (the node hides, its transform
	# stays where it was), and the compare-gated legs re-assert exactly what
	# changed once the gate releases it.
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{ "type_id": TYPE_RIFLEMAN, "handle": 2, "x": 7.0 }]
	_present(p, snap)
	var avatar: ObjectModel = p.resolve_wire_handle(2)
	assert_not_null(avatar)
	assert_true(avatar.visible)
	assert_almost_eq(avatar.position.x, 7.0, 0.001)

	p.set_render_culled(2, true)
	snap.entities[0]["x"] = 9.0
	_present(p, snap)
	assert_false(avatar.visible, "a culled row's node hides")
	assert_almost_eq(avatar.position.x, 7.0, 0.001,
			"no presentation leg runs for a culled row")

	p.set_render_culled(2, false)
	_present(p, snap)
	assert_true(avatar.visible, "the released row draws again")
	assert_almost_eq(avatar.position.x, 9.0, 0.001,
			"the transform leg re-asserts the current wire pose on release")

	# The verdict pairs with the sim's applied baseline: a baseline reset
	# forgets every verdict at once, and the next frame re-emits the full set.
	p.set_render_culled(2, true)
	_present(p, snap)
	assert_false(avatar.visible)
	p.clear_render_culled()
	_present(p, snap)
	assert_true(avatar.visible, "clearing the verdicts releases the row")


func test_wire_plan_survives_reorder_then_prunes_and_rebuilds_reused_type() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [
		{ "type_id": TYPE_PUMP, "handle": 0x1004, "x": 4.0 },
		{ "type_id": TYPE_ARMORY, "handle": 0x1005, "x": 5.0 },
	]
	_present(p, snap)
	var first: ObjectModel = p.resolve_wire_handle(0x1004)
	var second: ObjectModel = p.resolve_wire_handle(0x1005)

	snap.entities = [
		{ "type_id": TYPE_ARMORY, "handle": 0x1005, "x": 50.0 },
		{ "type_id": TYPE_PUMP, "handle": 0x1004, "x": 40.0 },
	]
	_present(p, snap, 2)
	assert_eq(p.resolve_wire_handle(0x1004), first)
	assert_eq(p.resolve_wire_handle(0x1005), second)
	assert_almost_eq(first.position.x, 40.0, 0.001)
	assert_almost_eq(second.position.x, 50.0, 0.001)

	snap.entities = [
		{ "type_id": TYPE_ARMORY, "handle": 0x1005, "x": 51.0 },
	]
	_present(p, snap, 3)
	assert_null(p.resolve_wire_handle(0x1004),
			"despawn prunes the retired row plan and visual")
	assert_eq(p.wire_entity_count(), 1)

	snap.entities[0] = { "type_id": TYPE_RIFLEMAN, "handle": 0x1005, "x": 60.0 }
	_present(p, snap, 4)
	var replacement: ObjectModel = p.resolve_wire_handle(0x1005)
	assert_ne(replacement, second,
			"a recycled handle with a new type cannot retain the old visual")
	assert_almost_eq(replacement.position.x, 60.0, 0.001)


func test_cold_materialization_is_bounded_and_converges_while_live_rows_update() -> void:
	var placed := _index_with_placed(11)
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var observer := SpawnObserver.new()
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container, placed["index"], {
		"cold_spawn_budget": 2,
		"camera": camera,
	})
	p.wire_node_spawned.connect(observer.on_spawned)
	var snap := Snapshot.new()
	snap.entities = [
		{ "type_id": TYPE_PUMP, "handle": 0x1004, "x": 4.0 },
		{ "type_id": TYPE_PUMP, "handle": 0x1005, "x": 5.0 },
		{ "type_id": TYPE_PUMP, "handle": 0x1006, "x": 6.0 },
		{ "type_id": TYPE_PUMP, "handle": 0x1007, "x": 7.0 },
		{ "type_id": TYPE_PUMP, "handle": 0x1008, "x": 8.0 },
		{ "type_id": TYPE_PUMP, "handle": 0x1009, "kind": 1, "index": 0,
				"bms_id": 11 },
	]
	_present(p, snap)
	assert_eq(p.wire_entity_count(), 2,
			"one presentation call cannot build beyond its cold-spawn budget")
	assert_eq(int(p.get_wire_stats_record().pending), 3)
	assert_eq(observer.calls.size(), 2)
	assert_eq(camera.position, Vector3.ZERO,
			"one-shot spectator framing waits for the complete cold cohort")
	assert_eq(observer.calls[0].position, Vector3(4, 0, 0),
			"each callback still runs after its first transform is applied")

	var first: ObjectModel = p.resolve_wire_handle(0x1004)
	snap.entities[0]["x"] = 40.0
	_present(p, snap)
	assert_eq(p.wire_entity_count(), 4)
	assert_eq(int(p.get_wire_stats_record().pending), 1)
	assert_almost_eq(first.position.x, 40.0, 0.001,
			"already-live rows keep updating while later cold rows drain")
	assert_eq(observer.calls.size(), 4)
	assert_eq(camera.position, Vector3.ZERO)

	_present(p, snap)
	assert_eq(p.wire_entity_count(), 5)
	assert_eq(int(p.get_wire_stats_record().pending), 0)
	assert_eq(observer.calls.size(), 5,
			"every materialized row is registered exactly once across batches")
	assert_almost_eq(camera.position.x, 13.2, 0.001,
			"the converged spectator frame uses every materialized wire row")

	snap.entities[4]["x"] = 80.0
	_present(p, snap)
	assert_eq(observer.calls.size(), 5,
			"the converged topology returns to the native stable-plan fast path")
	assert_almost_eq(p.resolve_wire_handle(0x1008).position.x, 80.0, 0.001)


func test_layout_change_mid_backlog_discards_stale_rows_and_rebudgets_replacements() -> void:
	var observer := SpawnObserver.new()
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container, null, {
		"cold_spawn_budget": 1,
	})
	p.wire_node_spawned.connect(observer.on_spawned)
	var snap := Snapshot.new()
	snap.entities = [
		{ "type_id": TYPE_PUMP, "handle": 0x1004, "x": 4.0 },
		{ "type_id": TYPE_PUMP, "handle": 0x1005, "x": 5.0 },
		{ "type_id": TYPE_PUMP, "handle": 0x1006, "x": 6.0 },
	]
	_present(p, snap)
	var retired: ObjectModel = p.resolve_wire_handle(0x1004)
	assert_not_null(retired)

	# The pending topology changes before it converges: the one materialized
	# slot changes identity, another pending slot disappears, a new one arrives.
	snap.entities = [
		{ "type_id": TYPE_ARMORY, "handle": 0x1005, "x": 55.0 },
		{ "type_id": TYPE_RIFLEMAN, "handle": 0x1004, "x": 44.0 },
		{ "type_id": TYPE_ARMORY, "handle": 0x1007, "x": 77.0 },
	]
	_present(p, snap, 2)
	assert_null(p.resolve_wire_handle(0x1004),
			"a now-mismatched live node is retired even after this frame spends its budget")
	assert_eq(int(p.get_wire_stats_record().pending), 2)

	_present(p, snap, 2)
	_present(p, snap, 2)
	assert_ne(p.resolve_wire_handle(0x1004), retired)
	assert_eq(p.wire_entity_count(), 3)
	assert_eq(int(p.get_wire_stats_record().pending), 0)
	assert_eq(observer.calls.size(), 4,
			"the retired incarnation and each replacement register only once")


func test_unresolved_attempt_consumes_budget_without_stranding_later_rows() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container, null, {
		"cold_spawn_budget": 1,
	})
	var snap := Snapshot.new()
	snap.entities = [
		{ "type_id": TYPE_UNRESOLVED, "handle": 0x1004 },
		{ "type_id": TYPE_PUMP, "handle": 0x1005 },
	]
	_present(p, snap)
	assert_eq(int(p.get_wire_stats_record().unresolved), 1)
	assert_eq(int(p.get_wire_stats_record().pending), 1)
	_present(p, snap)
	assert_eq(p.wire_entity_count(), 1,
			"a cached unresolved row does not consume every later batch")
	assert_eq(int(p.get_wire_stats_record().pending), 0)
	_present(p, snap)
	assert_eq(int(p.get_wire_stats_record().unresolved), 1,
			"a stable unresolved type is not retried once the plan converges")


func test_runtime_reset_rematerializes_the_restored_same_type_slot() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{ "type_id": TYPE_PUMP, "handle": 0x1004 }]
	_present(p, snap)
	var first: ObjectModel = p.resolve_wire_handle(0x1004)

	p.reset_wire_runtime_state()
	assert_eq(p.wire_entity_count(), 0)
	assert_null(p.resolve_wire_handle(0x1004))
	_present(p, snap)
	var restored: ObjectModel = p.resolve_wire_handle(0x1004)
	assert_not_null(restored)
	assert_ne(restored, first,
			"restart builds a fresh restored-incarnation visual")


func test_synthetic_attachment_uses_panm_and_hidden_visibility_contract() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container, _empty_index(), {
		"synthetic_origin_only": true,
	})
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_PUMP,
		"handle": 0x1005,
		"kind": 255,
		"index": 0xFFFFFF,
		"alive": 0,
		"section_mask_valid": 1,
		"section_mask": 0b00010,
		"active1": 1,
		"phase1": 0x2345,
		"active2": 1,
		"phase2": 0x6789,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x1005)
	var parts: Dictionary = model.get_render_part_nodes()
	assert_eq(parts.size(), 5, "the synthetic model exposes all fixture sections")
	assert_true((parts[0] as Node3D).visible)
	assert_false((parts[1] as Node3D).visible,
			"a severed-piece row hides the original half of its model")
	assert_true((parts[2] as Node3D).visible)
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL1"), 0x2345,
			"attached items consume the same two PANM channels as placed items")
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL2"), 0x6789)
	snap.entities[0]["active1"] = 0
	snap.entities[0]["phase2"] = 0
	snap.entities[0]["section_mask"] = 0
	_present(p, snap)
	assert_false(model.get_ctrl_values().has("VEHICLE_SPECIAL1"),
			"wire presentation releases a no-longer-owned SPECIAL1 value")
	assert_eq(_ctrl(model, "VEHICLE_SPECIAL2"), 0,
			"wire presentation submits an owned zero endpoint")
	assert_true((parts[1] as Node3D).visible,
			"a changed severed-piece mask restores the matching model part")
	assert_true(model.visible,
			"a dead attached item retains its graphic or husk until explicitly hidden")
	snap.entities[0]["hidden"] = 1
	_present(p, snap)
	assert_false(model.visible, "PF_HIDDEN ends attached-item presentation")


func test_wire_model_spawn_registers_after_identity_and_transform_are_ready() -> void:
	var container := _container()
	var observer := SpawnObserver.new()
	var p := _wire_pass(_sim(), _placer(), container)
	p.wire_node_spawned.connect(observer.on_spawned)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_PUMP,
		"handle": 0x1004,
		# A real joiner's ClientState has no authoritative BMS origin, so
		# PF_KIND is -1. The callback must derive pool 1 -> KIND_ITEM from the
		# wire handle.
		"kind": -1,
		"index": 9,
		"bms_id": 77,
		"x": 4.0,
		"y": 5.0,
		"z": 6.0,
		"yaw": 90.0,
	}]
	_present(p, snap)

	assert_eq(observer.calls.size(), 1, "the new wire model is registered exactly once")
	var call: Dictionary = observer.calls[0]
	assert_eq(int(call.kind), MissionData.KIND_ITEM)
	assert_eq(int(call.item_id), 106100)
	assert_eq(call.position, Vector3(4, 5, 6),
			"registration runs after the production transform is applied")
	var node := call.node as Node3D
	var ref: EntityRef = (node as ObjectModel).entity_ref
	assert_eq(ref.wire_handle, 0x1004)
	assert_eq(ref.origin_kind, -1)

	_present(p, snap)
	assert_eq(observer.calls.size(), 1, "steady presentation never re-registers the model")
	# A consumer that subscribes after the first present replays the live
	# bodies itself: the signal is never re-emitted for them.
	var late := SpawnObserver.new()
	p.wire_node_spawned.connect(late.on_spawned)
	assert_eq(late.calls.size(), 0, "connecting late fires nothing by itself")
	var live: Array = p.wire_nodes()
	assert_eq(live.size(), 1, "wire_nodes() lists every already-live wire body")
	for live_node in live:
		var live_ref: EntityRef = (live_node as ObjectModel).entity_ref
		late.on_spawned(live_node, live_ref.kind, live_ref.item_id)
	assert_eq(late.calls.size(), 1, "late consumers receive every already-live wire node")
	assert_eq(int(late.calls[0].kind), MissionData.KIND_ITEM)
	assert_eq(int(late.calls[0].item_id), 106100)
	assert_eq(late.calls[0].node, node, "the replayed body is the one the signal announced")


func test_wire_model_applies_the_same_packed_overlay_result() -> void:
	var angles := PackedVector3Array()
	for i in range(9):
		angles.append(Vector3(-4.0 + i, 70.0 + i, 1.0 + i))
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x1004,
		"anim_state": 43,
		"anim_phase": 11,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(6.0, 33.0, -2.0),
		"aim_angles": angles,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x1004)
	assert_eq(model.get_active_body_clip(), "anim_idle",
			"presentation forwards the state to the completion-aware remote channel")
	var deltas: Array = model.get_aim_overlay()
	assert_eq(deltas.size(), 9)
	var body_basis := MissionObjectPlacer.bms_to_godot_basis(
			Vector3(6.0, 33.0, -2.0))
	assert_true(model.basis.is_equal_approx(body_basis))
	assert_true((deltas[8] as Basis).is_equal_approx(
			body_basis.inverse() * MissionObjectPlacer.bms_to_godot_basis(angles[8])))

	# The model owns transition acceptance and completion. A repeated semantic
	# state preserves its free-running playhead even for a revisionless source;
	# only the next state edge is dispatched.
	var time_after_first := model.get_animation_time()
	snap.entities[0]["anim_phase"] = 22
	_present(p, snap)
	assert_almost_eq(model.get_animation_time(), time_after_first, 0.0001,
			"a repeated semantic state is not re-pinned to the wire phase")
	snap.entities[0]["anim_state"] = 1
	snap.entities[0]["anim_phase"] = 6
	_present(p, snap)
	assert_eq(model.get_active_body_clip(), "anim_walk_forward",
			"state edges reach the completion-aware remote animation channel")


func test_wire_model_free_runs_compact_infantry_when_phase_is_absent() -> void:
	# Production infantry compacts carry the state byte but no player-channel
	# phase byte. Re-presenting that snapshot must preserve local playback
	# instead of externally pinning the selected clip to tick zero.
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x0004,
		"anim_state": 43,
	}]
	_present(p, snap)
	_present(p, snap)

	var model: ObjectModel = p.resolve_wire_handle(0x0004)
	assert_eq(model.get_active_body_clip(), "anim_idle",
			"phase-less compact infantry is accepted once")
	assert_true(model.is_playing(),
			"the accepted clip free-runs locally instead of pinning to tick zero")


func test_host_current_body_state_is_posed_without_remote_rearbitration() -> void:
	# Host-loopback snapshots carry AiEntity's already-accepted current state
	# and phase, not a compact pending request. It must retain the direct pose
	# path (externally phased, no remote latch).
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x0004,
		"anim_state": 43,
		"anim_phase": 11,
		"anim_remote_request": 0,
	}]
	_present(p, snap)

	var model: ObjectModel = p.resolve_wire_handle(0x0004)
	assert_eq(model.get_active_body_clip(), "anim_idle",
			"host current state keeps the authoritative direct-phase pose path")
	var fps: float = model.get_skeletal_anim().get_clip_fps("anim_idle")
	assert_almost_eq(model.get_animation_time(), 11.0 / (2.0 * fps), 0.0001,
			"the sim phase poses the clip directly")
	assert_false(model.remote_body_needs_fixed_tick(),
			"host current state is not submitted to the receive-side request channel")


func test_host_current_body_state_uses_authoritative_blend_tuple() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x0004,
		"anim_source_state": 43,
		"anim_source_phase": 18,
		"anim_state": 1,
		"anim_phase": 4,
		"anim_blend_weight": 0.3,
		"anim_remote_request": 0,
	}]
	_present(p, snap)

	var model: ObjectModel = p.resolve_wire_handle(0x0004)
	assert_eq(model.get_active_body_clip(), "anim_walk_forward")
	var blend := model.get_body_blend()
	assert_eq(blend.source_key, "anim_idle",
			"host-loopback consumes authority rather than reconstructing a blend")
	var fps: float = model.get_skeletal_anim().get_clip_fps("anim_idle")
	assert_almost_eq(blend.source_time,
			18.0 / (2.0 * fps), 0.0001)
	assert_almost_eq(blend.weight, 0.3, 0.000001)


func test_remote_blend_uses_logic_tick_delta_not_present_call_count() -> void:
	var sim := _ticking_sim()
	var container := _container()
	var p := _wire_pass(sim, _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x0004,
		"anim_state": 43,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x0004)
	assert_eq(model.get_active_body_clip(), "anim_idle")

	# The state edge arrives after one logic tick and stages the incoming
	# channel; the fixed-tick seam then owns the weight ramp.
	sim.step()
	snap.entities[0]["anim_state"] = 1
	_present(p, snap)
	assert_true(model.remote_body_needs_fixed_tick(),
			"an accepted state edge arms the fixed-tick transition latch")
	var weight_armed := model.get_body_blend().weight

	# Render-only presents at the same fixed tick must not accelerate the blend.
	_present(p, snap)
	_present(p, snap)
	assert_almost_eq(model.get_body_blend().weight,
			weight_armed, 0.000001,
			"duplicate presents at one logic tick do not advance a fixed-tick blend")

	# A catch-up frame advances every omitted fixed tick, not just one call.
	sim.step()
	sim.step()
	sim.step()
	_present(p, snap)
	var caught_up := model.get_body_blend()
	var finished := caught_up == null
	var advanced := caught_up.weight if caught_up != null else 1.0
	assert_true(finished or advanced > weight_armed,
			"a three-tick catch-up advances the transition weight")
	_present(p, snap)
	assert_eq(model.get_body_blend() == null, finished,
			"a repeated presentation of the catch-up result is idempotent")


func test_wire_model_receives_the_transition_pulse_before_the_current_state() -> void:
	# A tapped prone roll rides the wire as 41/42 for a single 0x0A sample (the
	# emitted byte is `pending ?: current`), and several datagrams fold per
	# render frame, so the sim surfaces the buried transition as
	# PF_ANIM_STATE_PULSE. Presentation dispatches it FIRST — retail applies the
	# anim byte per record [orig: @0x4c1153] — so the locked roll clip accepts
	# and the follow-up state queues behind it at the model.
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x1004,
		"anim_state": 43,
		"anim_phase": 60,
		"anim_pulse": 1,
		"anim_pulse_ticks": 6,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x1004)
	# The pulse (walk_forward) was accepted first with its own phase; the
	# current state (idle) then arbitrates behind it at the model.
	assert_true(model.get_active_body_clip() in ["anim_walk_forward", "anim_idle"],
			"the buried pulse dispatches before the current state")
	var settled := model.get_active_body_clip()
	var settled_time := model.get_animation_time()

	# A steady frame with no pulse and an unchanged state stays on the
	# edge-gated fast path: nothing re-poses.
	snap.entities[0]["anim_pulse"] = -1
	snap.entities[0]["anim_pulse_ticks"] = -1
	_present(p, snap)
	assert_eq(model.get_active_body_clip(), settled,
			"pulse-free same-state frames keep the edge-gate skip")
	assert_almost_eq(model.get_animation_time(), settled_time, 0.05,
			"the retained pose is not re-pinned")


func test_state_edge_latch_survives_a_revision_bumped_plan_rebuild() -> void:
	var sim := _ticking_sim()
	var container := _container()
	var p := _wire_pass(sim, _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x0004,
		"anim_state": 43,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x0004)
	sim.step()
	snap.entities[0]["anim_state"] = 1
	_present(p, snap)
	assert_true(model.remote_body_needs_fixed_tick(),
			"an accepted state edge arms the fixed-tick transition latch")

	# A revision bump forces a cold plan rebuild over the same live node: the
	# per-handle remote cache must restore the live latch instead of
	# re-submitting the same state into a fresh epoch.
	_present(p, snap, 2)
	assert_true(model.remote_body_needs_fixed_tick(),
			"a cold row plan restores its live transition latch")


func test_wire_model_resets_remote_body_channel_on_respawn_revision_change() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x0004,
		"anim_state": 43,
		"anim_phase": 11,
		"respawn_revision": 0,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x0004)
	assert_eq(model.get_active_body_clip(), "anim_idle",
			"the first lifecycle sample initializes rather than resets a new model")

	# A revision jump represents one or more dead->alive edges folded before
	# this render. Reset must precede the animation request so an identical
	# state ID is accepted into a fresh remote body-channel epoch.
	snap.entities[0]["respawn_revision"] = 2
	snap.entities[0]["anim_phase"] = 6
	_present(p, snap)
	assert_eq(model.get_active_body_clip(), "anim_idle",
			"respawn reset runs before the compact animation is re-applied")
	assert_false(model.remote_body_needs_fixed_tick(),
			"the fresh epoch carries no stale transition latch")


func test_wire_model_applies_and_restores_mounted_right_hand_collapse() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x1004,
		"aim_overlay_valid": 1,
		"right_hand_collapsed": 1,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x1004)
	assert_true(model.is_right_hand_collapsed(),
			"the wire pose consumes the same mount verdict")
	snap.entities[0]["right_hand_collapsed"] = 0
	_present(p, snap)
	assert_false(model.is_right_hand_collapsed(), "and restores on dismount")


func test_wire_model_applies_and_clears_named_emplaced_controls() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_PUMP,
		"handle": 0x1004,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x2000,
		"emplaced_gun_pitch": 0xE000,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x1004)
	assert_eq(model.get_ctrl_values(), {
		"EWEAP_GUNYAW": 0x2000,
		"EWEAP_GUNPITCH": 0xE000,
	})

	snap.entities[0]["emplaced_controls_valid"] = 0
	_present(p, snap)
	assert_true(model.get_ctrl_values().is_empty())


func test_wire_direct_carrier_applies_and_releases_scoped_world_heat() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_PUMP,
		"handle": 0x1004,
		"world_heat_glow_valid": 1,
		"world_heat_glow": 0,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x1004)
	assert_eq(_ctrl(model, "HEAT_GLOW"), 0,
			"the live UseGun carrier scope owns retail's cold zero")

	snap.entities[0]["world_heat_glow"] = 0xFFFF
	_present(p, snap)
	assert_eq(_ctrl(model, "HEAT_GLOW"), 0xFFFF,
			"wire-direct carrier presentation keeps the world unsigned-word cap")

	snap.entities[0]["world_heat_glow_valid"] = 0
	_present(p, snap)
	assert_false(model.get_ctrl_values().has("HEAT_GLOW"),
			"an unscoped/joiner row releases rather than synthesizes heat")


func test_wire_direct_numbered_zone_applies_and_releases_callback_controls() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_PUMP,
		"handle": 0x1004,
		"tex_team_valid": 1,
		"tex_team": 2,
		"zone_ctrl_valid": 1,
		"team_swing": 0x10000,
		"lfp_camp_percent_valid": 1,
		"lfp_camp_percent": 0x4000,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x1004)
	assert_eq(model.get_ctrl_values(), {
		"TEX_TEAM": 2,
		"TEAMSWING": 0x10000,
		"LFP_CAMPPERCENT": 0x4000,
	}, "an unresolved pool-1 zone still runs the generic-world CTRL callback")

	snap.entities[0]["lfp_camp_percent_valid"] = 0
	_present(p, snap)
	assert_false(model.get_ctrl_values().has("LFP_CAMPPERCENT"),
			"missing timer entry is an omitted write, not a fabricated zero")
	assert_true(model.get_ctrl_values().has("TEAMSWING"))
	snap.entities[0]["tex_team_valid"] = 0
	snap.entities[0]["zone_ctrl_valid"] = 0
	_present(p, snap)
	assert_true(model.get_ctrl_values().is_empty())


func test_wire_model_honors_local_first_person_parent_cull() -> void:
	# Host-side dynamic/unplaced mount targets are owned by this pass rather
	# than MissionPresentPass. They consume the same transient render verdict.
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container, _empty_index())
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_PUMP,
		"handle": 0x1004,
		"alive": 1,
		"local_view_suppressed": 1,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x1004)
	assert_false(model.visible,
			"the dynamic local UseGun parent skips its own world model")

	snap.entities[0]["local_view_suppressed"] = 0
	_present(p, snap)
	assert_true(model.visible, "clearing the transient verdict restores the parent")
	snap.entities[0]["alive"] = 0
	_present(p, snap)
	assert_true(model.visible,
			"a dead non-hidden organic remains visible as a corpse")
	snap.entities[0]["hidden"] = 1
	_present(p, snap)
	assert_false(model.visible,
			"the authoritative compact hidden bit suppresses the world model")


func test_wire_model_clears_overlay_when_snapshot_selector_is_invalid() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x1004,
		"aim_overlay_valid": 0,
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x1004)
	model.set_aim_overlay([Basis()])  # retained from an earlier owner
	_present(p, snap, 2)
	assert_eq(model.get_aim_overlay(), [],
			"a remote model cannot retain an overlay after selector invalidation")


# A remote player's upper-body weapon pose. The sim derives the state (nothing
# about the weapon channel crosses the wire — every observer re-derives it from
# the peer's equipped ADM index and Flags bit 0x10), and the wire pass drives
# it onto that peer's model. A -1 state means "no channel this frame" and must
# CLEAR the pose, or a peer keeps the hold it had before it switched weapons.
# [orig: the selection Entity_UpdateInfantryPlayerBody @0x4b5dad, which retail
#  runs for every player body it draws rather than only the local one]
func test_wire_model_applies_and_clears_the_remote_weapon_channel() -> void:
	var container := _container()
	var p := _wire_pass(_sim(), _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_RIFLEMAN,
		"handle": 0x1004,
		"aim_overlay_valid": 1,
		"wpn_anim_state": 51,   # pistol hold
		"wpn_phase_ticks": -1,  # the secondary playhead is not replicated
	}]
	_present(p, snap)
	var model: ObjectModel = p.resolve_wire_handle(0x1004)
	var channel := model.get_weapon_channel()
	assert_eq(channel.key, Simulation.infantry_anim_key(51),
			"the state id resolves to the pistol hold clip key")
	assert_eq(channel.phase_ticks, -1)

	# The peer stows its weapon: the channel goes away and the pose must clear.
	snap.entities[0]["wpn_anim_state"] = -1
	_present(p, snap)
	assert_true(model.get_weapon_channel() == null,
			"a -1 state clears the hold pose rather than leaving the last one posed")


# A remote player's HELD WEAPON. The sim folds the draw gate into the ADM
# field — a hidden or unarmed body reports 0, which is both our weapon table's
# null row and the original's own `if (entity->equippedAdmIndex)` precondition
# — so a zero row must build no model at all, and a nonzero one must resolve
# its gfx3 through the ADM-indexed table.
# [orig: BoneCallback_org0_World draw 5, precondition @0x4e3c97; gate
#  Entity_CanFireWeapon @0x4dcb10]
func test_wire_row_builds_a_held_weapon_only_when_it_is_armed() -> void:
	var sim := _sim()
	assert_eq(sim.load_weapon_table(_flat_root(), "weapon.def"), OK,
			"the fixture weapon table loads (adm 1 -> M9K_3rd)")
	var container := _container()
	var p := _wire_pass(sim, _placer(), container)
	var snap := Snapshot.new()
	snap.entities = [{
		"type_id": TYPE_PUMP,
		"handle": 0x1004,
		"aim_overlay_valid": 1,
		"held_weapon_adm": 0,   # unarmed / hidden by the gate
	}]
	_present(p, snap)
	assert_null(p.held_weapon_node(0x1004),
			"an ADM of 0 draws nothing — no model is built at all")
	var body: ObjectModel = p.resolve_wire_handle(0x1004)
	# The static pump has no rig of its own; the hand chain the attach math
	# walks is scaffolded exactly as the placed body would carry it.
	var skeleton := Skeleton3D.new()
	for bone_index in range(EntityPresenter.HELD_WEAPON_BONE_INDEX + 1):
		skeleton.add_bone("Bone%d" % bone_index)
	body.add_child(skeleton)
	# Replica sun quality is cached by wire identity. It applies immediately to
	# the body and must also reach a held weapon built AFTER this change. The
	# factor is the x (effectScale) lane of the per-entity instance uniform the
	# object shaders scale the pass-global directional term by.
	var expected_light := Vector4(0.25, 0.0, 0.0, 0.0)
	p.set_entity_lighting_context(0x1004, 0.25, false, 0.0)
	_assert_entity_light(body, expected_light,
			"wire sun quality dims the body directional term")

	# Now the peer is holding something the table can resolve.
	snap.entities[0]["held_weapon_adm"] = 1
	_present(p, snap)
	var weapon: ObjectModel = p.held_weapon_node(0x1004)
	assert_not_null(weapon, "the model is resolved from the ADM index the wire carries")
	assert_true(weapon.visible)
	assert_eq(weapon.get_authored_lod_owner(), body,
			"the held weapon draws at its body's RLOD level, never its own walk")
	_assert_entity_light(weapon, expected_light,
			"a late-built held weapon inherits its body's cached sun quality")
	snap.entities[0]["hidden"] = 1
	_present(p, snap)
	assert_false(weapon.visible,
			"the weapon consumes this snapshot's body visibility without a one-tick lag")
	snap.entities[0]["hidden"] = 0
	_present(p, snap)
	assert_true(weapon.visible,
			"the caster and color model return on the same visible snapshot")
	_assert_entity_light(body, expected_light,
			"a hidden-visible cycle preserves the last pushed body lighting")
	_assert_entity_light(weapon, expected_light,
			"a hidden-visible cycle preserves the last pushed weapon lighting")

	# Stowing it again retires the node rather than leaving a gun floating.
	snap.entities[0]["held_weapon_adm"] = 0
	_present(p, snap)
	assert_null(p.held_weapon_node(0x1004),
			"going unarmed frees the weapon rather than rebuilding one")


# --- Native held-weapon parity ---------------------------------------------

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
		var expected := EntityPresenter.held_weapon_hand_frame_basis(b)
		var got: Basis = EntityPresenter.held_weapon_hand_frame_basis(b)
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
	for bone_index in range(EntityPresenter.HELD_WEAPON_BONE_INDEX + 1):
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
	skeleton.set_bone_pose_rotation(EntityPresenter.HELD_WEAPON_BONE_INDEX,
			Quaternion(Vector3(0.3, -0.8, 0.52).normalized(), -1.1))
	for angles: Vector3 in [Vector3.ZERO, Vector3(15, -120, 40), Vector3(-80, 270, -30)]:
		for hand_frame in [false, true]:
			var expected: Variant = EntityPresenter.held_weapon_attach_transform(
					body, angles, hand_frame)
			var got: Variant = EntityPresenter.held_weapon_attach_transform(
					skeleton, angles, hand_frame)
			assert_not_null(expected, "the reference places a transform")
			assert_true((got as Transform3D).is_equal_approx(expected as Transform3D),
					"attach parity (hand=%s, %s): native %s vs gd %s" % [
							hand_frame, angles, got, expected])
	# The cannot-place leg: too few bones -> null from both.
	var short_skel := Skeleton3D.new()
	add_child_autofree(short_skel)
	short_skel.add_bone("only")
	assert_null(EntityPresenter.held_weapon_attach_transform(
			short_skel, Vector3.ZERO, false))
