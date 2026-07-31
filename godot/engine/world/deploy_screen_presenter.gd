class_name NovaDeployScreenPresenter
extends Node

## The deploy-map screen — death.mnu's DEATH screen — shown while the host holds
## the joining player respawn-pending (0x0A flags1 bit1) and a deployment pick is
## owed. The witnessed original drives this same .mnu screen in code: the shroud
## reveals immediately when the deploy flag is up, the SPAWNPOINTS_LIST carries
## the Default Spawn row plus one lettered row per team-owned SECURED deploy
## zone, and a list select queues the pick — re-picks stay possible because a
## host silently drops an invalid/contested pick and the screen only closes when
## the server-side respawn-pending flag falls (the deployment release).
## [orig: death.mnu <NAME>DEATH</NAME>; sub_554730 @0x554730 (shroud reveal:
##  immediate on g_deploy_screen_active, 240 ticks after death; content refresh
##  every 16 ticks); UI_UpdateDeathScreenContent @0x5536a0 (list populate:
##  row 0 "'<DEFAULT_SPAWN_KEY>' <HOME>" node 0, then per zone
##  "'<A+idx>' <WPNames/STRWPNAME%03d>" node idx+1, team color tags <c4040FF>/
##  <cFF2020>); UI_RegisterDeathScreenCallbacks @0x554610 (SPAWNPOINTS_LIST
##  select -> Input_QueueEvent(12, node) @0x55364d); update_death_screen_ui
##  @0x553150 (map zoom fit from the zone AABB, SWAP_TEAMS/BUTTON_TEAMLIST only
##  in the TDM family)]
##
## The MAP window's terrain/zone/blip draw (the windowed map renderer
## sub_5A58E0 @0x5a58e0, sibling of HUD_DrawMapOverlay @0x5a5f40) is the tracked
## next map-phase witness (docs/interface/hud-re.md follow-ups); until it lands
## the authored window chrome renders without the map image.

const MENU_FILE := "death.mnu"
const MENU_SCREEN := "DEATH"
const STYLESHEET_FILE := "menu_style.mns"
const DESIGN_SIZE := Vector2(800, 600)
const MUSIC_VAR_INDEX := 3  # death.mnu <MUSICVAR>3</MUSICVAR>
const REFRESH_INTERVAL_S := 0.25  # [orig: every 16 ticks of the 62 Hz loop @0x55477d]

signal opened
signal closed

var _world = null          # GameWorld
var _ui_parent: Node = null
var _menu: NovaMnuMenu = null
var _menu_root: NovaResourceRoot = null
var _refresh_accum := 0.0


func setup(world, ui_parent: Node) -> void:
	_world = world
	_ui_parent = ui_parent
	_connect_layout_source()


func is_open() -> bool:
	return _menu != null and is_instance_valid(_menu) and _menu.visible


## Open over the live world when the join owes a deployment pick.
func open() -> bool:
	if is_open() or _world == null or _ui_parent == null:
		return false
	var sim = _world.get_sim() if _world.has_method("get_sim") else null
	if sim == null or not sim.has_method("is_join_deploy_pick_pending"):
		return false
	if not bool(sim.is_join_deploy_pick_pending()):
		return false
	if not _ensure_menu():
		return false
	_apply_static_visibility()
	_populate_spawn_list(sim)
	_ui_parent.move_child(_menu, _ui_parent.get_child_count() - 1)
	_menu.visible = true
	_refresh_accum = 0.0
	set_process(true)
	opened.emit()
	return true


func close() -> void:
	set_process(false)
	if not is_open():
		return
	_menu.visible = false
	closed.emit()


func teardown() -> void:
	set_process(false)
	if _menu != null and is_instance_valid(_menu):
		_menu.queue_free()
	_menu = null
	_menu_root = null


func _process(delta: float) -> void:
	if not is_open():
		set_process(false)
		return
	var sim = _world.get_sim() if _world != null and _world.has_method("get_sim") else null
	if sim == null:
		close()
		return
	# A DEAD session is not a deployment release. The host's punt closes the connection,
	# whose terminal phase clears the pick-pending flag below — so closing there would hand
	# the shell back to State.WORLD and re-capture the mouse over a world the player can no
	# longer play. Tear the screen down instead and leave the exit to the session-loss leg:
	# retail's disconnect exits the mission outright on this edge, it never resumes play.
	# [orig: CNapiNetwork_OnDisconnectedFromServer @0x4c63d0 -> Input_QueueEvent(3)
	#  @0x4c67a4 -> g_mission_exit_reason = 1 (Input_HandleActionBinding case 3 @0x49af2c),
	#  the nav-push "MainMenu" teardown every abort leg takes @0x568654]
	if sim.has_method("is_session_lost") and bool(sim.is_session_lost()):
		teardown()
		return
	# The screen's lifetime is the server-driven hold: it closes when the deployment
	# release lands (in-match) — one 0x0A frame with flags1 bit1 clear in retail
	# [orig: the §5.61 hold chain — g_deploy_screen_active follows the bit every frame].
	if bool(sim.is_joined_in_match()) or not bool(sim.is_join_deploy_pick_pending()):
		close()
		return
	# Periodic content refresh: zone security/ownership can change while picking
	# [orig: UI_UpdateDeathScreenContent every 16 ticks @0x55477d].
	_refresh_accum += delta
	if _refresh_accum >= REFRESH_INTERVAL_S:
		_refresh_accum = 0.0
		_populate_spawn_list(sim)


