extends GutTest


const MISSION := "03TR.bms"
const CARRIER_ITEM_ID := 102010
const CARRIER_TYPE_ID := 2010
const CARRIER_GRAPHIC := "Dblkhwk1"
const CHILD_TYPE_ID := 1871
const ANCHOR_NAMES := ["ewep01", "ewep02"]
const DBUGGY_ITEM_ID := 101291
const DBUGGY_TYPE_ID := 1291
const DBUGGY_GRAPHIC := "Dbuggy1"
const MRK5_ITEM_ID := 101299
const MRK5_TYPE_ID := 1299
const MRK5_GRAPHIC := "Dmrk51"


func _retail_attachment_basis(direction: Vector3) -> Basis:
	var forward := direction.normalized()
	var right := Vector3(forward.z, 0.0, -forward.x)
	if right.length_squared() <= 0.00000001:
		right = Vector3.RIGHT
	else:
		right = right.normalized()
	var up := forward.cross(right).normalized()
	# IDA's exact retail writes are raw row-major columns right/up/forward:
	# m[0,4,8]=right, m[1,5,9]=up, m[2,6,10]=forward. Spell out the
	# loader's row-matrix-to-Godot conversion element by element so this oracle
	# cannot share a mistaken transpose/conjugation expression with production.
	return Basis(
			Vector3(right.x, -up.x, -forward.x),
			Vector3(-right.y, up.y, forward.y),
			Vector3(-right.z, up.z, forward.z))


func _blackhawk_carriers(mission: MissionData) -> Array:
	var carriers: Array = []
	for raw in mission.get_all_entities():
		var entity: MissionEntityRecord = raw
		if entity.item_id == CARRIER_ITEM_ID:
			carriers.append(entity)
	return carriers


func _blackhawk_anchors(data: ObjectData) -> Array:
	var anchors: Array = []
	for wanted in ANCHOR_NAMES:
		for index in range(data.get_user_point_count()):
			var candidate := data.get_user_point_info(index)
			if candidate.name.to_lower() == wanted:
				anchors.append(candidate)
				break
	return anchors


func _authored_attachment_anchors(
		item_db: ItemDatabase, item_id: int, data: ObjectData) -> Array:
	# Oracle-side rebuild of the authored addeweap anchors: the items.def row
	# names the child item + userpoint, the .3di USRP row supplies the raw
	# retail frame. Whole-name case-insensitive resolve, first match — the same
	# rule the runtime applies, so the expected frames pair with produced rows.
	var rows: Array = []
	for authored: ItemEmplacementAttachment in item_db.get_emplacement_attachments(item_id):
		var wanted := authored.userpoint.strip_edges()
		var row := {
			"item_id": authored.item_id,
			"source_name": wanted,
			"subobject": -1,
			"raw_position": Vector3.ZERO,
			"raw_rotation": Vector3.ZERO,
			"anchor_found": false,
		}
		for index in range(data.get_user_point_count()):
			var up := data.get_user_point_info(index)
			if up.name.strip_edges().nocasecmp_to(wanted) != 0:
				continue
			row["anchor_found"] = true
			row["source_name"] = up.name
			row["subobject"] = up.subobject
			row["raw_position"] = up.position
			row["raw_rotation"] = up.rotation
			break
		rows.append(row)
	return rows


func _present_miniguns(sim: Simulation) -> Array:
	var children: Array = []
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) != CHILD_TYPE_ID:
			continue
		children.append({
			"position": Vector3(
					snapshot[base + Simulation.PF_POS_X],
					snapshot[base + Simulation.PF_POS_Y],
					snapshot[base + Simulation.PF_POS_Z]),
			"rotation": Vector3(
					snapshot[base + Simulation.PF_PITCH_DEG],
					snapshot[base + Simulation.PF_YAW_DEG],
					snapshot[base + Simulation.PF_ROLL_DEG]),
		})
	return children


func _synthetic_attachment_rows(
		sim: Simulation, child_types: Dictionary) -> Dictionary:
	var rows := {}
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		var type_id := int(snapshot[base + Simulation.PF_TYPE_ID])
		if not child_types.has(type_id):
			continue
		if int(snapshot[base + Simulation.PF_KIND]) != 255 \
				or int(snapshot[base + Simulation.PF_INDEX]) != 0xFFFFFF:
			continue
		rows[int(snapshot[base + Simulation.PF_WIRE_HANDLE])] = {
			"type_id": type_id,
			"position": Vector3(
					snapshot[base + Simulation.PF_POS_X],
					snapshot[base + Simulation.PF_POS_Y],
					snapshot[base + Simulation.PF_POS_Z]),
			"rotation": Vector3(
					snapshot[base + Simulation.PF_PITCH_DEG],
					snapshot[base + Simulation.PF_YAW_DEG],
					snapshot[base + Simulation.PF_ROLL_DEG]),
		}
	return rows


