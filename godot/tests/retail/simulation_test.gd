extends GutTest

# Simulation (the GDExtension binding): promote a synthetic BMS mission into a live
# world + AI system, tick it, and confirm the AI walks entities along their authored route.
# This is the in-Godot end of step 1 (promotion) + step 2 (locomotion).

# --- Native fixture roots (S16) ---------------------------------------------
# The Dictionary seat seam (set_item_seat_specs) and the render-placer
# collision sources are gone: a sim poses/attaches ONLY from its own asset
# root (set_asset_root) and the native extractor
# (install_seat_specs_for_type_ids over items.def rows + model userpoints).
# Tests compose a flat loose dir of committed fixtures. ResourceRoot
# rejects user:// paths, so the dirs live under OS.get_cache_dir().

var _native_fixture_dirs: Array[String] = []


func after_each() -> void:
	for dir in _native_fixture_dirs:
		TestFs.remove_dir_recursive(dir)
	_native_fixture_dirs.clear()


# An authored 3DI variant minted once from the retired edit surface
# (fixtures/README.md): the CTRL names, PANM rows and flags the sim's own
# parse-once cache consumes from disk.
const SYN_MOUNT_HEAT_GLOW_SLIDE := "res://../fixtures/threedi/synth/mount_heat_glow_slide_part1.3di"
# The USE scan admits a seat only inside the player's view cone (just under
# 90 deg standing, 5 deg seated); a test that presses USE looks at the seat first.
const MountLook := preload("res://tests/support/mount_look.gd")


func _native_fixture_dir() -> String:
	var dir := TestFs.cache_dir(self, "sim_native")
	_native_fixture_dirs.append(dir)
	return dir


func _copy_fixture(dir: String, source_res_path: String, dest_name: String) -> void:
	TestFs.copy(self, source_res_path, dir.path_join(dest_name))


func _item_db_from_text(dir: String, text: String) -> ItemDatabase:
	TestFs.write_text(self, dir.path_join("items.def"), text)
	var db := ItemDatabase.new()
	assert_eq(db.load(dir.path_join("items.def")), OK)
	return db


const BINOC_REL := "bad/BINOC.bad"


# The shipped BINOC.bad from the reference fixture set; "" (after pending)
# without OPENNOVA_JO_ASSETS. Every rig test starts by checking it.
func _binoc_path() -> String:
	var path := RetailData.fixture(BINOC_REL)
	if path.is_empty():
		pending(RetailData.fixture_pending_text(BINOC_REL))
	return path


# The 19-bone person + BINOC rig as a named organic graphic: <graphic>.3di,
# <graphic>.adm (the anim map the native pose provider resolves through the
# item's anim_def), and the shared BINOC.bad clip (the caller has checked
# _binoc_path()).
func _write_char_rig(dir: String, graphic: String) -> void:
	_copy_fixture(dir, "res://../fixtures/threedi/synth/person.3di",
			graphic + ".3di")
	TestFs.copy(self, RetailData.fixture(BINOC_REL), dir.path_join("BINOC.bad"))
	var quote := String.chr(34)
	TestFs.write_text(self, dir.path_join(graphic + ".adm"),
			"anim_reset %sBINOC.bad%s\r\n" % [quote, quote]
			+ "anim_idle %sBINOC.bad%s\r\n" % [quote, quote]
			+ "anim_idle_2 %sBINOC.bad%s\r\n" % [quote, quote]
			+ "anim_emplaced %sBINOC.bad%s\r\n" % [quote, quote])


# These pose fixtures have no walking clips. Spawn their boarder at the
# authored entry/seat point so they exercise attachment within the real radius.
func _seat_fixture_spawn(model_path: String, point_name: String,
		position: Vector3, rotation: Vector3 = Vector3.ZERO) -> Vector3:
	var model := ObjectData.new()
	assert_eq(model.open_file(model_path), OK)
	for i in range(model.get_user_point_count()):
		var point := model.get_user_point_info(i)
		if String(point.name).to_lower() == point_name.to_lower():
			var posed := MissionObjectPlacer.entity_transform(position, rotation) * point.position
			return Vector3(posed.x, -posed.z, posed.y)
	fail_test("missing authored seat point " + point_name)
	return position


func _install_native_seat_table(sim: Simulation, dir: String,
		item_db: ItemDatabase, type_ids: PackedInt32Array) -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	sim.set_asset_root(root)
	# Boarding requires the same definition metadata as the authored seat table.
	sim.resolve_item_traits(item_db)
	assert_true(sim.install_seat_specs_for_type_ids(item_db, type_ids),
			"native seat-spec install over the composed fixture root")
	return root


func _native_asset_root(sim: Simulation, dir: String) -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	sim.set_asset_root(root)
	return root


# Retail runs the entity update only while its world holds a human: on the
# authority, Game_ProcessMainFrame skips Entity_UpdateAllEntities once the WAC
# clock has started and Server_BuildEntitySlotLists counted no visible player,
# and Entity_UpdateAllEntities itself returns while there is no local player
# entity. A single-player world always has its player, so a fixture built
# without one stands its local player in the world: far from the entities
# under test unless the test places it.
const FIXTURE_HUMAN_POSITION := Vector3(0, 0, 2000)


func _spawn_fixture_human(sim: Simulation,
		position: Vector3 = FIXTURE_HUMAN_POSITION, team: int = 1) -> void:
	assert_true(sim.spawn_local_player(position, 0.0, team),
			"the fixture's local player is the human that keeps the world running")


const ANIM_FIXTURES := "res://../fixtures/anim"


# (The duck-typed placer stubs are gone with S3b, and the placer argument
# with the boot-contract cleanup: every pose/collision source is the sim's
# own asset root, composed per test from the committed fixtures.)

func _anim_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(ANIM_FIXTURES))
	return root


func _aimed_shot_available(sim: Simulation) -> bool:
	return sim.get_local_player_weapon_state().aimed_shot_available


func _present_field_for_origin(sim: Simulation, kind: int, index: int,
		field: int) -> int:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_KIND]) == kind \
				and int(snapshot[base + Simulation.PF_INDEX]) == index:
			return int(snapshot[base + field])
	return -1


func test_hud_spread_row_tracks_stance_and_settled_aim_state() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# HUD ERROR is selected from two stance triplets. Air/water/mount overrides
	# live in the body; this public seam pins the ordinary stance order and the
	# settled-first-person +3 verdict. [orig: HUD_DrawCrosshair
	# @0x592b35..0x592b87; Player_IsOpticalViewVisible @0x5cf780]
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	# A compact deterministic FSM is enough for the view toggle; the entity's
	# equipped ADM index still resolves the exact M4 ERROR table loaded above.
	var def_2 := WeaponDef.new()
	def_2.name = "WPN_M4AUTO"
	def_2.set_actions([
		WeaponActionRow.make("idle", 0, 0),
		WeaponActionRow.make("scopeup", 0, 0),
		WeaponActionRow.make("scopedown", 0, 0),
	])
	def_2.flags = 0x1
	def_2.clipsize = 30
	def_2.startrounds = 300
	sim.set_local_player_weapon(def_2, {})
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().hud_spread_row, 2,
			"standing selects row 2")

	assert_true(sim.request_local_player_stance(2))
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().hud_spread_row, 0,
			"prone selects row 0")

	sim.set_water_z(1.0)
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().hud_spread_row, 2,
			"below-water source height forces the standing row")
	sim.set_water_z(0.0)
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().hud_spread_row, 0,
			"leaving water restores the authored prone row")

	assert_true(sim.request_local_player_scope_toggle())
	var aimed_row_seen := false
	for _i in range(15):
		sim.step()
		assert_false(_aimed_shot_available(sim),
				"Scoped ADS never promotes before the ease endpoint")
	for _i in range(9):
		sim.step()
		if sim.get_local_player_weapon_state().hud_spread_row == 3:
			aimed_row_seen = true
			break
	assert_true(aimed_row_seen,
			"settled first-person aim adds the second-triplet offset")

	sim.set_local_player_debug_third_person(true)
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().hud_spread_row, 0,
			"third person clears aimed-shot availability without changing stance")


func test_local_fire_exports_recoil_camera_and_hud_spread() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# Keep the world's built-in player ItemDef traits intact: the compact items.def
	# fixture intentionally lacks retail's player template 105305, while recoil's
	# source gate requires a person with an ItemDef. [orig: RoundData_SpawnRound
	# @0x4ec0d0; recoil add @0x4ec8a3]
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	var def_3 := WeaponDef.new()
	def_3.name = "WPN_M4AUTO"
	def_3.set_actions([
		WeaponActionRow.make("idle", 0, 0),
		WeaponActionRow.make("fire", 0, 0),
		WeaponActionRow.make("recoil", 0, 0),
	])
	def_3.flags = 0
	def_3.clipsize = 30
	def_3.startrounds = 300
	sim.set_local_player_weapon(def_3, {})
	sim.step()
	sim.set_local_player_weapon_input(false, true, false)
	for _i in range(4):
		sim.step()
		if sim.get_local_player_weapon_state().fired_serial > 0:
			break

	var weapon_state := sim.get_local_player_weapon_state()
	var recoil_pitch := weapon_state.recoil_pitch_bam
	var weight_spread := weapon_state.weapon_weight_spread_bam
	assert_gt(recoil_pitch, 0,
			"the successful M4 round stamps the standing ammo recoil impulse")
	assert_eq(weapon_state.hud_spread_row, 2,
			"standing hip fire selects the first triplet's standing row")
	assert_eq(weapon_state.hud_spread_fp16,
			0x4000 + (recoil_pitch >> 7) + (weight_spread >> 7),
			"HUD spread preserves exact ERROR plus both live SAR terms")
	var recoil_view := sim.get_local_player_view()
	assert_almost_eq(recoil_view.fp_pitch_recoil_deg,
			float(recoil_pitch) * 2.0 * 360.0 / 4294967296.0, 0.0001,
			"the bridge exports retail's wrapped 2*recoil camera pitch")


