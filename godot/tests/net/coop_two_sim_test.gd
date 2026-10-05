extends GutTest


# Co-op LAN bidirectional bring-up (D.2) at the Simulation layer: a HOST listen server
# (enable_host_listen) and a JOINER (enable_join) run in the same headless process, each on a
# real loopback UDP socket (UdpPump), and free-run their own step() — no manual
# byte carry. This exercises the full witnessed JOIN end to end: the joiner handshakes
# (ClientHello/Auth), drives the spawn-gate burst, name-matches the host's S2C 0x0C organic
# spawn to learn its wire handle H, spawns its local player L, and uplinks C2S 0x0C; the host
# admits it and SNAPs it from the uplink. (The handshake bytes are unit-tested in libs —
# tests/novaworld/joiner_session_test; this is the Godot-layer glue + the two-handle present.)
#
# Most tests assert the DATA layer (the present snapshots both sides decode). The focused
# B50 aim regression additionally loads the authored mount.3di into a real ObjectModel
# and applies the decoded controls through the production presenter, so headless verifies
# the actual yaw-controlled ROBJ transform too. The §5.38b two-handle (L vs H)
# reconciliation shows up as: the host's present carries the joiner (SSN 0xFFEF), and the
# joiner's wire present carries the host player (type 0x14B9) while self-filtering its own echo H.


# A tiny world covering every pool the host streams: two Generic Soldier
# Persons (pool 0, type 0x14BF),
# one static building (pool 2, type 0x0123) and one marker (pool 3, type 0x1773). The AI carry a
# RESOLVABLE non-zero type id so they survive the present's `type_id != 0` filter — with the old
# item_id 0 they were invisible by construction, which is why the joiner only ever saw the host
# player. The witnessed initial-state burst streams EVERY entity pool to the joiner at world-load
# [orig: Server_SendInitialGameStateToPlayer @0x51bba0]: pool-2 statics (0x10), pool-1 (0x0D, omitted
# by our host — D-NET-97), pool-0 dynamics (0x0C), pool-3 markers (0x20). Terrain/environment/tile
# resources are resolved from the exact 0x0B header and optional 0x45 overlay; every allocated
# entity-pool row is streamed rather than reconstructed from a local BMS body.
const AI_TYPE := 0x14BF       # Generic Soldier (items.def id 105311, org1 Person)
const BUILDING_TYPE := 0x0123 # a static structure
const MARKER_TYPE := 0x1773   # a start marker
const SPAWN_ZONE_TYPE := 1359 # pool-1 fixture; ItemDef supplies SpawnPoint (0x40000)
# Objective Co-op's no-pick primary. Retail indexes the authored 6094 rows by
# player slot; it does not fall through to DM's 6002 family.
# [orig: Server_PositionPlayerForSpawn @0x50D1A7..0x50D201]
const OBJECTIVE_COOP_START_TYPE := 6094
# The USE scan admits a seat only inside the player's view cone (just under
# 90 deg standing, 5 deg seated); a peer that presses USE looks at the seat first.
const MountLook := preload("res://tests/support/mount_look.gd")

# S16 native seat tables: the Dictionary install seam is gone. Tests compose a
# flat asset dir under the gitignored res://.godot (ResourceRoot rejects
# user:// roots), wire it with sim.set_asset_root FIRST, then
# install_seat_specs_for_type_ids(item_db, ...) runs the ONE engine extractor
# (mission::extract_item_seat_specs) over items.def rows + .3di userpoints.
# carrierzero.3di is the committed carrier with its ctrlx13 seat local zeroed and
# the sitex rows retired: the corpus authors no zero-offset control seat, and
# the same-frame carrier-follow pin below compares L against the carrier root.
const NATIVE_MODEL_DIR := "res://.godot/native_3dp_coop_two_sim"


func before_all() -> void:
	DirAccess.make_dir_recursive_absolute(
			ProjectSettings.globalize_path(NATIVE_MODEL_DIR))
	assert_true(DirAccess.dir_exists_absolute(
			ProjectSettings.globalize_path(NATIVE_MODEL_DIR)),
			"created the flat native asset dir")
	assert_true(NativeModelFixture.write_native_model(NATIVE_MODEL_DIR, "mount.3di",
			NativeModelFixture.repo_file_bytes("res://../fixtures/threedi/synth/mount.3di")),
			"composed the mount native model fixture")
	var carrier := NativeModelFixture.with_user_point_position(
			NativeModelFixture.repo_file_bytes("res://../fixtures/threedi/synth/carrier.3di"),
			"ctrlx13", 0, 0, 0)
	for site in ["sitex00d", "sitex08c", "sitex06b", "sitex12a"]:
		carrier = NativeModelFixture.with_renamed_user_point(carrier, site, "x" + site.substr(1))
	assert_true(NativeModelFixture.write_native_model(NATIVE_MODEL_DIR, "carrierzero.3di", carrier),
			"composed the zero-offset control-seat carrier fixture")


func _native_asset_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path(NATIVE_MODEL_DIR)), OK)
	assert_true(root.has_file("mount.3di"),
			"the composed flat asset dir indexes the fixture models")
	return root


func _fixture_items_db() -> ItemDatabase:
	var def_root := ResourceRoot.new()
	assert_eq(def_root.set_root_dir(DefFixture.directory()), OK)
	var db := ItemDatabase.new()
	assert_eq(db.load_from_resource_root(def_root, "items.def"), OK)
	return db


func _attachment_items_db(anchor_name: String, ambiguous: bool) -> ItemDatabase:
	# The synthetic 5004 carrier, authored the native way: an items.def row
	# whose addeweap child anchors at the DISCOVERED articulated userpoint.
	# The ambiguous variant repeats the child type on a second missing-anchor
	# row (stored slot 2), the exact shape the old hand dictionary pinned.
	# Deliberately NO 101419 row: the child rides the parent's attachment
	# metadata only, so it lands no spec/seat of its own and the mount scan
	# can only take the carrier's Usegun — the old table's exact shape.
	var path := ProjectSettings.globalize_path(
			"res://.godot/coop_attachment_items_ambiguous.def" if ambiguous
			else "res://.godot/coop_attachment_items.def")
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return null
	file.store_string(TestFs.crlf("""begin "Attachment Fixture Carrier"
  id 105004
  graphic mount
  primary_weapon WPN_EMPLCD50NA
  addeweap %s 101419
""" % anchor_name))
	if ambiguous:
		file.store_string("  addeweap missing 101419\r\n")
	file.store_string("end\r\n")
	file.close()
	var db := ItemDatabase.new()
	assert_eq(db.load(path), OK)
	return db


func _combat_mission() -> MissionData:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	# Two independent north-facing fire lanes. Objective Co-op indexes 6094
	# starts by player slot, so the host takes x=20 and the joiner takes x=0.
	# Both yaw-zero players therefore
	# have a Generic Soldier down their own +mission-y lane (host at nine
	# metres, joiner at eight).
	assert_not_null(md.add_entity(MissionData.KIND_ORGANIC, 5311,
			Vector3(0, 8, 0), Vector3.ZERO))
	assert_not_null(md.add_entity(MissionData.KIND_ORGANIC, 5311,
			Vector3(20, 9, 0), Vector3.ZERO))
	assert_not_null(md.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(20, 0, 0), Vector3.ZERO))
	assert_not_null(md.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(0, 0, 0), Vector3.ZERO))
	return md


func _peer_duel_mission() -> MissionData:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	# Objective Co-op's slot-indexed start order puts the host at (0, 8) and the
	# joiner at (0, 0). Both
	# yaw-zero players face +mission-y, putting the host directly in the
	# joiner's fire lane without a debug teleport or invented aim override.
	assert_not_null(md.add_entity(MissionData.KIND_ORGANIC, 5311,
			Vector3(4, 0, 0), Vector3.ZERO))
	assert_not_null(md.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(0, 8, 0), Vector3.ZERO))
	assert_not_null(md.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(0, 0, 0), Vector3.ZERO))
	return md


func _vehicle_peer_mission() -> MissionData:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	# Keep the replicated vehicle well away from both deploy markers. The listen
	# host presents its authoritative placed row while the joiner presents the
	# decoded pool-1 wire row; their world poses must remain the same.
	assert_not_null(md.add_entity(MissionData.KIND_ITEM, 101291,
			Vector3(40, 30, 0), Vector3(10, 0, 20)))
	# A synthetic cbot with non-zero authored attitude catches first-arm
	# prediction accidentally replacing the retained spawn Euler with zero. It
	# sits beside the joiner start so the same real-UDP case can also exercise a
	# local control-seat body following the final predicted carrier pose.
	assert_not_null(md.add_entity(MissionData.KIND_ITEM, 105008,
			Vector3(2, 0, 0), Vector3(13, 0, -17)))
	# Slot order puts the host at (0, 8), then the joiner at (0, 0), within the
	# retail four-unit seat scan of the boat.
	assert_not_null(md.add_entity(MissionData.KIND_ORGANIC, 5311,
			Vector3(4, 0, 0), Vector3.ZERO))
	assert_not_null(md.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(0, 8, 0), Vector3.ZERO))
	assert_not_null(md.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(0, 0, 0), Vector3.ZERO))
	return md


func _net_watercraft_item_db() -> ItemDatabase:
	# resolve_item_traits sweeps every live entity, so this test database must be
	# a complete items table rather than a one-row replacement. Otherwise the
	# second resolve turns the mission's player and Dune Buggy rows Unknown.
	var base_items := ItemDbFixture.fixture_text(self)
	if base_items.is_empty():
		return null
	# The shared compact fixture intentionally omits most vehicle-physics fields.
	# This real-UDP case needs the Dune Buggy to materialize a VehicleTraits row so
	# it can prove that move_function cveh wins over ai_function chel.
	const DBUGGY_CALLBACK_BLOCK := "  ai_function chel\n  render_function cveh\n  move_function cveh\n"
	const DBUGGY_PHYSICS_BLOCK := DBUGGY_CALLBACK_BLOCK + \
			"  turn_rate 65\n  turn_rate2 41\n  acceleration 15\n" + \
			"  deceleration 70\n  player_speed 94\n  physics 1\n  torque 3\n"
	assert_true(base_items.contains(DBUGGY_CALLBACK_BLOCK))
	base_items = base_items.replace(DBUGGY_CALLBACK_BLOCK, DBUGGY_PHYSICS_BLOCK)
	# Native seat extraction reads the graphic's .3di userpoints: both drivable
	# rows ride the composed carrierzero model (one zero-offset ctrlx seat).
	assert_true(base_items.contains("  graphic Dbuggy1\n"))
	base_items = base_items.replace(
			"  graphic Dbuggy1\n", "  graphic carrierzero\n")
	return ItemDbFixture.load_with(self, "net_watercraft_items.def", base_items,
			"""begin "Net Watercraft Fixture"
  id 105008
  type vehicle
  graphic carrierzero
  sid netwatercraft
  ai_function cbot
  render_function cbot
  move_function cbot
  attrib: AIData neutral PlayerControl
  hp 3000
  turn_rate 65
  turn_rate2 41
  acceleration 15
  deceleration 70
  player_speed 94
  water_speed 94
  physics 1
  torque 3
end
""")


func _net_spawn_zone_item_db() -> ItemDatabase:
	# Keep the normal compact table intact, then add one deploy-selectable pool-1
	# row (ItemDbFixture.SPAWN_ZONE_ROW, ItemDef 101359).
	return ItemDbFixture.with_rows(self, "net_spawn_zone_items.def",
			ItemDbFixture.SPAWN_ZONE_ROW)


func _install_combat_tables(sim: Simulation) -> void:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(DefFixture.directory()), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	sim.resolve_item_traits(item_db)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	# Current-motor eyes and collision capsules require the native clip map.
	assert_gt(sim.set_infantry_anim_map(PresenterFixture.anim_root(self), "soldier.adm"), 0)


func _fixture_m4() -> WeaponDef:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(DefFixture.directory()), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(root, "weapon.def"), OK)
	var index := weapons.find_weapon("WPN_M4AUTO")
	assert_gte(index, 0)
	return weapons.get_weapon(index) if index >= 0 else null


func _fixture_m9() -> WeaponDef:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(DefFixture.directory()), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(root, "weapon.def"), OK)
	var index := weapons.find_weapon("WPN_M9Beretta")
	assert_gte(index, 0)
	return weapons.get_weapon(index) if index >= 0 else null


func _fixture_emplaced_50() -> WeaponDef:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(DefFixture.directory()), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(root, "weapon.def"), OK)
	var index := weapons.find_weapon("WPN_EMPLCD50NA")
	assert_gte(index, 0)
	return weapons.get_weapon(index) if index >= 0 else null


func _inventory_clip(sim: Simulation, weapon_name: String) -> int:
	for value in sim.get_local_player_inventory().slots:
		var slot: PlayerInventorySlot = value
		if slot.name == weapon_name:
			return slot.clip
	return -1


func _organic_index_at_x(sim: Simulation, x: float) -> int:
	for ai_index in range(sim.get_entity_count()):
		var card: EntityCard = sim.entity_card_by_ai_index(ai_index)
		if card == null:
			continue
		var item_id := card.get_item_id()
		if item_id != 5311 and item_id != 105311:
			continue
		var pos: Vector3 = card.get_position()
		if absf(pos.x - x) < 0.25:
			return ai_index
	return -1


func _drive_pair_to_match(host: Simulation, joiner: Simulation) -> bool:
	for _i in range(800):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match():
			# Give the recipient round watermark a quiet frame to arm so the
			# following shot is live traffic, never pre-join backlog.
			for _settle in range(8):
				host.step()
				joiner.step()
				OS.delay_msec(2)
			return true
		OS.delay_msec(2)
	return false


func _two_organics() -> MissionData:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, AI_TYPE, Vector3(0, 0, 0), Vector3.ZERO)    # KIND_ORGANIC (pool 0)
	md.add_entity(3, AI_TYPE, Vector3(10, 0, 0), Vector3.ZERO)
	md.add_entity(2, BUILDING_TYPE, Vector3(20, 0, 0), Vector3.ZERO) # KIND_BUILDING (pool 2)
	md.add_entity(0, MARKER_TYPE, Vector3(30, 0, 0), Vector3.ZERO)   # KIND_MARKER (pool 3)
	return md


func _two_organics_with_spawn_zone() -> MissionData:
	var md := _two_organics()
	var zone := md.add_entity(
			MissionData.KIND_ITEM, SPAWN_ZONE_TYPE,
			Vector3(40, 0, 0), Vector3.ZERO)
	assert_not_null(zone)
	if zone != null:
		assert_true(md.set_entity_property_int(
				MissionData.KIND_ITEM, zone.index, "team", 1))
	return md


