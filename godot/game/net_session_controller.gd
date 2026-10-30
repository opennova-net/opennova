class_name NetSessionController
extends JoinFlow

# The game shell's NET SESSION device: every way a networked session starts (the mp.mnu LAN
# browser/host screens, the NovaWorld panel, the --lan-host / --lan-join launch flags) runs the
# engine's join flow (inmatch::JoinFlow: the enumerate, the choice, the dial, state 9,
# g_ReturnToNovaWorldMenu), and this node is its device: the browse's LanSession, the shell's join
# screen and loads, the mount, the held NovaWorld session and its panel.

var _shell: MainGame  # start_world_load / current_resource_root
# The NovaWorld servers that refused this install this process, with each refusal's text: the
# one record (engine BrowserRefusals), handed to every panel this controller opens.
var _refusals := NovaWorldServerBrowser.new()
var _world: GameWorld
var _menu_shell: MenuShell
var _panel_layer: Node  # where the NovaWorld panel mounts (the menu layer)
var _novaworld_panel: NovaWorldPanel
var _mp_companion: MpMenuCompanion
var _advanced_host_button: Button
var _advanced_host_dialog: HostSettingsPanel
var _host_return_file := ""
var _host_return_screen := ""
var _menu_visible_before_novaworld := false
var _menu_key_input_before_novaworld := false
var _novaworld_menu_state_captured := false
# The NovaWorld session a resolved join or a granted host hands over, held from the panel's
# dismissal until the load takes it (or an abandoned join hands it back to the NovaWorld menu).
var _novaworld_client: NovaWorldClient
## The join screen the choice shows on when the controller runs without its shell
## (the GUT harness opens one over an authored pre.mnu); the shell's own otherwise.
var standalone_join_screen: PreGameMenuPresenter = null
# The flow's browse (the enumerate-and-join's, or the launch search's).
var _browse: LanSession

const NOVAWORLD_PANEL_SCENE := preload("res://game/novaworld_panel.tscn")


func _exit_tree() -> void:
	if is_instance_valid(_advanced_host_dialog):
		_advanced_host_dialog.queue_free()
	if is_instance_valid(_advanced_host_button):
		_advanced_host_button.queue_free()
	_end_browse()
	_release_novaworld_client()


func setup(shell: MainGame, world: GameWorld, menu_shell: MenuShell, panel_layer: Node) -> void:
	_shell = shell
	_world = world
	_menu_shell = menu_shell
	_panel_layer = panel_layer
	_advanced_host_button = Button.new()
	_advanced_host_button.text = "All server options…"
	_advanced_host_button.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	_advanced_host_button.position = Vector2(-230, -56)
	_advanced_host_button.size = Vector2(210, 40)
	_advanced_host_button.visible = false
	_advanced_host_button.pressed.connect(_open_advanced_host_settings)
	_panel_layer.add_child(_advanced_host_button)


# The multiplayer menu companion's session requests route here.
func wire_menu_companions(mp_companion: MpMenuCompanion) -> void:
	_mp_companion = mp_companion
	mp_companion.novaworld_host_start_requested.connect(_start_configured_novaworld_host)
	mp_companion.novaworld_host_cancelled.connect(_cancel_novaworld_host_setup)
	mp_companion.lan_host_start_requested.connect(_on_lan_host_start_requested)
	mp_companion.lan_join_requested.connect(join)
	mp_companion.lan_join_refused.connect(refuse)


# --- The host screens (mp.mnu MULTI_PLAYER_HOST) -----------------------------------

func _process(_delta: float) -> void:
	if _advanced_host_button == null or _menu_shell == null:
		return
	var driver := _menu_shell.get_driver()
	_advanced_host_button.visible = driver != null and _menu_shell.visible \
			and driver.get_current_screen().nocasecmp_to("MULTI_PLAYER_HOST") == 0 \
			and (_novaworld_panel == null or not _novaworld_panel.visible)