func test_local_fire_spawns_the_authoritative_round_and_impact() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# The listen-server loopback handler skips C2S 0x06 because retail local fire
	# already appends/spawns synchronously. Pin that local action seam end-to-end:
	# FSM fired -> RoundSim -> the organic effects_table row.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	# Spawn yaw 0 = mission yaw 0 = engine heading 90 BAM-deg, which faces
	# mission +y (bearing 90). The target sits 8 m along +y so the shot connects
	# only when the round bearing rides the heading frame directly — the old
	# (90 - heading) flip flew the shot along +x and only an east-side target
	# could pass (the compensating-error pair the fp_impact probe pinned;
	# ledger D-WPN-18).
	# Use the fixture's Generic Soldier (wire id 5311 -> items.def id 105311),
	# then resolve traits through the same production seam as MissionRoot. Retail
	# returns before projectile damage when the struck entity has no ItemDef.
	assert_not_null(md.add_entity(MissionData.KIND_ORGANIC, 5311,
			Vector3(0, 8, 0), Vector3.ZERO))
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	sim.resolve_item_traits(item_db)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	# The motor derives the firing eye and target capsule from native clips.
	assert_gt(sim.set_infantry_anim_map(_anim_root(), "soldier.adm"), 0)
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO")
	var fire_def := WeaponDef.new()
	fire_def.name = "WPN_M4AUTO"
	fire_def.set_actions([
		WeaponActionRow.make("idle", 0, 0),
		WeaponActionRow.make("fire", 0, 0),
		WeaponActionRow.make("recoil", 0, 0),
	])
	fire_def.flags = 0
	fire_def.clipsize = 30
	fire_def.startrounds = 300
	sim.set_local_player_weapon(fire_def, {})
	sim.step()
	sim.drain_local_player_weapon_events()
	sim.set_local_player_weapon_input(false, true, false)
	var impacts: Array = []
	for _i in range(4):
		sim.step()
		impacts.append_array(sim.drain_round_impacts())

	var weapon_state := sim.get_local_player_weapon_state()
	assert_eq(weapon_state.round_ring_count, 1,
			"local FIRE appends exactly one tag-2 fan-out record")
	# The fixture M4's first shot samples the pre-consume 30-round magazine, so
	# ((30 & 3) << 4) | 2 produces 0x22, not a hard-coded 0x02 and not the
	# unrelated category/rank weapon-slot combo.
	# [orig: WeaponAction_Fire @ 0x542c11; net-re §5.9.1 capture cross-witness]
	assert_eq(weapon_state.last_round_flags, 0x22)
	assert_eq(weapon_state.last_round_subtype, 12,
			"ordinary on-foot hip fire carries the retail default zoom subtype")
	assert_eq(weapon_state.last_round_slot_byte, 0,
			"the sole modeled local weapon slot has retail slot id zero")
	assert_eq(weapon_state.last_round_seq, 1)
	assert_eq(impacts.size(), 1, "one local shot reaches the target and emits one impact")
	if impacts.size() == 1:
		# The victim is a NON-LOCAL person, so the flesh row (23), not the local
		# player's row (2). Real small-arms ammo authors Effect_AmHitBody on both,
		# so only the SOUND distinguishes them.
		# [orig: Projectile_HandleTerrainImpact_0 @ 0x4e98f0 — local-player compare
		#  @0x4e9a55, push 2 @0x4e9aa1, push 17h @0x4e9ad7]
		assert_eq((impacts[0] as RoundImpactRow).effect, "Effect_AmHitBody")
		assert_eq((impacts[0] as RoundImpactRow).sound, "IMP_BULLET_FLESH")
		# The drained position is Godot-space (x, z_up, -y): the +y_m flight lands
		# near (0, ~eye, -8). Pins the local fire bearing = the engine heading
		# frame (D-WPN-18; RoundSim's wire-validated (cos, sin) mapping).
		var impact_pos := (impacts[0] as RoundImpactRow).position
		assert_almost_eq(impact_pos.x, 0.0, 0.75,
				"the shot flies the aim bearing, not its 90-deg mirror")
		assert_between(-impact_pos.z, 6.0, 8.5,
				"the impact lands at the north-side target range")

	# Presentation generations reset when the weapon is remounted, but the wire
	# round sequence belongs to the shooter and stays monotonic across adm/slot
	# changes. [orig: word_B7C670; net-re §5.9.1 capture 0x020b..0x0217]
	sim.set_local_player_weapon(fire_def, {})
	sim.step()
	sim.drain_local_player_weapon_events()
	sim.set_local_player_weapon_input(false, true, false)
	sim.step()
	weapon_state = sim.get_local_player_weapon_state()
	assert_eq(weapon_state.round_ring_count, 2)
	assert_eq(weapon_state.last_round_seq, 2,
			"weapon remount does not reset the shooter-lifetime sequence")


func test_local_round_damages_enemy_mounted_on_rotated_emplaced_gun() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	if _binoc_path().is_empty():
		return
	# Exact player report: the target rendered in a rotated UseGun seat must keep
	# its authored COBJ sections under the carried body basis while its look yaw
	# remains independent, so a local-owned round through a visible section can
	# damage it.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var reference_gun := md.add_entity(
			MissionData.KIND_ITEM, 101294,
			Vector3(-20, 8, 0), Vector3.ZERO)
	var reference_enemy := md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			_seat_fixture_spawn("res://../fixtures/threedi/synth/mount.3di", "Usegun", Vector3(-20, 8, 0)), Vector3.ZERO)
	var rotated_gun := md.add_entity(
			MissionData.KIND_ITEM, 101294,
			Vector3(0, 8, 0), Vector3(0, -90, 0))
	var rotated_enemy := md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			_seat_fixture_spawn("res://../fixtures/threedi/synth/mount.3di", "Usegun", Vector3(0, 8, 0), Vector3(0, -90, 0)), Vector3.ZERO)
	for pair in [
		[reference_enemy, reference_gun],
		[rotated_enemy, rotated_gun],
	]:
		assert_not_null(pair[0] as EntityRef)
		assert_not_null(pair[1] as EntityRef)
		assert_true(md.set_entity_property_int(
				MissionData.KIND_ORGANIC,
				(pair[0] as EntityRef).index,
				"waypoint_id", 125))
		assert_true(md.set_entity_property_int(
				MissionData.KIND_ORGANIC,
				(pair[0] as EntityRef).index,
				"wp_number", (pair[1] as EntityRef).bms_id))

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# Native root: the guns pose their UseGun seat from mount's authored
	# Usegun point; both enemies pose their COBJ sections from the US02
	# person + BINOC rig resolved through their anim_def.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	_write_char_rig(dir, "us02")
	var item_db := _item_db_from_text(dir, ItemDbFixture.fixture_text(self).replace(
			"id 101294", "id 101294\n  graphic mount"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	assert_gte(sim.resolve_collision_instances(item_db), 2,
			"precondition: both enemies use authored posed COBJ collision")
	# Command-125 boarders spawn ON FOOT and attach through the infantry
	# think's board leg; the think gate is (logic_tick + 36*net_id) & 15,
	# so drive a few 16-tick boundaries instead of a single step.
	# Co-located with the carrier, the first think boards.
	for _board_tick in range(48):
		sim.step()
	var reference_card: EntityCard = sim.entity_card_by_ai_index(0)
	var rotated_card: EntityCard = sim.entity_card_by_ai_index(1)
	assert_true(reference_card.is_mounted())
	assert_true(rotated_card.is_mounted())
	assert_eq(rotated_card.get_mount_type(), 3)
	var health_before := rotated_card.get_health()
	assert_gt(health_before, 0)

	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	# Advance one authoritative frame so collision and the decoded presentation
	# snapshot expose the same mounted body pose.
	sim.step()
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	var reference_row_base := -1
	var rotated_row_base := -1
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_KIND]) != MissionData.KIND_ORGANIC:
			continue
		var mission_index := int(snapshot[base + Simulation.PF_INDEX])
		if mission_index == reference_enemy.index:
			reference_row_base = base
		elif mission_index == rotated_enemy.index:
			rotated_row_base = base
	assert_gte(reference_row_base, 0,
			"the reference gunner reached the decoded presentation")
	assert_gte(rotated_row_base, 0,
			"the rotated gunner reached the decoded presentation")
	if reference_row_base < 0 or rotated_row_base < 0:
		return
	assert_eq(int(snapshot[reference_row_base +
			Simulation.PF_AIM_OVERLAY_VALID]), 1)
	assert_eq(int(snapshot[rotated_row_base +
			Simulation.PF_AIM_OVERLAY_VALID]), 1)
	var sections: Array = sim.get_hitbox_debug().organics
	var reference_by_section := {}
	var rotated_by_section := {}
	for value in sections:
		var row: HitboxDebugOrganic = value
		var handle := row.entity_handle
		var section := row.section
		if handle == 0:
			reference_by_section[section] = row
		elif handle == 1:
			rotated_by_section[section] = row
	assert_eq(reference_by_section.size(), 19)
	assert_eq(rotated_by_section.size(), 19)

	var reference_pos: Vector3 = reference_card.get_position()
	var rotated_pos: Vector3 = rotated_card.get_position()
	# PF_YAW_DEG / debug yaw is the gunner's independent look. The final body
	# field is the basis EntityPresenter.aim_apply actually applies to the rendered model
	# and the same body class build_section_matrices uses for posed collision.
	var reference_body_yaw := snapshot[reference_row_base +
			Simulation.PF_AIM_BODY_YAW_DEG]
	var rotated_body_yaw := snapshot[rotated_row_base +
			Simulation.PF_AIM_BODY_YAW_DEG]
	var relative_basis := MissionObjectPlacer.bms_to_godot_basis(
			Vector3(0, rotated_body_yaw, 0)) * MissionObjectPlacer.bms_to_godot_basis(
			Vector3(0, reference_body_yaw, 0)).inverse()
	var selected_section := -1
	var selected_score := -1.0
	# Hips, thighs, calves, and feet are the body/leg overlay classes. Their
	# mounted yaw is carried by the seat even when the two gunners independently
	# aim their upper bodies after the authoritative step.
	for section_value in [0, 7, 8, 11, 12, 17, 18]:
		var section := int(section_value)
		if not reference_by_section.has(section):
			continue
		var row: HitboxDebugOrganic = reference_by_section[section]
		var offset: Vector3 = row.pos - reference_pos
		var radius := maxf(row.radius, 0.001)
		var score := Vector2(offset.x, offset.z).length() / radius
		if rotated_by_section.has(section) and score > selected_score:
			selected_score = score
			selected_section = section
	assert_gte(selected_section, 0,
			"a shared off-axis authored section exists")
	var reference_center: Vector3 = (
			reference_by_section[selected_section] as HitboxDebugOrganic).pos
	var expected_center := rotated_pos + relative_basis * (
			reference_center - reference_pos)
	var collision_center: Vector3 = (
			rotated_by_section[selected_section] as HitboxDebugOrganic).pos
	assert_lt(collision_center.distance_to(expected_center), 0.01,
			"posed collision follows the mounted entity's rendered body yaw")

	var radial := expected_center - rotated_pos
	radial.y = 0.0
	assert_gt(radial.length(), 0.01)
	var tangent := Vector3(-radial.z, 0.0, radial.x).normalized()
	assert_gte(sim.debug_spawn_round(
			expected_center - tangent,
			tangent, "AMMO_CAR15_556MM"), 0,
			"a local-owned live round starts through the rendered section")
	for _i in range(2):
		sim.step()
	var rotated_after: EntityCard = sim.entity_card_by_ai_index(1)
	assert_lt(rotated_after.get_health(), health_before,
			"the local round damages the rotated mounted enemy organic")


