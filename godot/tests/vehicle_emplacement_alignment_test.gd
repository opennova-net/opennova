extends GutTest

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const ItemSeatSpecs := preload("res://engine/world/item_seat_specs.gd")

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


func _blackhawk_carriers(mission: NovaMissionData) -> Array:
	var carriers: Array = []
	for raw in mission.get_all_entities():
		var entity: Dictionary = raw
		if int(entity.get("item_id", 0)) == CARRIER_ITEM_ID:
			carriers.append(entity)
	return carriers


func _blackhawk_anchors(data: NovaObjectData) -> Array:
	var anchors: Array = []
	for wanted in ANCHOR_NAMES:
		for index in range(data.get_user_point_count()):
			var candidate: Dictionary = data.get_user_point_info(index)
			if String(candidate.get("name", "")).to_lower() == wanted:
				anchors.append(candidate)
				break
	return anchors


func _present_miniguns(sim: NovaSimulation) -> Array:
	var children: Array = []
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + NovaSimulation.PF_TYPE_ID]) != CHILD_TYPE_ID:
			continue
		children.append({
			"position": Vector3(
					snapshot[base + NovaSimulation.PF_POS_X],
					snapshot[base + NovaSimulation.PF_POS_Y],
					snapshot[base + NovaSimulation.PF_POS_Z]),
			"rotation": Vector3(
					snapshot[base + NovaSimulation.PF_PITCH_DEG],
					snapshot[base + NovaSimulation.PF_YAW_DEG],
					snapshot[base + NovaSimulation.PF_ROLL_DEG]),
		})
	return children


func _synthetic_attachment_rows(
		sim: NovaSimulation, child_types: Dictionary) -> Dictionary:
	var rows := {}
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		var type_id := int(snapshot[base + NovaSimulation.PF_TYPE_ID])
		if not child_types.has(type_id):
			continue
		if int(snapshot[base + NovaSimulation.PF_KIND]) != 255 \
				or int(snapshot[base + NovaSimulation.PF_INDEX]) != 0xFFFFFF:
			continue
		rows[int(snapshot[base + NovaSimulation.PF_WIRE_HANDLE])] = {
			"type_id": type_id,
			"position": Vector3(
					snapshot[base + NovaSimulation.PF_POS_X],
					snapshot[base + NovaSimulation.PF_POS_Y],
					snapshot[base + NovaSimulation.PF_POS_Z]),
			"rotation": Vector3(
					snapshot[base + NovaSimulation.PF_PITCH_DEG],
					snapshot[base + NovaSimulation.PF_YAW_DEG],
					snapshot[base + NovaSimulation.PF_ROLL_DEG]),
		}
	return rows


func test_03tr_blackhawk_miniguns_follow_authored_ewep_forward() -> void:
	var install_dir := OS.get_environment("OPENNOVA_JO_DIR").strip_edges()
	if install_dir.is_empty() or not DirAccess.dir_exists_absolute(install_dir):
		pending("OPENNOVA_JO_DIR / retail JO PFFs are required for the 03TR Blackhawk witness")
		return

	var root := NovaResourceRoot.new()
	assert_eq(root.mount_runtime(install_dir, "", false, "jo"), OK)
	var mission := NovaMissionData.new()
	assert_eq(mission.open_from_resource_root(root, MISSION), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	assert_eq(String(item_db.get_graphic(CARRIER_ITEM_ID)), CARRIER_GRAPHIC)

	var carriers := _blackhawk_carriers(mission)
	assert_eq(carriers.size(), 2, "03TR carries the two base Blackhawks")
	var data := NovaObjectData.new()
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
				carrier.get("position", Vector3.ZERO),
				carrier.get("rotation_deg", Vector3.ZERO))
		for anchor in anchors:
			var direction: Vector3 = anchor.get("rotation", Vector3.ZERO)
			assert_gt(direction.length_squared(), 0.99,
					"%s carries an authored forward direction" % anchor.get("name", ""))
			expected.append({
				"label": "SSN %d %s" % [
						int(carrier.get("bms_id", 0)),
						String(anchor.get("name", ""))],
				"position": carrier_xform * (
						anchor.get("position", Vector3.ZERO) as Vector3),
				"forward": (
						carrier_xform.basis
						* _retail_attachment_basis(direction)
						* Vector3.BACK).normalized(),
			})

	var carrier_spec := ItemSeatSpecs.seat_specs_for_item(
			root, item_db, CARRIER_ITEM_ID, CARRIER_TYPE_ID, true)
	assert_eq((carrier_spec.get("emplacement_attachments", []) as Array).size(), 2)
	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([carrier_spec])
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
	sim.free()