func _open_advanced_host_settings() -> void:
	if is_instance_valid(_advanced_host_dialog):
		return
	GameCfgStore.block().read_host_screen(_menu_shell.get_driver())
	_advanced_host_dialog = HostSettingsPanel.new()
	_panel_layer.add_child(_advanced_host_dialog)
	_advanced_host_dialog.popup_centered()


func _open_novaworld_host_setup() -> void:
	if _menu_shell == null or _mp_companion == null or _novaworld_panel == null:
		return
	_host_return_file = _menu_shell.get_current_menu_file()
	_host_return_screen = _menu_shell.get_driver().get_current_screen()
	if not _menu_shell.open_menu("mp.mnu", "MULTI_PLAYER_HOST"):
		return
	_mp_companion.host_channel = HostSessionConfig.CHANNEL_NOVAWORLD
	_novaworld_panel.hide()
	_menu_shell.show_menu()
	_menu_shell.set_process_unhandled_key_input(true)


func _cancel_novaworld_host_setup() -> void:
	if _novaworld_panel == null:
		return
	_novaworld_panel.show()
	_menu_shell.set_process_unhandled_key_input(false)
	# Restore outside the driver's screen_changed dispatch.
	_restore_host_return_menu.call_deferred()


func _restore_host_return_menu() -> void:
	if not _host_return_file.is_empty() and _menu_shell != null:
		_menu_shell.open_menu(_host_return_file, _host_return_screen)
	_host_return_file = ""
	_host_return_screen = ""


func _start_configured_novaworld_host(config: HostSessionConfig) -> void:
	if _novaworld_panel == null:
		return
	_mp_companion.host_channel = HostSessionConfig.CHANNEL_LAN
	_novaworld_panel.show()
	_menu_shell.set_process_unhandled_key_input(false)
	_novaworld_panel.begin_configured_host(config)
	_restore_host_return_menu.call_deferred()


# --- The join flow's device (engine inmatch::JoinFlow decides; JoinFlow binding) ----

# The browse: the enumerate of a target's endpoint (unwindowed, under its network type, a
# NovaWorld join's on the held session's socket with its proxy rendezvous), or with no target the
# `--lan-join browse` search on the LAN screen's defaults. Its socket binds on the block's client
# quad (engine inmatch::transport_bind_ports, D-NET-958).
func _begin_browse(target: JoinTarget, serial: int) -> bool:
	_end_browse()
	_browse = LanSession.new()
	_browse.resource_root = _resource_root()
	_browse.game_config = GameCfgStore.block()
	if target == null:
		_browse.name = "LaunchBrowse"
		add_child(_browse)
		_browse.servers_changed.connect(browse_rows.bind(serial))
		_browse.browse_finished.connect(browse_finished.bind(serial))
		return int(_browse.start_browsing()) == OK
	_browse.name = "SpectatorPreflight"
	_browse.set_windowed(false)
	if _novaworld_client != null:
		_browse.set_transport(_novaworld_client.get_game_pump())
	add_child(_browse)
	_browse.servers_changed.connect(browse_rows.bind(serial))
	_browse.set_proxy_from_target(target)
	return int(_browse.start_browsing(target.host_ip, target.port, target.port,
			target.network_type)) == OK


func _adopt_browse(target: JoinTarget) -> void:
	target.transport = _browse.take_transport()
	if _shell != null:
		_shell.adopt_lan_records(_browse)


func _end_browse() -> void:
	if _browse != null:
		_browse.stop()
		_browse.queue_free()
	_browse = null


func _drain_browse() -> void:
	if _browse != null:
		_browse.drain_records()


func _drain_enumerator() -> void:
	if _shell != null:
		_shell.drain_lan_records()


func _show_enumerating() -> bool:
	if _shell != null:
		_shell.begin_join_enumerate(cancel)
	return true