func _on_spawn_row_selected(index: int) -> void:
	var list := _spawn_list()
	if list == null or index < 0 or index >= list.item_count:
		return
	var sim = _world.get_sim() if _world != null and _world.has_method("get_sim") else null
	if sim == null or not sim.has_method("send_deployment_pick"):
		return
	# [orig: the SPAWNPOINTS_LIST select callback -> Input_QueueEvent(12, node)
	#  @0x55364d; node 0 = the Default Spawn -> the parameter-0 pick]
	sim.send_deployment_pick(int(list.get_item_metadata(index)))


func _populate_spawn_list(sim) -> void:
	var list := _spawn_list()
	if list == null:
		return
	var selected := list.get_selected_items()
	var keep_param := -1
	if selected.size() > 0:
		keep_param = int(list.get_item_metadata(selected[0]))
	list.clear()
	# Team text color rides the row [orig: the "<c4040FF>" tag, or "<cFF2020>" only
	# when Team == 2, in the list text @0x5536a0; our list styles the row directly].
	# The pre-spawn joiner's team is the S2C 0x04 latch the sim surfaces.
	var team := 0
	if sim.has_method("get_join_assigned_team"):
		team = int(sim.get_join_assigned_team())
	var row_color := Color("ff2020") if team == 2 else Color("4040ff")
	# Row 0: the default spawn [orig: "'<DEFAULT_SPAWN_KEY>' <HOME>", node 0].
	var default_text := "'%s' %s" % [
		_menu_text("DEFAULT_SPAWN_KEY", "D"),
		_menu_text("HOME", "Home Base"),
	]
	var row := list.add_item(default_text, null, true)
	list.set_item_metadata(row, 0)
	list.set_item_custom_fg_color(row, row_color)
	# One lettered row per team-owned secured deploy zone (see NovaSimulation.
	# get_deploy_spawn_zones for the witnessed filter + letter/name keying).
	for value in sim.get_deploy_spawn_zones():
		var zone := value as Dictionary
		var zone_name := _game_text(
				"WPNames", String(zone.get("name_key", "")), "Spawn Point")
		row = list.add_item(
				"'%s' %s" % [String(zone.get("letter", "")), zone_name], null, true)
		list.set_item_metadata(row, int(zone.get("param", 0)))
		list.set_item_custom_fg_color(row, row_color)
	if keep_param >= 0:
		for index in range(list.item_count):
			if int(list.get_item_metadata(index)) == keep_param:
				list.select(index)
				break


# The join deploy screen has no medic/revive leg and no team-change service yet:
# hide the medic statics (their witnessed show condition is the dead-with-revive
# case) and the swap/team buttons (retail shows them only for the TDM family; the
# team-change wire service is unmodeled). [orig: update_death_screen_ui @0x553150]
func _apply_static_visibility() -> void:
	for control_name in ["STATIC_MEDIC_MSG1", "STATIC_CALLMEDIC_MSG",
			"STATIC_PSPRESPAWN_MSG1", "SWAP_TEAMS", "BUTTON_TEAMLIST"]:
		var node := _find(control_name)
		if node is CanvasItem:
			(node as CanvasItem).visible = false


func _spawn_list() -> ItemList:
	return _find("SPAWNPOINTS_LIST") as ItemList


func _find(control_name: String) -> Node:
	if _menu == null or not is_instance_valid(_menu):
		return null
	return _menu.find_child(control_name, true, false)


