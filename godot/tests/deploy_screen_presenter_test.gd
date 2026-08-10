extends GutTest

# DeployScreenPresenter on the typed surfaces (ADR 0034): the joiner state is a
# REAL loopback join — host + joiner Simulations over real UDP, the
# coop_two_sim recipe — whose spawn-zone admission genuinely holds
# is_join_deploy_pick_pending, and the world seam is a GameWorld subclass
# harness. The deployment RELEASE is the real thing too: the picked C2S 0x0E
# reaches the host and the falling pending bit closes the screen.
#
# (The old value-double's host-side zone-mutation refresh leg — a zone turning
# contested between refreshes — has no typed equivalent without host-side zone
# capture orchestration; the param-keyed selection survival across the periodic
# rebuild is pinned below on unchanged zones.)

const DeployPresenter := preload("res://game/world/deploy_screen_presenter.gd")
const TMP_DIR := "res://.godot/deploy_screen_presenter_test"

const AI_TYPE := 0x14BF        # Generic Soldier (items.def id 105311)
const SPAWN_ZONE_TYPE := 1359  # pool-1 fixture; ItemDef supplies SpawnPoint


class DeployWorldHarness:
	extends GameWorld
	var root: ResourceRoot
	var sim: Simulation

	func get_sim() -> Simulation:
		return sim

	func get_resource_root() -> ResourceRoot:
		return root


var _overlay: Control = null


func before_each() -> void:
	Strings.clear()
	var dir := ProjectSettings.globalize_path(TMP_DIR)
	if not DirAccess.dir_exists_absolute(dir):
		assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_copy_fixture("res://../fixtures/mnu/jo_death.mnu", dir.path_join("death.mnu"))
	_copy_fixture("res://../fixtures/rtxt/menutxt.bin", dir.path_join("menutxt.BIN"))
	_copy_fixture("res://../fixtures/rtxt/gametext.bin", dir.path_join("gametext.bin"))


func after_each() -> void:
	_overlay = null
	Strings.clear()


func after_all() -> void:
	var dir := ProjectSettings.globalize_path(TMP_DIR)
	for name in ["death.mnu", "menutxt.BIN", "gametext.bin"]:
		var path := dir.path_join(name)
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	DirAccess.remove_absolute(dir)
	var zone_def := ProjectSettings.globalize_path(
			"res://.godot/deploy_spawn_zone_items.def")
	if FileAccess.file_exists(zone_def):
		DirAccess.remove_absolute(zone_def)


func _copy_fixture(source: String, target: String) -> void:
	var output := FileAccess.open(target, FileAccess.WRITE)
	assert_not_null(output, "temporary deploy-screen fixture opens for write")
	if output != null:
		output.store_buffer(FileAccess.get_file_as_bytes(source))
		output.close()


func _make_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(TMP_DIR)), OK)
	return root


func _anim_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/anim")), OK)
	return root


# The fixture items.def plus one deploy-selectable SpawnPoint row (the
# coop_two_sim recipe; mission id 1359 promotes to ItemDef 101359).
func _spawn_zone_item_db() -> ItemDatabase:
	var base_path := ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")
	var base_file := FileAccess.open(base_path, FileAccess.READ)
	assert_not_null(base_file)
	if base_file == null:
		return null
	var base_items := base_file.get_as_text().replace("\r\n", "\n")
	base_file.close()
	var path := ProjectSettings.globalize_path(
			"res://.godot/deploy_spawn_zone_items.def")
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return null
	file.store_string(base_items)
	if not base_items.ends_with("\n"):
		file.store_string("\n")
	file.store_string("""begin "Deploy Spawn Zone Fixture"
  id 101359
  type object
  graphic MrkAlpha
  sid deploy_spawn_zone
  hp 100
  attrib: SpawnPoint
end
""")
	file.close()
	var result := ItemDatabase.new()
	assert_eq(result.load(path), OK)
	return result


func _spawn_zone_mission() -> MissionData:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, AI_TYPE, Vector3(0, 0, 0), Vector3.ZERO)
	var zone := md.add_entity(
			MissionData.KIND_ITEM, SPAWN_ZONE_TYPE, Vector3(40, 0, 0), Vector3.ZERO)
	assert_false(zone.is_empty())
	if not zone.is_empty():
		assert_true(md.set_entity_property_int(
				MissionData.KIND_ITEM, int(zone.get("index", -1)), "team", 1))
	return md