func _present_position_for_type(sim: Simulation, type_id: int) -> Vector3:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == type_id:
			return Vector3(
					snapshot[base + Simulation.PF_POS_X],
					snapshot[base + Simulation.PF_POS_Y],
					snapshot[base + Simulation.PF_POS_Z])
	return Vector3.INF


func _present_field_for_type(sim: Simulation, type_id: int, field: int) -> int:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == type_id:
			return int(snapshot[base + field])
	return -1


func _present_record_for_type(sim: Simulation, type_id: int) -> Dictionary:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == type_id:
			return {"snapshot": snapshot, "base": base}
	return {}


func _moving_eweap_userpoint(data: ObjectData) -> Dictionary:
	var neutral: Dictionary = data.evaluate_panm(0, 0, {
		"EWEAP_GUNYAW": 0,
		"EWEAP_GUNPITCH": 0,
	})
	var turned: Dictionary = data.evaluate_panm(0, 0, {
		"EWEAP_GUNYAW": 16384,
		"EWEAP_GUNPITCH": 0,
	})
	for index in range(data.get_user_point_count()):
		var info := data.get_user_point_info(index)
		var part := info.subobject
		if not neutral.has(part) or not turned.has(part):
			continue
		var rest: Transform3D = neutral[part]
		var live: Transform3D = turned[part]
		var authored: Vector3 = info.position
		var point_in_part := rest.affine_inverse() * authored
		if (live * point_in_part).distance_to(authored) > 0.25:
			return {"index": index, "info": info}
	return {}


func test_joiner_learns_mission_before_wire_world_load_on_same_session() -> void:
	var mission := _two_organics()
	assert_true(mission.set_header_string("mission_name", "Preload Island"))

	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Preload Host"
	host_options.mission_name = "Preload Island"
	host_options.mission_file = "PRELOAD_A1.BMS"
	host_options.expansion = "jox01"
	host_options.game_type = 0x30020
	host_options.max_players = 4
	host_options.class_allow_mask = 0x0155
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "PreloadJoiner"))
	joiner.set_join_world_ready(false)

	# Retail authenticates before constructing the wire-header world. Drive only
	# that connection until post-auth 0x7B supplies map_file; no World exists yet and
	# the spawn/load drive remains held.
	var learned := false
	for _i in range(600):
		joiner.poll_join_preload()
		host.step()
		if joiner.has_join_mission():
			learned = true
			break
		OS.delay_msec(2)
	assert_true(learned, "joiner learned the host mission through the post-auth 0x7B")
	assert_eq(joiner.get_join_server_name(), "Preload Host")
	# Retail LAN duplicates g_MapFileName in 0x7B fields 4 AND 5 — field 4 is
	# NOT the MissionText display title, so the configured "Preload Island" can
	# never reach a LAN joiner pre-load. [orig: NapiNPMsg_0x7B_BuildPayload
	# @0x507740; the PR #403 live retail-LAN witness]
	assert_eq(joiner.get_join_mission_name(), "PRELOAD_A1.BMS")
	assert_eq(joiner.get_join_mission_file(), "PRELOAD_A1.BMS")
	assert_eq(joiner.get_join_game_type(), 0x30020)
	assert_eq(joiner.get_join_expansion(), "jox01")
	assert_false(joiner.is_loaded(), "metadata connect did not fabricate or pre-load a World")
	assert_false(joiner.is_joined_in_match(), "spawn drive waits for the wire-header world")
	assert_eq(host.get_host_peer_count(), 1, "one authenticated connection exists before load")

	# This low-level simulation fixture uses a complete mission object to construct
	# its World and release the same ClientRuntime/socket's world-ready gate; the
	# production GameWorld path supplies an exact header-only document instead.
	# A reconnect would create a second host node or lose the live session.
	assert_true(joiner.load_from_mission_data(mission))
	var reached := false
	for _i in range(800):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match():
			reached = true
			break
		OS.delay_msec(2)
	assert_true(reached, "the preloaded session resumed through spawn on the same socket")
	assert_eq(host.get_host_peer_count(), 1, "resume did not reconnect")
	assert_eq(joiner.get_class_allow_mask(), 0x0155,
			"the joiner consumed the host's S2C 0x76 class policy over real UDP")


func test_joiner_receives_the_host_hud_relays_over_real_udp() -> void:
	# BMS ShowWinSubgoal and SubGoalLost on the host: the authority relays S2C
	# 0x3F kind 0 (the objective notification, re-run by the joiner's fold as
	# the "objective" presentation effect) and kind 1 (the announcement's
	# mission-text key, posted through "mission_text_chat"). The joiner never
	# evaluates the mission events itself, so both effects can only come from
	# the relay. A repeating event re-runs both every processing pass so one
	# relay lands after the join.
	var mission := _two_organics()
	assert_gte(mission.add_event(1, 1, 0), 0)
	assert_true(mission.add_event_action(0, 35, 0, 2, 1)) # ShowWinSubgoal slot 2, shown
	assert_true(mission.add_event_action(0, 15, 0, 3)) # SubGoalLost slot 3
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "ObjectiveJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return
	var objective := false
	var chat_key := ""
	for _i in range(400):
		host.step()
		joiner.step()
		for e in joiner.drain_effects():
			var effect := e as MissionEffect
			if effect == null:
				continue
			if effect.kind == "objective" and effect.a == 2 and effect.b == 1:
				objective = true
			elif effect.kind == "mission_text_chat":
				chat_key = effect.text
		if objective and not chat_key.is_empty():
			break
		OS.delay_msec(2)
	assert_true(objective, "the joiner re-ran the host objective notification from S2C 0x3F")
	assert_true(chat_key.begins_with("STRLOSEMSG"),
			"the joiner received the SubGoalLost key from S2C 0x3F kind 1")


func test_both_peers_fold_the_visible_players_table_over_real_udp() -> void:
	# The join burst's C2S 0x23 and the host's 0x4D join fan drive the S2C 0x4C
	# snapshot on both sides: in a co-op (team) game each peer's table lists the
	# host's slot and the joiner's, and the map feed turns every entry into a
	# bit-5 loop-1 row.
	var mission := _two_organics()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "SlotJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return
	var joiner_rows := 0
	var host_rows := 0
	for _i in range(600):
		host.step()
		joiner.step()
		joiner_rows = joiner.get_hud_minimap_overlays(null).player_slot_count
		host_rows = host.get_hud_minimap_overlays(null).player_slot_count
		if joiner_rows >= 2 and host_rows >= 2:
			break
		OS.delay_msec(2)
	assert_eq(joiner_rows, 2, "the joiner's table lists both players")
	assert_eq(host_rows, 2, "the listen host's loopback table lists both players")


func test_joiner_folds_the_phase2_environment_into_its_weather_home() -> void:
	var mission := _two_organics()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	# WAC-equivalent commands on the host's weather home, deliberately unlike
	# every stock fixture. No map or game-type branch can manufacture these.
	assert_true(host.command_fog_distance(291))
	assert_true(host.command_sky_speed(170))
	assert_true(host.command_snow(0, 1))
	assert_true(host.command_quake(60))

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "EnvironmentJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	var received: EnvironmentSnapshot = null
	for _i in range(1200):
		host.step()
		joiner.step()
		received = joiner.get_environment_snapshot()
		if received != null and received.fog_target_metres == 291:
			break
		OS.delay_msec(2)

	assert_not_null(received, "the joiner exposes its environment record")
	if received == null:
		return
	assert_eq(received.fog_target_metres, 291,
			"the OpenNova joiner folds the host's scheduled phase-2 sample into its weather home")
	assert_eq(received.sky_speed_target, 170)
	assert_eq(received.precipitation_kind, 1)
	assert_gt(received.quake_ticks, 0,
			"the host's quake countdown reaches the joiner")


