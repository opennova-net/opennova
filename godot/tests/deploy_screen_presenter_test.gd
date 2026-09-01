extends GutTest

# DeployScreenPresenter on the typed surfaces (ADR 0034): the joiner state is a
# REAL loopback join — host + joiner Simulations over real UDP, the
# coop_two_sim recipe — driven to the DEATH edge: the initial join deploys with
# no pick (retail sends no initial C2S 0x0E), then the authority kills the
# joiner and begin_redeployment genuinely holds is_join_deploy_pick_pending.
# The world seam is a GameWorld subclass harness. The deployment RELEASE is the
# real thing too: the picked C2S 0x0E reaches the host and the falling pending
# bit closes the screen.
#
# (The old value-double's host-side zone-mutation refresh leg — a zone turning
# contested between refreshes — has no typed equivalent without host-side zone
# capture orchestration; the param-keyed selection survival across the periodic
# rebuild is pinned below on unchanged zones.)

const DeployPresenter := preload("res://game/world/deploy_screen_presenter.gd")
const TMP_DIR := "res://.godot/deploy_screen_presenter_test"

# The retail death.mnu and the string tables it resolves come from the
# reference fixture set (docs/asset-gated-tests.md); the whole script skips
# without it.
const STAGED_FIXTURES := {
	"mnu/jo_death.mnu": "death.mnu",
	"rtxt/menutxt.bin": "menutxt.BIN",
	"rtxt/gametext.bin": "gametext.bin",
}


func should_skip_script():
	for rel in STAGED_FIXTURES:
		if RetailData.fixture(rel).is_empty():
			return RetailData.fixture_pending_text(rel)
	return false

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
	for rel in STAGED_FIXTURES:
		_copy_fixture(RetailData.fixture(rel), dir.path_join(STAGED_FIXTURES[rel]))


func after_each() -> void:
	_overlay = null
	Strings.clear()


func after_all() -> void:
	var dir := ProjectSettings.globalize_path(TMP_DIR)
	for name in STAGED_FIXTURES.values():
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


# A REAL loopback join driven to in-match with NO pick owed (retail's initial
# join sends no C2S 0x0E) over the spawn-zone host, which keeps the D-NET-156
# respawn-pending hold. Returns {host, joiner}; both are autofreed Nodes.
func _join_pair_in_match() -> Dictionary:
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
	assert_false(joiner.is_join_deploy_pick_pending(),
			"the initial join deploys with no forced C2S 0x0E")
	return {"host": host, "joiner": joiner}


# The in-match pair driven onward to the DEATH deploy pick: the authority kills
# the joiner and the death edge re-arms the pick (begin_redeployment). Returns
# {host, joiner}; both are autofreed Nodes.
func _join_pair_with_pending_pick() -> Dictionary:
	var pair := _join_pair_in_match()
	var host: Simulation = pair.host
	var joiner: Simulation = pair.joiner
	# The kill targets the joiner's wire handle: wait for the 0x0C name-match to
	# bind it, then the authority's real death transaction re-arms the pick (the
	# DEATH screen).
	var self_bound := false
	for _i in range(400):
		host.step()
		joiner.step()
		if joiner.get_joiner_self_handle() > 0:
			self_bound = true
			break
		OS.delay_msec(2)
	assert_true(self_bound, "the joiner bound its wire handle before the kill")
	assert_eq(host.debug_kill_player_entity(joiner.get_joiner_self_handle()), OK,
			"the host queued the joiner's death")
	var pick_pending := false
	for _i in range(240):
		host.step()
		joiner.step()
		if joiner.is_join_deploy_pick_pending():
			pick_pending = true
			break
		OS.delay_msec(2)
	assert_true(pick_pending,
			"the death edge holds the deploy pick (begin_redeployment)")
	# A death within 620 ticks of the deployment arms the host's 3-second pick
	# penalty (silently dropped picks). Settle past it so each test's single
	# pick/release leg operates in the clean accepting window.
	for _i in range(260):
		host.step()
		joiner.step()
	assert_true(joiner.is_join_deploy_pick_pending(),
			"nothing auto-picks while the death pick stays owed")
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


# The visible list row carrying a node PARAM (the sorted list's row order is
# retail's text sort, so tests never assume fixed indices).
func _row_index_for_param(presenter: DeployPresenter, param: int) -> int:
	var rows: Array = presenter.get_spawn_rows()
	for row in rows.size():
		if int((rows[row] as Dictionary).get("param", -2)) == param:
			return row
	return -1


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