func _show_choice(server_flags: int) -> bool:
	if _shell != null:
		return _shell.begin_join_choice(server_flags, commit_choice, cancel)
	if standalone_join_screen == null or not standalone_join_screen.show_choice(server_flags):
		return false
	if not standalone_join_screen.choice_committed.is_connected(commit_choice):
		standalone_join_screen.choice_committed.connect(commit_choice)
		standalone_join_screen.cancelled.connect(cancel)
	return true


func _show_refusal(text: String) -> bool:
	return _shell != null and _shell.show_join_refusal(text, cancel)


func _close_join_screen() -> void:
	if _shell != null:
		_shell.end_join_enumerate()


func _callsign() -> String:
	return PlayerProfile.load_callsign()


func _switch_expansion(expansion: String) -> String:
	return _world.mount_join_expansion(expansion) if _world != null else "no world to mount"


func _save_profile() -> void:
	PlayerProfile.save()


func _reload_profile() -> void:
	if _shell != null:
		_shell.refresh_local_profile_for_mount()


func _save_config() -> void:
	GameCfgStore.save()


func _start_join(target: JoinTarget) -> void:
	if _shell == null:
		return
	var load_info := LoadingScreenInfo.make(target.mission, true, target.server_name, "",
			target.game_type, "")
	_adopt_held_novaworld_client()
	_shell.start_join_load(load_info, _world.load_mission_as_joiner.bind(target))


func _start_launch_host(mission: String, rotation: PackedStringArray, max_players: int,
		port: int, game_type: int, lan_mode: int) -> void:
	var config := HostSessionConfig.new()
	config.mission = mission
	if not rotation.is_empty():
		config.missions.assign(Array(rotation))
	config.max_players = max_players
	if port > 0:
		config.bind_port = port
	config.game_type_auto = game_type < 0
	if not config.game_type_auto:
		config.game_type = game_type
	if lan_mode > 0:
		config.lan_mode = lan_mode
	_on_lan_host_start_requested(config)


func _cancel_pre_game() -> void:
	if _world != null:
		_world.pre_game_cancelled()


func _show_main_menu() -> void:
	if _menu_shell != null:
		_menu_shell.show_menu()


func _route_novaworld_session(reenter: bool) -> void:
	var held := _novaworld_client
	_novaworld_client = null
	_enter_menu_with(held, reenter)


# --- LAN co-op (mp.mnu) --------------------------------------------------------

# Host a LAN co-op game: the same menu->world handoff as a single-player start, but the world
# loads as a listen-server host (ADR 0011) configured from the mp.mnu host screen. LAN reads no
# NovaWorld service configuration; g_ExpansionName is the expansion the process mounted.
func _on_lan_host_start_requested(config: HostSessionConfig) -> void:
	config.player_name = PlayerProfile.load_callsign()
	var root := _resource_root()
	config.expansion = root.get_expansion() if root != null else ""
	var load_info := LoadingScreenInfo.for_host(root, config.mission, config.server_name,
			config.game_type, config.game_type_auto, config.custom_text)
	_shell.start_world_load(
		load_info,
		_world.load_mission_as_host.bind(config.to_session_options()))


# --- NovaWorld (online multiplayer) ---------------------------------------------

# The menu's NovaWorld control: open the panel over the authored multiplayer
# screen. The full-rect panel blocks pointer input; suspend the MenuShell's
# unhandled-key path as well so Enter/Escape cannot activate controls behind
# the compact overlay.
func open_novaworld_panel(start_client := true) -> void:
	if _novaworld_panel != null:
		return
	_capture_menu_behind_novaworld()
	_novaworld_panel = NOVAWORLD_PANEL_SCENE.instantiate() as NovaWorldPanel
	# A return from a match hands the panel its session (or its error) instead
	# of starting a fresh connect.
	_novaworld_panel.start_client_on_ready = start_client
	# Hand the panel the mounted menu root so its host Map picker can list .bms missions (the world's
	# own root is null until a mission loads). Set BEFORE add_child so _ready can populate the scene.
	_novaworld_panel.resource_root = _resource_root()
	_novaworld_panel.refusals = _refusals
	_panel_layer.add_child(_novaworld_panel)
	_novaworld_panel.closed.connect(_on_novaworld_closed)
	# Bridge the panel's resolved join into the ONE joiner path (the same handler the LAN browser +
	# --lan-join launch use); the panel emits the same typed JoinTarget load_mission_as_joiner
	# consumes. Hosting from the panel routes through the shared host bring-up.
	_novaworld_panel.join_in_match_requested.connect(_on_novaworld_join_requested)
	_novaworld_panel.host_requested.connect(_on_novaworld_host_requested)
	_novaworld_panel.host_setup_requested.connect(_open_novaworld_host_setup)
	# A mod's install or update hands the process to the updater (ADR 0074).
	_novaworld_panel.update_started.connect(_on_novaworld_update_started)


