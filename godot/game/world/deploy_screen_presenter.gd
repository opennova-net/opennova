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
## [orig: death.mnu <NAME>DEATH</NAME>; DeathScreen_UpdateShroudReveal @0x554730 (shroud reveal:
##  immediate on g_deploy_screen_active, 240 ticks after death; content refresh
##  every 16 ticks); UI_UpdateDeathScreenContent @0x5536a0 (list populate:
##  row 0 "'<DEFAULT_SPAWN_KEY>' <HOME>" node 0, then per zone
##  "'<A+idx>' <WPNames/STRWPNAME%03d>" node idx+1, team color tags <c4040FF>/
##  <cFF2020>, the whole-list text sort, then the per-zone wave occupant rows
##  + blank separator with node -1 — the engine builder world/deploy_screen_feed
##  owns both loops; the STATIC_RESPAWN_MSG1 penalty/wave line, the
##  STATIC_PSPRESPAWN_MSG1 hold, the STATIC_MEDIC_MSG1/STATIC_CALLMEDIC_MSG
##  revive-window pair); UI_RegisterDeathScreenCallbacks @0x554610 (SPAWNPOINTS_LIST
##  select -> Input_QueueEvent(12, node) @0x55364d, guarded on node != -1);
##  update_death_screen_ui @0x553150 (map zoom fit from the zone AABB,
##  SWAP_TEAMS/BUTTON_TEAMLIST only in the TDM family)]
##
## The MAP window's terrain/zone/blip draw (the windowed map renderer
## MapOverlay_DrawView @0x5a58e0, sibling of HUD_DrawMapOverlay @0x5a5f40) is the tracked
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
# The content refresh cadence: engine truth 0.256 s, 16 ticks of the 62.5 Hz
# loop (world/deploy_screen_feed.h kDeployRefreshTicks carries the witness;
# the cadence rides the tick, not a round 250 ms).
static var REFRESH_INTERVAL_S: float = Simulation.deploy_refresh_interval_seconds()
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


## Build + wire the presenter under `parent` in one call (the shell's seam):
## `on_opened`/`on_closed` report the screen's cursor ownership.
static func install(parent: Node, world: GameWorld, ui_parent: Node,
		on_opened: Callable, on_closed: Callable) -> DeployScreenPresenter:
	var presenter := DeployScreenPresenter.new()
	presenter.name = "DeployScreenPresenter"
	parent.add_child(presenter)
	presenter.setup(world, ui_parent)
	presenter.opened.connect(func() -> void: on_opened.call())
	presenter.closed.connect(func() -> void: on_closed.call())
	return presenter


## The live menu driver over the deploy frame (ADR 0018 read seam for tests and
## diagnostics; null until the first open builds the menu).
func get_menu_driver() -> MenuDriver:
	return _driver


## The presenter-side spawn row model ({label, param} per visible list row,
## aligned with the compiled list's rows) — ADR 0018 read seam for tests.
func get_spawn_rows() -> Array:
	return _spawn_rows


## ADR 0018 test seams over the row model: append one presenter row (the shape
## the engine builder emits — an occupant row is {label, param: -1}) and fire
## the list select the compiled list would raise for a row.
func append_spawn_row(label: String, param: int) -> void:
	_spawn_rows.append({"label": label, "param": param})


func select_spawn_row(row: int) -> void:
	_on_widget_value_changed(SPAWN_LIST, "list", row, "")


## Open over the live world when the join owes a deployment pick, or when the
## host drives the deploy-map OVERLAY (0x0F game_flags bit0 / per-frame 0x0A
## flags1 bit1) — retail opens this same death.mnu DEATH screen for both, and
## the once-per-arming open latch belongs to the engine's ClientState, like
## retail's frame loop. The witnesses live on ClientState.deploy_overlay_active
## (engine/net/netsim/client_state.h) and hud-re D-HUD-19.
func open() -> bool:
	if is_open() or _world == null or _ui_parent == null:
		return false
	var sim: Simulation = _world.get_sim()
	if sim == null or not (bool(sim.is_join_deploy_pick_pending())
			or bool(sim.is_join_deploy_overlay_active())):
		return false
	if not _ensure_menu():
		return false
	_hide_team_service_buttons()
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