func test_mounted_rendered_head_matrix_matches_collision_and_authoritative_shot() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	if _binoc_path().is_empty():
		return
	# Config 6 is the decisive per-config witness: the mounted body/neck stay on
	# the carrier frame while the head alone consumes aim. Build an actual
	# ObjectModel from the same person + BINOC rig as collision, feed it
	# the packed presentation result, and compare its final head deformation to
	# COBJ section 14 before shooting through that rendered point.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(
			MissionData.KIND_ITEM, 101294,
			Vector3(0, 8, 0), Vector3(45, 0, 0))
	var enemy := md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			Vector3(0, 8, 0), Vector3.ZERO)
	assert_not_null(gun)
	assert_not_null(enemy)
	assert_true(md.set_entity_property_int(
			MissionData.KIND_ORGANIC, enemy.index,
			"waypoint_id", 125))
	assert_true(md.set_entity_property_int(
			MissionData.KIND_ORGANIC, enemy.index,
			"wp_number", gun.bms_id))

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# Native sim sources: mount's authored Usegun seat (bone 6) with the def's
	# phrase_set 6, plus the US02 person + BINOC rig for the mounted pose.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	_write_char_rig(dir, "us02")
	var item_db := _item_db_from_text(dir, ItemDbFixture.fixture_text(self).replace(
			"id 101294", "id 101294\n  graphic mount\n  phrase_set 6"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	# The render-side witness model keeps its own rig objects; the sim no
	# longer reads them (native-only pose sources).
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth/person.3di")), OK)
	var bad_root := ResourceRoot.new()
	assert_eq(bad_root.set_root_dir(RetailData.fixture(BINOC_REL).get_base_dir()), OK)
	var skeletal := SkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {"anim_emplaced": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()),
			"mounted person rig loads: %s" % skeletal.get_last_error())
	assert_gte(sim.resolve_collision_instances(item_db), 1,
			"the mounted enemy owns authored posed COBJ collision")
	var ammo_root := ResourceRoot.new()
	assert_eq(ammo_root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_ammo_table(ammo_root, "ammo.def"), OK)

	# Advance one authoritative frame so the mounted seat frame, collision pose,
	# and listen-server client snapshot all describe the same clip phase.
	# Command-125 boarders spawn ON FOOT and attach through the infantry
	# think's board leg; the think gate is (logic_tick + 36*net_id) & 15,
	# so drive a few 16-tick boundaries instead of a single step.
	# Co-located with the carrier, the first think boards.
	for _board_tick in range(48):
		sim.step()
	var enemy_idx := _first_organic_ai_index(sim)
	assert_gte(enemy_idx, 0)
	var card: EntityCard = sim.entity_card_by_ai_index(enemy_idx)
	assert_true(card.is_mounted())
	assert_true(card.is_mount_config_valid())
	assert_eq(card.get_mount_config(), 6)
	assert_eq(card.get_anim_key(), "anim_emplaced")
	var enemy_handle := sim.get_entity_wire_handle(enemy_idx)
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	var row_base := -1
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_KIND]) == MissionData.KIND_ORGANIC \
				and int(snapshot[base + Simulation.PF_INDEX]) == enemy.index:
			row_base = base
			break
	assert_gte(row_base, 0, "the mounted placed enemy reached the decoded present")
	if row_base < 0:
		return
	assert_eq(int(snapshot[row_base + Simulation.PF_AIM_OVERLAY_VALID]), 1)
	assert_eq(int(snapshot[row_base + Simulation.PF_RIGHT_HAND_COLLAPSED]), 1,
			"the mounted NPC exports retail's BN17 final-row verdict")
	var packed_body := Vector3(
			snapshot[row_base + Simulation.PF_AIM_BODY_PITCH_DEG],
			snapshot[row_base + Simulation.PF_AIM_BODY_YAW_DEG],
			snapshot[row_base + Simulation.PF_AIM_BODY_ROLL_DEG])
	var head_offset: int = (row_base + Simulation.PF_AIM_ANGLES
			+ 8 * Simulation.PF_AIM_CLASS_STRIDE)
	var packed_head := Vector3(
			snapshot[head_offset], snapshot[head_offset + 1],
			snapshot[head_offset + 2])
	assert_gt(packed_head.distance_to(packed_body), 10.0,
			"the config-6 witness really drives head aim away from the mounted body")

	var model = ObjectModel.new()
	add_child_autofree(model)
	model.set_skeletal_anim(skeletal)
	model.set_object_data(data)
	assert_true(model.has_skeleton())
	var presented_position := Vector3(
			snapshot[row_base + Simulation.PF_POS_X],
			snapshot[row_base + Simulation.PF_POS_Y],
			snapshot[row_base + Simulation.PF_POS_Z])
	assert_lt(presented_position.distance_to(card.get_position()), 0.5,
			"the selected row is the mounted placed enemy")
	model.global_position = presented_position
	model.play_body_clip_at(
			"anim_emplaced",
			int(snapshot[row_base + Simulation.PF_ANIM_PHASE_TICKS]))
	EntityPresenter.aim_apply(model, snapshot, row_base)
	await get_tree().process_frame
	await get_tree().process_frame

	var skeleton: Skeleton3D = model.get_skeleton()
	# Inspect the same composed pose once without the clip verdict to capture the
	# animated joint, then restore the snapshot's mounted verdict.
	model.set_right_hand_collapsed(false)
	await get_tree().process_frame
	await get_tree().process_frame
	skeleton.force_update_all_bone_transforms()
	var authored_hand_joint := (skeleton.global_transform
			* skeleton.get_bone_global_pose(16).origin)
	model.set_right_hand_collapsed(true)
	await get_tree().process_frame
	await get_tree().process_frame
	skeleton.force_update_all_bone_transforms()
	# The synthetic person's COBJ 14/head authors center=(1/16, 0, 13/16)
	# in signed 16.16 collision space (tests/fixtures/minimal_3di_gen.cpp). The
	# retail fixed-to-render sandwich maps local collision (x,y,z) to the
	# skeleton's node frame as (y,z,x) before the live bone deformation.
	var head_center_model := Vector3(0.0, 0.8125, 0.0625)
	var rendered_head_matrix := (skeleton.global_transform
			* skeleton.get_bone_global_pose(14)
			* skeleton.get_bone_global_rest(14).affine_inverse())
	var rendered_head_center: Vector3 = rendered_head_matrix * head_center_model
	var rendered_hand_matrix := (skeleton.global_transform
			* skeleton.get_bone_global_pose(16)
			* skeleton.get_bone_global_rest(16).affine_inverse())
	assert_true(rendered_hand_matrix.basis.x.is_zero_approx()
			and rendered_hand_matrix.basis.y.is_zero_approx()
			and rendered_hand_matrix.basis.z.is_zero_approx(),
			"render skinning receives retail's zero-scale BN17 basis")
	assert_true(rendered_hand_matrix.origin.is_equal_approx(authored_hand_joint),
			"render skinning collapses BN17 at the animated joint, never world origin")

	var collision_head := Vector3.INF
	var collision_hand := Vector3.INF
	for value in sim.get_hitbox_debug().organics:
		var section: HitboxDebugOrganic = value
		if section.entity_handle == enemy_handle \
				and section.section == 14:
			collision_head = section.pos
		if section.entity_handle == enemy_handle \
				and section.section == 16:
			collision_hand = section.pos
	assert_ne(collision_head, Vector3.INF,
			"authoritative collision exposes mounted head section 14")
	if collision_head != Vector3.INF:
		assert_lt(collision_head.distance_to(rendered_head_center), 0.01,
				"rendered final head bone matrix and collision section 14 are identical")
	assert_ne(collision_hand, Vector3.INF,
			"authoritative collision exposes mounted right-hand section 16")
	if collision_hand != Vector3.INF:
		assert_true(collision_hand.is_zero_approx(),
				"authoritative COBJ 16 retains retail's separate all-zero final row")

	var health_before := card.get_health()
	var incoming := rendered_head_matrix.basis.x.normalized()
	# The round's segment as a plain geometric trace first (the picker walks
	# the same posed person bone spheres the round will): it resolves the
	# rendered mounted target at its posed head section, never a stand-in
	# sphere — read before the shot lands, since the kill spawns the corpse
	# twin on the same spot.
	var pick := sim.debug_pick_entity(rendered_head_center - incoming * 2.0, incoming, 4.0)
	assert_true(pick.hit,
			"the authoritative shot resolves against the rendered mounted target")
	if pick.hit:
		assert_eq(pick.entity_handle, enemy_handle)
		assert_eq(pick.hit_class, "person")
		assert_eq(pick.section, 14,
				"the primary posed-hit section remains authoritative")
	assert_gte(sim.debug_spawn_round(
			rendered_head_center - incoming * 2.0,
			incoming, "AMMO_CAR15_556MM"), 0,
			"a local-owned round starts through the rendered mounted head")
	for _tick in range(2):
		sim.step()
	var impacts := sim.drain_round_impacts()
	assert_eq(impacts.size(), 1)
	if impacts.size() == 1:
		var impact: RoundImpactRow = impacts[0]
		assert_lt(impact.direction.distance_to(incoming), 0.001,
				"the incoming shot direction remains authoritative for reactions")
	assert_lt(sim.entity_card_by_ai_index(enemy_idx).get_health(),
			health_before, "the posed head shot damages the mounted enemy")


