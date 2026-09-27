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


func should_skip_script():
	return RetailData.def_root_skip()


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


func _install_combat_tables(sim: Simulation) -> void:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	sim.resolve_item_traits(item_db)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	# Current-motor eyes and collision capsules require the native clip map.
	assert_gt(sim.set_infantry_anim_map(PresenterFixture.anim_root(self), "soldier.adm"), 0)


func _retail_smoke_grenade() -> WeaponDef:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(root, "weapon.def"), OK)
	var index := weapons.find_weapon("WPN_GRENADESM")
	assert_gte(index, 0)
	return weapons.get_weapon(index) if index >= 0 else null


func _throwable_visual_count(sim: Simulation, item_id: int) -> int:
	var count := 0
	for value in sim.get_throwable_visuals():
		if (value as ThrowableVisualRow).item_id == item_id:
			count += 1
	return count


func _throwable_move_effect(sim: Simulation, item_id: int) -> String:
	for value in sim.get_throwable_visuals():
		var visual := value as ThrowableVisualRow
		if visual.item_id == item_id:
			return visual.move_effect
	return ""


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


func test_host_smoke_grenade_survives_arm_age_on_joiner_until_real_fuse() -> void:
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
			"127.0.0.1", host.get_host_listen_port(), "SmokeObserver"))
	assert_true(joiner.load_from_mission_data(mission))
	_install_combat_tables(joiner)
	assert_true(_drive_pair_to_match(host, joiner),
			"joiner reached the real-UDP in-match seam")
	if not joiner.is_joined_in_match():
		return

	var smoke := _retail_smoke_grenade()
	assert_true(host.apply_local_player_loadout([WeaponKitEntry.make("WPN_GRENADESM")], 8))
	host.set_local_player_weapon(smoke, {})
	for _settle in range(20):
		host.step()
		joiner.step()
		OS.delay_msec(1)

	var host_health_before := host.get_local_player_health()
	var joiner_health_before := joiner.get_local_player_health()
	host.set_local_player_weapon_input(true, true, false)
	for _windup in range(5):
		host.step()
		joiner.step()
		OS.delay_msec(1)
	host.set_local_player_weapon_input(false, false, false)

	var first_seen := -1
	for tick in range(200):
		host.step()
		joiner.step()
		if _throwable_visual_count(host, 1875) > 0 \
				and _throwable_visual_count(joiner, 1875) > 0:
			first_seen = tick
			break
		OS.delay_msec(1)
	assert_gte(first_seen, 0,
			"the host's production smoke round crossed tag-2 into joiner flight")
	if first_seen < 0:
		return

	var host_arm_sounds := 0
	var joiner_arm_sounds := 0
	var host_move_effect_survived_arm := true
	var joiner_move_effect_survived_arm := true
	for _tick in range(320):
		host.step()
		joiner.step()
		host_move_effect_survived_arm = host_move_effect_survived_arm \
				and _throwable_move_effect(host, 1875) == "Effect_SmokeToss"
		joiner_move_effect_survived_arm = joiner_move_effect_survived_arm \
				and _throwable_move_effect(joiner, 1875) == "Effect_SmokeToss"
		for value in host.drain_round_impacts():
			if (value as RoundImpactRow).sound == "EXPLO_SMOK_GREN":
				host_arm_sounds += 1
		for value in joiner.drain_round_impacts():
			if (value as RoundImpactRow).sound == "EXPLO_SMOK_GREN":
				joiner_arm_sounds += 1
		OS.delay_msec(1)

	assert_eq(_throwable_visual_count(host, 1875), 1,
			"authority retains the smoke grenade after arm_age")
	assert_eq(_throwable_visual_count(joiner, 1875), 1,
			"the tag-2 visual client retains the smoke grenade after arm_age")
	assert_true(host_move_effect_survived_arm,
			"authority keeps the continuous smoke move effect through arm_age")
	assert_true(joiner_move_effect_survived_arm,
			"tag-2 reconstruction keeps the same smoke move effect through arm_age")
	assert_eq(host_arm_sounds, 1,
			"authority presents the witnessed obj-row smoke-pour event once")
	assert_eq(joiner_arm_sounds, 1,
			"visual-only flight presents the same witnessed arm event once")
	assert_eq(host.get_local_player_health(), host_health_before,
			"arm_age is not an authoritative detonation")
	assert_eq(joiner.get_local_player_health(), joiner_health_before,
			"the visual-only client cannot apply a grenade consequence")
	var diagnostics: Dictionary = joiner.get_joiner_network_diagnostics()
	assert_eq(int(diagnostics.get("gap_depth", -1)), 0,
			"the steady replicated session has no ordered sequence gap")
	assert_false(bool(diagnostics.get("freeze_suspected", true)),
			"flat idle records without a gap are not a replication freeze")

	var host_fuse_sounds := 0
	var joiner_fuse_sounds := 0
	for _tick in range(2300):
		host.step()
		joiner.step()
		for value in host.drain_round_impacts():
			if (value as RoundImpactRow).sound == "EXPLO_SMOK_GREN":
				host_fuse_sounds += 1
		for value in joiner.drain_round_impacts():
			if (value as RoundImpactRow).sound == "EXPLO_SMOK_GREN":
				joiner_fuse_sounds += 1
		if _throwable_visual_count(host, 1875) == 0 \
				and _throwable_visual_count(joiner, 1875) == 0:
			break
		OS.delay_msec(1)

	# The loop bound only has to outlast the mounted fuse (base JO authors 1860
	# ticks, the revx02 expansion 2480); what this pins is that BOTH sides retire
	# the round on the same fuse and neither presents a fuse event: the expiry
	# head presents the obj row only for a nonzero kztype, which grenadesm lacks.
	# [orig: Projectile_UpdatePhysics @0x4E9DC6, kztype @0x4E9DDC..0x4E9DE1]
	assert_eq(_throwable_visual_count(host, 1875), 0,
			"authority releases the smoke grenade on its authored fuse")
	assert_eq(_throwable_visual_count(joiner, 1875), 0,
			"joiner releases the replicated smoke grenade on the same fuse")
	assert_eq(host_fuse_sounds, 0, "authority presents no fuse event for the class-less smoke")
	assert_eq(joiner_fuse_sounds, 0, "joiner presents no fuse event for the class-less smoke")