# Retail's deploy-screen keys 'X' and SPACE route input case 12 (dialogs reset
# + a 0x0E). On the OVERLAY-only screen (no pick owed) they act as the local
# dismiss; the DEATH pick flow keeps its list-select picks, so the keys stay
# inert there rather than inventing an unpicked default send. (The key and
# case-12 witnesses live in hud-re D-HUD-19; the 0x0E half is unported — see
# _on_widget_value_changed.)
func _unhandled_key_input(event: InputEvent) -> void:
	if not is_open():
		return
	var key := event as InputEventKey
	if key == null or not key.pressed or key.echo:
		return
	if key.keycode != KEY_X and key.keycode != KEY_SPACE:
		return
	var sim: Simulation = _world.get_sim() if _world != null else null
	if sim == null or bool(sim.is_join_deploy_pick_pending()):
		return
	close()
	get_viewport().set_input_as_handled()


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
	# The screen's lifetime: a pending DEATH pick holds it, and so does the
	# host-driven overlay bit (which follows the per-frame 0x0A flags1 bit1,
	# set AND cleared). Only both falling closes it — retail's frame loop
	# closes the latched screen exactly when its two open triggers are gone
	# (the per-frame fold and close-on-clear witnesses live on
	# ClientState.deploy_overlay_active and hud-re D-HUD-19).
	if not bool(sim.is_join_deploy_pick_pending()) \
			and not bool(sim.is_join_deploy_overlay_active()):
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
	#  @0x55364d; node 0 = the Default Spawn -> the parameter-0 pick; the
	#  occupant/blank rows carry node -1 and the callback guards node != -1]
	var param := int((_spawn_rows[index] as Dictionary).get("param", 0))
	if param == -1:
		return
	if not bool(sim.is_join_deploy_pick_pending()):
		# The OVERLAY-only screen (the player is already deployed — a wave
		# host): retail's input case 12 resets the dialogs (closing this
		# screen) and still sends one C2S 0x0E the host is free to drop. The
		# send half is NOT ported yet: case 12 also re-arms the client uplink
		# hold, and how a host releases that hold for an already-deployed
		# player is unwitnessed (the stock wave-join capture carries zero
		# 0x0E) — silence is the wire-safe posture until a capture pins it.
		# (The case-12 witnesses live in hud-re D-HUD-19.)
		close()
		return
	sim.send_deployment_pick(param)


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
	# The compiled row set comes from the engine builder (world/deploy_screen_feed):
	# the Default row, the secured team zones, the whole-list text sort, then
	# each zone's wave occupants + blank separator at node -1. The row texts
	# carry retail's team colour tag and the '<b><cFF4040>** name **' self
	# marker; the compiled list renders the tags through its own markup.
	# [orig: UI_UpdateDeathScreenContent @0x553aef..0x553de3]
	_spawn_rows = []
	var labels := PackedStringArray()
	var zone_names := {}
	for value in sim.get_deploy_spawn_zones():
		var zone := value as Dictionary
		var key := String(zone.get("name_key", ""))
		zone_names[key] = _game_text("WPNames", key, "Spawn Point")
	for value in sim.get_deploy_list_rows(_menu_text("DEFAULT_SPAWN_KEY", "D"),
			_menu_text("HOME", "Home Base"), zone_names):
		var row := value as Dictionary
		# The compiled list has no inline markup channel yet (the row-style
		# residue in D-HUD-19): the engine text keeps retail's <cRRGGBB>/<b>
		# tags, the list shows them stripped. The sort already ran over the
		# tagged text, so the row order is retail's.
		var label := _strip_inline_tags(String(row.get("text", "")))
		labels.append(label)
		_spawn_rows.append({"label": label, "param": int(row.get("value", -1))})
	# set_widget_items resets the selection to row 0; restore the previous pick by
	# parameter without emitting (picks ride user clicks only, never the refill).
	_driver.set_widget_items(list_id, labels)
	if keep_param >= 0:
		for row in _spawn_rows.size():
			if int((_spawn_rows[row] as Dictionary).get("param", -1)) == keep_param:
				_driver.select_row(list_id, row, false)
				break
	_apply_statics(sim)


# Retail's inline text markup (<cRRGGBB> colour, <b> bold) the compiled list
# cannot draw yet — stripped for display only, with retail's own stripper
# (the engine's hud::strip_inline_tags [orig: Chat_StripHtmlTags @0x4983f0]).
static func _strip_inline_tags(text: String) -> String:
	return Simulation.strip_inline_tags(text)