func test_joiner_handshakes_and_sees_host_bidirectional() -> void:
	var mission := _two_organics_with_spawn_zone()
	var host := Simulation.new()
	# Captured retail Co-op g_GameType: bit 0x20000 makes every phase-3
	# 0x0A carry a 16-byte objective block before the local-health tail.
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0), "host bound an OS-assigned UDP port")
	assert_true(host.load_from_mission_data(mission), "host promoted with the net seam")
	# A co-op host is playable — it spawns its own pool-0 player (0x14B9), which the joiner must
	# see over the wire. Without it the only player entity would be the joiner's own echo.
	assert_true(host.spawn_local_player(Vector3(5, 0, 5), 0.0, 1), "host spawned its own player")
	var host_anim_root := PresenterFixture.anim_root(self)
	assert_gt(host.set_infantry_anim_map(host_anim_root, "soldier.adm"), 0)
	var host_item_db := _net_spawn_zone_item_db()
	assert_not_null(host_item_db)
	host.resolve_item_traits(host_item_db)
	host.resolve_infantry_adm_ids(host_anim_root, host_item_db)
	var host_port: int = host.get_host_listen_port()
	assert_gt(host_port, 0, "host got a real bound port")

	var joiner := Simulation.new()
	assert_true(joiner.enable_join("127.0.0.1", host_port, "JoinerOne"), "joiner dialed the host")
	assert_true(joiner.is_joiner(), "joiner flag set before load")
	assert_eq(joiner.get_joiner_phase(), 0, "joiner phase Idle before the first frame")
	assert_true(joiner.load_from_mission_data(mission), "joiner promoted as a client")
	var joiner_anim_root := PresenterFixture.anim_root(self)
	assert_gt(joiner.set_infantry_anim_map(joiner_anim_root, "soldier.adm"), 0)
	var joiner_item_db := _net_spawn_zone_item_db()
	assert_not_null(joiner_item_db)
	joiner.resolve_item_traits(joiner_item_db)
	joiner.resolve_infantry_adm_ids(joiner_anim_root, joiner_item_db)

	var before_peers: int = host.get_host_peer_count()

	# Free-run both sims (real loopback UDP) until the joiner name-matches into the match.
	var reached := false
	for _i in range(800):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match():
			reached = true
			break
		OS.delay_msec(2)
	assert_true(reached, "joiner reached in-match (handshake -> spawn gate -> 0x0C name-match)")
	assert_eq(host.get_host_peer_count(), before_peers + 1, "host registered exactly one joiner peer")
	assert_true(joiner.has_local_player(), "joiner spawned its local player L at the H-learned pose")
	# Retail's initial join sends no C2S 0x0E: the initial 0x5A grant pair
	# completes admission and the joiner deploys directly (witnessed on the wire
	# against a live retail co-op host). The 0x0E spawn pick belongs to the DEATH
	# flow — drive it there through the authority's real death transaction.
	assert_false(joiner.is_join_deploy_pick_pending(),
			"the initial join deploys with no forced C2S 0x0E")
	var health_before_death := joiner.get_local_player_health()
	assert_gt(health_before_death, 0,
			"the initial authoritative spawn latch keeps local L alive")
	assert_true(_kill_joiner_from_host(host, joiner),
			"the authority's death transaction killed the joiner")
	var death_pick_pending := false
	for _i in range(240):
		host.step()
		joiner.step()
		if joiner.is_join_deploy_pick_pending():
			death_pick_pending = true
			break
		OS.delay_msec(2)
	assert_true(death_pick_pending,
			"the death edge re-arms the deploy pick (begin_redeployment)")
	# A death within 620 ticks of the deployment arms the host's 3-second pick
	# penalty (silently dropped picks); settle past it so the single zone pick
	# below is accepted.
	for _i in range(260):
		host.step()
		joiner.step()
	assert_true(joiner.is_join_deploy_pick_pending(),
			"nothing auto-picks while the death pick stays owed")
	var initial_position := joiner.get_local_player_position()
	var deploy_rows := joiner.get_deploy_spawn_zones()
	assert_eq(deploy_rows.size(), 1,
			"the fixture exposes one team-owned non-default spawn-zone row")
	var zone_param := (deploy_rows[0] as DeployZoneRow).param \
			if not deploy_rows.is_empty() else 0
	assert_gt(zone_param, 0)
	assert_true(joiner.send_deployment_pick(zone_param),
			"the displaced non-default spawn-zone pick was queued")
	# Deliberately do not step the host. Input case 12 has now queued C2S 0x0E
	# and re-armed dword_81474C, but that gameplay hold is not a death signal.
	joiner.step()
	joiner.step()
	assert_true(joiner.is_join_deploy_pick_pending(),
			"the deploy UI remains pending while the host has not handled the pick")
	var debug_host_own := host.get_local_player_wire_handle()
	var debug_host_remote_index := -1
	for debug_index in range(host.get_entity_count()):
		var debug_card: EntityCard = host.entity_card_by_ai_index(debug_index)
		if debug_card.get_item_id() == 0x14B9 \
				and debug_card.get_wire_handle() != debug_host_own:
			debug_host_remote_index = debug_index
			break
	var host_applied_zone_pose := false
	for _i in range(20):
		# The pick may have landed inside a retail send-holdoff interval. Pump the
		# sender immediately before the host, but never after it: once the host
		# applies the pick, its release is still unread on the joiner at this seam.
		joiner.step()
		host.step()
		if debug_host_remote_index >= 0:
			var host_pick_position: Vector3 = host.entity_card_by_ai_index(
					debug_host_remote_index).get_position()
			if absf(host_pick_position.x - 40.0) < 1.0:
				host_applied_zone_pose = true
				break
		OS.delay_msec(2)
	assert_true(host_applied_zone_pose,
			"the host applies the selected non-default zone before releasing the joiner")
	assert_true(joiner.is_join_deploy_pick_pending(),
			"the host pose is observable before the joiner folds the release")
	var death_pick_released := false
	var death_pick_pose_snapped := false
	for _i in range(160):
		host.step()
		joiner.step()
		death_pick_released = not joiner.is_join_deploy_pick_pending()
		death_pick_pose_snapped = \
				joiner.get_local_player_position().distance_to(initial_position) > 5.0
		if death_pick_released and death_pick_pose_snapped:
			break
		OS.delay_msec(2)
	assert_true(death_pick_released,
			"the ACK-qualified death-pick 0x5A retires the deploy-screen wait")
	assert_true(death_pick_pose_snapped,
			"the post-pick release snaps L to the host's displaced zone pose")
	assert_lt(absf(joiner.get_local_player_position().x - 40.0), 1.0,
			"the joiner adopts the selected host spawn-zone x coordinate")
	# The release reuses L; the following fresh positive recipient-health tail
	# revives it (the same revive mechanics the later default-pick leg pins).
	var revived_after_zone_pick := false
	for _i in range(240):
		host.step()
		joiner.step()
		if not joiner.is_local_player_dead() \
				and joiner.get_local_player_health() > 0:
			revived_after_zone_pick = true
			break
		OS.delay_msec(2)
	assert_true(revived_after_zone_pick,
			"the zone deploy revives local L for the gameplay legs below")
	var h: int = joiner.get_joiner_self_handle()
	assert_gt(h, 0, "joiner learned its wire handle H")

	# Drive the joiner forward + settle: its C2S 0x0C uplink reaches the host (SNAP), and the
	# host's S2C 0x0A carries everyone (incl. the host player + the joiner) back to the joiner.
	joiner.set_player_input(true, false, false, false, false, false, false)
	for _i in range(30):
		host.step()
		joiner.step()
		OS.delay_msec(2)

	var stride: int = host.get_present_stride()

	# HOST sees the JOINER: its client-decoded present carries the admitted joiner. [D-NET-112] Both
	# players carry net_id 0 (no SSN); a player is identified by its WIRE HANDLE, so the joiner is
	# the player-typed row (0x14B9, synthetic origin) whose handle is NOT the host's own handle.
	var host_own: int = host.get_local_player_wire_handle()
	var hsnap: PackedFloat32Array = host.get_present_snapshot()
	var host_sees_joiner := false
	var host_joiner_type := -1
	var host_joiner_kind := -1
	var host_joiner_index := -1
	for rec in range(hsnap.size() / stride):
		var base := rec * stride
		if int(hsnap[base + Simulation.PF_TYPE_ID]) == 0x14B9 \
				and int(hsnap[base + Simulation.PF_WIRE_HANDLE]) != host_own:
			host_sees_joiner = true
			host_joiner_type = int(hsnap[base + Simulation.PF_TYPE_ID])
			host_joiner_kind = int(hsnap[base + Simulation.PF_KIND])
			host_joiner_index = int(hsnap[base + Simulation.PF_INDEX])
	assert_true(host_sees_joiner, "host's present includes the admitted joiner (a player row that isn't the host's own)")
	# The per-entity attrib override is the authority's alone: the joiner's rows
	# are replicas the wire re-writes, so its seam refuses outright while the
	# host's accepts the same call on its own row.
	assert_eq(int(joiner.debug_set_entity_item_attrib(host_own, 0, 0)), ERR_UNAUTHORIZED,
			"a joiner never overrides an item attrib")
	var host_card: EntityCard = host.entity_card(host_own)
	assert_not_null(host_card)
	assert_eq(int(host.debug_set_entity_item_attrib(host_own,
			int(host_card.get_item_attrib()), int(host_card.get_item_attrib2()))), OK,
			"the host's seam accepts the override on its own player row")
	assert_eq(host_joiner_type, 0x14B9,
			"the host's joiner row resolves the player runtime type so the wire pass builds its avatar")
	# An admitted player has NO authored .bms identity: its row must carry the
	# none/synthetic spawn origin (kind 255, index 0xFFFFFF). The Entity default 0
	# reads as authored record (kind 0, index 0), which the host's wire present pass
	# DEFERS to a placed node that does not exist — the joiner avatar never builds.
	assert_eq(host_joiner_kind, 255,
			"the admitted joiner row carries the synthetic origin kind, not a fake authored identity")
	assert_eq(host_joiner_index, 0xFFFFFF,
			"the admitted joiner row carries the synthetic origin index sentinel")
	var host_remote_adm := ""
	var host_remote_index := -1
	for ai_index in range(host.get_entity_count()):
		var card: EntityCard = host.entity_card_by_ai_index(ai_index)
		if card.get_item_id() == 0x14B9 \
				and card.get_wire_handle() != host_own:
			host_remote_adm = card.get_adm_name()
			host_remote_index = ai_index
			break
	assert_eq(host_remote_adm, "US01.adm",
			"the real host admission path binds the remote player's body ADM")

	# JOINER sees the HOST: its wire-decoded present carries a player row (type 0x14B9) that is
	# NOT its own echo, and its own wire echo (handle H) is self-filtered out.
	var jsnap: PackedFloat32Array = joiner.get_present_snapshot()
	var joiner_sees_host := false
	var saw_self_echo := false
	var host_player_anim_state := -1
	var host_player_anim_phase := -2
	for rec in range(jsnap.size() / stride):
		var base := rec * stride
		var tid := int(jsnap[base + Simulation.PF_TYPE_ID])
		var whandle := int(jsnap[base + Simulation.PF_WIRE_HANDLE])
		if whandle == h:
			saw_self_echo = true
		if tid == 0x14B9 and whandle != h:
			joiner_sees_host = true
			host_player_anim_state = int(
					jsnap[base + Simulation.PF_ANIM_STATE])
			host_player_anim_phase = int(
					jsnap[base + Simulation.PF_ANIM_PHASE_TICKS])
	assert_true(joiner_sees_host, "joiner's wire present includes the host player (0x14B9), not itself")
	assert_false(saw_self_echo, "the joiner's own wire echo (handle H) is self-filtered from its present")
	assert_gte(host_player_anim_state, 0,
			"the host player's authoritative body state reaches presentation")
	assert_gte(host_player_anim_phase, 0,
			"the root-motion channel exposes its compact-seeded simulation playhead")
	var joiner_local_adm := ""
	for ai_index in range(joiner.get_entity_count()):
		var card: EntityCard = joiner.entity_card_by_ai_index(ai_index)
		# Joiner get_local_player_wire_handle() deliberately returns host identity H,
		# not local simulation handle L. Its local World contains only L as a player.
		if card.get_item_id() == 0x14B9:
			joiner_local_adm = card.get_adm_name()
			break
	assert_eq(joiner_local_adm, "US01.adm",
			"the real joiner name-match path binds local L's body ADM")

	# JOINER sees the host's full ENTITY world: the witnessed initial-state burst streams EVERY entity
	# pool to a joining player [orig: Server_SendInitialGameStateToPlayer @0x51bba0, sync-state 4] —
	# phase 1 pool-2 statics under S2C 0x10, phase 2 pool-1 under 0x0D, phase 3 pool-0 dynamics (AI +
	# players) under 0x0C, phase 4 pool-3 markers under 0x20 (each bounded by Pool_GetUsedCount(idx)).
	# So the joiner's present carries the host AI organics AND the pool-2 building. What is not an
	# entity batch is the header-named terrain/environment/tile resource set (plus the optional 0x45
	# overlay); pool-2 entity instances themselves DO stream. No local BMS body is opened.
	# (D-NET-98 corrected; net-re §5.38c.)
	var ai_seen := 0
	var ai_with_anim_state := 0
	var ai_with_sim_phase := 0
	var building_seen := false
	for rec in range(jsnap.size() / stride):
		var base := rec * stride
		var tid := int(jsnap[base + Simulation.PF_TYPE_ID])
		if tid == AI_TYPE:
			ai_seen += 1
			if int(jsnap[base + Simulation.PF_ANIM_STATE]) >= 0:
				ai_with_anim_state += 1
			if int(jsnap[base + Simulation.PF_ANIM_PHASE_TICKS]) >= 0:
				ai_with_sim_phase += 1
		elif tid == BUILDING_TYPE:
			building_seen = true
	assert_eq(ai_seen, 2, "joiner's present carries both host AI Persons (type 0x14BF) from the 0x0C stream")
	assert_eq(ai_with_anim_state, 2,
			"both compact infantry records carry their body state")
	assert_eq(ai_with_sim_phase, 2,
			"both armed AI Person rows present the simulation playhead that drives root motion")
	assert_true(building_seen, "pool-2 static building IS wire-streamed to the joiner under S2C 0x10 (the join burst's phase 1) [orig: @0x51bba0]")

	# The recipient-specific 0x0A tail, not the lossy self-echo H, owns the
	# joiner's health. Kill H on the authority and verify the same received
	# frame is applied to local motor entity L for HUD and death state.
	assert_gte(host_remote_index, 0, "host resolves the joiner's authoritative player H")
	assert_gt(joiner.get_local_player_health(), 0, "joiner starts alive before the authority kill")
	host.debug_set_entity_health(host_remote_index, 0)
	var death_arrived := false
	for _i in range(120):
		host.step()
		joiner.step()
		if joiner.get_local_player_health() == 0:
			death_arrived = true
			break
		OS.delay_msec(2)
	assert_true(death_arrived,
			"the authoritative 0x0A tail health reaches the joiner's local player L")
	var local_death_card: EntityCard = null
	for ai_index in range(joiner.get_entity_count()):
		var card: EntityCard = joiner.entity_card_by_ai_index(ai_index)
		if card.get_item_id() == 0x14B9:
			local_death_card = card
			break
	assert_not_null(local_death_card, "joiner retains its local player L after death")
	assert_eq(local_death_card.get_health(), 0)
	assert_eq(local_death_card.get_ai_health(), 0)
	assert_false(local_death_card.is_alive())

	# A positive tail without a new deploy edge is not a respawn. This also
	# protects the next infantry tick from hydrating its motor copy back to life.
	host.debug_set_entity_health(host_remote_index, 100)
	for _i in range(8):
		host.step()
		joiner.step()
		OS.delay_msec(2)
	var after_stale_positive: EntityCard = null
	for ai_index in range(joiner.get_entity_count()):
		var card: EntityCard = joiner.entity_card_by_ai_index(ai_index)
		if card.get_item_id() == 0x14B9:
			after_stale_positive = card
			break
	assert_eq(after_stale_positive.get_health(), 0)
	assert_eq(after_stale_positive.get_ai_health(), 0)
	assert_false(after_stale_positive.is_alive(),
			"positive health without a deploy edge cannot partially revive local L")
	# Restore the authority-side death predicate before requesting deployment.
	# debug_set_entity_health(100) deliberately made H alive to synthesize the
	# stale positive tail above; retail's 0x0E handler correctly rejects a pick
	# from an alive, non-pending player.
	host.debug_set_entity_health(host_remote_index, 0)
	for _i in range(3):
		host.step()
		joiner.step()
		OS.delay_msec(2)

	# The player's 0x0E pick is released by an ACK-qualified 0x5A. That edge
	# reuses L (rather than spawning a second local entity), but revival still
	# waits for the following fresh positive recipient-health tail.
	assert_true(joiner.is_join_deploy_pick_pending(),
			"death returned the established joiner to its deployment picker")
	assert_true(joiner.send_deployment_pick(0),
			"the default 0xFFFF redeployment pick was queued")
	var revived := false
	var revived_card: EntityCard = null
	for _i in range(240):
		host.step()
		joiner.step()
		for ai_index in range(joiner.get_entity_count()):
			var card: EntityCard = joiner.entity_card_by_ai_index(ai_index)
			if card.get_item_id() == 0x14B9:
				revived_card = card
				break
		if revived_card != null \
				and revived_card.is_alive() \
				and revived_card.get_health() > 0:
			revived = true
			break
		OS.delay_msec(2)
	assert_true(revived,
			"the post-pick release plus a later positive tail revives existing local L")
	assert_gt(revived_card.get_health(), 0)
	assert_eq(revived_card.get_ai_health(), revived_card.get_health(),
			"the registry and infantry motor health revive atomically")
	assert_false(joiner.is_join_deploy_pick_pending(),
			"the valid release retires the deploy-screen wait")
	assert_true(joiner.is_joined_in_match(),
			"the ACK-qualified release returns the connection to InMatch")

	# Look heading is a direct 0x0C extended-uplink field and therefore a
	# deterministic transport witness. Translational displacement also depends
	# on the local terrain/motor fixture and can remain stationary even while
	# every uplink is admitted and applied.
	var host_yaw_before_resume := float(
			host.entity_card_by_ai_index(host_remote_index).get_yaw_deg())
	joiner.set_local_player_mouse(511, false)
	joiner.add_local_player_look(500.0, 0.0)
	var uplink_resumed := false
	for _i in range(90):
		host.step()
		joiner.step()
		var remote_yaw := float(
				host.entity_card_by_ai_index(host_remote_index).get_yaw_deg())
		if absf(wrapf(remote_yaw - host_yaw_before_resume, -180.0, 180.0)) > 0.1:
			uplink_resumed = true
			break
		OS.delay_msec(2)
	assert_true(uplink_resumed,
			"after revival local L resumes 0x0C heading uplinks to the same authority H")



