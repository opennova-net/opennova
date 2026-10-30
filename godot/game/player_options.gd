class_name PlayerOptions
extends RefCounted

## The player-facing options shared by the front-end and pause menus, held for the process: the
## typed record over the cfg block's game.cfg words is the engine's (PlayerOptionsState over
## engine/runtime/inmatch/player_options.h: the words' keys, the clamps, the defaults and the
## mission start's apply rule), read and written through the block (GameCfgStore), which
## reaches game.cfg at its save points. This object keeps the current record, publishes each
## change and drives the device's mixer buses with the volumes.

# One home for the crosshair colour default / mask and spread default: the native HudOverlay
# binding over the engine's HudLayout.
const CROSSHAIR_COLOR_MASK := HudOverlay.CROSSHAIR_COLOR_MASK
const DEFAULT_CROSSHAIR_COLOR := HudOverlay.DEFAULT_CROSSHAIR_COLOR
const DEFAULT_CROSSHAIR_SPREAD := HudOverlay.DEFAULT_CROSSHAIR_SPREAD != 0

# The device's mixer buses each volume drives.
const SOUND_FX_BUSES := [&"SFX", &"Ambient"]
const DIALOG_BUS := &"Voice"
const MUSIC_BUS := &"Music"

signal changed(state: PlayerOptionsState)

var _state: PlayerOptionsState


func _init() -> void:
	_state = GameCfgStore.block().read_player_options()


## A detached snapshot: callers edit it and hand it back to update().
func current() -> PlayerOptionsState:
	return _state.copy()


## The record into the cfg block (normalized there), the device settings applied, then the new
## detached state published. Live application is retail's preview behavior; the pause dialog's
## OPT_CANCEL and the front-end OPTIONS BACK roll the model back to its entry snapshot through
## OptionsMenuController (docs/mnu/menu-re.md "The in-game options dialog").
func update(state: PlayerOptionsState) -> void:
	if state == null:
		return
	_state = GameCfgStore.block().write_player_options(state)
	apply()
	changed.emit(current())


## Take the words from the cfg block again (after a read of game.cfg, or the first launch's
## video test), apply them and publish the state.
func reload() -> void:
	_state = GameCfgStore.block().read_player_options()
	apply()
	changed.emit(current())


## The volumes to the mixer buses and, at a mission start (the world loaded), the aspect mode as
## the session's copy (the engine's apply rule, inmatch/player_options.h).
func apply(simulation: Simulation = null) -> void:
	for bus_name: StringName in SOUND_FX_BUSES:
		_set_bus_volume(bus_name, _state.sound_fx_volume)
	_set_bus_volume(DIALOG_BUS, _state.dialog_volume)
	_set_bus_volume(MUSIC_BUS, _state.music_volume)
	if simulation != null:
		simulation.set_local_player_aspect_mode(_state.aspect_mode)


## The aspect mode a fresh profile seeds from the primary desktop (the engine's rule,
## renderer/aspect_ratio.h).
static func fresh_profile_aspect_mode(desktop: Vector2i) -> int:
	return Simulation.fresh_profile_aspect_mode(desktop.x, desktop.y)


## The primary desktop's size, as retail's video test samples it.
static func desktop_size() -> Vector2i:
	return GameConfigBlock.primary_desktop_size()


static func _set_bus_volume(bus_name: StringName, volume: int) -> void:
	var bus := AudioServer.get_bus_index(bus_name)
	if bus >= 0:
		AudioServer.set_bus_volume_db(bus, SoundSelector.volume_db_from_255(volume))