func test_armory_reads_and_clears_authoritative_local_loadout() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)

	assert_eq(sim.get_local_player_class(), 8, "spawned player exposes its rifleman class")
	assert_eq(sim.get_local_player_team(), 2, "spawned player exposes the assigned red team")
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
		"weapon-table load exposes the entity's stamped default instead of the FP fallback")
	assert_false(sim.has_explicit_spawn_loadout(),
		"the engine's WPN_M4AUTO fallback is not an authored spawn kit")
	# The profile-less kit is the program's whole single-player page, M4AUTO first
	# (D-PLAYERINFO-13, docs/playerinfo/player-sav-re.md).
	var fallback_loadout := sim.get_local_player_loadout()
	assert_eq(fallback_loadout.size(), 8,
		"the canonical transport exposes the effective default kit")
	assert_eq((fallback_loadout[0] as WeaponKitEntry).name, "WPN_M4AUTO")
	assert_true(sim.set_local_player_class(7),
		"profile class can commit without replacing the current weapon kit")
	assert_eq(sim.get_local_player_class(), 7)
	sim.set_spawn_loadout([WeaponKitEntry.make("WPN_M4AUTO")], true)
	assert_true(sim.has_explicit_spawn_loadout(),
		"a supplied mission/profile kit is distinguishable from the fallback")

	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 6))
	assert_eq(sim.get_local_player_class(), 6, "accepted class is authoritative on reopen")
	var accepted_loadout := sim.get_local_player_loadout()
	assert_eq(accepted_loadout.size(), 1)
	assert_eq((accepted_loadout[0] as WeaponKitEntry).ammo_primary, -1,
		"unspecified canonical ammo remains the retail fallback sentinel")
	var inv := sim.get_local_player_inventory()
	assert_true(inv.valid, "the ACCEPT rebuilt the slot pool")
	assert_eq(inv.equipped_name, "WPN_M4AUTO",
		"the ACCEPT re-selected the accepted weapon")
	assert_true(sim.apply_local_player_loadout([], 9), "the all-NONE kit is a valid apply")
	assert_eq(sim.get_local_player_class(), 9, "NONE still commits the selected class")
	assert_eq(sim.get_local_player_weapon_name(), "", "NONE clears the equipped AdmDef")
	assert_true(sim.get_local_player_loadout().is_empty(),
		"an explicit all-NONE kit remains empty instead of falling back to M4")


func test_same_name_armory_accept_refills_the_live_weapon_slot() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(root, "weapon.def"), OK)
	var m4_index := weapons.find_weapon("WPN_M4AUTO")
	assert_gte(m4_index, 0)
	var m4: WeaponDef = weapons.get_weapon(m4_index)
	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	sim.set_local_player_weapon(m4, {})
	sim.step()
	var full_clip := sim.get_local_player_weapon_state().clip
	sim.set_local_player_weapon_input(false, true, false)
	sim.step()
	sim.set_local_player_weapon_input(false, false, false)
	var spent_clip := sim.get_local_player_weapon_state().clip
	assert_lt(spent_clip, full_clip, "the live M4 spent a round before reopening armory")

	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	var inventory := sim.get_local_player_inventory()
	var equipped_combo := inventory.equipped_combo
	var rebuilt_clip := -1
	for value in inventory.slots:
		var slot: PlayerInventorySlot = value
		if slot.combo == equipped_combo:
			rebuilt_clip = slot.clip
			break
	assert_gt(rebuilt_clip, spent_clip, "ACCEPT rebuilt the same named slot at full clip")
	sim.set_local_player_weapon(m4, {})
	var mounted := sim.get_local_player_weapon_state()
	assert_eq(mounted.clip, rebuilt_clip,
		"same-name real mount reads the rebuilt authoritative inventory")
	assert_eq(mounted.current_action, 0)
	assert_eq(mounted.next_action, 0)
	assert_false(mounted.windup_active)
	assert_false(sim.get_local_player_view().scope_engaged)


func test_loadout_weapon_category_switch_changes_equipped_weapon() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)

	assert_true(sim.apply_local_player_loadout([
		WeaponKitEntry.make("WPN_M4AUTO"),
		WeaponKitEntry.make("WPN_colt45"),
	], 8))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var primary_index := weapons.find_weapon("WPN_M4AUTO")
	assert_gte(primary_index, 0)
	sim.set_local_player_weapon(weapons.get_weapon(primary_index), {})
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
		"the primary is equipped before switching")
	sim.drain_local_player_weapon_events()

	# The retail '2' binding requests category 2 (secondary). Let the outgoing
	# weapon's SWITCHFROM action complete, then observe the committed slot.
	sim.request_local_player_weapon_category(2)
	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_name() == "WPN_colt45":
			break
	assert_eq(sim.get_local_player_weapon_name(), "WPN_colt45",
		"switching to a secondary in the accepted loadout changes the equipped weapon")


func test_weapon_switch_requested_during_draw_commits_without_a_second_press() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([
		WeaponKitEntry.make("WPN_M4AUTO"),
		WeaponKitEntry.make("WPN_colt45"),
	], 8))

	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var primary_index := weapons.find_weapon("WPN_M4AUTO")
	var secondary_index := weapons.find_weapon("WPN_colt45")
	assert_gte(primary_index, 0)
	assert_gte(secondary_index, 0)
	sim.set_local_player_weapon(weapons.get_weapon(primary_index), {})

	# First switch normally, then mirror the LocalPlayerPresenter installing the new
	# weapon definition. Its first tick enters SWITCHTO (the draw animation).
	sim.request_local_player_weapon_category(2)
	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_name() == "WPN_colt45":
			break
	assert_eq(sim.get_local_player_weapon_name(), "WPN_colt45")
	sim.set_local_player_weapon(weapons.get_weapon(secondary_index), {})
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().current_action, 6,
		"the newly equipped secondary is drawing through SWITCHTO")

	# One press during that draw must be remembered and committed after it settles.
	sim.request_local_player_weapon_category(3)
	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_name() == "WPN_M4AUTO":
			break
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
		"a switch requested during draw does not require a second press")