func test_03tr_blackhawk_miniguns_follow_authored_ewep_forward() -> void:
	var install_dir := RetailData.install()
	if install_dir.is_empty():
		pending("OPENNOVA_JO_DIR / retail JO PFFs are required for the 03TR Blackhawk witness")
		return

	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(install_dir, "", false, "jo"), OK)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, MISSION), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	assert_eq(String(item_db.get_graphic(CARRIER_ITEM_ID)), CARRIER_GRAPHIC)

	var carriers := _blackhawk_carriers(mission)
	assert_eq(carriers.size(), 2, "03TR carries the two base Blackhawks")
	var data := ObjectData.new()
	assert_eq(data.open_from_resource_root(root, CARRIER_GRAPHIC + ".3di"), OK)
	var anchors := _blackhawk_anchors(data)
	assert_eq(anchors.size(), 2, "Dblkhwk1 carries ewep01 and ewep02")
	if carriers.size() != 2 or anchors.size() != 2:
		return

	# Build the expected frames directly from the retail BMS placement and USRP
	# rows. The production item-spec/provider path below is the system under test.
	var expected: Array = []
	for carrier in carriers:
		var carrier_xform := MissionObjectPlacer.entity_transform(
				carrier.position,
				carrier.rotation_deg)
		for anchor: ModelUserPoint in anchors:
			var direction := anchor.rotation
			assert_gt(direction.length_squared(), 0.99,
					"%s carries an authored forward direction" % anchor.name)
			expected.append({
				"label": "SSN %d %s" % [carrier.bms_id, anchor.name],
				"position": carrier_xform * anchor.position,
				"forward": (
						carrier_xform.basis
						* _retail_attachment_basis(direction)
						* Vector3.BACK).normalized(),
			})

	var carrier_card := item_db.extract_seat_specs_for_item(root, CARRIER_ITEM_ID)
	assert_eq(carrier_card.get_emplacement_attachments().size(), 2)
	# S16: the seat/mount table is the native extraction over items.def rows +
	# .3di userpoints — the asset root must be installed before the seed walk.
	var sim := Simulation.new()
	sim.enable_listen_server(true)
	sim.set_asset_root(root)
	assert_true(sim.install_seat_specs_for_type_ids(
			item_db, PackedInt32Array([CARRIER_TYPE_ID])))
	assert_gt(sim.get_mounted_graphic_source_count(), 0,
			"the native install fed the mounted-pose resolver")
	assert_true(sim.load_from_mission_data(mission))
	sim.resolve_item_traits(item_db)
	sim.step()
	var actual := _present_miniguns(sim)
	assert_gte(actual.size(), expected.size(),
			"the four attached Blackhawk miniguns reached the listen client")

	var unused := actual.duplicate()
	for wanted in expected:
		var nearest_index := -1
		var nearest_distance := INF
		for index in range(unused.size()):
			var distance: float = (unused[index]["position"] as Vector3).distance_to(
					wanted["position"] as Vector3)
			if distance < nearest_distance:
				nearest_distance = distance
				nearest_index = index
		assert_gte(nearest_index, 0, "%s has a presented minigun" % wanted["label"])
		if nearest_index < 0:
			continue
		var child: Dictionary = unused.pop_at(nearest_index)
		assert_lt(nearest_distance, 0.05,
				"%s remains attached at its authored anchor" % wanted["label"])
		var actual_basis := MissionObjectPlacer.bms_to_godot_basis(
				child["rotation"] as Vector3)
		var actual_forward := (actual_basis * Vector3.BACK).normalized()
		var alignment := actual_forward.dot(wanted["forward"] as Vector3)
		assert_gt(alignment, 0.999,
				"%s minigun forward follows its own authored userpoint (dot %.6f)" % [
						wanted["label"], alignment])