func test_joiner_reconstructs_eweap_attachment_userpoint_from_decoded_gunner() -> void:
	# A child hanging from an articulated EWEAP userpoint has no live compact of
	# its own. The remote client has enough stock wire state to recover this one
	# family: the parent's pose plus its decoded gunner's yaw/pitch. Generic
	# PLAYPARTANIM is intentionally not part of this contract.
	var model := ObjectData.new()
	assert_eq(model.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth/mount.3di")), OK)
	var moving_anchor := _moving_eweap_userpoint(model)
	assert_false(moving_anchor.is_empty(),
			"mount exposes a userpoint carried by EWEAP_GUNYAW")
	if moving_anchor.is_empty():
		return
	var anchor: ModelUserPoint = moving_anchor["info"]
	var anchor_name := anchor.name
	# This attachment-specific fixture deliberately keeps the parent and its
	# synthetic child on distinct wire types (the following UDP/presentation
	# test uses the real B50 item/type end to end). The authored addeweap row
	# anchors the child at the DISCOVERED articulated userpoint; the native
	# extractor derives the carrier's Usegun seat from the same mount graphic.
	var root := _native_asset_root()
	var attach_db := _attachment_items_db(anchor_name, false)
	assert_not_null(attach_db)
	if attach_db == null:
		return
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_not_null(mission.add_entity(
			MissionData.KIND_ITEM, 105004,
			Vector3(2, 0, 0), Vector3.ZERO))

	var host := Simulation.new()
	assert_true(host.enable_host_listen(0))
	host.set_asset_root(root)
	assert_true(host.install_seat_specs_for_type_ids(
			attach_db, PackedInt32Array([5004])))
	assert_gt(host.get_mounted_graphic_source_count(), 0,
			"the native install resolved the carrier model source")
	assert_true(host.load_from_mission_data(mission))
	var def_root := ResourceRoot.new()
	assert_eq(def_root.set_root_dir(DefFixture.directory()), OK)
	assert_eq(host.load_weapon_table(def_root, "weapon.def"), OK)
	assert_true(host.spawn_local_player(Vector3.ZERO, 120.0, 1))
	MountLook.face(self, host, Vector3(2, 0, 0))
	assert_true(host.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 1))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(DefFixture.directory().path_join("weapon.def")), OK)
	var personal: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var mounted: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50NA"))
	host.set_local_player_weapon(personal, {})
	host.drain_local_player_weapon_events()
	assert_true(host.local_player_toggle_mount())
	host.step()
	for raw in host.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_EMPLCD50NA":
			host.set_local_player_weapon(mounted, {}, true)
	assert_true(host.entity_card_by_ai_index(0).is_mounted(),
			"host player remains mounted after the local switch commit")

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "AttachmentJoiner"))
	joiner.set_asset_root(root)
	assert_true(joiner.install_seat_specs_for_type_ids(
			attach_db, PackedInt32Array([5004])))
	assert_true(joiner.load_from_mission_data(mission))
	var reached := false
	for _tick in range(800):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match():
			reached = true
			break
		OS.delay_msec(2)
	assert_true(reached, "joiner reached the in-match client view")
	if not reached:
		return
	assert_true(host.entity_card_by_ai_index(0).is_mounted(),
			"join handshake does not detach the host player")
	for _tick in range(20):
		host.step()
		joiner.step()
		OS.delay_msec(2)
	var host_before := _present_position_for_type(host, 1419)
	var joiner_before := _present_position_for_type(joiner, 1419)
	assert_true(host_before.is_finite() and joiner_before.is_finite(),
			"both client views materialized the synthetic child")

	host.set_local_player_mouse(511, false)
	host.add_local_player_look(500.0, 0.0)
	for _tick in range(40):
		host.step()
		joiner.step()
		OS.delay_msec(2)
	var host_after := _present_position_for_type(host, 1419)
	var joiner_after := _present_position_for_type(joiner, 1419)
	assert_eq(_present_field_for_type(joiner, 5004,
			Simulation.PF_EMPLACED_CONTROLS_VALID), 1,
			"joiner decoded the mounted gunner relationship")
	assert_ne(_present_field_for_type(joiner, 5004,
			Simulation.PF_EWEAP_GUNYAW), 0,
			"joiner derived a non-neutral EWEAP yaw control")
	assert_gt(host_after.distance_to(host_before), 0.05,
			"authoritative child follows the live EWEAP userpoint")
	assert_gt(joiner_after.distance_to(joiner_before), 0.05,
			"remote child does not remain at its rigid spawn offset")
	assert_lt(joiner_after.distance_to(host_after), 0.35,
			"decoded gunner controls reconstruct the remote articulated anchor")

	# The stock child record identifies parent + type, not the authored attachment
	# row. If that type occurs more than once, even when only one row resolved a
	# userpoint, a remote client must keep the spawn-derived rigid pose rather than
	# guess which sibling owns the decoded child.
	assert_true(joiner.install_seat_specs_for_type_ids(
			_attachment_items_db(anchor_name, true), PackedInt32Array([5004])))
	var ambiguous_position := _present_position_for_type(joiner, 1419)
	assert_lt(ambiguous_position.distance_to(joiner_before), 0.01,
			"ambiguous same-type children retain the rigid decoded fallback")


func test_joiner_mount_aim_and_detach_are_authoritative_over_real_udp() -> void:
	# This is the reported emplaced-gun failure at the actual ownership seam:
	# the player operating the B50 is the UDP joiner, not the listen host. The
	# local L entity waits for the host's 0x0A relationship echo, while the host
	# applies its uplink look to H and drives the parent's EWEAP controls.
	var model := ObjectData.new()
	assert_eq(model.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth/mount.3di")), OK)
	var moving_anchor := _moving_eweap_userpoint(model)
	assert_false(moving_anchor.is_empty(),
			"mount exposes a real ROBJ carried by EWEAP_GUNYAW")
	if moving_anchor.is_empty():
		return
	var yaw_part := (moving_anchor["info"] as ModelUserPoint).subobject
	# The real B50 item end to end: items.def row 101419 (graphic mount,
	# primary WPN_EMPLCD50NA) plus the model's authored Usegun userpoint,
	# through the ONE native extractor on both peers.
	var root := _native_asset_root()
	var seat_db := _fixture_items_db()
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var mounted_item := mission.add_entity(
			MissionData.KIND_ITEM, 101419,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_not_null(mounted_item)
	var mounted_bms_id := mounted_item.bms_id
	assert_gt(mounted_bms_id, 0)

	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.serve_and_play = false
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	host.set_asset_root(root)
	assert_true(host.install_seat_specs_for_type_ids(
			seat_db, PackedInt32Array([1419])))
	assert_gt(host.get_mounted_graphic_source_count(), 0,
			"the native install resolved the B50 model source")
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "MountedJoiner"))
	joiner.set_asset_root(root)
	assert_true(joiner.install_seat_specs_for_type_ids(
			seat_db, PackedInt32Array([1419])))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return
	var host_joiner_index := -1
	for ai_index in range(host.get_entity_count()):
		var card: EntityCard = host.entity_card_by_ai_index(ai_index)
		if card.get_item_id() == 0x14B9:
			host_joiner_index = ai_index
			break
	assert_gte(host_joiner_index, 0,
			"the authority exposes the admitted joiner entity H")

	var personal := _fixture_m4()
	var mounted := _fixture_emplaced_50()
	var weapon_defs := {
		"WPN_M4AUTO": personal,
		"WPN_EMPLCD50NA": mounted,
	}
	assert_true(joiner.apply_local_player_loadout(
			[WeaponKitEntry.make("WPN_M4AUTO")], 8))
	joiner.set_local_player_weapon(personal, {})
	for _settle in range(80):
		joiner.step()
		host.step()
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		if joiner.get_local_player_weapon_state().current_action < 2:
			break
		OS.delay_msec(1)
	assert_false(joiner.get_local_player_view().mounted)

	MountLook.face(self, joiner, Vector3(2, 0, 0))
	assert_true(joiner.local_player_toggle_mount(),
			"Shift queues the joiner's C2S 0x26 attach")
	assert_false(joiner.get_local_player_view().mounted,
			"attach is not locally predicted before the authority echo")
	var mounted_echoed := false
	for _tick in range(180):
		joiner.step()
		host.step()
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		if joiner.get_local_player_view().mounted \
				and host.entity_card_by_ai_index(host_joiner_index).is_mounted() \
				and _present_field_for_type(joiner, 1419,
						Simulation.PF_EMPLACED_CONTROLS_VALID) == 1:
			mounted_echoed = true
			break
		OS.delay_msec(2)
	assert_true(mounted_echoed,
			"host validated H's seat and 0x0A attached local L with active gun controls")
	if not mounted_echoed:
		return

	# Make the phase-8 witness deliberately non-default after both peers have
	# mounted. The authority mutates the real world MountSlot; only a decoded
	# flags2&0x0f==8 record can put these values into the joiner's active gun.
	assert_eq(host.debug_set_world_entity_weapon_ammo(
			mounted_bms_id, 7, 19), OK)
	var phase8_ammo_applied := false
	for _tick in range(96):
		host.step()
		joiner.step()
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		var joined_ammo := joiner.get_local_player_weapon_state()
		if joined_ammo.clip == 7 \
				and joined_ammo.reserve == 19:
			phase8_ammo_applied = true
			break
		OS.delay_msec(1)
	assert_true(phase8_ammo_applied,
			"joiner applies the authority's non-default phase-8 mounted ammo words")

	# Allow the borrowed emplaced slot's draw-in to settle, then drive look from
	# the joiner. This independently pins the reported "gun does not rotate"
	# path: C2S uplink -> host H heading -> parent EWEAP_GUNYAW -> S2C view.
	for _settle in range(100):
		joiner.step()
		host.step()
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		if joiner.get_local_player_weapon_state().current_action < 2:
			break
		OS.delay_msec(1)
	var authority_yaw_before := float(host.entity_card_by_ai_index(
			host_joiner_index).get_yaw_deg())
	var yaw_before := _present_field_for_type(
			joiner, 1419, Simulation.PF_EWEAP_GUNYAW)
	var visual: Node3D = add_child_autofree(ObjectModel.new())
	visual.set_object_data(model)
	var visual_parts: Dictionary = visual.get_render_part_nodes()
	assert_has(visual_parts, yaw_part,
			"the authored yaw-controlled B50 ROBJ exists in the rendered model")
	var visual_before_record := _present_record_for_type(joiner, 1419)
	assert_false(visual_before_record.is_empty(),
			"the joined client presents the real emplaced item type 1419")
	if not visual_parts.has(yaw_part) or visual_before_record.is_empty():
		return
	var visual_before_snap: PackedFloat32Array = visual_before_record["snapshot"]
	var visual_before_base := int(visual_before_record["base"])
	assert_eq(EntityPresenter.emplaced_apply(
			visual, visual_before_snap, visual_before_base, false), 3,
			"production presentation consumes the emplaced yaw, pitch and spin controls")
	var visual_yaw_before: Basis = (
			visual_parts[yaw_part] as Node3D).transform.basis
	joiner.set_local_player_mouse(511, false)
	joiner.add_local_player_look(500.0, 0.0)
	var aim_echoed := false
	for _tick in range(120):
		joiner.step()
		host.step()
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		var authority_yaw := float(host.entity_card_by_ai_index(
				host_joiner_index).get_yaw_deg())
		var presented_yaw := _present_field_for_type(
				joiner, 1419, Simulation.PF_EWEAP_GUNYAW)
		if absf(wrapf(authority_yaw - authority_yaw_before,
				-180.0, 180.0)) > 0.1 and presented_yaw != yaw_before:
			aim_echoed = true
			break
		OS.delay_msec(2)
	var yaw_after := _present_field_for_type(
			joiner, 1419, Simulation.PF_EWEAP_GUNYAW)
	assert_true(aim_echoed,
			"joiner look changed authority H and returned as visible gun articulation")
	assert_ne(yaw_after, yaw_before,
			"joiner look rotates the authoritative emplaced gun")
	assert_ne(yaw_after, 0,
			"the host publishes a non-neutral EWEAP yaw phase")
	assert_eq(_present_field_for_type(joiner, 1419,
			Simulation.PF_EMPLACED_CONTROLS_VALID), 1,
			"the joiner reconstructs the same active gunner controls")
	assert_ne(_present_field_for_type(joiner, 1419,
			Simulation.PF_EWEAP_GUNYAW), 0,
			"the joiner sees the rotated articulated gun")
	var visual_after_record := _present_record_for_type(joiner, 1419)
	assert_false(visual_after_record.is_empty())
	if not visual_after_record.is_empty():
		var visual_after_snap: PackedFloat32Array = visual_after_record["snapshot"]
		var visual_after_base := int(visual_after_record["base"])
		assert_eq(EntityPresenter.emplaced_apply(
				visual, visual_after_snap, visual_after_base, false), 3)
		var visual_yaw_after: Basis = (
				visual_parts[yaw_part] as Node3D).transform.basis
		var visual_yaw_delta_deg := rad_to_deg(
				visual_yaw_before.orthonormalized().get_rotation_quaternion().angle_to(
						visual_yaw_after.orthonormalized().get_rotation_quaternion()))
		assert_gt(visual_yaw_delta_deg, 0.1,
				"decoded network aim rotates the real B50 presentation ROBJ")

	for _settle in range(100):
		joiner.step()
		host.step()
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		if joiner.get_local_player_weapon_state().current_action < 2:
			break
		OS.delay_msec(1)
	assert_true(joiner.local_player_toggle_mount(),
			"a mounted Shift queues C2S 0x27 immediately")
	assert_true(joiner.get_local_player_view().mounted,
			"detach also waits for the authority echo")
	var detached_echoed := false
	for _tick in range(180):
		joiner.step()
		host.step()
		NativeModelFixture.apply_weapon_switch_events(joiner, weapon_defs)
		if not joiner.get_local_player_view().mounted \
				and not host.entity_card_by_ai_index(host_joiner_index).is_mounted():
			detached_echoed = true
			break
		OS.delay_msec(2)
	assert_true(detached_echoed,
			"host echo retires the local mount")
	# The ewep writer has no occupant test and nothing clears the gun words on
	# a detach: once the echoed seat change has landed, the emptied gun keeps
	# publishing its last traverse.
	# [orig: HUD_CacheWeaponSlotInfo @0x440930 via the 'ewep' render-class
	#  row @0x82CFA0]
	for _settle in range(60):
		joiner.step()
		host.step()
		OS.delay_msec(2)
	var held_yaw := _present_field_for_type(joiner, 1419, Simulation.PF_EWEAP_GUNYAW)
	for _hold in range(30):
		joiner.step()
		host.step()
		OS.delay_msec(2)
	assert_eq(_present_field_for_type(joiner, 1419,
			Simulation.PF_EMPLACED_CONTROLS_VALID), 1,
			"the emptied gun still publishes its controls")
	assert_eq(_present_field_for_type(joiner, 1419,
			Simulation.PF_EWEAP_GUNYAW), held_yaw,
			"the emptied gun holds its last traverse")
	assert_ne(held_yaw, 0,
			"the held traverse is the gunner's, not a return to rest")