func test_late_spawn_player_resolves_own_adm_before_configured_usegun_pose() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# MissionRoot resolves per-entity ADMs once after the host player spawn. A
	# joiner's local L and host-admitted remote players spawn later; they must still
	# receive US01 rather than retaining the default E_STAND/soldier map. Otherwise
	# B50 phrase_set 4 cannot select anim_emplaced_5 and silently uses the generic
	# emplaced pose, visibly placing the body beside the gun.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
			MissionData.KIND_ITEM, 101419,
			Vector3(2, 0, 0), Vector3.ZERO))
	var sim := Simulation.new()
	# The fixture def row (101419) authors graphic mount, phrase_set 4, and
	# primary_weapon WPN_EMPLCD50NA — the native install reads all three.
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1419]))
	assert_true(sim.load_from_mission_data(md))
	assert_gt(sim.set_infantry_anim_map(_anim_root(), "soldier.adm"), 0)
	# Reproduce the production ordering bug: the one-time sweep happens before
	# this player exists.
	sim.resolve_infantry_adm_ids(_anim_root(), item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(self, sim, Vector3(2, 0, 0))

	var weapon_root := ResourceRoot.new()
	assert_eq(weapon_root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(weapon_root, "weapon.def"), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	sim.set_local_player_weapon(
			weapons.get_weapon(weapons.find_weapon("WPN_M4AUTO")), {})
	for _tick in range(120):
		if sim.get_local_player_weapon_state().current_action < 2:
			break
		sim.step()
	assert_true(sim.local_player_toggle_mount())
	sim.step()
	assert_eq(sim.get_local_player_anim_key(), "anim_idle",
			"the mount selects its next pose after this tick advances the playing clip")
	sim.step()
	assert_eq(sim.get_local_player_anim_key(), "anim_emplaced_5",
			"a late-spawn player receives US01 before phrase_set 4 selects its pose")


func _first_organic_ai_index(sim: Simulation) -> int:
	for i in 64:
		var d: EntityCard = sim.entity_card_by_ai_index(i)
		if d == null:
			break
		if d.get_pool() == 0:
			return i
	return -1

func test_local_first_person_usegun_parent_cull_follows_live_mount_slot() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# Retail suppresses the parent model only after THIS parent's MountSlot is
	# EquippedSlot in first person. Pre-commit attach, third person, and detach
	# render it normally. [orig: Entity_RenderVehicleModel @0x4407d0;
	# predicate @0x4407f6..0x44084c; Render_SubmitEntity @0x440918]
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(MissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_not_null(gun)
	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# This cull witness pairs the emplacement with WPN_EMPLCD50 (a def with an
	# authored gfx1), so the superset def overrides the fixture's AVENGER row.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	var item_db := _item_db_from_text(dir, ItemDbFixture.fixture_text(self)
			.replace("id 101294", "id 101294\n  graphic mount")
			.replace("primary_weapon WPN_AVENGER", "primary_weapon WPN_EMPLCD50"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(self, sim, Vector3(2, 0, 0))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 1))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var personal: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var mounted: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	sim.step() # seed the decoded listen-client present rows
	var gun_index := gun.index
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"an unattached emplacement renders in the world pass")

	assert_true(sim.local_player_toggle_mount())
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the parent stays visible until its embedded slot commits")
	sim.step()
	var mount_event_found := false
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_EMPLCD50":
			mount_event_found = true
	assert_true(mount_event_found)
	sim.set_local_player_weapon(mounted, {}, true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"authored gfx1 alone cannot cull when its first-person model failed to resolve")
	sim.set_local_player_first_person_model_available(true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 1,
			"the live parent slot with a resolved FP model suppresses the duplicate world gun")

	sim.set_local_player_debug_third_person(true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"third person restores the parent model")
	sim.set_local_player_debug_third_person(false)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 1)
	for _tick in range(120):
		if sim.get_local_player_weapon_state().current_action < 2:
			break
		sim.step()
	assert_true(sim.local_player_toggle_mount())
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"detach restores the world model immediately, before the holster commit")


func test_world_model_heat_glow_samples_parent_slot_and_caps_below_fp() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# The emplacement carries a deterministic HEAT_GLOW collision track: the
	# mount fixture with its LOD0 rows replaced by one register-driven slide of
	# part 1 (0..4 wu on CTRL 0 = HEAT_GLOW). The gun's visual/collision frame
	# samples its embedded MountSlot through the ewep writer, occupied or not;
	# the local FP state is the comparison witness for the intentionally
	# different endpoint.
	# [orig: attachment caller @ 0x546518;
	#  HUD_CacheWeaponSlotInfo stores @ 0x440969 / @ 0x440991]
	var object_data := ObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(
			SYN_MOUNT_HEAT_GLOW_SLIDE)), OK)
	assert_eq(String((object_data.get_control_registers()[0] as Dictionary).get("name", "")),
			"HEAT_GLOW")
	assert_eq(object_data.get_part_anim_count(0), 1)
	# (The row itself — a register-driven translation slide of part 1 on
	# CTRL 0 — is pinned by the minimal_3di_gen ctest: mount_heat_glow_slide_part1.)

	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(MissionData.KIND_ITEM, 101419,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_not_null(gun)
	var gun_index := gun.index

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# The authored HEAT_GLOW track rides the fixture bytes; the sim's own parse
	# of mount.3di is the only seat/collision source.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, SYN_MOUNT_HEAT_GLOW_SLIDE, "mount.3di")
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1419]))
	assert_eq(sim.get_mounted_graphic_source_count(), 1,
			"the fixture model resolves as the one mounted-pose source")
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(self, sim, Vector3(2, 0, 0))
	# Listen-host player creation rebuilds the authoritative registry. Bind the
	# collision instance to that final registry identity, as GameWorld does.
	assert_eq(sim.resolve_collision_instances(item_db), 1)

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 1))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var personal: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var mounted: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50NA"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	sim.step()

	# The 'ewep' render class's writer has no occupant test: an unoccupied
	# gun publishes its own cold slot.
	# [orig: HUD_CacheWeaponSlotInfo @0x440930 via the 'ewep' render-class
	#  row @0x82CFA0]
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_WORLD_HEAT_GLOW_VALID), 1,
			"the ewep writer publishes an unoccupied gun too")
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_WORLD_HEAT_GLOW), 0,
			"an unoccupied gun's slot is cold")
	var before_rows: Array = sim.get_hitbox_debug().entities
	assert_eq(before_rows.size(), 1)
	if before_rows.size() != 1:
		return
	var before: PackedVector3Array = (before_rows[0] as HitboxDebugEntity).tris

	assert_true(sim.local_player_toggle_mount())
	sim.step()
	var mounted_switch := false
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_EMPLCD50NA":
			mounted_switch = true
	assert_true(mounted_switch)
	sim.set_local_player_weapon(mounted, {}, true)
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_WORLD_HEAT_GLOW_VALID), 1)
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_WORLD_HEAT_GLOW), 0,
			"a live UseGun carrier owns the cold zero branch")

	sim.set_local_player_weapon_input(true, true, false)
	var fp_heat_glow := 0
	for _tick in range(3000):
		sim.step()
		fp_heat_glow = sim.get_local_player_weapon_state().heat_glow
		if fp_heat_glow >= 0x10000:
			break
	sim.set_local_player_weapon_input(false, false, false)
	assert_eq(fp_heat_glow, 0x10000,
			"the mounted first-person consumer reaches its distinct endpoint")
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_WORLD_HEAT_GLOW_VALID), 1)
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_WORLD_HEAT_GLOW), 0xFFFF,
			"the same inline slot saturates the world model at 0xFFFF")

	var after_rows: Array = sim.get_hitbox_debug().entities
	assert_eq(after_rows.size(), 1)
	if after_rows.size() != 1:
		return
	var after: PackedVector3Array = (after_rows[0] as HitboxDebugEntity).tris
	assert_eq(after.size(), before.size())
	var moved := 0
	for index in before.size():
		if before[index].distance_to(after[index]) > 3.9:
			moved += 1
	assert_gt(moved, 0,
			"headless collision consumes the same authoritative HEAT_GLOW frame")