func test_mrk5_nonplanar_anchors_use_the_retail_row_matrix_frame() -> void:
	var install_dir := OS.get_environment("OPENNOVA_JO_DIR").strip_edges()
	if install_dir.is_empty() or not DirAccess.dir_exists_absolute(install_dir):
		pending("OPENNOVA_JO_DIR / retail JO PFFs are required for the MRK5 witness")
		return

	var root := NovaResourceRoot.new()
	assert_eq(root.mount_runtime(install_dir, "", false, "jo"), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	assert_eq(String(item_db.get_graphic(MRK5_ITEM_ID)), MRK5_GRAPHIC)
	var carrier_spec := ItemSeatSpecs.seat_specs_for_item(
			root, item_db, MRK5_ITEM_ID, MRK5_TYPE_ID, true)
	var attachments: Array = carrier_spec.get(
			"emplacement_attachments", []) as Array
	assert_eq(attachments.size(), 4, "the shipped MRK5 has four attachment anchors")
	var child_types := {}
	for raw in attachments:
		var attachment: Dictionary = raw
		child_types[int(attachment.get("item_id", 0)) - 100000] = true

	var data := carrier_spec.get("model_data") as NovaObjectData
	assert_not_null(data)
	if data == null:
		return
	var rest_parts := data.evaluate_panm(0, 0, {})
	var live_parts := data.evaluate_panm(0, 16, {})
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	var carrier_position := Vector3(7.0, -3.0, 2.0)
	var carrier_rotation := Vector3(0.0, 37.0, 0.0)
	assert_false(mission.add_entity(
			NovaMissionData.KIND_ITEM, MRK5_ITEM_ID,
			carrier_position, carrier_rotation).is_empty())
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

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	sim.set_item_seat_specs([carrier_spec])
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
	sim.free()


func test_real_dbuggy_attachment_nodes_follow_when_driven() -> void:
	var install_dir := OS.get_environment("OPENNOVA_JO_DIR").strip_edges()
	if install_dir.is_empty() or not DirAccess.dir_exists_absolute(install_dir):
		pending("OPENNOVA_JO_DIR / retail JO PFFs are required for the DBuggy witness")
		return

	var root := NovaResourceRoot.new()
	assert_eq(root.mount_runtime(install_dir, "", false, "jo"), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	assert_eq(String(item_db.get_graphic(DBUGGY_ITEM_ID)), DBUGGY_GRAPHIC)
	var carrier_spec := ItemSeatSpecs.seat_specs_for_item(
			root, item_db, DBUGGY_ITEM_ID, DBUGGY_TYPE_ID, true)
	var attachments: Array = carrier_spec.get(
			"emplacement_attachments", []) as Array
	assert_gt(attachments.size(), 0,
			"the shipped DBuggy authors at least one child emplacement")
	var child_types := {}
	var attachment_by_type := {}
	for raw in attachments:
		var attachment: Dictionary = raw
		var child_type := int(attachment.get("item_id", 0)) - 100000
		child_types[child_type] = true
		attachment_by_type[child_type] = attachment

	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	var placed := mission.add_entity(
			NovaMissionData.KIND_ITEM, DBUGGY_ITEM_ID,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_false(placed.is_empty())
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := MissionObjectPlacer.new(root, item_db)
	var placement_stats := placer.place(mission, container)
	assert_eq(int(placement_stats.get("animated", 0)), 1,
			"the driven DBuggy has an individually presentable model")
	var mission_objects := container.get_node_or_null(
			MissionObjectPlacer.CONTAINER_NAME) as Node3D
	assert_not_null(mission_objects)
	if mission_objects == null:
		return

	var rt = preload("res://engine/world/mission_runtime.gd").new()
	add_child_autofree(rt)
	assert_gt(int(rt.setup(mission, mission_objects, {
		"resource_root": root,
		"item_db": item_db,
		"placer": placer,
		"playable": true,
	})), 0)
	assert_true(rt.tick())
	var carrier_node := rt.get_registry().resolve(
			int(placed.get("bms_id", 0)),
			NovaMissionData.KIND_ITEM,
			int(placed.get("index", 0))) as Node3D
	assert_not_null(carrier_node, "the DBuggy model resolves through the placed registry")
	var before_rows := _synthetic_attachment_rows(rt.get_sim(), child_types)
	assert_eq(before_rows.size(), attachments.size(),
			"every DBuggy attachment reaches the wire-present path")
	var relative_before := {}
	for handle_v in before_rows:
		var handle := int(handle_v)
		var row: Dictionary = before_rows[handle]
		var child_node := rt.get_wire_presenter().resolve_wire_handle(handle) as Node3D
		assert_not_null(child_node, "attachment %04x has a live model" % handle)
		if child_node != null and carrier_node != null:
			var carrier_local := (
					carrier_node.global_transform.affine_inverse()
					* child_node.global_transform)
			relative_before[handle] = carrier_local
			var attachment: Dictionary = attachment_by_type.get(
					int(row.get("type_id", 0)), {})
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
	for _tick in range(124):
		rt.get_sim().set_player_input(
				true, false, true, false, false, false, false)
		assert_true(rt.tick())
	var carrier_after := (
			carrier_node.global_transform
			if carrier_node != null else Transform3D.IDENTITY)
	assert_gt(carrier_after.origin.distance_to(carrier_before.origin), 1.0,
			"the real DBuggy model moved under player control")
	assert_gt(
			(carrier_after.basis * Vector3.BACK).angle_to(
					carrier_before.basis * Vector3.BACK),
			deg_to_rad(5.0),
			"the real DBuggy model turned under steering input")
	for handle_v in relative_before:
		var handle := int(handle_v)
		var child_node := rt.get_wire_presenter().resolve_wire_handle(handle) as Node3D
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