func test_joiner_fire_and_reload_round_trip_over_real_udp() -> void:
	var mission := _combat_mission()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "CombatJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	# Apply before admission, like the shell: a later grant must not refill a
	# magazine while this test is exercising the fire/reload transaction.
	assert_true(joiner.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	joiner.set_local_player_weapon(_fixture_m4(), {})
	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	assert_eq(joiner.get_join_assigned_team(), 1,
			"the pre-spawn 0x04 advertises the co-op team the host entity received")
	for _settle in range(3):
		joiner.step()
		host.step()
	assert_eq(joiner.get_local_player_inventory().equipped_name, "WPN_M4AUTO",
			"the host's team-filtered 0x5A preserves the accepted co-op weapon")

	var host_target := _organic_index_at_x(host, 0.0)
	var joiner_target := _organic_index_at_x(joiner, 0.0)
	assert_gte(host_target, 0, "host resolved the joiner's north-lane target")
	assert_gte(joiner_target, 0, "joiner retained its visual copy of the target")
	var host_health_before := host.entity_card_by_ai_index(host_target).get_health()
	var joiner_health_before := joiner.entity_card_by_ai_index(joiner_target).get_health()
	var before_fire := joiner.get_local_player_weapon_state()
	var fired_before := before_fire.fired_serial
	joiner.drain_round_impacts()
	host.drain_round_impacts()
	joiner.drain_fire_presentation_events()

	joiner.set_local_player_weapon_input(false, true, false)
	var host_impacts: Array = []
	var joiner_impacts: Array = []
	var joiner_fire_events: Array = []
	for _tick in range(20):
		joiner.step()
		host.step()
		host_impacts.append_array(host.drain_round_impacts())
		joiner_impacts.append_array(joiner.drain_round_impacts())
		joiner_fire_events.append_array(joiner.drain_fire_presentation_events())
		OS.delay_msec(2)

	var after_fire := joiner.get_local_player_weapon_state()
	assert_eq(after_fire.fired_serial, fired_before + 1,
			"one local weapon action fired")
	assert_lt(host.entity_card_by_ai_index(host_target).get_health(),
			host_health_before,
			"the framed C2S 0x06 spawned a damaging authoritative host round")
	assert_eq(joiner.entity_card_by_ai_index(joiner_target).get_health(),
			joiner_health_before,
			"the joiner's predicted projectile is visual-only")
	assert_eq(host_impacts.size(), 1,
			"the authoritative projectile resolved one collision/impact")
	assert_eq(joiner_impacts.size(), 1,
			"the shooter predicted and resolved its own visual impact")
	assert_eq(joiner_fire_events.size(), 1,
			"one predicted fire presentation event was emitted")
	if joiner_fire_events.size() == 1:
		assert_true((joiner_fire_events[0] as FirePresentationEvent).is_local_player,
				"the predicted event maps local L even though wire attribution uses H")

	# The joiner has spent one round. One reload action must queue one C2S 0x25;
	# only the requester's echoed S2C 0x49 may refill it, and it is applied once.
	var spent_clip := after_fire.clip
	var reload_before := after_fire.reload_serial
	var applied_before := after_fire.reload_applied_serial
	assert_lt(spent_clip, 30, "the fire consumed one local magazine round")
	assert_eq(_inventory_clip(joiner, "WPN_M4AUTO"), spent_clip,
			"inventory readback exposes the spent shared ammo bucket")
	joiner.set_local_player_weapon_input(false, false, true)
	joiner.step()
	var awaiting_echo := joiner.get_local_player_weapon_state()
	assert_eq(awaiting_echo.reload_serial, reload_before + 1,
			"the local action queued one C2S 0x25 before the host pumped")
	assert_eq(awaiting_echo.reload_applied_serial, applied_before,
			"the request cannot eagerly apply its own refill")
	assert_eq(awaiting_echo.clip, spent_clip,
			"ammo remains spent until S2C 0x49 returns")
	for _tick in range(120):
		host.step()
		joiner.step()
		OS.delay_msec(2)
	var reloaded := joiner.get_local_player_weapon_state()
	assert_eq(reloaded.reload_serial, reload_before + 1,
			"one reload action queues exactly one C2S 0x25")
	assert_eq(reloaded.reload_applied_serial, applied_before + 1,
			"the echoed S2C 0x49 applies exactly once")
	assert_eq(reloaded.clip, 30,
			"only the echoed reload notification refills the joiner's clip")



# Kill the joiner's player entity on the AUTHORITY through the real death
# transaction (route_round_deaths) and pump until the joiner's recipient-local
# 0x0A tail reads dead. The kill targets the joiner's wire handle, so first
# pump until the 0x0C name-match binds it.
func _kill_joiner_from_host(host: Simulation, joiner: Simulation) -> bool:
	var self_bound := joiner.get_joiner_self_handle() > 0
	for _tick in range(400):
		if self_bound:
			break
		host.step()
		joiner.step()
		self_bound = joiner.get_joiner_self_handle() > 0
		OS.delay_msec(2)
	assert_true(self_bound, "the joiner bound its wire handle before the kill")
	assert_eq(host.debug_kill_player_entity(joiner.get_joiner_self_handle()), OK,
			"the host queued the joiner's death")
	for _tick in range(120):
		host.step()
		joiner.step()
		if joiner.is_local_player_dead():
			return true
		OS.delay_msec(2)
	return joiner.is_local_player_dead()


# The dead player's medic call: alive, the request is refused; dead, one
# call queues the reliable C2S 0x2E (the serial advances) and arms the
# 310-tick cooldown that refuses a second call until it runs out.
# [orig: Input_HandleActionBinding case 217 @0x49b4b4..0x49b51b;
#  Player_UpdatePerFrame @0x4de73e]
func test_joiner_medic_call_is_gated_on_death_and_the_310_tick_cooldown() -> void:
	var mission := _combat_mission()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)
	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "MedicJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return
	assert_false(joiner.is_local_player_dead(), "the joiner deploys alive")
	assert_false(joiner.request_local_player_medic(),
			"an alive player cannot call a medic")
	# The 0x81 hit-confirm edge is consume-once: nothing landed, nothing plays.
	assert_null(joiner.take_score_feedback(),
			"no score delta landed on the fresh joiner")
	assert_eq(joiner.local_medic_request_serial(), 0)

	assert_true(_kill_joiner_from_host(host, joiner),
			"the authority's rounds killed the joiner's entity")
	if not joiner.is_local_player_dead():
		return
	joiner.step()
	assert_true(joiner.request_local_player_medic(),
			"a dead joiner queues the medic call")
	assert_eq(joiner.local_medic_request_serial(), 1,
			"one call queues one C2S 0x2E")
	assert_eq(joiner.local_medic_request_cooldown_ticks(), 310,
			"the send arms the 310-tick cooldown")
	assert_false(joiner.request_local_player_medic(),
			"the cooldown refuses a second call")
	for _tick in range(20):
		host.step()
		joiner.step()
		OS.delay_msec(2)
	assert_eq(joiner.local_medic_request_cooldown_ticks(), 290,
			"the cooldown counts one per 62.5 Hz tick")
	assert_eq(joiner.local_medic_request_serial(), 1,
			"no second call rode the cooldown")


# The camera arbiter enters the death lerp camera (mode 4) on the joiner's
# death and leaves it when the deployment release brings the player back
# alive [orig: Render_ProcessMainSceneFrame @0x5ca217..0x5ca24b -> 4;
#  Camera_SetTrackedEntity @0x439257 computes the lerp; alive again -> 0].
func test_joiner_death_enters_the_lerp_camera_and_the_deploy_release_leaves_it() -> void:
	var mission := _combat_mission()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)
	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "CameraJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return
	assert_eq(joiner.get_local_player_view().camera_mode, 0,
			"alive on foot: first person")
	assert_true(_kill_joiner_from_host(host, joiner),
			"the authority's death transaction killed the joiner")
	if not joiner.is_local_player_dead():
		return
	joiner.step()
	var view := joiner.get_local_player_view()
	assert_eq(view.camera_mode, 4,
			"the dead joiner's arbiter resolves the death lerp camera")
	assert_true(view.camera_pose_valid,
			"mode 4 composes a camera pose")
	# The deployment release: pick the default spawn and pump until the host
	# releases; the respawned player is alive again -> first person.
	assert_true(joiner.is_join_deploy_pick_pending(),
			"death re-arms the deploy pick")
	# The host holds a fresh death's pick behind the +360/+364 penalty (three
	# seconds after a death within 620 ticks of the deployment) and silently
	# drops picks inside it, so re-click the default row like a player would.
	var alive := false
	for i in range(1200):
		if i % 64 == 0:
			joiner.send_deployment_pick(0)
		host.step()
		joiner.step()
		if not joiner.is_local_player_dead() and not joiner.is_join_deploy_pick_pending():
			alive = true
			break
		OS.delay_msec(1)
	assert_true(alive, "the release brought the joiner back alive")
	joiner.step()
	assert_eq(joiner.get_local_player_view().camera_mode, 0,
			"alive again: the arbiter returns to first person")


