class_name MpMenuCompanion
extends MenuCompanion

# The device half of the LAN multiplayer screens (mp.mnu's LAN browser and host
# settings): the screens' rules are the engine's LAN controller
# (runtime/inmatch/lan_multiplayer_menu.h, the shell's "lan" delegate, through the
# LanMultiplayerMenu binding); this script hands it the string registry, the
# process's cfg block and the LAN discovery session, saves the block at the LAN
# START's save point, and turns the controller's join and host requests into the
# typed records the shell's owner dials and hosts with.

# The player chose to join the highlighted discovered LAN server. The payload is the
# typed dial target decoded from the discovery row (JoinTarget.from_lan_row).
signal lan_join_requested(target: JoinTarget)
# The player chose to join a LAN row whose index names no listed session: the join
# machine refuses it before anything is sent, with gameerr "MP Errors" CVUNABLETOCREATE
# (`text`) on the join screen.
signal lan_join_refused(text: String)
# START_GAME on the host screen: the host request made of the cfg block and the rotation.
signal lan_host_start_requested(config: HostSessionConfig)
signal novaworld_host_start_requested(config: HostSessionConfig)
signal novaworld_host_cancelled

## The host screen's channel (HostSessionConfig.CHANNEL_LAN / CHANNEL_NOVAWORLD): the
## NovaWorld panel's host setup hosts through it on NovaWorld.
var host_channel: String:
	get:
		return HostSessionConfig.CHANNEL_NOVAWORLD if _screen.is_novaworld_host() \
				else HostSessionConfig.CHANNEL_LAN
	set(value):
		_screen.set_novaworld_host(value == HostSessionConfig.CHANNEL_NOVAWORLD)

var _screen := LanMultiplayerMenu.new()


func _init() -> void:
	_screen.set_sources(func(table: String) -> RtxtStringFile: return Strings.get_table(table),
			func() -> GameConfigBlock: return GameCfgStore.block())
	_screen.join_requested.connect(lan_join_requested.emit)
	_screen.join_refused.connect(lan_join_refused.emit)
	_screen.novaworld_host_cancelled.connect(novaworld_host_cancelled.emit)
	_screen.config_save_requested.connect(func() -> void: GameCfgStore.save())
	_screen.host_start_requested.connect(_on_host_start_requested)


## The engine controller's binding the shell hands mp.mnu to.
func screen() -> MenuScreenBinding:
	return _screen


# Provide the LAN discovery session (its resource root is the screens' too). Kept
# injectable for menu and socket seam tests.
func set_lan_session(session: LanSession) -> void:
	_screen.set_lan_session(session)


## The catalog comes from the mounted resource root; tests may supply catalog rows.
func seed_host_pool(rows: Array[MissionCatalogRow]) -> void:
	_screen.seed_host_pool(rows)


# The request the controller made of the screen: the session's words are the cfg
# block's (HostSessionConfig seeds from it), the rotation and the expansion the
# controller's. The selected mission's mode owns the wire game type.
func _on_host_start_requested(missions: PackedStringArray, launch_options: PackedInt32Array,
		mission: String, game_type_attr: String, expansion: String, novaworld: bool) -> void:
	var config := HostSessionConfig.new()
	config.missions.assign(missions)
	config.mission_launch_options.assign(launch_options)
	config.mission = mission
	config.game_type_attr = game_type_attr
	config.game_type_auto = true
	config.expansion = expansion
	if novaworld:
		config.channel = HostSessionConfig.CHANNEL_NOVAWORLD
		novaworld_host_start_requested.emit(config)
	else:
		config.channel = HostSessionConfig.CHANNEL_LAN
		lan_host_start_requested.emit(config)