# The host-driven deploy-map OVERLAY: the spawn-zone host keeps the D-NET-156
# respawn-pending hold, whose per-frame 0x0A flags1 bit1 arms
# is_join_deploy_overlay_active while NO pick is owed. The same death.mnu
# screen opens off it, live frames keep it open while the host keeps the bit
# set, and a spawn-row click dismisses it LOCALLY with no 0x0E (retail input
# case 12's dialogs-reset half; the send half is unported pending a
# deployed-dismiss capture — the presenter documents why).
# [orig: the open Render_ProcessMainSceneFrame @0x5cab5e; the per-frame fold
#  NapiNPClientMsg_0x00A @0x42ff82]
func test_overlay_opens_with_no_pick_and_a_row_click_dismisses() -> void:
	var pair := _join_pair_in_match()
	var overlay := false
	for _i in range(240):
		pair.host.step()
		pair.joiner.step()
		if pair.joiner.is_join_deploy_overlay_active():
			overlay = true
			break
		OS.delay_msec(2)
	assert_true(overlay, "the held joiner's per-frame flags1 bit1 arms the overlay")
	assert_false(pair.joiner.is_join_deploy_pick_pending(), "no pick is owed")
	var presenter := _make_presenter(pair.joiner)
	watch_signals(presenter)
	assert_true(presenter.open(), "the overlay opens the deploy screen without a pick")
	assert_signal_emitted(presenter, "opened")
	for _i in range(20):
		pair.host.step()
		pair.joiner.step()
		OS.delay_msec(2)
	await get_tree().process_frame
	assert_true(presenter.is_open(), "the host-held overlay keeps the screen open")
	var rows: Array = presenter.get_spawn_rows()
	var row := -1
	for i in rows.size():
		if int((rows[i] as Dictionary).get("param", -1)) != -1:
			row = i
			break
	assert_gte(row, 0, "the spawn list carries a selectable row")
	presenter.select_spawn_row(row)
	assert_false(presenter.is_open(), "the overlay row click closes the screen locally")
	assert_signal_emitted(presenter, "closed")
	assert_false(pair.joiner.is_join_deploy_pick_pending(),
			"no 0x0E was sent — nothing armed a pick or release wait")


# Retail's deploy keys 'X' and SPACE (input case 12) dismiss the OVERLAY-only
# screen locally; while a pick is owed they stay inert (the pick flow keeps its
# list-select). The witnesses live in hud-re D-HUD-19.
func test_overlay_x_and_space_dismiss_only_without_a_pending_pick() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	watch_signals(presenter)
	assert_true(presenter.open(), "the pending deploy UI opens")
	assert_true(pair.joiner.is_join_deploy_pick_pending(), "a pick is owed")
	var key := InputEventKey.new()
	key.keycode = KEY_X
	key.pressed = true
	presenter.get_viewport().push_input(key)
	await get_tree().process_frame
	assert_true(presenter.is_open(),
			"X stays inert while the pick flow owns the screen")

	var pair2 := _join_pair_in_match()
	var overlay := false
	for _i in range(240):
		pair2.host.step()
		pair2.joiner.step()
		if pair2.joiner.is_join_deploy_overlay_active():
			overlay = true
			break
		OS.delay_msec(2)
	assert_true(overlay, "the held joiner's overlay arms")
	assert_false(pair2.joiner.is_join_deploy_pick_pending(), "no pick is owed")
	var overlay_presenter := _make_presenter(pair2.joiner)
	watch_signals(overlay_presenter)
	assert_true(overlay_presenter.open(), "the overlay opens without a pick")
	var space := InputEventKey.new()
	space.keycode = KEY_SPACE
	space.pressed = true
	overlay_presenter.get_viewport().push_input(space)
	await get_tree().process_frame
	assert_false(overlay_presenter.is_open(),
			"SPACE dismisses the overlay-only screen locally")
	assert_signal_emitted(overlay_presenter, "closed")