## The relaunch after a mod's install for a join (`--novaworld-join <rid>`, ADR
## 0074): the NovaWorld menu opens and joins that server once signed in and
## listed. False when the launch names none.
func maybe_open_novaworld_from_flags() -> bool:
	var rid := LaunchFlags.novaworld_join()
	if rid <= 0:
		return false
	open_novaworld_panel()
	if _novaworld_panel != null:
		_novaworld_panel.join_when_listed(rid)
	return true


func _on_novaworld_update_started(reason: String) -> void:
	if _shell != null:
		_shell.exit_to_desktop(reason)


# The NovaWorld screen's close (NW_MULTI_PLAYER's deactivation) saves the menu's cookie jar to
# nw_cdata.coo (NovaWorldClient.save_cookie_jar; engine MenuBrowserState).
func _on_novaworld_closed() -> void:
	NovaWorldClient.save_cookie_jar()
	_dismiss_novaworld_panel()


## A join that failed on the join screen: a NovaWorld server that refused this install outright
## (engine inmatch::refuses_this_install) reads as unjoinable in the browser for the rest of the
## process, with the refusal's own text.
func note_join_failure(error: ConnectionError) -> void:
	var rid := join_failed(error)
	if rid > 0:
		_refusals.note_refusal(rid, error.reason_text(Strings.get_table(Strings.TABLE_GAMEERR)))


func _dismiss_novaworld_panel() -> void:
	if _novaworld_panel != null:
		_novaworld_panel.queue_free()
		_novaworld_panel = null
	_restore_menu_after_novaworld()


func _capture_menu_behind_novaworld() -> void:
	if _menu_shell == null:
		return
	_menu_visible_before_novaworld = _menu_shell.visible
	_menu_key_input_before_novaworld = _menu_shell.is_processing_unhandled_key_input()
	_novaworld_menu_state_captured = true
	# show_menu only changes presentation/process state; it keeps the current
	# .mnu document, screen, and back stack intact behind the overlay.
	_menu_shell.show_menu()
	_menu_shell.set_process_unhandled_key_input(false)


func _restore_menu_after_novaworld() -> void:
	if not _novaworld_menu_state_captured or _menu_shell == null:
		return
	_menu_shell.set_process_unhandled_key_input(_menu_key_input_before_novaworld)
	if _menu_visible_before_novaworld:
		_menu_shell.show_menu()
	else:
		_menu_shell.hide_menu()
	_novaworld_menu_state_captured = false


# The NovaWorld panel's session is hosting (the service's ServerHostResult landed). The host is
# retail's host dialog left untouched (engine inmatch/host_settings.cpp carries the witness), loaded
# as a listen host through the SAME bring-up the mp.mnu host screen uses; the hosting session
# rides in with the load and keeps the match registered.
func _on_novaworld_host_requested(config: HostSessionConfig) -> void:
	if config.mission.is_empty():
		push_warning("NetSessionController: NovaWorld host requested with no mission")
		if _novaworld_panel != null:
			_novaworld_panel.host_failed("No mission selected to host.")
		return
	_hold_novaworld_client(_novaworld_panel.release_client())
	host_from_novaworld()
	_dismiss_novaworld_panel()
	config.player_name = _novaworld_player_name()
	_adopt_held_novaworld_client()
	_shell.start_world_load(
			LoadingScreenInfo.for_host(_resource_root(), config.mission, config.server_name,
					config.game_type, config.game_type_auto, config.custom_text),
			_world.load_mission_as_host.bind(config.to_session_options()))