func test_local_usegun_aim_articulates_emplaced_weapon_model() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# mount's authored PANM binds its turret and barrel to the semantic
	# EWEAP_GUNYAW/EWEAP_GUNPITCH registers. A mounted local player's live look
	# must pose those parts in authoritative model space, not only turn the camera.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
			MissionData.KIND_ITEM, 101419,
			Vector3(2, 0, 0), Vector3.ZERO))
	var object_data := ObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth/mount.3di")), OK)
	var usegun_info: ModelUserPoint = null
	for point_index in range(object_data.get_user_point_count()):
		var info := object_data.get_user_point_info(point_index)
		if info.name.nocasecmp_to("Usegun") == 0:
			usegun_info = info
	assert_not_null(usegun_info, "mount exposes its authored Usegun seat")
	var expected_usegun_world := MissionObjectPlacer.entity_transform(
			Vector3(2, 0, 0), Vector3.ZERO) * Vector3(
					usegun_info.position)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var sim := Simulation.new()
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1419]))
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	# Start well off the gun's authored zero yaw. Retail's attach request snaps the
	# requester's look/body heading to the UseGun heading before establishing the
	# relationship; leaving this stale produces the visible torso twist at the grips.
	const PRE_ATTACH_YAW_DEG := 160.0
	assert_true(sim.spawn_local_player(Vector3.ZERO, PRE_ATTACH_YAW_DEG, 1))
	assert_gt(absf(wrapf(sim.get_local_player_yaw_deg(), -180.0, 180.0)),
			90.0, 'fixture starts far from the gun yaw')
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 1))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var personal: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var mounted: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50NA"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	assert_true(sim.local_player_toggle_mount())
	sim.step()
	assert_lt(sim.get_local_player_position().distance_to(expected_usegun_world),
			0.001, "mounted player origin coincides with the authored Usegun point")
	assert_lt(absf(wrapf(sim.get_local_player_yaw_deg(), -180.0, 180.0)),
			0.01, 'UseGun attach pre-snaps a mismatched local look to the gun yaw')
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_EMPLCD50NA":
			sim.set_local_player_weapon(mounted, {}, true)
	sim.debug_set_panm_time_ms(0)
	var initial_weapon_state := sim.get_local_player_weapon_state()
	assert_true(initial_weapon_state.emplaced_controls_valid)
	var initial_yaw_control := initial_weapon_state.emplaced_gun_yaw
	assert_eq(initial_yaw_control, 0,
			'the attach snap starts EWEAP_GUNYAW at its neutral phase')
	var initial_overlay := sim.get_local_player_aim_overlay()
	assert_not_null(initial_overlay)
	var initial_body := initial_overlay.body_angles
	var initial_angles := initial_overlay.segment_angles
	assert_gt(initial_angles.size(), 0)
	assert_lt(absf(wrapf(initial_body.y, -180.0, 180.0)), 0.01,
			'the mounted body neutral overlay follows the snapped gun yaw')
	var max_initial_twist_deg := 0.0
	for angle in initial_angles:
		max_initial_twist_deg = maxf(max_initial_twist_deg,
				absf(wrapf(angle.y - initial_body.y, -180.0, 180.0)))
	assert_lt(max_initial_twist_deg, 0.01,
			'no segment retains the pre-attach look as a torso twist')
	var initial_pitch_control := initial_weapon_state.emplaced_gun_pitch

	var before_entities: Array = sim.get_hitbox_debug().entities
	assert_eq(before_entities.size(), 1)
	var before: PackedVector3Array = (before_entities[0] as HitboxDebugEntity).tris
	assert_gt(before.size(), 0)

	sim.set_local_player_mouse(511, false)
	var yaw_before := sim.get_local_player_yaw_deg()
	sim.add_local_player_look(100.0, 0.0)
	sim.step()
	assert_lt(sim.get_local_player_position().distance_to(expected_usegun_world),
			0.001, "look yaw cannot move the player off the Usegun point")
	assert_gt(absf(sim.get_local_player_yaw_deg() - yaw_before), 0.1,
			"the mounted local player's authoritative yaw changed")
	assert_ne(sim.get_local_player_weapon_state().emplaced_gun_yaw, initial_yaw_control,
			"look yaw reaches the retail EWEAP_GUNYAW phase")
	var yaw_entities: Array = sim.get_hitbox_debug().entities
	var after_yaw: PackedVector3Array = (yaw_entities[0] as HitboxDebugEntity).tris
	var yaw_moved := 0
	for index in before.size():
		if before[index].distance_to(after_yaw[index]) > 0.001:
			yaw_moved += 1
	assert_gt(yaw_moved, 0,
			"EWEAP_GUNYAW moves the authored mount turret with local look")

	var pitch_before := sim.get_local_player_pitch_deg()
	sim.add_local_player_look(0.0, 100.0)
	sim.step()
	assert_lt(sim.get_local_player_position().distance_to(expected_usegun_world),
			0.001, "look pitch cannot move the player off the Usegun point")
	assert_gt(absf(sim.get_local_player_pitch_deg() - pitch_before), 0.1,
			"the mounted local player's authoritative pitch changed")
	assert_ne(sim.get_local_player_weapon_state().emplaced_gun_pitch,
			initial_pitch_control,
			"look pitch reaches the retail EWEAP_GUNPITCH phase")
	var pitch_entities: Array = sim.get_hitbox_debug().entities
	var after_pitch: PackedVector3Array = (pitch_entities[0] as HitboxDebugEntity).tris
	var pitch_moved := 0
	for index in after_yaw.size():
		if after_yaw[index].distance_to(after_pitch[index]) > 0.001:
			pitch_moved += 1
	assert_gt(pitch_moved, 0,
			"EWEAP_GUNPITCH moves the authored mount barrel with local look")


func test_local_usegun_switches_viewmodel_and_borrows_parent_weapon_slot() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(
			MissionData.KIND_ITEM, 101294, Vector3(2, 0, 0), Vector3.ZERO)
	assert_not_null(gun)
	var sim := Simulation.new()
	# The fixture def row already authors primary_weapon WPN_AVENGER (a finite
	# clip makes parent-slot persistence observable across remounts); the
	# superset adds the mount graphic for the authored Usegun seat.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	var item_db := _item_db_from_text(dir, ItemDbFixture.fixture_text(self).replace(
			"id 101294", "id 101294\n  graphic mount"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(self, sim, Vector3(2, 0, 0))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([
		WeaponKitEntry.make("WPN_M4AUTO"),
		WeaponKitEntry.make("WPN_M4"),
	], 1))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var personal_idx := weapons.find_weapon("WPN_M4AUTO")
	var mounted_idx := weapons.find_weapon("WPN_AVENGER")
	assert_gte(personal_idx, 0)
	assert_gte(mounted_idx, 0)
	var personal_def: WeaponDef = weapons.get_weapon(personal_idx)
	var mounted_def: WeaponDef = weapons.get_weapon(mounted_idx)
	# Exercise the merge seam with a PowerThrow-shaped personal def while the
	# authoritative inventory still selects WPN_M4AUTO. A UseGun handoff must
	# cancel its windup before borrowing the parent's persistent slot.
	var powerthrow_personal := personal_def.copy()
	powerthrow_personal.flags = personal_def.flags | (1 << 31)
	sim.set_local_player_weapon(powerthrow_personal, {})
	sim.drain_local_player_weapon_events()
	sim.step() # move beyond tick zero, the windup's idle sentinel
	sim.set_local_player_weapon_input(true, true, false)
	for _tick in range(5):
		sim.step()
	var wound_before_mount := sim.get_local_player_weapon_state()
	assert_true(wound_before_mount.windup_active)
	var personal_fired_before := wound_before_mount.fired_serial
	var personal_rounds_before := wound_before_mount.round_ring_count
	var personal_clip := sim.get_local_player_weapon_state().clip
	var inventory_clip := int(
			sim.get_local_player_inventory().slots[0].clip)

	assert_true(sim.local_player_toggle_mount())
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
			"local UseGun stages the parent slot instead of assigning it immediately")
	# The still-held PowerThrow plus a simultaneous trigger edge cannot overwrite
	# the pending mount action.
	sim.set_local_player_weapon_input(true, true, false)
	sim.step()
	sim.set_local_player_weapon_input(false, false, false)
	var mount_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		var event: PlayerWeaponEvent = raw
		if event.switch_to_weapon == "WPN_AVENGER":
			mount_event = event
	assert_false(mount_event == null,
			"an Emplaced pending def takes retail's same-pump -901 commit")
	assert_true(mount_event.preserve_slot_state,
			"the host must not reset the emplacement's persistent slot")
	assert_eq(sim.get_local_player_weapon_name(), "WPN_AVENGER")
	var handed_off := sim.get_local_player_weapon_state()
	assert_false(handed_off.windup_active,
			"UseGun input suppression cancels the outgoing PowerThrow windup")
	assert_eq(handed_off.fired_serial, personal_fired_before)
	assert_eq(handed_off.round_ring_count, personal_rounds_before,
			"releasing through the handoff cannot leak a charged personal round")
	sim.set_local_player_weapon(mounted_def, {}, true)
	var mounted_before := sim.get_local_player_weapon_state()
	assert_eq(mounted_before.clip,
			mounted_def.clipsize)
	var mounted_slot_before_rebake := {
		"current_action": mounted_before.current_action,
		"next_action": mounted_before.next_action,
		"phase": mounted_before.phase,
		"clip": mounted_before.clip,
	}
	sim.rebake_local_player_weapon(mounted_def, {
		"anim_wpn_idle": PackedFloat32Array([0.2]),
	}, true)
	var mounted_after_rebake := sim.get_local_player_weapon_state()
	for field in mounted_slot_before_rebake:
		assert_eq(int(mounted_after_rebake.get(field)),
				int(mounted_slot_before_rebake[field]),
				"late mounted-def rebake preserves parent slot %s" % field)

	sim.set_local_player_weapon_input(true, true, false)
	var fired_before := mounted_before.fired_serial
	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_state().fired_serial > fired_before:
			break
	sim.set_local_player_weapon_input(false, false, false)
	var mounted_clip_after := int(
			sim.get_local_player_weapon_state().clip)
	assert_eq(mounted_clip_after, mounted_before.clip - 1,
			"local LMB consumes the parent's embedded weapon slot")
	assert_eq(sim.get_local_player_inventory().slots[0].clip, inventory_clip,
			"mounted fire cannot consume the saved personal magazine")

	for _tick in range(180):
		sim.step()
		if sim.get_local_player_weapon_state().current_action < 2:
			break
	assert_true(sim.local_player_toggle_mount())
	assert_eq(sim.get_local_player_weapon_name(), "WPN_AVENGER",
			"detach also waits for the mounted slot's holster commit")
	# The entity is already detached, but its borrowed slot still owns the pending
	# UseGun SWITCHFROM. Manual requests in this frame must not replace that action.
	sim.request_local_player_weapon_category(3)
	sim.request_local_player_weapon_cycle(1)
	sim.set_local_player_weapon_input(true, true, false)
	sim.step()
	sim.set_local_player_weapon_input(false, false, false)
	var detach_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		var event: PlayerWeaponEvent = raw
		if event.switch_to_weapon == "WPN_M4AUTO":
			detach_event = event
	assert_false(detach_event == null,
			"outgoing Emplaced detach also commits on the next pump")
	assert_true(detach_event.preserve_slot_state)
	sim.set_local_player_weapon(personal_def, {}, true)
	assert_eq(sim.get_local_player_weapon_state().clip,
			personal_clip, "the personal slot resumes with its original magazine")

	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_state().current_action < 2:
			break
	# A dismount leaves the player standing on the seat point (retail never
	# displaces the rider); step a unit back and look at the gun to remount.
	MountLook.face(self, sim, Vector3(2, 0, 0), Vector3(1, 0, 0))
	assert_true(sim.local_player_toggle_mount())
	var remount_event: PlayerWeaponEvent = null
	for _tick in range(120):
		sim.step()
		for raw in sim.drain_local_player_weapon_events():
			var event: PlayerWeaponEvent = raw
			if event.switch_to_weapon == "WPN_AVENGER":
				remount_event = event
		if not remount_event == null:
			break
	assert_false(remount_event == null)
	sim.set_local_player_weapon(mounted_def, {}, true)
	assert_eq(sim.get_local_player_weapon_state().clip,
			mounted_clip_after,
			"the emplacement keeps its own clip state while nobody is attached")

	sim.reset_session()
	var restart_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		var event: PlayerWeaponEvent = raw
		if event.switch_to_weapon == "WPN_M4AUTO":
			restart_event = event
	assert_false(restart_event == null,
			"restart explicitly restores the saved personal presentation")
	assert_false(restart_event.preserve_slot_state,
			"restart installs a fresh personal slot epoch")
	assert_false(sim.get_local_player_weapon_state().active,
			"the mounted definition cannot pump the restored personal slot")