func test_mrk5_nonplanar_anchors_use_the_retail_row_matrix_frame() -> void:
	var install_dir := RetailData.install()
	if install_dir.is_empty():
		pending("OPENNOVA_JO_DIR / retail JO PFFs are required for the MRK5 witness")
		return

	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(install_dir, "", false, "jo"), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	assert_eq(String(item_db.get_graphic(MRK5_ITEM_ID)), MRK5_GRAPHIC)
	var carrier_card := item_db.extract_seat_specs_for_item(root, MRK5_ITEM_ID)
	assert_eq(carrier_card.get_emplacement_attachments().size(), 4,
			"the shipped MRK5 has four attachment anchors")
	var data := ObjectData.new()
	var open_err := data.open_from_resource_root(root, MRK5_GRAPHIC + ".3di")
	assert_eq(open_err, OK)
	if open_err != OK:
		return
	var attachments := _authored_attachment_anchors(item_db, MRK5_ITEM_ID, data)
	assert_eq(attachments.size(), 4)
	var child_types := {}
	for raw in attachments:
		var attachment: Dictionary = raw
		child_types[int(attachment.get("item_id", 0)) - 100000] = true

	var rest_parts := data.evaluate_panm(0, 0, {})
	var live_parts := data.evaluate_panm(0, 16, {})
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var carrier_position := Vector3(7.0, -3.0, 2.0)
	var carrier_rotation := Vector3(0.0, 37.0, 0.0)
	assert_not_null(mission.add_entity(
			MissionData.KIND_ITEM, MRK5_ITEM_ID,
			carrier_position, carrier_rotation))
	var carrier_xform := MissionObjectPlacer.entity_transform(
			carrier_position, carrier_rotation)
	var expected: Array = []
	var nonplanar_count := 0
	for raw in attachments:
		var attachment: Dictionary = raw
		var part := int(attachment.get("subobject", -1))
		assert_true(rest_parts.has(part) and live_parts.has(part))
		if not rest_parts.has(part) or not live_parts.has(part):
			continue
		var rest: Transform3D = rest_parts[part]
		var live: Transform3D = live_parts[part]
		var authored_position: Vector3 = attachment.get(
				"raw_position", Vector3.ZERO)
		var authored_direction: Vector3 = attachment.get(
				"raw_rotation", Vector3.ZERO)
		if absf(authored_direction.y) > 0.01:
			nonplanar_count += 1
		expected.append({
			"label": String(attachment.get("source_name", "")),
			"position": carrier_xform * (
					live * (rest.affine_inverse() * authored_position)),
			"basis": (
					carrier_xform.basis
					* live.basis
					* rest.basis.inverse()
					* _retail_attachment_basis(authored_direction)
					).orthonormalized(),
		})
	assert_gt(nonplanar_count, 0,
			"the MRK5 fixture distinguishes row-matrix conversion from yaw-only data")

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	sim.set_asset_root(root)
	assert_true(sim.install_seat_specs_for_type_ids(
			item_db, PackedInt32Array([MRK5_TYPE_ID])))
	assert_gt(sim.get_mounted_graphic_source_count(), 0,
			"the native install fed the mounted-pose resolver")
	assert_true(sim.load_from_mission_data(mission))
	sim.resolve_item_traits(item_db)
	sim.step()
	var rows := _synthetic_attachment_rows(sim, child_types)
	assert_eq(rows.size(), attachments.size())
	var unused: Array = rows.values()
	for wanted in expected:
		var nearest_index := -1
		var nearest_distance := INF
		for index in range(unused.size()):
			var distance: float = (unused[index]["position"] as Vector3).distance_to(
					wanted["position"] as Vector3)
			if distance < nearest_distance:
				nearest_distance = distance
				nearest_index = index
		assert_gte(nearest_index, 0, "%s has a presented child" % wanted["label"])
		if nearest_index < 0:
			continue
		var child: Dictionary = unused.pop_at(nearest_index)
		assert_lt(nearest_distance, 0.05)
		var actual_basis := MissionObjectPlacer.bms_to_godot_basis(
				child["rotation"] as Vector3)
		var expected_basis: Basis = wanted["basis"]
		assert_gt(
				(actual_basis * Vector3.BACK).dot(
						expected_basis * Vector3.BACK),
				0.9998,
				"%s forward follows the converted retail frame" % wanted["label"])
		assert_gt(
				(actual_basis * Vector3.UP).dot(expected_basis * Vector3.UP),
				0.9998,
				"%s up follows the converted retail frame" % wanted["label"])