# Build the runtime menu the same way the armory presenter does: the shared NovaMnuMenu
# fed death.mnu from the world's mounted resource root.
func _ensure_menu() -> bool:
	if _menu != null and is_instance_valid(_menu):
		return true
	var root: NovaResourceRoot = _world.get_resource_root() \
			if _world.has_method("get_resource_root") else null
	if root == null:
		return false
	var bytes := root.read_file(MENU_FILE)
	if bytes.is_empty():
		push_warning("NovaDeployScreenPresenter: %s not found in the resource root" % MENU_FILE)
		return false
	var doc := NovaMnuDocument.new()
	if doc.load_from_bytes(bytes) != OK:
		push_warning("NovaDeployScreenPresenter: %s did not parse" % MENU_FILE)
		return false
	_register_text_tables(root)
	_menu = NovaMnuMenu.new()
	_menu.name = "DeployScreenMenu"
	_menu.build_on_ready = false
	_menu.set_edit_mode(false)
	_menu.set_resource_root(root)
	var menu_text: RtxtStringFile = NovaStrings.get_table("menutxt")
	if menu_text != null:
		_menu.set_text_resource(menu_text)
	var style := _load_style(root)
	if style != null:
		_menu.set_stylesheet(style)
	_menu.set_music_director(NovaMusicService.director())
	_menu.set_music_var_index(MUSIC_VAR_INDEX)
	_menu.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_menu.size = DESIGN_SIZE
	_ui_parent.add_child(_menu)
	_recompute_fit()
	_menu.set_menu_file(MENU_FILE)
	_menu.menu = doc
	_menu.show_screen(MENU_SCREEN)
	_menu_root = root
	var list := _spawn_list()
	if list != null and not list.item_selected.is_connected(_on_spawn_row_selected):
		# Every click on a row queues a pick, including a click on the row that is
		# already highlighted: the original's select callback is a COMMAND, fired
		# unconditionally from the list event with no "did the selection change"
		# test and no re-entry gate. Godot's ItemList suppresses item_selected for
		# the already-selected row unless allow_reselect is set, and the row DOES
		# stay selected — the menu survives close() (_ensure_menu reuses it) and
		# _populate_spawn_list restores the previous pick by parameter — so without
		# this a re-opened DEATH screen, and any re-pick after the host silently
		# drops one, can never send a second C2S 0x0E.
		# [orig: DeathScreen_OnSpawnListSelect @ 0x553630 -> Input_QueueEvent(12,
		#  node) @ 0x55364d, guarded only on node != -1]
		list.allow_reselect = true
		list.item_selected.connect(_on_spawn_row_selected)
	return true


func _register_text_tables(root: NovaResourceRoot) -> void:
	for spec in [["menutxt", "menutxt.BIN"], ["gametext", "gametext.bin"],
			["gameui", "Game.bin"]]:
		if NovaStrings.get_table(spec[0]) != null:
			continue
		var bytes := root.read_file(spec[1])
		if bytes.is_empty():
			continue
		var loaded := RtxtStringFile.new()
		if loaded.load_from_byte_array(bytes) == OK:
			NovaStrings.register_table(spec[0], loaded)


func _menu_text(key: String, fallback: String) -> String:
	# [orig: TextResource_GetStringWithFallback(g_TextMenuUi, "Menu", key) — the
	#  DEFAULT_SPAWN_KEY / HOME row-0 tokens @0x5536a0]
	for spec in [["menutxt", "Menu"], ["gameui", "Menu"]]:
		var t: RtxtStringFile = NovaStrings.get_table(spec[0])
		if t != null and t.has_string_in_section(spec[1], key):
			return t.get_string_in_section(spec[1], key)
	return fallback


func _game_text(section: String, key: String, fallback: String) -> String:
	# [orig: GameText_GetString("WPNames", "STRWPNAME%03d") @0x5536a0]
	var t: RtxtStringFile = NovaStrings.get_table("gametext")
	if t != null and not key.is_empty() and t.has_string_in_section(section, key):
		return t.get_string_in_section(section, key)
	return fallback


func _load_style(root: NovaResourceRoot) -> MnsStyleSheet:
	var bytes := root.read_file(STYLESHEET_FILE)
	if bytes.is_empty():
		return null
	var s := MnsStyleSheet.new()
	return s if s.load_from_bytes(bytes) == OK and s.is_runtime_valid() else null


func _connect_layout_source() -> void:
	if _ui_parent is Control:
		var control := _ui_parent as Control
		if not control.resized.is_connected(_recompute_fit):
			control.resized.connect(_recompute_fit)
		return
	var viewport := _ui_parent.get_viewport() if _ui_parent != null else null
	if viewport != null and not viewport.size_changed.is_connected(_recompute_fit):
		viewport.size_changed.connect(_recompute_fit)


func _recompute_fit() -> void:
	if _menu == null or not is_instance_valid(_menu):
		return
	var target_size := Vector2.ZERO
	if _ui_parent is Control:
		target_size = (_ui_parent as Control).size
	elif _ui_parent != null and _ui_parent.get_viewport() != null:
		target_size = _ui_parent.get_viewport().get_visible_rect().size
	if target_size.x <= 1.0 or target_size.y <= 1.0:
		return
	_menu.position = Vector2.ZERO
	_menu.size = DESIGN_SIZE
	_menu.scale = Vector2(target_size.x / DESIGN_SIZE.x, target_size.y / DESIGN_SIZE.y)
