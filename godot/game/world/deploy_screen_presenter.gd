class_name DeployScreenPresenter
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
# The menumus discriminator SLOT the driver pushes each screen's authored
# MUSICVAR value into (death.mnu authors <MUSICVAR>3</MUSICVAR>; the slot is
# the same as MenuShell's — the witness [orig: UI_DispatchScreenEvent
# @ 0x54e6a0, store @ 0x54eff4 -> AudioVM_SetVariable(2, v)] lives at engine
# audio/music_policy.h kMenuMusicVarSlot).
const MUSIC_VAR_INDEX := MusicDirector.MENU_MUSIC_VAR_SLOT
# The content refresh cadence: engine truth 0.256 s — 16 ticks of the 62.5 Hz
# loop (world/tick_accumulator.h kTickDt; the witness
# [orig: every 16 ticks @0x55477d] rides the tick, not a round 250 ms).
# Deliberate correction: the old godot-side 0.25 approximated the tick rate.
static var REFRESH_INTERVAL_S: float = 16.0 * Simulation.tick_dt()
const SPAWN_LIST := "SPAWNPOINTS_LIST"

signal opened
signal closed

var _world: GameWorld = null
var _ui_parent: Node = null
# The layout source, converted ONCE at setup: a Control parent (test overlays)
# drives the fit from its own size/resized; a CanvasLayer parent (the game HUD)
# has no size, so the fit follows the viewport instead.
var _layout_control: Control = null
# The compiled menu surface: the frame draws + hit-tests, the audio node plays
# widget SFX, and the RefCounted driver orchestrates both over the parsed
# document (freed with the presenter; only the two nodes need queue_free).
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
var _menu_root: ResourceRoot = null
var _refresh_accum := 0.0
# The spawn list's presenter-side row model, aligned with the compiled list's
# visible rows: {label, param} per row, rebuilt by _populate_spawn_list. The
# compiled list carries labels only, so the node parameter the pick serializes
# lives here (the old ItemList's item metadata).
var _spawn_rows: Array = []


func setup(world: GameWorld, ui_parent: Node) -> void:
	_world = world
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	_connect_layout_source()


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


## The live menu driver over the deploy frame (ADR 0018 read seam for tests and
## diagnostics; null until the first open builds the menu).
func get_menu_driver() -> MenuDriver:
	return _driver


## The presenter-side spawn row model ({label, param} per visible list row,
## aligned with the compiled list's rows) — ADR 0018 read seam for tests.
func get_spawn_rows() -> Array:
	return _spawn_rows


## Open over the live world when the join owes a deployment pick.
func open() -> bool:
	if is_open() or _world == null or _ui_parent == null:
		return false
	var sim: Simulation = _world.get_sim()
	if sim == null or not bool(sim.is_join_deploy_pick_pending()):
		return false
	if not _ensure_menu():
		return false
	_apply_static_visibility()
	_populate_spawn_list(sim)
	_ui_parent.move_child(_frame, _ui_parent.get_child_count() - 1)
	_frame.visible = true
	_refresh_accum = 0.0
	set_process(true)
	opened.emit()
	return true


func close() -> void:
	set_process(false)
	if not is_open():
		return
	_frame.visible = false
	closed.emit()


func teardown() -> void:
	set_process(false)
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null
	_menu_root = null
	_spawn_rows = []


func _process(delta: float) -> void:
	if not is_open():
		set_process(false)
		return
	# The blink/marquee clock rides the OS tick like the original's GetTickCount
	# gate (the shell does the same for the front-end menus).
	_driver.tick(Time.get_ticks_msec())
	var sim: Simulation = _world.get_sim() if _world != null else null
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
	if bool(sim.is_session_lost()):
		teardown()
		return
	# The screen's lifetime is the server-driven deployment-pending bit, independent
	# from the active-session/gameplay state. Retail's initial 0x5A grants resume
	# uplinks before the player picks, while flags1 bit1 keeps this screen visible;
	# only the host clearing that bit closes it.
	# [orig: the §5.61 hold chain — g_deploy_screen_active follows the bit every frame].
	if not bool(sim.is_join_deploy_pick_pending()):
		close()
		return
	# Periodic content refresh: zone security/ownership can change while picking
	# [orig: UI_UpdateDeathScreenContent every 16 ticks @0x55477d].
	_refresh_accum += delta
	if _refresh_accum >= REFRESH_INTERVAL_S:
		_refresh_accum = 0.0
		_populate_spawn_list(sim)