func test_real_blackhawk_rotor_register_spins_while_crewed() -> void:
	# The rotor spin machine: a player-control item seeds its rate while the
	# engine-running occupant latch holds, the speed ramps and the angle
	# accumulates; the cveh callback publishes the angle's high word as
	# HELO_ROTOR / HELO_TAILROTOR and the present applier stores it on the
	# carrier model [orig: Entity_UpdatePartSpinAccumulator @0x4928B0;
	#  Entity_CacheVehicleHUDStats @0x492ACA..0x492ADE].
	var install_dir := RetailData.install()
	if install_dir.is_empty():
		pending("OPENNOVA_JO_DIR / retail JO PFFs are required for the Blackhawk witness")
		return
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(install_dir, "", false, "jo"), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	assert_eq(String(item_db.get_graphic(CARRIER_ITEM_ID)), CARRIER_GRAPHIC)

	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var placed := mission.add_entity(
			MissionData.KIND_ITEM, CARRIER_ITEM_ID,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_not_null(placed)
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := MissionObjectPlacer.create(root, item_db)
	placer.place(mission, container)
	var mission_objects := container.get_node_or_null("MissionObjects") as Node3D
	assert_not_null(mission_objects)
	if mission_objects == null:
		return
	var rt = MissionRoot.new()
	add_child_autofree(rt)
	var model_options := MissionSetupOptions.new()
	model_options.resource_root = root
	model_options.item_db = item_db
	model_options.placer = placer
	model_options.playable = true
	assert_gt(int(rt.setup(mission, mission_objects, model_options)), 0)
	assert_true(rt.tick())
	var carrier_model := rt.get_entity_index().resolve(
			placed.bms_id,
			MissionData.KIND_ITEM,
			placed.index) as ObjectModel
	assert_not_null(carrier_model, "the Blackhawk resolves through the placed registry")
	if carrier_model == null:
		return
	assert_eq(int(carrier_model.get_ctrl_values().get("HELO_ROTOR", 0)), 0,
			"an uncrewed Blackhawk's rotor rests at zero")

	# The mount command seats the player through the best-seat pick, which
	# prefers the ctrlx/drvrx control seat [orig: Entity_FindBestSeatSlot
	# @0x4351f0 — ctrlx/drvrx 0x2000 below sitex 0x200000, lowest wins
	# @0x4353ee..0x43540e]; the USE toggle's nearest-seat scan can land on a
	# passenger bench, and a passenger never claims the engine-start latch
	# (+0x170 — retail: Entity_AttachToVehicleSlot @0x4946d0 claims it for
	# ctrlx/drvrx only), so the rotor would rest in retail too.
	assert_eq(rt.get_sim().debug_crew_local_player(placed.bms_id), OK,
			"the local player takes the Blackhawk's control seat")
	rt.play()
	for _tick in range(62):
		var input := MissionFrameInput.new()
		input.delta_seconds = Simulation.tick_dt()
		assert_true(rt.advance_session_frame(input).did_tick())
	var card: EntityCard = rt.get_sim().entity_card_by_ai_index(0)
	assert_eq(card.get_item_id(), CARRIER_TYPE_ID,
			"AI card 0 is the Blackhawk's brain")
	assert_eq(card.get_profile_type(), 1,
			"a bare-placed Blackhawk still takes the type-HELO helo1 profile")
	assert_true(card.has_primary_occupant(),
			"the pilot claimed the engine-start latch")
	var ctrls: Dictionary = carrier_model.get_ctrl_values()
	assert_true(ctrls.has("HELO_ROTOR"),
			"the crewed carrier owns the HELO_ROTOR register")
	# The listen host runs the shared aircraft mover for a player-crewed
	# helicopter (ai_system.cpp: the direct-air-mover leg's PLAYER pilot arm ->
	# aircraft_client_tick, whose tail is vehicle_part_anim_tick), so a crewed
	# Blackhawk's rotor register MUST advance here. A zero is a regression,
	# never a pending: [orig: Entity_UpdateAircraftPhysics @0x490310 -> the
	# HELO rotor twin Entity_UpdateHeloRotorSpin @0x48FA70 at its tail
	# @0x4905A6]. The twin runs only for a brain whose profile is type HELO
	# (the `profile+0x10 == 1` gate @0x48fa98), and this Blackhawk authors no
	# ai_textfile — retail's helicopter init falls back to "helo1.aip"
	# [orig: Entity_InitHelicopterAIFromDef @0x4683C0, the helo1 arm
	# @0x4684c9], which the boot resolver + promote reproduce, so the register
	# spins without an authored profile exactly as a bare-placed retail
	# Blackhawk's does.
	assert_gt(int(ctrls.get("HELO_ROTOR", 0)), 0,
			"the rotor angle's high word advanced while crewed")
	assert_eq(int(ctrls.get("HELO_TAILROTOR", -1)), int(ctrls.get("HELO_ROTOR", 0)),
			"both rotor ordinals publish the one +0x464 accumulator word")


func test_real_dbuggy_attachment_nodes_follow_when_driven() -> void:
	var install_dir := RetailData.install()
	if install_dir.is_empty():
		pending("OPENNOVA_JO_DIR / retail JO PFFs are required for the DBuggy witness")
		return

	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(install_dir, "", false, "jo"), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	assert_eq(String(item_db.get_graphic(DBUGGY_ITEM_ID)), DBUGGY_GRAPHIC)
	var carrier_card := item_db.extract_seat_specs_for_item(root, DBUGGY_ITEM_ID)
	assert_gt(carrier_card.get_emplacement_attachments().size(), 0,
			"the shipped DBuggy authors at least one child emplacement")
	var data := ObjectData.new()
	var open_err := data.open_from_resource_root(root, DBUGGY_GRAPHIC + ".3di")
	assert_eq(open_err, OK)
	if open_err != OK:
		return
	var attachments := _authored_attachment_anchors(item_db, DBUGGY_ITEM_ID, data)
	assert_eq(attachments.size(), carrier_card.get_emplacement_attachments().size(),
			"the native extraction carries every authored DBuggy attachment")
	var child_types := {}
	var attachment_by_type := {}
	for raw in attachments:
		var attachment: Dictionary = raw
		var child_type := int(attachment.get("item_id", 0)) - 100000
		child_types[child_type] = true
		attachment_by_type[child_type] = attachment

	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var placed := mission.add_entity(
			MissionData.KIND_ITEM, DBUGGY_ITEM_ID,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_not_null(placed)
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := MissionObjectPlacer.create(root, item_db)
	var placement_stats := placer.place(mission, container)
	assert_eq(placement_stats.animated, 1,
			"the driven DBuggy has an individually presentable model")
	var mission_objects := container.get_node_or_null(
			"MissionObjects") as Node3D
	assert_not_null(mission_objects)
	if mission_objects == null:
		return

	var rt = MissionRoot.new()
	add_child_autofree(rt)
	var node_options := MissionSetupOptions.new()
	node_options.resource_root = root
	node_options.item_db = item_db
	node_options.placer = placer
	node_options.playable = true
	assert_gt(int(rt.setup(mission, mission_objects, node_options)), 0)
	assert_true(rt.tick())
	var carrier_node := rt.get_entity_index().resolve(
			placed.bms_id,
			MissionData.KIND_ITEM,
			placed.index) as Node3D
	assert_not_null(carrier_node, "the DBuggy model resolves through the placed registry")
	var before_rows := _synthetic_attachment_rows(rt.get_sim(), child_types)
	assert_eq(before_rows.size(), attachments.size(),
			"every DBuggy attachment reaches the wire-present path")
	var relative_before := {}
	for handle_v in before_rows:
		var handle := int(handle_v)
		var row: Dictionary = before_rows[handle]
		var child_node := rt.get_entity_presenter().resolve_wire_handle(handle) as Node3D
		assert_not_null(child_node, "attachment %04x has a live model" % handle)
		if child_node != null and carrier_node != null:
			var carrier_local := (
					carrier_node.global_transform.affine_inverse()
					* child_node.global_transform)
			relative_before[handle] = carrier_local
			var attachment: Dictionary = attachment_by_type.get(
					row.type_id, {})
			var authored_forward: Vector3 = attachment.get(
					"raw_rotation", Vector3.ZERO)
			assert_gt(authored_forward.length_squared(), 0.99,
					"the DBuggy attachment has its own authored direction")
			assert_gt(
					(carrier_local.basis * Vector3.BACK).normalized().dot(
							(_retail_attachment_basis(authored_forward)
							* Vector3.BACK).normalized()),
					0.999,
					"the already-correct DBuggy direction is not globally flipped")

	var carrier_before := (
			carrier_node.global_transform
			if carrier_node != null else Transform3D.IDENTITY)
	assert_true(rt.get_sim().local_player_toggle_mount(),
			"the local player mounts the real DBuggy controller")
	# The drive rides the typed frame input: the session re-applies the
	# frame's movement before every tick (ADR 0035), so a latch deposited
	# on the sim is clobbered by the empty MissionFrameInput rt.tick() builds.
	rt.play()
	for _tick in range(124):
		var input := MissionFrameInput.new()
		input.delta_seconds = Simulation.tick_dt()
		input.set_movement(true, false, true, false, false, false, false)
		var outcome: MissionFrameOutcome = rt.advance_session_frame(input)
		assert_true(outcome != null and outcome.did_tick(),
				"each drive frame runs one logic tick")
	var carrier_after := (
			carrier_node.global_transform
			if carrier_node != null else Transform3D.IDENTITY)
	assert_gt(carrier_after.origin.distance_to(carrier_before.origin), 1.0,
			"the real DBuggy model moved under player control")
	# The wheel phase integrates the speed word every driven tick and the cveh
	# callback publishes its high word as VEHICLE_WHEELS, which the present
	# applier stores on the carrier model as an owned CTRL override
	# [orig: the +0x2B8 accumulate @0x48c4c5..0x48c4d0; the +0x2BA read in
	#  Entity_CacheVehicleHUDStats @0x4929B4].
	var carrier_model := carrier_node as ObjectModel
	assert_not_null(carrier_model, "the driven DBuggy presents as an ObjectModel")
	if carrier_model != null:
		var ctrls: Dictionary = carrier_model.get_ctrl_values()
		assert_true(ctrls.has("VEHICLE_WHEELS"),
				"the driven carrier owns the VEHICLE_WHEELS register")
		assert_ne(int(ctrls.get("VEHICLE_WHEELS", 0)), 0,
				"the wheel phase advanced under drive")
	assert_gt(
			(carrier_after.basis * Vector3.BACK).angle_to(
					carrier_before.basis * Vector3.BACK),
			deg_to_rad(5.0),
			"the real DBuggy model turned under steering input")
	for handle_v in relative_before:
		var handle := int(handle_v)
		var child_node := rt.get_entity_presenter().resolve_wire_handle(handle) as Node3D
		assert_not_null(child_node, "attachment %04x remains materialized" % handle)
		if child_node != null and carrier_node != null:
			var expected := carrier_after * (
					relative_before[handle] as Transform3D)
			assert_lt(
					child_node.global_position.distance_to(expected.origin),
					0.05,
					"attachment %04x follows the translated and turned DBuggy" % handle)
			assert_gt(
					(child_node.global_basis * Vector3.BACK).normalized().dot(
							(expected.basis * Vector3.BACK).normalized()),
					0.999,
					"attachment %04x follows the DBuggy heading" % handle)
			assert_gt(
					(child_node.global_basis * Vector3.UP).normalized().dot(
							(expected.basis * Vector3.UP).normalized()),
					0.999,
					"attachment %04x follows the DBuggy roll frame" % handle)
	# The mounted third-person camera (after the attachment checks, since the
	# settle frames below let the buggy coast on): in a control seat the chase
	# anchor is the carrier lifted max(1, 0.375 r), the eye backs off 1 + 1.5 r
	# (a longer reach than the on-foot 1.0 u), and the composed look-at toward
	# the 6 u look-ahead point looks DOWN on the vehicle from its -11.25 deg
	# seed [orig: ThirdPersonCamera_Update @0x437B1F..0x437B4B; the mounted leg
	#  of Camera_ComputeThirdPersonView @0x438121..0x438150, the look-ahead
	#  @0x438811..0x4388b5]. Nothing selects the mode here: boarding a control
	# seat with the default chase preference resolves third person by itself
	# [orig: the arbiter Render_ProcessMainSceneFrame @0x5ca1d2..0x5ca1f2 over
	#  g_camera_third_person_selected @0xA860DF, 1 from the session reset].
	for _tick in range(62):
		var settle := MissionFrameInput.new()
		settle.delta_seconds = Simulation.tick_dt()
		assert_true(rt.advance_session_frame(settle).did_tick())
	var view := rt.get_sim().get_local_player_view()
	assert_true(view.third_person,
			"a control seat resolves the chase camera without any camera write")
	assert_true(view.camera_mounted,
			"a control-seat rider engages the mounted camera leg")
	assert_true(view.camera_pose_valid)
	var eye := view.camera_eye
	var carrier_pos: Vector3 = carrier_node.global_position
	var horizontal := Vector2(eye.x - carrier_pos.x, eye.z - carrier_pos.z).length()
	assert_gt(horizontal, 1.5,
			"the mounted eye backs off further than the on-foot 1.0 u chase")
	assert_gt(eye.y, carrier_pos.y,
			"the mounted eye sits above the lifted carrier anchor")
	assert_lt(view.camera_pitch_deg, 0.0,
			"the mounted camera looks down on the vehicle")