# The team-change service is unmodeled: hide the swap/team buttons (retail
# shows them only for the TDM family). [orig: update_death_screen_ui @0x553150]
func _hide_team_service_buttons() -> void:
	for control_name in ["SWAP_TEAMS", "BUTTON_TEAMLIST"]:
		# A -1 id means this screen simply does not author the control.
		var id := _driver.widget_id(control_name)
		if id >= 0:
			_driver.set_widget_shown(id, false)


# The witnessed STATIC show/text rules, refreshed with the list
# [orig: UI_UpdateDeathScreenContent @0x5536a0]:
#  * STATIC_RESPAWN_MSG1 (@0x5538e7..0x553a7b): hidden; the penalty timer
#    "<STROVER_PENALTYTIMER>  <cFF4040><n>" wins; else the wave zone listing the
#    local player — numbered "'<WPNames name>':  <cFF4040><n>", lettered
#    "<letter>:  <cFF4040><n>";
#  * STATIC_LIST_TITLE shown with the list (@0x553ab4);
#  * STATIC_PSPRESPAWN_MSG1 (@0x553e10): "<STROVER_PSPRESPAWN>  <cFF4040><n>"
#    while the spawn-target hold runs;
#  * STATIC_MEDIC_MSG1 + STATIC_CALLMEDIC_MSG (@0x553e74..0x553f60): while the
#    local revive window runs and the player is not in a seat —
#    "<STROVER_MEDICTIMER>  <cFF4040><n>" and STROVER_CALLMEDIC formatted with
#    the MedicReq binding's display string (KeyBinding_FormatDisplayString).
func _apply_statics(sim: Simulation) -> void:
	var status: Dictionary = sim.get_deploy_status()
	var title_id := _driver.widget_id("STATIC_LIST_TITLE")
	if title_id >= 0:
		_driver.set_widget_shown(title_id, true)
	var respawn_id := _driver.widget_id("STATIC_RESPAWN_MSG1")
	if respawn_id >= 0:
		var kind := int(status.get("queued_kind", 0))
		_driver.set_widget_shown(respawn_id, kind != 0)
		if kind != 0:
			# The three sprintf arms are the engine's deploy_status_text
			# (world/deploy_screen_feed.h), resolved through gametext by the sim.
			_driver.set_widget_text(respawn_id,
					sim.get_deploy_status_text(Strings.get_table("gametext")))
	var psp_id := _driver.widget_id("STATIC_PSPRESPAWN_MSG1")
	if psp_id >= 0:
		var show_psp := bool(status.get("show_psp_respawn", false))
		_driver.set_widget_shown(psp_id, show_psp)
		if show_psp:
			_driver.set_widget_text(psp_id, "%s  <cFF4040>%d" % [
					_game_text("Overlays", "STROVER_PSPRESPAWN", "Spawn point available in"),
					int(status.get("hold_seconds", 0))])
	var medic_id := _driver.widget_id("STATIC_MEDIC_MSG1")
	var call_id := _driver.widget_id("STATIC_CALLMEDIC_MSG")
	if medic_id >= 0 and call_id >= 0:
		var show_medic := bool(status.get("show_medic", false))
		_driver.set_widget_shown(medic_id, show_medic)
		_driver.set_widget_shown(call_id, show_medic)
		if show_medic:
			_driver.set_widget_text(medic_id, "%s  <cFF4040>%d" % [
					_game_text("Overlays", "STROVER_MEDICTIMER", "Medic time remaining"),
					int(status.get("revive_seconds", 0))])
			var key_label: String = ControlsBindings.model().display_text_for_token("MedicReq")
			var call_format := _game_text("Overlays", "STROVER_CALLMEDIC", "Press %s to call a medic")
			_driver.set_widget_text(call_id,
					call_format % key_label if call_format.contains("%s") else call_format)


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
	_driver.set_music_director(MusicService.director())
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


# The compiled frame is a passive surface; MenuFrameSurface.forward_gui_input
# forwards its gui input to the driver the way MenuShell does.
func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	MenuFrameSurface.forward_gui_input(event, _driver, _frame)


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
	return MenuFrameSurface.load_style(root, STYLESHEET_FILE)


func _connect_layout_source() -> void:
	MenuFrameSurface.connect_layout_source(_layout_control, _ui_parent, _recompute_fit)


func _recompute_fit() -> void:
	# MenuFrameSurface.fit_frame (shared with the other presenters).
	MenuFrameSurface.fit_frame(_frame, _layout_control, _ui_parent)