# The NovaWorld panel resolved a join target: the panel goes and the flow joins it
# (JoinFlow.join_from_novaworld: the profile and game.cfg saves, the return to the NovaWorld menu).
func _on_novaworld_join_requested(target: JoinTarget) -> void:
	var rid := _novaworld_panel.joining_rid()
	_hold_novaworld_client(_novaworld_panel.release_client())
	target.player_name = _novaworld_player_name()
	_dismiss_novaworld_panel()
	join_from_novaworld(target, rid)


# The local player's name in the held NovaWorld session, what a join sends as its ClientAuth NA
# and a host plays under: the session's CHAR cookie (the active character the service names),
# else the profile's callsign (D-NET-1299; the retail session apply's rule, witnessed at
# engine/net/novaworld/http_login.h novaworld_character_name, which a join and a host both run).
func _novaworld_player_name() -> String:
	var callsign := PlayerProfile.load_callsign()
	if _novaworld_client == null:
		return callsign
	return _novaworld_client.get_session_player_name(callsign)


## The post-mission route's NovaWorld half, once the shell is back in the menu. A normal exit
## keeps the NovaWorld session: a player who left from the NovaWorld menu re-enters it with the
## session. An exit with an error text shows the NovaWorld error dialog first, the session already
## reset with the world (the router's verdict, engine: inmatch/mission_exit.h).
func return_from_mission(client: NovaWorldClient, error_exit: bool, error_text: String) -> void:
	var reenter := take_return_to_novaworld()
	if not error_exit:
		_enter_menu_with(client, reenter)
		return
	if client != null:
		client.stop()
		client.queue_free()
	open_novaworld_panel(false)
	if _novaworld_panel != null:
		_novaworld_panel.show_post_mission_error(error_text, reenter)


# The main menu's entry with a kept NovaWorld session: back to the NovaWorld menu when the flow's
# return says so, else stopped.
func _enter_menu_with(client: NovaWorldClient, reenter: bool) -> void:
	if client == null:
		return
	if reenter:
		open_novaworld_panel(false)
		if _novaworld_panel != null:
			_novaworld_panel.adopt_client(client)
			return
	client.stop()
	client.queue_free()


# The play the service admitted stays up through the match: the client waits
# here (alive, still pumping) until the joiner load hands it to the world.
func _hold_novaworld_client(client: NovaWorldClient) -> void:
	_release_novaworld_client()
	_novaworld_client = client
	if client != null:
		add_child(client)


# The world takes the held session with the load it is about to run.
func _adopt_held_novaworld_client() -> void:
	if _novaworld_client == null:
		return
	_world.adopt_novaworld_client(_novaworld_client)
	_novaworld_client = null


## The process exit's teardown of a NovaWorld session the controller holds
## (MainGame.exit_process): its goodbye burst alone, no stop statement.
func disconnect_at_exit() -> void:
	var panel_client: NovaWorldClient = null
	if _novaworld_panel != null:
		panel_client = _novaworld_panel.release_client()
	for client: NovaWorldClient in [_novaworld_client, panel_client]:
		if client != null:
			client.disconnect_at_exit()
			client.queue_free()
	_novaworld_client = null


# The controller's teardown (or a new hold over an old one) ends a held session: it leaves its
# play and goes.
func _release_novaworld_client() -> void:
	if _novaworld_client != null:
		_novaworld_client.stop()
		_novaworld_client.queue_free()
	_novaworld_client = null
	release_novaworld()


# --- Shared lookups --------------------------------------------------------------

func _resource_root() -> ResourceRoot:
	return _shell.current_resource_root() if _shell != null else null