# A REAL loopback join held at the deploy pick: host + joiner free-run until
# the joiner is in-match with the pick pending (the coop_two_sim recipe).
# Returns {host, joiner}; both are autofreed Nodes.
func _join_pair_with_pending_pick() -> Dictionary:
	var mission := _spawn_zone_mission()
	var item_db := _spawn_zone_item_db()
	assert_not_null(item_db)

	var host := Simulation.new()
	autofree(host)
	host.configure_host_session({"gametype": 0x30020})
	assert_true(host.enable_host_listen(0), "host bound an OS-assigned UDP port")
	assert_true(host.load_from_mission_data(mission))
	assert_true(host.spawn_local_player(Vector3(5, 0, 5), 0.0, 1))
	var host_anim := _anim_root()
	assert_gt(host.set_infantry_anim_map(host_anim, "soldier.adm"), 0)
	host.resolve_item_traits(item_db)
	host.resolve_infantry_adm_ids(host_anim, item_db)
	var port: int = host.get_host_listen_port()
	assert_gt(port, 0)

	var joiner := Simulation.new()
	autofree(joiner)
	assert_true(joiner.enable_join("127.0.0.1", port, "DeployJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	var joiner_anim := _anim_root()
	assert_gt(joiner.set_infantry_anim_map(joiner_anim, "soldier.adm"), 0)
	joiner.resolve_item_traits(item_db)
	joiner.resolve_infantry_adm_ids(joiner_anim, item_db)

	var reached := false
	for _i in range(800):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match():
			reached = true
			break
		OS.delay_msec(2)
	assert_true(reached, "the joiner reached in-match over real loopback UDP")
	assert_true(joiner.is_join_deploy_pick_pending(),
			"the spawn-zone join holds the player-paced deploy pick")
	return {"host": host, "joiner": joiner}


func _make_presenter(sim: Simulation) -> DeployPresenter:
	var world := DeployWorldHarness.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	world.root = _make_root()
	world.sim = sim
	add_child_autofree(world)
	_overlay = Control.new()
	_overlay.size = Vector2(800, 600)
	add_child_autofree(_overlay)
	var presenter := DeployPresenter.new()
	presenter.setup(world, _overlay)
	add_child_autofree(presenter)
	return presenter


# The presenter's row PARAM at a visible list row (the old ItemList metadata's
# successor: the presenter row model carries the node parameter per row).
func _row_param(presenter: DeployPresenter, row: int) -> int:
	var rows: Array = presenter.get_spawn_rows()
	if row < 0 or row >= rows.size():
		return -1
	return int((rows[row] as Dictionary).get("param", -1))


# The shell leaves State.WORLD on `opened` and returns on `closed`; without those
# two signals firing, LocalPlayerPresenter re-captures the mouse every frame and the
# SPAWNPOINTS_LIST rows cannot be clicked at all.
func test_open_and_close_emit_the_shell_state_signals() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	watch_signals(presenter)

	assert_false(presenter.is_open(), "the screen starts closed")
	assert_true(presenter.open(), "a pending pick opens the screen")
	assert_signal_emitted(presenter, "opened", "opening tells the shell to release the cursor")
	assert_true(presenter.is_open())

	presenter.close()
	assert_signal_emitted(presenter, "closed", "closing hands gameplay input back to the world")
	assert_false(presenter.is_open())

	# close() is idempotent: a second call must not re-emit and strand the shell.
	assert_signal_emit_count(presenter, "closed", 1)
	presenter.close()
	assert_signal_emit_count(presenter, "closed", 1, "closing an already-closed screen is a no-op")


func test_open_refuses_when_no_pick_is_owed() -> void:
	# A spawned OFFLINE player owes no deployment pick — the real
	# is_join_deploy_pick_pending is false outside a held join.
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new()
	autofree(sim)
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var presenter := _make_presenter(sim)
	watch_signals(presenter)
	assert_false(presenter.open(), "no pending pick means no screen")
	assert_signal_emit_count(presenter, "opened", 0, "a refused open must not move the shell state")
	assert_false(presenter.is_open())


# Retail enters the active session and resumes uplinks after its initial 0x5A grants,
# before the player chooses a spawn row. The deploy UI is driven by the independent
# authoritative pending bit and must survive that in-match edge.
func test_in_match_does_not_close_a_still_pending_deploy_screen() -> void:
	var pair := _join_pair_with_pending_pick()
	assert_true(pair.joiner.is_joined_in_match(),
			"the real join is in-match while the pick stays pending")
	var presenter := _make_presenter(pair.joiner)
	watch_signals(presenter)

	assert_true(presenter.open(), "the pending deploy UI opens in an active session")
	await get_tree().process_frame

	assert_true(presenter.is_open(), "in-match gameplay does not dismiss a pending deploy UI")
	assert_signal_emit_count(presenter, "closed", 0,
			"only authority clearing deployment-pending closes a healthy screen")


func test_refresh_preserves_selected_spawn_identity_by_param() -> void:
	# Retail rebuilds the row set every 16 ticks and preserves the selected
	# node/param, not the visual row number. The periodic rebuild runs against
	# the REAL zone rows here; selection must survive the clear+repopulate.
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)

	assert_true(presenter.open(), "the player-paced join opens death.mnu")
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	if driver == null:
		return
	var list_id := driver.widget_id("SPAWNPOINTS_LIST")
	assert_gte(list_id, 0, "death.mnu authors the spawn list")
	var zone_rows: Array = pair.joiner.get_deploy_spawn_zones()
	assert_eq(zone_rows.size(), 1,
			"the fixture exposes one team-owned non-default spawn-zone row")
	assert_eq(driver.get_widget_items(list_id).size(), 1 + zone_rows.size(),
			"default plus the secured zone")
	assert_eq(presenter.get_spawn_rows().size(), 1 + zone_rows.size(),
			"the presenter row model aligns with the compiled list")
	var zone_param := int((zone_rows[0] as Dictionary).get("param", 0))
	assert_gt(zone_param, 0)
	driver.select_row(list_id, 1, false)  # highlight only; picks ride user clicks
	assert_eq(_row_param(presenter, 1), zone_param,
			"the zone row is selected by deploy param")

	await get_tree().create_timer(DeployPresenter.REFRESH_INTERVAL_S + 0.05).timeout

	var reselected := driver.selected_row(list_id)
	assert_gte(reselected, 0, "the selected spawn survives the periodic rebuild")
	assert_eq(_row_param(presenter, reselected), zone_param,
			"selection follows the spawn param across the clear+repopulate")


# Death mid-match re-arms the pick and the shell reopens THIS SAME presenter: the menu is
# configured once (_ensure_menu reuses the frame+driver) and close() only hides it, so the
# driver's list state — and the row the player picked last time — survive into the next
# deploy screen, where _populate_spawn_list re-selects that row by param. The driver
# re-emits widget_value_changed on every list click (the old ItemList allow_reselect=true
# successor semantics), so a click on the already-selected row still queues a pick.
# [orig: DeathScreen_OnSpawnListSelect @0x553630 -> Input_QueueEvent(12, node) @0x55364d]
# The tail is the REAL deployment release: the default-spawn pick rides C2S 0x0E to the
# live host and the falling pending bit closes the screen through `closed`.
func test_a_reopened_death_screen_repicks_and_the_release_closes_it() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	watch_signals(presenter)

	assert_true(presenter.open(), "the join deploy screen opens")
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	if driver == null:
		return
	var list_id := driver.widget_id("SPAWNPOINTS_LIST")
	assert_gte(list_id, 0, "death.mnu authors the spawn list")

	# Close and reopen WITHOUT a release: the once-configured menu and its
	# selected row must come back (the death-edge reopen shape).
	driver.select_row(list_id, 0, false)  # the player's pick, highlight only
	presenter.close()
	assert_false(presenter.is_open())
	assert_true(pair.joiner.is_join_deploy_pick_pending(),
			"nothing was released; the pick is still owed")
	assert_true(presenter.open(), "the death edge reopens the deploy screen")
	assert_eq(presenter.get_menu_driver(), driver,
			"the menu is configured once; close() only hides it")
	var selected := driver.selected_row(list_id)
	assert_gte(selected, 0, "the reopened screen restores the previous pick")
	assert_eq(_row_param(presenter, selected), 0,
			"and the restored row is the default spawn")

	# The re-pick CLICK on the already-selected row queues the REAL C2S 0x0E
	# (the driver re-emits on every click — allow_reselect successor); the
	# host's release clears the pending bit, and the presenter's poll closes
	# the screen through `closed` (the deployment release, not a teardown).
	driver.select_row(list_id, selected)
	var released := false
	for _i in range(800):
		pair.host.step()
		pair.joiner.step()
		if not pair.joiner.is_join_deploy_pick_pending():
			released = true
			break
		OS.delay_msec(2)
	assert_true(released, "the picked deployment releases on the live host")
	await get_tree().process_frame
	assert_false(presenter.is_open(), "the release closes the screen")
	# One `closed` from the manual death-edge close above, one from the release.
	assert_signal_emit_count(presenter, "closed", 2,
			"the release hands gameplay input back through `closed`")