func test_joiner_pool1_vehicle_stays_at_authoritative_pose_over_real_udp() -> void:
	var mission := _vehicle_peer_mission()
	var watercraft_db := _net_watercraft_item_db()
	assert_not_null(watercraft_db)
	if watercraft_db == null:
		return
	# Both drivable rows (the placed Dune Buggy and the synthetic watercraft)
	# resolve their control seat natively from the carrierzero graphic: one
	# zero-offset ctrlx userpoint, the exact shape the old hand table carried.
	var root := _native_asset_root()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	host.set_asset_root(root)
	assert_true(host.install_seat_specs_for_type_ids(
			watercraft_db, PackedInt32Array([1291, 5008])))
	assert_gt(host.get_mounted_graphic_source_count(), 0,
			"the native install resolved the drivable model sources")
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)
	host.resolve_item_traits(watercraft_db)

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "VehicleObserver"))
	joiner.set_asset_root(root)
	assert_true(joiner.install_seat_specs_for_type_ids(
			watercraft_db, PackedInt32Array([1291, 5008])))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	joiner.resolve_item_traits(watercraft_db)
	assert_true(_drive_pair_to_match(host, joiner),
			"vehicle observer reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	var joiner_player: EntityCard = null
	for ai_index in range(joiner.get_entity_count()):
		var card: EntityCard = joiner.entity_card_by_ai_index(ai_index)
		if card.get_item_id() == 0x14B9:
			joiner_player = card
			break
	assert_not_null(joiner_player,
			"the joiner materializes its name-matched local player")
	if joiner_player != null:
		assert_eq(joiner_player.get_character_anim_slot(), 1,
				"the local player retains the host-stamped retail avatar selector")
		assert_eq(joiner_player.get_minimap_net_id(), 0x0200,
				"the local player retains the packed side-A character id")
		# [D-NET-112] The packed id is the wire NetId ONLY — L's SSN stays 0 like the host's
		# own player, so it never enters the WAC/BMS find_by_net_id space.
		assert_eq(joiner_player.get_net_id(), 0,
				"the local player carries no SSN (the packed id is not reused as one)")

	# Runtime type 1291 is items.def id 101291 after the BMS namespace strip.
	# Compare the two presentation feeds, not either local mission copy.
	for _settle in range(20):
		host.step()
		joiner.step()
		OS.delay_msec(2)
	var host_vehicle := _present_position_for_type(host, 1291)
	var joiner_vehicle := _present_position_for_type(joiner, 1291)
	assert_true(host_vehicle.is_finite(),
			"the authority publishes the placed dune buggy")
	assert_true(joiner_vehicle.is_finite(),
			"the joiner publishes the decoded pool-1 dune buggy")
	if host_vehicle.is_finite() and joiner_vehicle.is_finite():
		assert_lt(joiner_vehicle.distance_to(host_vehicle), 0.05,
				"the joiner vehicle does not collapse onto or float over its player")
		var player_pos: Vector3 = joiner.get_local_player_position()
		assert_gt(joiner_vehicle.distance_to(player_pos), 20.0,
				"the authored distant vehicle stays distant from the joiner")
	var host_record := _present_record_for_type(host, 1291)
	var joiner_record := _present_record_for_type(joiner, 1291)
	assert_false(host_record.is_empty(), "the host vehicle has a presentation row")
	assert_false(joiner_record.is_empty(), "the joiner vehicle has a presentation row")
	if not host_record.is_empty() and not joiner_record.is_empty():
		var host_snapshot: PackedFloat32Array = host_record["snapshot"]
		var joiner_snapshot: PackedFloat32Array = joiner_record["snapshot"]
		var host_base: int = host_record["base"]
		var joiner_base: int = joiner_record["base"]
		assert_almost_eq(
				joiner_snapshot[joiner_base + Simulation.PF_PITCH_DEG],
				host_snapshot[host_base + Simulation.PF_PITCH_DEG], 0.01,
				"the joiner publishes the decoded/predicted vehicle pitch")
		assert_almost_eq(
				joiner_snapshot[joiner_base + Simulation.PF_ROLL_DEG],
				host_snapshot[host_base + Simulation.PF_ROLL_DEG], 0.01,
				"the joiner publishes the decoded/predicted vehicle roll")
	# The physics family is the item's def trait, so it is read from the
	# placed row's own card. A placed item owns an AI brain only when its row
	# carries the AI-class attrib AND a brain-class ai_function at spawn
	# (Entity_SpawnFromBMSRecord's AIData test, then the class init), and
	# this fixture resolves its items after the load, so the buggy is not an
	# AI-pool row.
	var family := -1
	if not host_record.is_empty():
		var host_rows: PackedFloat32Array = host_record["snapshot"]
		var buggy_card: EntityCard = host.entity_card(
				int(host_rows[int(host_record["base"]) + Simulation.PF_WIRE_HANDLE]))
		if buggy_card != null:
			assert_eq(buggy_card.get_item_id(), 1291, "the presented row is the placed buggy")
			family = buggy_card.get_vehicle_family()
	assert_eq(family, 0,
			"the ai_function chel / move_function cveh Dune Buggy uses Ground physics")

	var host_boat_record := _present_record_for_type(host, 5008)
	var joiner_boat_record := _present_record_for_type(joiner, 5008)
	assert_false(host_boat_record.is_empty(),
			"the authority publishes the synthetic watercraft")
	assert_false(joiner_boat_record.is_empty(),
			"the joiner publishes the predicted synthetic watercraft")
	if not host_boat_record.is_empty() and not joiner_boat_record.is_empty():
		var host_boat: PackedFloat32Array = host_boat_record["snapshot"]
		var joiner_boat: PackedFloat32Array = joiner_boat_record["snapshot"]
		var host_boat_base := int(host_boat_record["base"])
		var joiner_boat_base := int(joiner_boat_record["base"])
		assert_ne(host_boat[host_boat_base + Simulation.PF_PITCH_DEG], 0.0,
				"the authored watercraft pitch is non-zero")
		assert_ne(host_boat[host_boat_base + Simulation.PF_ROLL_DEG], 0.0,
				"the authored watercraft roll is non-zero")
		assert_almost_eq(
				joiner_boat[joiner_boat_base + Simulation.PF_PITCH_DEG],
				host_boat[host_boat_base + Simulation.PF_PITCH_DEG], 0.01,
				"first prediction arms from the retained watercraft pitch")
		assert_almost_eq(
				joiner_boat[joiner_boat_base + Simulation.PF_ROLL_DEG],
				host_boat[host_boat_base + Simulation.PF_ROLL_DEG], 0.01,
				"first prediction arms from the retained watercraft roll")

	# Joiner L does not predict an attach: wait for the host relationship echo,
	# then drive the zero-offset control seat. On every frame where the predicted
	# carrier advances, joiner_pump must run its pose-only carrier follow after the
	# vehicle mover; the old pre-prediction pose trails by exactly one motor step.
	MountLook.face(self, joiner, Vector3(2, 0, 0))
	assert_true(joiner.local_player_toggle_mount(),
			"the nearby synthetic watercraft queues a real C2S attach")
	var mounted_echoed := false
	for _tick in range(180):
		joiner.step()
		host.step()
		if joiner.get_local_player_view().mounted:
			mounted_echoed = true
			break
		OS.delay_msec(2)
	assert_true(mounted_echoed,
			"the authority echoes the joiner's synthetic control-seat relationship")
	if mounted_echoed:
		joiner.set_player_input(true, false, false, false, false, false, false)
		var previous_vehicle := _present_position_for_type(joiner, 5008)
		var moving_frames := 0
		for _tick in range(180):
			joiner.step()
			host.step()
			var current_vehicle := _present_position_for_type(joiner, 5008)
			if current_vehicle.is_finite() and previous_vehicle.is_finite() \
					and current_vehicle.distance_to(previous_vehicle) > 0.00001:
				moving_frames += 1
				assert_lt(joiner.get_local_player_position().distance_to(
						current_vehicle), 0.001,
						"local L uses the carrier's final same-frame predicted pose")
			previous_vehicle = current_vehicle
			if moving_frames >= 8:
				break
			OS.delay_msec(2)
		assert_gte(moving_frames, 8,
				"the mounted watercraft produced enough predicted moving frames")
		joiner.set_player_input(false, false, false, false, false, false, false)



func test_joiner_rifle_fire_does_not_drive_the_remote_host_body_or_weapon() -> void:
	var mission := _peer_duel_mission()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "AnimIsolationJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"animation-isolation joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	assert_true(host.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	assert_true(joiner.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	host.set_local_player_weapon(_fixture_m4(), {})
	joiner.set_local_player_weapon(_fixture_m4(), {})
	for _settle in range(8):
		host.step()
		joiner.step()

	var remote_before := _present_record_for_type(joiner, 0x14B9)
	assert_false(remote_before.is_empty(),
			"the joiner presents the listen host as a remote player")
	if remote_before.is_empty():
		return
	var before_snap: PackedFloat32Array = remote_before["snapshot"]
	var before_base := int(remote_before["base"])
	var host_anim_before := int(before_snap[
			before_base + Simulation.PF_ANIM_STATE])
	var host_weapon_before := host.get_local_player_weapon_state()
	var host_fired_before := host_weapon_before.fired_serial
	# The viewmodel clip channel is SEPARATE from the fire channel: the first-person
	# parts are re-posed every tick from (anim_key, anim_variant, anim_advance_ticks) and
	# a play event bumps play_serial without necessarily bumping fired_serial
	# [play write site: Simulation weapon_fsm_tick play_anim leg]. A remote shot
	# that perturbs any of these makes the host's own gun re-scrub its clip.
	var host_play_before := host_weapon_before.play_serial
	var host_anim_key_before := host_weapon_before.anim_key
	var host_anim_variant_before := host_weapon_before.anim_variant
	var host_anim_advance_before := host_weapon_before.anim_advance_ticks
	var joiner_play_before := int(
			joiner.get_local_player_weapon_state().play_serial)
	var joiner_wire_handle := joiner.get_joiner_self_handle()
	var host_wire_handle := host.get_local_player_wire_handle()
	var joiner_player_position := joiner.get_local_player_position()
	var host_player_position := host.get_local_player_position()
	assert_gt(joiner_wire_handle, 0,
			"the shot source has the joiner's retail wire handle")
	assert_ne(joiner_wire_handle, host_wire_handle,
			"the two same-weapon players retain distinct wire identities")
	host.drain_fire_presentation_events()
	joiner.drain_fire_presentation_events()

	joiner.set_local_player_weapon_input(false, true, false)
	var observed_remote_states := {}
	var host_fire_events: Array = []
	var joiner_fire_events: Array = []
	for _tick in range(20):
		joiner.step()
		host.step()
		host_fire_events.append_array(host.drain_fire_presentation_events())
		joiner_fire_events.append_array(joiner.drain_fire_presentation_events())
		var remote := _present_record_for_type(joiner, 0x14B9)
		if not remote.is_empty():
			var snap: PackedFloat32Array = remote["snapshot"]
			var base := int(remote["base"])
			observed_remote_states[int(
					snap[base + Simulation.PF_ANIM_STATE])] = true
		OS.delay_msec(2)
	joiner.set_local_player_weapon_input(false, false, false)

	assert_eq(host.get_local_player_weapon_state().fired_serial, host_fired_before,
			"the joiner's C2S fire does not advance the host's local weapon FSM")
	# The viewmodel channel, asserted independently of the fire channel. Falsifiability
	# is carried by the joiner-side pin below: the shooter's OWN play channel must move
	# in the same window, so a run where nothing fired cannot satisfy both.
	var host_weapon_after := host.get_local_player_weapon_state()
	assert_eq(host_weapon_after.play_serial, host_play_before,
			"the joiner's shot does not play a clip on the host's own viewmodel")
	assert_eq(host_weapon_after.anim_key, host_anim_key_before,
			"the joiner's shot does not re-key the host's viewmodel clip")
	assert_eq(host_weapon_after.anim_variant, host_anim_variant_before,
			"the joiner's shot does not consume a variant from the host's clip ring")
	# anim_advance_ticks is the playhead the first-person parts are posed at every
	# tick (the counter-gated channel position). It must keep advancing with the
	# host's own pump; a remote shot that resets the advance count drops it back
	# toward zero (re-scrubbing the clip), and an unsigned wrap sends it huge
	# (clamping a one-shot to its tail).
	var host_anim_advance_after := host_weapon_after.anim_advance_ticks
	assert_gte(host_anim_advance_after, host_anim_advance_before,
			"the host's viewmodel playhead never rewinds when a remote player fires")
	assert_lt(host_anim_advance_after - host_anim_advance_before, 1000,
			"the host's viewmodel playhead advances by its own elapsed ticks, not a wrap")
	assert_gt(joiner.get_local_player_weapon_state().play_serial, joiner_play_before,
			"the SHOOTER's own viewmodel did play its fire clip (guards the pins above)")
	assert_eq(observed_remote_states.size(), 1,
			"the joiner's shot does not request a second body state on the remote host")
	assert_true(observed_remote_states.has(host_anim_before),
			"the remote host remains in its pre-shot body state")

	var authoritative_joiner_events: Array = []
	var misattributed_host_events: Array = []
	for value in host_fire_events:
		var event: FirePresentationEvent = value
		var shooter_handle := event.shooter_handle
		if shooter_handle == joiner_wire_handle:
			authoritative_joiner_events.append(event)
		elif shooter_handle == host_wire_handle:
			misattributed_host_events.append(event)
	assert_eq(authoritative_joiner_events.size(), 1,
			"the authority presents one shot for the packet's joiner handle")
	assert_true(misattributed_host_events.is_empty(),
			"the same equipped gun never reattributes the shot to the listen host")
	if authoritative_joiner_events.size() == 1:
		var authoritative: FirePresentationEvent = authoritative_joiner_events[0]
		assert_false(authoritative.is_local_player,
				"the authority treats the packet shooter as its remote joiner")
		var origin := authoritative.origin
		assert_lt(origin.distance_to(joiner_player_position), 3.0,
				"the authoritative muzzle event stays at the joiner's player")
		assert_gt(origin.distance_to(host_player_position), 5.0,
				"the authoritative muzzle event cannot partially drive the host player")

	var predicted_joiner_events: Array = []
	var remote_host_events: Array = []
	for value in joiner_fire_events:
		var event: FirePresentationEvent = value
		var shooter_handle := event.shooter_handle
		if shooter_handle == joiner_wire_handle:
			predicted_joiner_events.append(event)
		elif shooter_handle == host_wire_handle:
			remote_host_events.append(event)
	assert_eq(predicted_joiner_events.size(), 1,
			"the shooter presents its one locally predicted shot under its own handle")
	assert_true(remote_host_events.is_empty(),
			"the joiner never presents its own shot as a remote-host shot")
	if predicted_joiner_events.size() == 1:
		assert_true((predicted_joiner_events[0] as FirePresentationEvent).is_local_player,
				"wire attribution H resolves back to the joiner's local L")



func test_listen_host_reload_relays_over_loopback_without_double_refill() -> void:
	var mission := _combat_mission()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "HostReloadWitness"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	var m4 := _fixture_m4()
	assert_true(host.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	assert_true(joiner.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	host.set_local_player_weapon(m4, {})
	joiner.set_local_player_weapon(m4, {})
	for _settle in range(3):
		host.step()
		joiner.step()

	# Spend one host round, then wait for the local FSM to return to an input-
	# accepting state. The authority refill remains immediate, but retail also
	# queues the local player's C2S 0x25 through transport-mode-1 so every peer
	# receives the same S2C 0x49.
	host.set_local_player_weapon_input(false, true, false)
	for _tick in range(20):
		host.step()
		joiner.step()
		OS.delay_msec(2)
	var spent := host.get_local_player_weapon_state()
	assert_lt(spent.clip, 30,
			"the listen host spent a magazine round before reloading")
	var host_reload_before := spent.reload_serial
	var host_applied_before := spent.reload_applied_serial
	var expected_reload_combo := host.get_local_player_inventory().equipped_combo
	var joiner_received_before := joiner.get_local_player_weapon_state().reload_received_serial

	host.set_local_player_weapon_input(false, false, true)
	host.step()
	var immediate := host.get_local_player_weapon_state()
	assert_eq(immediate.reload_serial, host_reload_before + 1,
			"the listen-host action emitted one local reload request")
	assert_eq(immediate.reload_applied_serial,
			host_applied_before + 1,
			"authority applied the local refill once at action start")
	assert_eq(immediate.clip, 30,
			"the authority refill completed immediately")

	for _tick in range(30):
		host.step()
		joiner.step()
		OS.delay_msec(2)
	var host_after := host.get_local_player_weapon_state()
	var joiner_after := joiner.get_local_player_weapon_state()
	assert_eq(host_after.reload_serial, host_reload_before + 1,
			"the loopback echo does not start another host reload")
	assert_eq(host_after.reload_applied_serial,
			host_applied_before + 1,
			"the loopback S2C 0x49 does not refill the authority twice")
	assert_eq(host_after.clip, 30,
			"the host magazine remains full after its loopback echo")
	assert_eq(joiner_after.reload_received_serial,
			joiner_received_before + 1,
			"the remote peer decoded exactly one relayed S2C 0x49")
	assert_eq(joiner_after.reload_received_entity,
			host.get_local_player_wire_handle(),
			"the relay names the listen host's player entity")
	assert_eq(joiner_after.reload_received_param,
			expected_reload_combo,
			"the relay carries the retail category*65+rank slot combo")



func test_late_reload_echo_refills_payload_weapon_after_joiner_switches() -> void:
	# PENDING: the joiner reverts to its profile kit page on the deploy release
	# that follows a granted kit change (pre-round or death-screen armory), where
	# retail's Player_InitPlayer rebuilds from the restrictionData the last S2C
	# 0x5A wrote. Until that deploy-fold reseed is fixed, no retail-legal fixture
	# can hold a two-weapon kit past the join (a live, deployed player outside an
	# armory volume is answered with its CURRENT list, as retail does).
	pending("joiner deploy release re-seeds the kit from the profile page (follow-up)")
	if true:
		return
	var mission := _combat_mission()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	# A deployed, live player outside an armory volume who re-submits a kit is
	# answered with its CURRENT list by a retail host (Server_HandlePlayerLoadout's
	# dead / armory-zone / pre-round gate). The two-weapon kit is therefore chosen
	# during the pre-round window (host StartDelay), the way a retail player picks
	# a kit before the round opens; the round then starts and the joiner fires.
	host_options.start_delay = 4
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "LateReloadJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	assert_true(joiner.apply_local_player_loadout([
		WeaponKitEntry.make("WPN_M4AUTO"),
		WeaponKitEntry.make("WPN_M9Beretta"),
	], 8))
	var kit_granted := false
	for _i in range(60):
		joiner.step()
		host.step()
		if _inventory_clip(joiner, "WPN_M9Beretta") == 15:
			kit_granted = true
			break
		OS.delay_msec(2)
	assert_true(kit_granted, "the pre-round armory submit is granted (S2C 0x5A)")
	# Let the pre-round timer expire (StartDelay seconds of authority ticks) so
	# the joiner may fire; the granted body survives the round start.
	for _i in range(4 * 62 + 40):
		joiner.step()
		host.step()
	assert_eq(_inventory_clip(joiner, "WPN_M9Beretta"), 15,
			"the granted body survives the round start")

	joiner.set_local_player_weapon(_fixture_m4(), {})
	for _settle in range(3):
		joiner.step()
		host.step()

	# Spend one M4 round while it is equipped, then queue its reload without
	# pumping the host. The request can reach the host socket, but no S2C 0x49
	# can be produced until the authority is stepped below.
	joiner.set_local_player_weapon_input(false, true, false)
	for _tick in range(20):
		joiner.step()
		host.step()
		OS.delay_msec(2)
	joiner.set_local_player_weapon_input(false, false, false)
	var spent_m4_clip := _inventory_clip(joiner, "WPN_M4AUTO")
	assert_lt(spent_m4_clip, 30, "the M4 slot spent one magazine round")
	assert_eq(_inventory_clip(joiner, "WPN_M9Beretta"), 15,
			"the secondary starts with its independent full magazine")

	var before_reload := joiner.get_local_player_weapon_state()
	var reload_before := before_reload.reload_serial
	var applied_before := before_reload.reload_applied_serial
	joiner.set_local_player_weapon_input(false, false, true)
	joiner.step()
	joiner.set_local_player_weapon_input(false, false, false)
	assert_eq(joiner.get_local_player_weapon_state().reload_serial, reload_before + 1,
			"the M4 reload request was queued before the host pumped")
	assert_eq(joiner.get_local_player_weapon_state().reload_applied_serial, applied_before,
			"the joiner cannot apply the refill before the echo")

	# Let the first reload action reach its retail DONE seam, but intercept it
	# before the empty-magazine loop can enter another reload. A category edge
	# during the ACTIVE phase is intentionally an action-cancel request in the
	# retail switch writer; issuing it at DONE makes this test exercise a committed
	# weapon switch rather than that separate input-timing rule.
	var reload_finished := false
	for _tick in range(120):
		var reload_state := joiner.get_local_player_weapon_state()
		if (reload_state.current_action == 4
				and reload_state.phase == 4):
			reload_finished = true
			break
		joiner.step()
	assert_true(reload_finished, "the first M4 reload action reached DONE without an echo")
	assert_eq(joiner.get_local_player_weapon_state().reload_serial, reload_before + 1,
			"the delayed host pump has not started a second reload request")

	# Switch the authoritative inventory selection while the echo is withheld.
	# The echoed payload still names the M4 combo and must not refill whichever
	# weapon happens to be equipped when it arrives.
	joiner.request_local_player_weapon_category(2)
	var switched := false
	for _tick in range(120):
		joiner.step()
		if joiner.get_local_player_inventory().equipped_name == "WPN_M9Beretta":
			switched = true
			break
	assert_true(switched, "the joiner switched to its secondary before the echo")
	# Mirror LocalPlayerPresenter consuming the committed switch event: mount the
	# newly selected definition so later ticks bridge the M9 FSM to the M9 slot.
	joiner.set_local_player_weapon(_fixture_m9(), {})
	assert_eq(_inventory_clip(joiner, "WPN_M4AUTO"), spent_m4_clip,
			"the original M4 slot stays spent while its echo is withheld")
	assert_eq(_inventory_clip(joiner, "WPN_M9Beretta"), 15,
			"switching does not alter the secondary magazine")

	for _tick in range(120):
		host.step()
		joiner.step()
		OS.delay_msec(2)
	var after_echo := joiner.get_local_player_weapon_state()
	assert_eq(after_echo.reload_applied_serial, applied_before + 1,
			"the delayed S2C 0x49 applies exactly once")
	assert_eq(joiner.get_local_player_inventory().equipped_name, "WPN_M9Beretta",
			"the late echo does not change the selected weapon")
	assert_eq(_inventory_clip(joiner, "WPN_M4AUTO"), 30,
			"the payload-addressed M4 slot receives the delayed refill")
	assert_eq(_inventory_clip(joiner, "WPN_M9Beretta"), 15,
			"the currently equipped secondary is not refilled by the M4 echo")



func test_reload_echo_at_done_prevents_same_slot_reload_loop() -> void:
	var mission := _combat_mission()
	var host := Simulation.new()
	# This regression isolates receive-before-actions ordering, so deliver the
	# authority echo on the next test tick instead of waiting on retail cadence.
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host_options.send_holdoff_ticks = 1
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "DoneEchoJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	assert_true(joiner.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	joiner.set_local_player_weapon(_fixture_m4(), {})
	for _settle in range(3):
		joiner.step()
		host.step()

	# Empty the magazine through the real local FSM/C2S fire path. Stop pumping
	# the host on the exact joiner frame that queues the empty-slot reload, so its
	# one C2S 0x25 is present but the echoed S2C 0x49 cannot yet exist.
	var before := joiner.get_local_player_weapon_state()
	var reload_before := before.reload_serial
	var applied_before := before.reload_applied_serial
	joiner.set_local_player_weapon_input(true, true, false)
	var first_reload_queued := false
	for _tick in range(400):
		joiner.step()
		var state := joiner.get_local_player_weapon_state()
		if state.reload_serial == reload_before + 1:
			first_reload_queued = true
			break
		host.step()
		OS.delay_msec(2)
	joiner.set_local_player_weapon_input(false, false, false)
	assert_true(first_reload_queued,
			"emptying the M4 queued its first automatic C2S 0x25")
	assert_eq(joiner.get_local_player_weapon_state().clip, 0,
			"the regression reaches the empty-magazine reload path")
	assert_eq(joiner.get_local_player_weapon_state().reload_applied_serial, applied_before,
			"the withheld host cannot have echoed the refill")

	# Hold the authority still while the first local reload animation reaches its
	# DONE transition. The next joiner weapon pump would enter idle, observe the
	# still-empty clip, and queue another reload unless this frame's echo is folded
	# before weapon actions, as retail Client_ProcessNetworkFrame does.
	var reload_finished := false
	for _tick in range(160):
		var state := joiner.get_local_player_weapon_state()
		if state.current_action == 4 \
				and state.phase == 4:
			reload_finished = true
			break
		joiner.step()
	assert_true(reload_finished,
			"the first empty-magazine reload reached DONE with its echo withheld")
	assert_eq(joiner.get_local_player_weapon_state().reload_serial, reload_before + 1,
			"only the original reload request exists at the DONE boundary")

	# Let the host consume that one request and place its one 0x49 echo on the
	# joiner's socket, then cross the DONE->IDLE boundary without switching slots.
	# A few extra client-only ticks make a stale queued kReload observable as a
	# second reload_requested edge.
	OS.delay_msec(2)
	host.step()
	OS.delay_msec(2)
	for _tick in range(4):
		joiner.step()
	var after_echo := joiner.get_local_player_weapon_state()
	assert_eq(after_echo.reload_applied_serial, applied_before + 1,
			"the single delayed S2C 0x49 was applied")
	assert_eq(after_echo.reload_serial, reload_before + 1,
			"recv-before-actions prevents a second same-slot C2S 0x25")
	assert_eq(after_echo.clip, 30,
			"the payload-addressed magazine was refilled once")



func test_joiner_round_hits_host_authoritatively_and_predicts_peer_impact() -> void:
	var mission := _peer_duel_mission()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	# Friendly fire ON (the stock mpattrib 0x3A02 carries NoFriendlyFire 0x200):
	# both co-op players are blue, and a same-team round lands only with the
	# bit clear [orig: Projectile_DamagePairEligible @0x4E74F0, D-WPN-42].
	host_options.mp_attributes = 0x3802
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "PeerShooter"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"peer shooter reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	assert_true(joiner.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	joiner.set_local_player_weapon(_fixture_m4(), {})
	for _settle in range(3):
		joiner.step()
		host.step()

	var host_health_before := host.get_local_player_health()
	var joiner_health_before := joiner.get_local_player_health()
	assert_gt(host_health_before, 0, "host peer starts alive in the joiner's fire lane")
	assert_gt(joiner_health_before, 0, "joiner starts alive before its predicted shot")
	# Every player entity (the listen host's own included) carries retail's
	# 620-tick post-spawn protection: Projectile_ProcessDamageOnTarget zeroes
	# the damage until the countdown expires or the player fires
	# (Server_UpdateAllActivePlayerSlots / Server_ClientFiredRound). The host
	# never fires in this lane, so let the authority count it down first.
	for _protection in range(640):
		joiner.step()
		host.step()
	host.drain_round_impacts()
	joiner.drain_round_impacts()

	joiner.set_local_player_weapon_input(false, true, false)
	var host_impacts: Array = []
	var joiner_impacts: Array = []
	for _tick in range(20):
		joiner.step()
		host.step()
		host_impacts.append_array(host.drain_round_impacts())
		joiner_impacts.append_array(joiner.drain_round_impacts())
		OS.delay_msec(2)

	# The fixture now carries the id-105305 multiplayer Player template (added
	# for the D-NET-196 glide leg: the traits sweep classifies it `plyr` like
	# retail data), so the authoritative person collision reaches real damage —
	# the retail-data behavior the old stripped-fixture negative documented as
	# unreachable.
	assert_lt(host.get_local_player_health(), host_health_before,
			"the authoritative person collision applies player damage (ItemDef 105305)")
	assert_eq(joiner.get_local_player_health(), joiner_health_before,
			"the shooter's peer proxy cannot mutate its local World health")
	assert_eq(host_impacts.size(), 1,
			"host authority resolves exactly one peer collision")
	assert_eq(joiner_impacts.size(), 1,
			"the shooter predicts exactly one visual impact on the decoded peer")
	if joiner_impacts.size() == 1:
		var predicted_hit := (joiner_impacts[0] as RoundImpactRow).position
		assert_lt(predicted_hit.distance_to(Vector3(0, 1, -7.4)), 0.75,
				"the visual impact lands on the host, never a self-H/local-L alias")



# Moving decoded non-player infantry collide at their DECODED wire pose. This is
# an explicit complete-BMS/debug negative-control fixture: the two sides load
# missions differing only in the AI's authored position, so the joiner retains a
# synthetic authored copy at (0, 12) while the host and wire hold the soldier at
# (0, 8). A production header-only join has no such copy. The predicted round
# must impact the wire proxy and leave the fixture copy untouched/non-colliding.
func test_joiner_round_hits_decoded_ai_at_wire_pose_not_local_ghost() -> void:
	var host_mission := MissionData.new()
	assert_eq(host_mission.create_default(), OK)
	assert_not_null(host_mission.add_entity(MissionData.KIND_ORGANIC, 5311,
			Vector3(0, 8, 0), Vector3.ZERO))
	assert_not_null(host_mission.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(20, 0, 0), Vector3.ZERO))
	assert_not_null(host_mission.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(0, 0, 0), Vector3.ZERO))
	var joiner_mission := MissionData.new()
	assert_eq(joiner_mission.create_default(), OK)
	assert_not_null(joiner_mission.add_entity(MissionData.KIND_ORGANIC, 5311,
			Vector3(0, 12, 0), Vector3.ZERO))
	assert_not_null(joiner_mission.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(20, 0, 0), Vector3.ZERO))
	assert_not_null(joiner_mission.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(0, 0, 0), Vector3.ZERO))

	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(host_mission))
	_install_combat_tables(host)

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "GhostWatch"))
	assert_true(joiner.load_from_mission_data(joiner_mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"ghost-pose shooter reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	assert_true(joiner.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	joiner.set_local_player_weapon(_fixture_m4(), {})
	for _settle in range(3):
		joiner.step()
		host.step()

	var ghost_index := _organic_index_at_x(joiner, 0.0)
	assert_gte(ghost_index, 0, "the complete-BMS fixture's authored AI copy exists")
	var ghost_health_before := int(
			joiner.entity_card_by_ai_index(ghost_index).get_health()) \
			if ghost_index >= 0 else -1
	host.drain_round_impacts()
	joiner.drain_round_impacts()

	joiner.set_local_player_weapon_input(false, true, false)
	var host_impacts: Array = []
	var joiner_impacts: Array = []
	for _tick in range(20):
		joiner.step()
		host.step()
		host_impacts.append_array(host.drain_round_impacts())
		joiner_impacts.append_array(joiner.drain_round_impacts())
		OS.delay_msec(2)

	assert_eq(joiner_impacts.size(), 1,
			"the shooter predicts exactly one visual impact on the decoded AI")
	if joiner_impacts.size() == 1:
		var predicted_hit := (joiner_impacts[0] as RoundImpactRow).position
		assert_lt(predicted_hit.distance_to(Vector3(0, 1, -7.4)), 0.75,
				"the visual impact lands at the DECODED wire pose (mission y=8), "
				+ "never at the complete-BMS fixture copy (y=12)")
	assert_eq(host_impacts.size(), 1,
			"host authority resolves the same round against its live AI")
	if ghost_index >= 0:
		assert_eq(int(joiner.entity_card_by_ai_index(ghost_index).get_health()),
				ghost_health_before,
				"the complete-BMS fixture AI never takes client damage")



func test_remote_host_round_event_resimulates_visually_on_joiner() -> void:
	var mission := _combat_mission()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)
	assert_true(host.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	host.set_local_player_weapon(_fixture_m4(), {})

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "RoundObserver"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"observer reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	var host_target := _organic_index_at_x(host, 20.0)
	var joiner_target := _organic_index_at_x(joiner, 20.0)
	assert_gte(host_target, 0, "host resolved its own north-lane target")
	assert_gte(joiner_target, 0, "observer retained the remote visual target")
	var host_health_before := host.entity_card_by_ai_index(host_target).get_health()
	var joiner_health_before := joiner.entity_card_by_ai_index(joiner_target).get_health()
	joiner.drain_round_impacts()
	host.drain_round_impacts()

	host.set_local_player_weapon_input(false, true, false)
	var host_impacts: Array = []
	var joiner_impacts: Array = []
	for _tick in range(20):
		host.step()
		joiner.step()
		host_impacts.append_array(host.drain_round_impacts())
		joiner_impacts.append_array(joiner.drain_round_impacts())
		OS.delay_msec(2)

	assert_lt(host.entity_card_by_ai_index(host_target).get_health(),
			host_health_before, "host fire remains authoritative")
	assert_eq(joiner.entity_card_by_ai_index(joiner_target).get_health(),
			joiner_health_before,
			"the observer's tag-2 re-simulation cannot change health")
	assert_eq(host_impacts.size(), 1, "host projectile resolved one impact")
	assert_eq(joiner_impacts.size(), 1,
			"decoded S2C 0x0A tag-2 fire re-simulates one visual impact")



func test_joiner_off_by_default() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_false(sim.is_joiner(), "joiner off by default")
	assert_eq(sim.get_joiner_phase(), -1, "no joiner phase when not joining")
	assert_false(sim.is_joined_in_match(), "not in a match")
	assert_eq(sim.get_joiner_self_handle(), 0, "no wire handle when not joining")


# The REAL shell ordering (game_world runtime start -> the spawn-loadout apply):
# the profile kit is applied right after runtime setup, BEFORE the joiner has name-matched
# and spawned L, and the shell arms the weapon FSM only when that apply reports success and
# the inventory is valid. The two-GUI regression: the pre-spawn apply was dropped, so the
# joiner could never shoot or reload.
func test_joiner_kit_applied_before_spawn_still_arms_fire_and_reload() -> void:
	var mission := _combat_mission()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "PrespawnKitJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)

	# Mirror LocalPlayerVisuals.apply_local_player_spawn_loadout: apply, then sync the FSM only on
	# success + a valid inventory — the shell's exact gate chain.
	var applied := bool(joiner.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	var inventory_valid := false
	if applied:
		var inventory := joiner.get_local_player_inventory()
		inventory_valid = inventory.valid
		if inventory_valid:
			joiner.set_local_player_weapon(_fixture_m4(), {})

	# A click during the join wait (the world once revealed at world-load completion,
	# one wire gate early — D-LOADSCR-3) must not discharge the pre-armed FSM
	# before L exists, nor cross the spawn edge as a queued phantom shot.
	joiner.set_local_player_weapon_input(false, true, false)

	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return
	for _settle in range(3):
		joiner.step()
		host.step()

	assert_true(applied, "the pre-spawn kit apply must latch, not drop, the kit")
	assert_true(inventory_valid,
			"the pre-spawn inventory must be valid so the shell arms the FSM")
	var at_spawn := joiner.get_local_player_weapon_state()
	assert_eq(at_spawn.fired_serial, 0,
			"a join-wait click fires no phantom pre-spawn round")
	assert_eq(at_spawn.clip, 30,
			"the joiner deploys with a full magazine")

	var before_fire := joiner.get_local_player_weapon_state()
	var fired_before := before_fire.fired_serial
	joiner.set_local_player_weapon_input(false, true, false)
	for _tick in range(20):
		joiner.step()
		host.step()
		OS.delay_msec(2)
	var after_fire := joiner.get_local_player_weapon_state()
	assert_eq(after_fire.fired_serial, fired_before + 1,
			"the joiner can fire with a kit applied before spawn")
	var spent_clip := after_fire.clip
	assert_lt(spent_clip, 30, "the fire consumed one magazine round")

	var reload_before := after_fire.reload_serial
	var applied_before := after_fire.reload_applied_serial
	joiner.set_local_player_weapon_input(false, false, true)
	for _tick in range(120):
		host.step()
		joiner.step()
		OS.delay_msec(2)
	var reloaded := joiner.get_local_player_weapon_state()
	assert_eq(reloaded.reload_serial, reload_before + 1,
			"the joiner can reload with a kit applied before spawn")
	assert_eq(reloaded.reload_applied_serial, applied_before + 1,
			"the echoed S2C 0x49 refilled the pre-spawn-kit joiner")
	assert_eq(reloaded.clip, 30, "the clip refilled to capacity")



func _subrate_walk_mission() -> MissionData:
	# A populated-enough entity set that the BANDWIDTH-capped 0x0A rotates
	# entities across frames (the D-NET-154 regime): eight parked AI organics
	# plus the deploy markers. They sit far from the walkers so nothing
	# interacts; they exist purely to fill the per-frame byte budget.
	# The fillers park BEHIND the joiner's deploy facing (mission +y): under
	# the FULL D-NET-139 priority score (view-angle terms, 2026-08-06) an
	# in-view filler legitimately outranks the behind-side walking host player
	# and the walker starves — retail's own geometry rule. Behind the viewer
	# the fillers' folded angle keeps the walker streaming every frame, which
	# is this test's baseline (the chase-glide contract, not starvation).
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	for i in range(8):
		assert_not_null(md.add_entity(MissionData.KIND_ORGANIC, 5311,
				Vector3(120 + 6 * i, -140, 0), Vector3.ZERO))
	assert_not_null(md.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(0, 8, 0), Vector3.ZERO))
	assert_not_null(md.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(0, 0, 0), Vector3.ZERO))
	return md


func test_joiner_remote_player_glides_across_subrate_records_over_real_udp() -> void:
	# D-NET-196 (net-re 5.38e): with the witnessed BANDWIDTH cap forcing the
	# per-entity 0x0A rotation, the joiner's presented pose for the HOST's
	# walking player must keep advancing every tick through the ported
	# per-class chase - never hold-then-teleport at the wire cadence.
	var mission := _subrate_walk_mission()
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host_options.entity_send_budget = 100
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)
	assert_true(host.spawn_local_player(Vector3(5, 0, 5), 0.0, 1),
			"host spawned its own player")

	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "GlideObserver"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"glide observer reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	# Move the host player authoritatively at run speed (the GUT anim fixtures
	# carry no root translation, so input-walking moves nothing; the wire and
	# the joiner-side chase under test are identical either way).
	var step_m := 0.0625 # per-tick run displacement (3.9 m/s at 62.5 Hz)
	var tick_i := 0
	for _warm in range(32):
		tick_i += 1
		host.debug_teleport_local_player(
				Vector3(tick_i * step_m, 0.0, 0.0), 0.0, 0.0)
		host.step()
		joiner.step()
		OS.delay_msec(2)

	var samples: Array[Vector3] = []
	for _t in range(64):
		tick_i += 1
		host.debug_teleport_local_player(
				Vector3(tick_i * step_m, 0.0, 0.0), 0.0, 0.0)
		host.step()
		joiner.step()
		var p := _present_position_for_type(joiner, 0x14B9)
		assert_true(p.is_finite(), "the joiner presents the host player row")
		if not p.is_finite():
			break
		samples.append(p)
		OS.delay_msec(2)

	if samples.size() < 32:
		return
	var total := 0.0
	var max_step := 0.0
	var stalled := 0
	for i in range(1, samples.size()):
		var d := samples[i].distance_to(samples[i - 1])
		total += d
		max_step = maxf(max_step, d)
		if d < 0.005:
			stalled += 1
	assert_gt(total, 1.0, "the remote player visibly walked during the window")
	var steps := float(samples.size() - 1)
	var mean_step := total / steps
	assert_lt(float(stalled) / steps, 0.25,
			"the presented pose keeps advancing between wire records (no ZOH holds)")
	assert_lt(max_step, maxf(3.0 * mean_step, 0.02),
			"no hold-then-teleport step (bounded near the mean advance)")
	# Bound the END-OF-WINDOW lag against the TRUE teleported pose: the chase's
	# witnessed equilibrium sits ~1 m behind at this speed, and the 2 m snap
	# threshold is the hard ceiling — a chase creeping at a fraction of true
	# speed accrues lag past it and must fail here, not just via max_step.
	var true_end := Vector3(tick_i * step_m, 0.0, 0.0)
	var lag_end := samples[samples.size() - 1].distance_to(true_end)
	assert_lt(lag_end, 2.0,
			"the presented pose tracks within the snap threshold of truth")