func test_local_usegun_direct_swap_targets_latest_parent_without_switchto() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	# Three guns in a row along their +y facing, 3.5 u apart: a seated USE
	# swaps only onto a seat inside the rider's 5 deg cone, which the gunner
	# reaches by looking down the row at the next seat point (3.5 u out, a
	# 10 deg depression from the eye); the 4.0 u reach keeps the third gun
	# out of the first mount.
	var first_gun := md.add_entity(MissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO)
	var second_gun := md.add_entity(MissionData.KIND_ITEM, 101295,
			Vector3(2, 3.5, 0), Vector3.ZERO)
	var third_gun := md.add_entity(MissionData.KIND_ITEM, 101296,
			Vector3(2, 7, 0), Vector3.ZERO)
	assert_not_null(first_gun)
	assert_not_null(second_gun)
	assert_not_null(third_gun)
	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# All three fixture emplacement rows keep their authored primaries
	# (AVENGER / EMPLCD50 / EMPLCD50); the shared mount graphic supplies the
	# one authored Usegun seat per gun.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	var item_db := _item_db_from_text(dir, ItemDbFixture.fixture_text(self)
			.replace("id 101294", "id 101294\n  graphic mount")
			.replace("id 101295", "id 101295\n  graphic mount")
			.replace("id 101296", "id 101296\n  graphic mount"))
	_install_native_seat_table(sim, dir, item_db,
			PackedInt32Array([1294, 1295, 1296]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(self, sim, Vector3(2, 0, 0))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var personal: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var first_mount: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_AVENGER"))
	var second_mount: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	# Seed the listen-client rows and let the personal slot reach an attachable
	# state. The cull verdict is produced against this decoded presentation view.
	for _tick in range(80):
		sim.step()
		if sim.get_local_player_weapon_state().current_action < 2:
			break
	var first_gun_index := first_gun.index
	var second_gun_index := second_gun.index
	var third_gun_index := third_gun.index

	assert_true(sim.local_player_toggle_mount())
	sim.step()
	var first_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_AVENGER":
			first_event = raw
	assert_false(first_event == null)
	sim.set_local_player_weapon(first_mount, {}, true)
	# Even a host-side resolution report cannot manufacture fpModel on a Def that
	# has none, and a different target Def must not inherit that model identity.
	sim.set_local_player_first_person_model_available(true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			first_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"a live AVENGER MountSlot without gfx1 keeps its world model")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"mounting AVENGER cannot suppress the unrelated 50-cal parent")
	for _tick in range(80):
		sim.step()
		if sim.get_local_player_weapon_state().current_action < 2:
			break

	# Look down the row at the second gun's seat point (the same seat offset,
	# 3.5 u further along +y from this seat).
	MountLook.face(self, sim, MountLook.local_player_mission_position(sim) + Vector3(0, 3.5, 0))
	assert_true(sim.local_player_toggle_mount(),
			"a second gun inside the seated 5 deg cone is a direct mounted-seat swap")
	assert_eq(sim.get_local_player_weapon_name(), "WPN_AVENGER",
			"the old parent remains equipped until the rank commit")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			first_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"pre-commit swap restores the old parent world model")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"pre-commit target is not culled before its MountSlot is equipped")
	sim.step()
	var swap_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_EMPLCD50":
			swap_event = raw
	assert_false(swap_event == null,
			"latest pending parent commits directly with no personal interlude")
	sim.set_local_player_weapon(second_mount, {}, true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			first_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the committed swap leaves the old AVENGER parent visible")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the new authored model does not cull before its FP graphic resolves")
	sim.set_local_player_first_person_model_available(true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 1,
			"the resolved EMPLCD50 replacement culls only its own world model")
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().current_action, 0,
			"SWITCHRANK does not queue SWITCHTO onto the target parent slot")
	assert_true(sim.get_local_player_weapon_state().borrowed_usegun_slot)

	# The scan walks the rider's proximity slice, which refreshes every 16
	# ticks; it was last built at the first gun's seat, 8 u from the third gun,
	# so let it refresh before looking down the row.
	for _tick in range(17):
		sim.step()
	MountLook.face(self, sim, MountLook.local_player_mission_position(sim) + Vector3(0, 3.5, 0))
	assert_true(sim.local_player_toggle_mount(),
			"a third gun down the row drives a second direct mounted-seat swap")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the outgoing parent restores before the same-Def swap commits")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			third_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the target parent stays visible before its slot is live")
	sim.step()
	var same_def_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_EMPLCD50":
			same_def_event = raw
	assert_false(same_def_event == null)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			third_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 1,
			"the same resolved Def model immediately suppresses the newly live parent")