# Selection commands arrive through the driver's aggregate relay. Every click on
# a row queues a pick, including a click on the row that is already highlighted:
# the original's select callback is a COMMAND, fired unconditionally from the
# list event with no "did the selection change" test and no re-entry gate — and
# the driver re-emits widget_value_changed on every list click, which carries the
# old ItemList allow_reselect=true semantics (a re-opened DEATH screen, and any
# re-pick after the host silently drops one, can still send another C2S 0x0E).
# [orig: DeathScreen_OnSpawnListSelect @ 0x553630 -> Input_QueueEvent(12,
#  node) @ 0x55364d, guarded only on node != -1]
func _on_widget_value_changed(widget_name: String, kind: String, index: int,
		_value: String) -> void:
	if kind != "list" or widget_name.nocasecmp_to(SPAWN_LIST) != 0:
		return
	if index < 0 or index >= _spawn_rows.size():
		return
	var sim: Simulation = _world.get_sim() if _world != null else null
	if sim == null:
		return
	# [orig: the SPAWNPOINTS_LIST select callback -> Input_QueueEvent(12, node)
	#  @0x55364d; node 0 = the Default Spawn -> the parameter-0 pick]
	sim.send_deployment_pick(int((_spawn_rows[index] as Dictionary).get("param", 0)))


func _populate_spawn_list(sim: Simulation) -> void:
	if _driver == null:
		return
	var list_id := _driver.widget_id(SPAWN_LIST)
	if list_id < 0:
		return
	# Preserve the pick across the periodic refill by PARAMETER, not row index —
	# zone security/ownership changes can reshuffle the rows.
	var keep_param := -1
	var selected := _driver.selected_row(list_id)
	if selected >= 0 and selected < _spawn_rows.size():
		keep_param = int((_spawn_rows[selected] as Dictionary).get("param", -1))
	# Team text color rode the row [orig: the "<c4040FF>" tag, or "<cFF2020>" only
	# when Team == 2, in the list text @0x5536a0]. DROPPED for now: the compiled
	# list has no per-row style channel — row coloring awaits the list row-style
	# channel (P2 parity will judge).
	_spawn_rows = []
	var labels := PackedStringArray()
	# Row 0: the default spawn [orig: "'<DEFAULT_SPAWN_KEY>' <HOME>", node 0].
	var default_text := "'%s' %s" % [
		_menu_text("DEFAULT_SPAWN_KEY", "D"),
		_menu_text("HOME", "Home Base"),
	]
	labels.append(default_text)
	_spawn_rows.append({"label": default_text, "param": 0})
	# One lettered row per team-owned secured deploy zone (see Simulation.
	# get_deploy_spawn_zones for the witnessed filter + letter/name keying).
	for value in sim.get_deploy_spawn_zones():
		var zone := value as Dictionary
		var zone_name := _game_text(
				"WPNames", String(zone.get("name_key", "")), "Spawn Point")
		var label := "'%s' %s" % [String(zone.get("letter", "")), zone_name]
		labels.append(label)
		_spawn_rows.append({"label": label, "param": int(zone.get("param", 0))})
	# set_widget_items resets the selection to row 0; restore the previous pick by
	# parameter without emitting (picks ride user clicks only, never the refill).
	_driver.set_widget_items(list_id, labels)
	if keep_param >= 0:
		for row in _spawn_rows.size():
			if int((_spawn_rows[row] as Dictionary).get("param", -1)) == keep_param:
				_driver.select_row(list_id, row, false)
				break


# The join deploy screen has no medic/revive leg and no team-change service yet:
# hide the medic statics (their witnessed show condition is the dead-with-revive
# case) and the swap/team buttons (retail shows them only for the TDM family; the
# team-change wire service is unmodeled). [orig: update_death_screen_ui @0x553150]
func _apply_static_visibility() -> void:
	for control_name in ["STATIC_MEDIC_MSG1", "STATIC_CALLMEDIC_MSG",
			"STATIC_PSPRESPAWN_MSG1", "SWAP_TEAMS", "BUTTON_TEAMLIST"]:
		# A -1 id means this screen simply does not author the control.
		var id := _driver.widget_id(control_name)
		if id >= 0:
			_driver.set_widget_shown(id, false)