func _present_pose_for_type(sim: Simulation, type_id: int) -> Dictionary:
	var record := _present_record_for_type(sim, type_id)
	if record.is_empty():
		return {}
	var snapshot: PackedFloat32Array = record["snapshot"]
	var base := int(record["base"])
	return {
		"position": Vector3(
				snapshot[base + Simulation.PF_POS_X],
				snapshot[base + Simulation.PF_POS_Y],
				snapshot[base + Simulation.PF_POS_Z]),
		"yaw": snapshot[base + Simulation.PF_YAW_DEG],
	}


func test_joiner_view_of_ai_emplacement_gunner_tracks_host() -> void:
	# The 00TRg Rebel Base report: AI organics mounted on .50 cals look seated
	# on the host but SPIN, face the wrong way and FLOAT on a joiner. The host
	# seats its gunner through the real event runtime (a board order, then the
	# BMS AttachToEmplaced action, which boards the vehicle the gunner's AI
	# slot names); the joiner is retail-faithful (world from the wire, no local
	# .bms body). The test pins the joiner's presented gunner row to the host's,
	# frame over frame.
	var root := _native_asset_root()
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	# The 00TRg emplacements are authored at non-cardinal yaws; a zero-yaw gun
	# would hide any carrier-frame recomposition error on the joiner.
	var gun := mission.add_entity(MissionData.KIND_ITEM, 101419,
			Vector3(2, 12, 0), Vector3(0, 0, 135))
	assert_not_null(gun)
	var gunner := mission.add_entity(MissionData.KIND_ORGANIC, 5311,
			Vector3(2, 11, 0), Vector3.ZERO)
	assert_not_null(gunner)
	var gunner_ssn := gunner.bms_id
	assert_gt(gunner_ssn, 0)
	# Deploy markers away from the emplacement so neither player spawns into it.
	assert_not_null(mission.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(20, 0, 0), Vector3.ZERO))
	assert_not_null(mission.add_entity(MissionData.KIND_MARKER, OBJECTIVE_COOP_START_TYPE,
			Vector3(24, 0, 0), Vector3.ZERO))
	# AttachToEmplaced (action 37) carries only the occupant; it boards the
	# vehicle the occupant's AI slot already names. Event 0 arms that slot with
	# the board order (RedirectSingleTo, action 19: command 125, target = the
	# gun), and the repeating event 1 retries the attach every processing pass
	# until the board think has cached the gun.
	assert_gte(mission.add_event(0, 0, 0), 0)
	assert_true(mission.add_event_action(0,
			19, 0, gunner_ssn, 125, gun.bms_id))
	assert_gte(mission.add_event(1, 1, 0), 0)
	assert_true(mission.add_event_action(1,
			37, 0, gunner_ssn))

	var fixture_def_root := ResourceRoot.new()
	assert_eq(fixture_def_root.set_root_dir(DefFixture.directory()), OK)
	var fixture_item_db := ItemDatabase.new()
	assert_eq(fixture_item_db.load_from_resource_root(
			fixture_def_root, "items.def"), OK)

	var host := Simulation.new()
	assert_true(host.enable_host_listen(0))
	host.set_asset_root(root)
	assert_true(host.install_seat_specs_for_type_ids(
			fixture_item_db, PackedInt32Array([1419])))
	assert_true(host.load_from_mission_data(mission))
	_install_combat_tables(host)
	# The live game shell always installs the infantry anim registry; without it
	# the joiner-side remote-motion movers never arm and the bug cannot show.
	var host_anim_root := PresenterFixture.anim_root(self)
	assert_gt(host.set_infantry_anim_map(host_anim_root, "soldier.adm"), 0)
	host.resolve_infantry_adm_ids(host_anim_root, fixture_item_db)
	assert_true(host.spawn_local_player(Vector3(20, 0, 0), 120.0, 1))

	# Retail-faithful joiner: the mission body never comes from a local .bms —
	# only the host's streamed 616-byte header plus the wire world (D-NET-194).
	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "GunnerWatcher"))
	joiner.set_join_world_ready(false)
	var header := PackedByteArray()
	for _tick in range(800):
		joiner.poll_join_preload()
		host.step()
		header = joiner.get_join_mission_header()
		if header.size() == 616:
			break
		OS.delay_msec(2)
	assert_eq(header.size(), 616,
			"the joiner received the host's S2C 0x0B mission header")
	if header.size() != 616:
		return
	var wire_mission := MissionData.new()
	assert_eq(wire_mission.open_wire_header(header), OK)
	assert_true(joiner.load_from_mission_data(wire_mission))
	_install_combat_tables(joiner)
	var joiner_anim_root := PresenterFixture.anim_root(self)
	assert_gt(joiner.set_infantry_anim_map(joiner_anim_root, "soldier.adm"), 0)
	joiner.resolve_infantry_adm_ids(joiner_anim_root, fixture_item_db)
	# NOTE deliberately NO joiner seat-spec install (and no asset root): the
	# live header-only joiner only gains seat specs when GameWorld's
	# admission-boundary prewarm runs install_seat_specs_for_type_ids over the
	# streamed types (GameWorld) — this test pins what the present must do
	# for a carrier whose spec has not been installed.
	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	# The event fired on the host's 16th tick, long before admission completed;
	# confirm the authority world really seated its gunner.
	var host_ai_index := -1
	for ai_index in range(host.get_entity_count()):
		var card: EntityCard = host.entity_card_by_ai_index(ai_index)
		var item_id := card.get_item_id()
		if item_id == 5311 or item_id == 105311:
			host_ai_index = ai_index
			break
	assert_gte(host_ai_index, 0, "the authority exposes the AI gunner entity")
	var host_mounted := false
	for _tick in range(400):
		host.step()
		joiner.step()
		if host_ai_index >= 0 and host.entity_card_by_ai_index(
				host_ai_index).is_mounted():
			host_mounted = true
			break
		OS.delay_msec(2)
	assert_true(host_mounted,
			"the BMS AttachToEmplaced event seated the AI on the emplacement")
	if not host_mounted:
		return

	# Let the mounted pose ride a few full wire records before judging.
	for _settle in range(30):
		host.step()
		joiner.step()
		OS.delay_msec(2)

	# Lockstep sampling window. The host's seated gunner is the truth; the
	# joiner's presented row must sit on it — not orbit it, not hover over it.
	# The presenter's static exposes the exact ROOT basis the wire walk gives
	# an aim-capable body node (present_one_wire_row), so the NODE facing is
	# sampled too — PF_YAW alone stays sane while the aim-overlay body frame
	# is what actually spins a rendered gunner.
	var worst_distance := 0.0
	var worst_vertical := 0.0
	var worst_yaw_disagreement := 0.0
	var worst_joiner_yaw_step := 0.0
	var worst_joiner_pos_step := 0.0
	var worst_body_disagreement := 0.0
	var worst_joiner_body_step := 0.0
	var previous_joiner_pose := {}
	var previous_joiner_body := Basis.IDENTITY
	var have_previous_body := false
	var sampled := 0
	for _frame in range(48):
		host.step()
		joiner.step()
		var host_pose := _present_pose_for_type(host, 5311)
		var joiner_pose := _present_pose_for_type(joiner, 5311)
		assert_false(host_pose.is_empty(), "host presents its AI gunner row")
		assert_false(joiner_pose.is_empty(), "joiner presents the AI gunner row")
		if host_pose.is_empty() or joiner_pose.is_empty():
			break
		var host_position: Vector3 = host_pose["position"]
		var joiner_position: Vector3 = joiner_pose["position"]
		worst_distance = maxf(worst_distance,
				host_position.distance_to(joiner_position))
		worst_vertical = maxf(worst_vertical,
				absf(host_position.y - joiner_position.y))
		worst_yaw_disagreement = maxf(worst_yaw_disagreement, absf(wrapf(
				float(host_pose["yaw"]) - float(joiner_pose["yaw"]),
				-180.0, 180.0)))
		var host_record := _present_record_for_type(host, 5311)
		var joiner_record := _present_record_for_type(joiner, 5311)
		if not host_record.is_empty() and not joiner_record.is_empty():
			var host_body: Basis = EntityPresenter.aim_root_basis(
					host_record["snapshot"], int(host_record["base"]),
					Basis.IDENTITY)
			var joiner_body: Basis = EntityPresenter.aim_root_basis(
					joiner_record["snapshot"], int(joiner_record["base"]),
					Basis.IDENTITY)
			worst_body_disagreement = maxf(worst_body_disagreement,
					rad_to_deg(host_body.get_rotation_quaternion().angle_to(
							joiner_body.get_rotation_quaternion())))
			if have_previous_body:
				worst_joiner_body_step = maxf(worst_joiner_body_step,
						rad_to_deg(joiner_body.get_rotation_quaternion().angle_to(
								previous_joiner_body.get_rotation_quaternion())))
			previous_joiner_body = joiner_body
			have_previous_body = true
		if not previous_joiner_pose.is_empty():
			worst_joiner_yaw_step = maxf(worst_joiner_yaw_step, absf(wrapf(
					float(joiner_pose["yaw"]) - float(previous_joiner_pose["yaw"]),
					-180.0, 180.0)))
			worst_joiner_pos_step = maxf(worst_joiner_pos_step,
					joiner_position.distance_to(
							previous_joiner_pose["position"]))
		previous_joiner_pose = joiner_pose
		sampled += 1
		OS.delay_msec(2)

	assert_gte(sampled, 32, "the sampling window ran")
	# Seated truth: the same tolerance the articulated-anchor reconstruction
	# already meets (0.35), vertical bounded tighter — "floating" is a y error.
	assert_lt(worst_distance, 0.35,
			"joiner presents the gunner ON its seat, not displaced from it")
	assert_lt(worst_vertical, 0.3,
			"joiner's gunner does not hover above/below the host's seat pose")
	# A seated gunner holds the gun's facing on both views.
	assert_lt(worst_yaw_disagreement, 20.0,
			"joiner's gunner faces the way the host's gunner faces")
	# The host gunner is static in this window; any wild per-frame yaw motion on
	# the joiner is the reported spin.
	assert_lt(worst_joiner_yaw_step, 25.0,
			"joiner's gunner yaw is stable frame over frame (no spin)")
	assert_lt(worst_joiner_pos_step, 0.5,
			"joiner's gunner does not jitter/teleport around the mount")
	# The NODE facing (the aim-overlay body frame the wire walk sets as the
	# root basis): both views must agree it is the SEAT frame, and it must not
	# sweep — this is the reported spin/wrong-facing surface.
	assert_lt(worst_body_disagreement, 20.0,
			"joiner's rendered gunner BODY faces the host's seat frame")
	assert_lt(worst_joiner_body_step, 25.0,
			"joiner's rendered gunner BODY holds its facing (no spin)")