func test_death_during_usegun_draw_restores_personal_weapon() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(MissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO))
	var sim := Simulation.new()
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	var item_db := _item_db_from_text(dir, ItemDbFixture.fixture_text(self).replace(
			"id 101294", "id 101294\n  graphic mount"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var mounted_def: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_AVENGER"))
	var personal_def: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	sim.set_local_player_weapon(personal_def, {})
	sim.drain_local_player_weapon_events()

	MountLook.face(self, sim, Vector3(2, 0, 0))
	assert_true(sim.local_player_toggle_mount())
	sim.step()
	var mount_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_AVENGER":
			mount_event = raw
	assert_false(mount_event == null,
			"the emplacement commits before its SWITCHTO draw")
	sim.set_local_player_weapon(mounted_def, {}, true)
	var fired_before := sim.get_local_player_weapon_state().fired_serial
	sim.set_local_player_weapon_input(true, true, false)
	sim.debug_set_entity_health(0, 0)
	var restore_event: PlayerWeaponEvent = null
	for _tick in range(160):
		sim.step()
		for raw in sim.drain_local_player_weapon_events():
			if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_M4AUTO":
				restore_event = raw
		if not restore_event == null:
			break
	assert_false(restore_event == null,
			"forced detach during SWITCHTO eventually restores the personal slot")
	assert_eq(sim.get_local_player_weapon_state().fired_serial,
			fired_before, "a dead local gunner cannot fire the emplacement")
	sim.set_local_player_weapon(personal_def, {}, true)
	assert_false(sim.get_local_player_weapon_state().borrowed_usegun_slot)


func _admit_standalone_script_ticks(sim: Simulation) -> void:
	assert_true(sim.compile_and_set_wac(PackedStringArray(["set(ticks,-1)"])))


func test_organic_collision_samples_current_skeletal_pose_headlessly() -> void:
	if _binoc_path().is_empty():
		return
	# BINOC is the shipped 19-bone, three-frame BAD. Pair it with person's
	# canonical 19-row model table/COBJ block so the real SkeletalAnim ->
	# Simulation -> CollisionWorld path can be tested without retail assets.
	var dir := _native_fixture_dir()
	_write_char_rig(dir, "us02")
	# BINOC is a static three-frame clip. Turn BN15/head frame 1 into an
	# identity quaternion at its parser-pinned rotation offset (1324 + 16)
	# to make a deterministic moving-bone fixture while retaining its real
	# 19-bone hierarchy and every other shipped byte.
	var moving_bad := FileAccess.open(
			dir.path_join("BINOC.bad"), FileAccess.READ_WRITE)
	assert_not_null(moving_bad)
	if moving_bad == null:
		return
	moving_bad.seek(1340)
	moving_bad.store_float(0.0)
	moving_bad.store_float(0.0)
	moving_bad.store_float(0.0)
	moving_bad.store_float(1.0)
	moving_bad.close()

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth/person.3di")), OK)
	assert_true(data.has_collision())
	var skeletal := SkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			root, "BINOC.bad", {"anim_idle": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()),
			"19-bone collision rig loads: %s" % skeletal.get_last_error())
	assert_eq(skeletal.get_bone_count(), 19)
	var direct_a: Array = skeletal.eval_pose("anim_idle", 0.0)
	var direct_b: Array = skeletal.eval_pose("anim_idle", 1.0 / 30.0)
	var fixture_moved := 0
	for i in mini(direct_a.size(), direct_b.size()):
		if not (direct_a[i] as Transform3D).is_equal_approx(
				direct_b[i] as Transform3D):
			fixture_moved += 1
	assert_gt(fixture_moved, 0, "fixture must contain an animated bone")

	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			Vector3(10, 0, 0), Vector3.ZERO))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_gt(sim.set_infantry_anim_map(root, "us02.adm"), 0)
	sim.set_asset_root(root)
	assert_eq(sim.resolve_collision_instances(item_db), 1)

	var before: Array = sim.get_hitbox_debug().organics
	assert_eq(before.size(), 19, "one posed sphere per person COBJ/bone")
	var before_by_section := {}
	for value in before:
		var row: HitboxDebugOrganic = value
		assert_false(row.fallback)
		var pos := row.pos as Vector3
		assert_lt(pos.distance_to(Vector3(10, 0, 0)), 3.0,
				"bind pose applies entity translation exactly once")
		before_by_section[row.section] = pos
	assert_true(before_by_section.has(14), "head COBJ/bone is present")

	# No presentation node or Skeleton3D is involved: advancing authoritative
	# clip_phase must move the CollisionWorld/F3 matrices directly. The human
	# stands behind the soldier inside the F3 person view's 80-unit range.
	_spawn_fixture_human(sim, Vector3(10, 0, 60))
	for _tick in 2:
		sim.step()
	var after: Array = sim.get_hitbox_debug().organics
	assert_eq(after.size(), 19)
	var moved_sections := 0
	for value in after:
		var row: HitboxDebugOrganic = value
		var section := row.section
		if before_by_section.has(section) and (
				before_by_section[section] as Vector3).distance_to(
						row.pos as Vector3) > 0.0001:
			moved_sections += 1
	assert_gt(moved_sections, 0,
			"current BAD pose, not bind/entity-only matrices, drives collision")


func test_late_spawned_player_resolves_posed_collision_on_demand() -> void:
	if _binoc_path().is_empty():
		return
	# Mission collision is resolved before deploy in production. A player added
	# afterward must demand the same authored COBJ + ADM source instead of
	# becoming the one-sphere fallback until the next explicit sweep.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	# The player's own graphic (US01) + rig live in the sim's asset root; the
	# demand resolve must find them there after the sweep already ran.
	var dir := _native_fixture_dir()
	_write_char_rig(dir, "us01")

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 0,
			"the initial pre-deploy sweep has no player to attach")
	assert_true(sim.spawn_local_player(Vector3(10, 0, 0), 0.0, 1))

	# F3 still exercises CollisionWorld's demand provider for late targets, but
	# presentation filters the local avatar before any posed/fallback row escapes.
	assert_true((sim.get_hitbox_debug().organics as Array).is_empty(),
			"the local avatar never renders posed or fallback hitboxes")
	var local_bms_id := sim.entity_card_by_ai_index(
			sim.get_entity_count() - 1).get_bms_id()
	assert_true(sim.has_collision_instance(local_bms_id),
			"the hidden local avatar was nevertheless attached on demand")


func test_f3_hides_local_player_and_omits_distant_posed_organic() -> void:
	if _binoc_path().is_empty():
		return
	# The F3 person view is a nearby diagnostic. It must neither wrap the local
	# avatar in debug spheres nor spend its pose/debug budget on a target more
	# than 80 mission units away.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			Vector3(200, 0, 0), Vector3.ZERO))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var dir := _native_fixture_dir()
	_write_char_rig(dir, "us02")

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	var rows: Array = sim.get_hitbox_debug().organics
	assert_true(rows.is_empty(),
			"F3 omits the 200-unit target while still excluding local handle 1")


func test_reused_player_slot_invalidates_old_collision_attempt_identity() -> void:
	if _binoc_path().is_empty():
		return
	# US02 intentionally cannot resolve through this provider, so the mission
	# soldier leaves a negative collision attempt on pool-0 slot 0. After WAC
	# removes it, the local US01 player reuses that exact packed handle. The new
	# registry spawn identity must invalidate the negative cache and resolve all
	# authored person sections on demand.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			Vector3.ZERO, Vector3.ZERO))
	var probe := Simulation.new()
	assert_true(probe.load_from_mission_data(md))
	var old_ssn := probe.get_entity_net_id(0)
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(
			0, 22, 0, old_ssn))

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	# A US01-only root: the placed US02 soldier deliberately cannot resolve,
	# while the replacement local player's graphic can.
	var dir := _native_fixture_dir()
	_write_char_rig(dir, "us01")

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	_admit_standalone_script_ticks(sim)
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 0,
			"US02 records one unresolved attempt on slot 0")
	for _tick in 16:
		sim.step()
	assert_true(sim.get_entity_effect_state_for_ssn(old_ssn).is_empty(),
			"the original slot occupant was removed")
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1),
			"US01 reuses the freed pool-0 slot")

	assert_true((sim.get_hitbox_debug().organics as Array).is_empty(),
			"the newly resolved local avatar remains hidden from F3")
	var local_bms_id := sim.entity_card_by_ai_index(
			sim.get_entity_count() - 1).get_bms_id()
	assert_true(sim.has_collision_instance(local_bms_id),
			"the old negative attempt cannot suppress the new slot identity")


func test_restart_re_resolves_the_restored_collision_identity() -> void:
	if _binoc_path().is_empty():
		return
	# Collision caches live outside World::Snapshot. Reusing slot 0 during play
	# must not leave the restored baseline actor unbound after Stop/Restart.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			Vector3.ZERO, Vector3.ZERO)
	assert_not_null(placed)
	var bms_id := placed.bms_id
	var probe := Simulation.new()
	assert_true(probe.load_from_mission_data(md))
	var old_ssn := probe.get_entity_net_id(0)
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(
			0, 22, 0, old_ssn))

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	# Both the placed US02 soldier and the replacement US01 local player
	# resolve from the same root.
	var dir := _native_fixture_dir()
	_write_char_rig(dir, "us02")
	_write_char_rig(dir, "us01")

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	_admit_standalone_script_ticks(sim)
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	assert_true(sim.has_collision_instance(bms_id))
	for _tick in 16:
		sim.step()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_true((sim.get_hitbox_debug().organics as Array).is_empty())
	var local_bms_id := sim.entity_card_by_ai_index(
			sim.get_entity_count() - 1).get_bms_id()
	assert_true(sim.has_collision_instance(local_bms_id),
			"the replacement local occupant receives the cached graphic")

	sim.reset_session()
	assert_true(sim.has_collision_instance(bms_id),
			"restart rebinds the baseline before any F3 or round demand query")
	var restored_rows: Array = sim.get_hitbox_debug().organics
	assert_eq(restored_rows.size(), 19,
			"the restored non-local actor exposes every authored section")
	for value in restored_rows:
		var row: HitboxDebugOrganic = value
		assert_eq(row.entity_handle, 0)
		assert_false(row.fallback)