# The active session keeps running under the death screen — per-frame S2C 0x0A
# traffic continues while the pick is owed. The deploy UI is driven by the
# independent authoritative pending bit and must survive live session frames.
func test_live_session_frames_do_not_close_a_still_pending_deploy_screen() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	watch_signals(presenter)

	assert_true(presenter.open(), "the pending deploy UI opens in an active session")
	# Live traffic with no pick sent: the authority keeps the hold.
	for _i in range(30):
		pair.host.step()
		pair.joiner.step()
		OS.delay_msec(2)
	await get_tree().process_frame

	assert_true(presenter.is_open(), "live session frames do not dismiss a pending deploy UI")
	assert_signal_emit_count(presenter, "closed", 0,
			"only authority clearing deployment-pending closes a healthy screen")


func test_refresh_preserves_selected_spawn_identity_by_param() -> void:
	# Retail rebuilds the row set every 16 ticks and preserves the selected
	# node/param, not the visual row number. The periodic rebuild runs against
	# the REAL zone rows here; selection must survive the clear+repopulate.
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)

	assert_true(presenter.open(), "the pending death pick opens death.mnu")
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
	# The whole-list text sort orders "'A' zone" before "'D' Home Base": find
	# the zone row by its param, never by a fixed index.
	var zone_row := _row_index_for_param(presenter, zone_param)
	assert_gte(zone_row, 0, "the zone row is in the compiled list")
	driver.select_row(list_id, zone_row, false)  # highlight only; picks ride user clicks
	assert_eq(_row_param(presenter, zone_row), zone_param,
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
# The populate's second loop lands wave occupants + a blank separator at node
# -1; the select callback guards node != -1, so a click on such a row queues
# NO C2S 0x0E — the pick stays owed. [orig: UI_UpdateDeathScreenContent
#  @0x553c5f..0x553de3; DeathScreen_OnSpawnListSelect @0x55364d]
func test_occupant_rows_carry_node_minus_one_and_never_pick() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	assert_true(presenter.open(), "the join deploy screen opens")
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	if driver == null:
		return
	var list_id := driver.widget_id("SPAWNPOINTS_LIST")
	# Every real row is a pick (0 default, index+1 zone) — the fixture's wave
	# groups are empty, so no node -1 rows exist yet.
	for row in presenter.get_spawn_rows():
		assert_gte(int((row as Dictionary).get("param", -1)), 0,
				"list rows without occupants are all picks")
	# The engine builder's row model IS what a wave group would insert: a
	# synthetic occupant row at the presenter seam proves the guard.
	presenter.append_spawn_row("Ace", -1)
	presenter.select_spawn_row(presenter.get_spawn_rows().size() - 1)
	for _i in range(40):
		pair.host.step()
		pair.joiner.step()
		OS.delay_msec(2)
	assert_true(pair.joiner.is_join_deploy_pick_pending(),
			"a node -1 row never sends the deploy pick")
	# The statics follow the witnessed gates: the pick penalty lapsed and no wave
	# lists the player (no status line, no hold), while the other-player kill
	# opened the 120-second revive window -> the MEDIC pair shows, the
	# RESPAWN/PSPRESPAWN pair stays hidden, the list title shows.
	var status: Dictionary = pair.joiner.get_deploy_status()
	assert_eq(int(status.get("queued_kind", -1)), 0, "no penalty or wave line")
	assert_true(bool(status.get("show_medic", false)),
			"the open revive window shows the medic pair")
	for control_name in ["STATIC_RESPAWN_MSG1", "STATIC_PSPRESPAWN_MSG1"]:
		var id := driver.widget_id(control_name)
		if id >= 0:
			assert_false(driver.is_widget_shown(id), control_name + " hidden")
	for control_name in ["STATIC_MEDIC_MSG1", "STATIC_CALLMEDIC_MSG"]:
		var id := driver.widget_id(control_name)
		if id >= 0:
			assert_true(driver.is_widget_shown(id), control_name + " shown")
	var title_id := driver.widget_id("STATIC_LIST_TITLE")
	if title_id >= 0:
		assert_true(driver.is_widget_shown(title_id), "the list title shows with the list")
	assert_eq(driver.get_widget_items(list_id).size(), presenter.get_spawn_rows().size() - 1,
			"the compiled list mirrors the real row model")


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
	var default_row := _row_index_for_param(presenter, 0)
	assert_gte(default_row, 0, "the Default Spawn row is in the compiled list")
	driver.select_row(list_id, default_row, false)  # the player's pick, highlight only
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