# Build the compiled menu surface the same way the armory presenter does: the
# shared MenuFrame + MenuAudio + MenuDriver stack fed death.mnu from the world's
# mounted resource root.
func _ensure_menu() -> bool:
	if _driver != null and _frame != null and is_instance_valid(_frame):
		return true
	var root: ResourceRoot = _world.get_resource_root()
	if root == null:
		return false
	var bytes := root.read_file(MENU_FILE)
	if bytes.is_empty():
		push_warning("DeployScreenPresenter: %s not found in the resource root" % MENU_FILE)
		return false
	var doc := MnuDocument.new()
	if doc.load_from_bytes(bytes) != OK:
		push_warning("DeployScreenPresenter: %s did not parse" % MENU_FILE)
		return false
	_register_text_tables(root)
	# death.mnu shares the retail menu's fixed 800x600 design space; the frame
	# scales it to its OWN size internally, so the fit just sizes the Control.
	# [orig: CUIScene_SetScreenScale @0x639480]
	_frame = MenuFrame.new()
	_frame.name = "DeployScreenMenu"
	_frame.set_anchors_preset(Control.PRESET_TOP_LEFT)
	# Unlike MenuShell (a Control parent sampling for a full-rect child frame),
	# the presenter overlays a foreign HUD parent, so the frame itself is the
	# input surface: its gui_input forwards into the driver's pump.
	_frame.mouse_filter = Control.MOUSE_FILTER_STOP
	_frame.gui_input.connect(_on_frame_gui_input)
	_ui_parent.add_child(_frame)
	_recompute_fit()
	# Widget <SOUND> triggers play through the MenuAudio device leg.
	_audio = MenuAudio.new()
	_audio.name = "DeployScreenMenuAudio"
	_audio.set_resource_root(root)
	_ui_parent.add_child(_audio)
	_driver = MenuDriver.new()
	_driver.attach(_frame, _audio)
	_driver.set_music_director(NovaMusicService.director())
	_driver.set_music_var_index(MUSIC_VAR_INDEX)
	_driver.widget_value_changed.connect(_on_widget_value_changed)
	var style := _load_style(root)
	var menu_text: RtxtStringFile = Strings.get_table("menutxt")
	if not _driver.open_document(doc, root, style, menu_text, MENU_FILE, MENU_SCREEN):
		push_warning("DeployScreenPresenter: %s has no screens" % MENU_FILE)
		teardown()
		return false
	_menu_root = root
	return true


# The compiled frame is a passive surface — it draws and hit-tests but never
# pumps input itself; forward its gui input to the driver the way MenuShell
# does (event positions are frame-local, the space process_mouse expects).
func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	if event is InputEventMouseMotion:
		var motion := event as InputEventMouseMotion
		_driver.process_mouse(motion.position,
				(motion.button_mask & MOUSE_BUTTON_MASK_LEFT) != 0)
	elif event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.button_index == MOUSE_BUTTON_LEFT:
			_driver.process_mouse(button.position, button.pressed)
			_frame.accept_event()


func _register_text_tables(root: ResourceRoot) -> void:
	for spec in [["menutxt", "menutxt.BIN"], ["gametext", "gametext.bin"],
			["gameui", "Game.bin"]]:
		if Strings.get_table(spec[0]) != null:
			continue
		var bytes := root.read_file(spec[1])
		if bytes.is_empty():
			continue
		var loaded := RtxtStringFile.new()
		if loaded.load_from_byte_array(bytes) == OK:
			Strings.register_table(spec[0], loaded)


func _menu_text(key: String, fallback: String) -> String:
	# [orig: TextResource_GetStringWithFallback(g_TextMenuUi, "Menu", key) — the
	#  DEFAULT_SPAWN_KEY / HOME row-0 tokens @0x5536a0]
	for spec in [["menutxt", "Menu"], ["gameui", "Menu"]]:
		var t: RtxtStringFile = Strings.get_table(spec[0])
		if t != null and t.has_string_in_section(spec[1], key):
			return t.get_string_in_section(spec[1], key)
	return fallback


func _game_text(section: String, key: String, fallback: String) -> String:
	# [orig: GameText_GetString("WPNames", "STRWPNAME%03d") @0x5536a0]
	var t: RtxtStringFile = Strings.get_table("gametext")
	if t != null and not key.is_empty() and t.has_string_in_section(section, key):
		return t.get_string_in_section(section, key)
	return fallback


func _load_style(root: ResourceRoot) -> MnsStyleSheet:
	var bytes := root.read_file(STYLESHEET_FILE)
	if bytes.is_empty():
		return null
	var s := MnsStyleSheet.new()
	return s if s.load_from_bytes(bytes) == OK and s.is_runtime_valid() else null


func _connect_layout_source() -> void:
	if _layout_control != null:
		if not _layout_control.resized.is_connected(_recompute_fit):
			_layout_control.resized.connect(_recompute_fit)
		return
	var viewport := _ui_parent.get_viewport() if _ui_parent != null else null
	if viewport != null and not viewport.size_changed.is_connected(_recompute_fit):
		viewport.size_changed.connect(_recompute_fit)


func _recompute_fit() -> void:
	if _frame == null or not is_instance_valid(_frame):
		return
	var target_size := Vector2.ZERO
	if _layout_control != null:
		target_size = _layout_control.size
	elif _ui_parent != null and _ui_parent.get_viewport() != null:
		target_size = _ui_parent.get_viewport().get_visible_rect().size
	if target_size.x <= 1.0 or target_size.y <= 1.0:
		return
	# The frame maps the 800x600 design space to its own rect internally
	# [orig: CUIScene_SetScreenScale @0x639480] — no Control scale math here.
	_frame.position = Vector2.ZERO
	_frame.size = target_size
