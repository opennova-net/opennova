class_name PlayerOptions
extends RefCounted

## Process-lifetime owner for the player-facing options shared by the front-end
## and pause menus. Values are normalized at the boundary, saved together, and
## applied through one runtime seam so neither menu needs its own settings copy.

const CONFIG_PATH := "user://opennova.cfg"

const AUDIO_SECTION := "audio"
const SOUND_FX_VOLUME_KEY := "sound_fx_volume"
const DIALOG_VOLUME_KEY := "dialog_volume"
const MUSIC_VOLUME_KEY := "music_volume"

const CONTROLS_SECTION := "controls"
const MOUSE_SENSITIVITY_KEY := "mouse_sensitivity"
const INVERT_MOUSE_KEY := "invert_mouse"

const PLAYER_SECTION := "player"
const CROSSHAIR_STYLE_KEY := "crosshair_style"

const MIN_VOLUME := 0
const MAX_VOLUME := 255
const DEFAULT_VOLUME := 255
const MIN_MOUSE_SENSITIVITY := 4
const MAX_MOUSE_SENSITIVITY := 511
const DEFAULT_MOUSE_SENSITIVITY := 128
const MIN_CROSSHAIR_STYLE := 0
const MAX_CROSSHAIR_STYLE := 24
const DEFAULT_CROSSHAIR_STYLE := 0

const SOUND_FX_BUSES := [&"SFX", &"Ambient"]
const DIALOG_BUS := &"Voice"
const MUSIC_BUS := &"Music"


class State extends RefCounted:
	var sound_fx_volume: int
	var dialog_volume: int
	var music_volume: int
	var mouse_sensitivity: int
	var invert_mouse: bool
	var crosshair_style: int

	func _init(p_sound_fx_volume := DEFAULT_VOLUME,
			p_dialog_volume := DEFAULT_VOLUME,
			p_music_volume := DEFAULT_VOLUME,
			p_mouse_sensitivity := DEFAULT_MOUSE_SENSITIVITY,
			p_invert_mouse := false,
			p_crosshair_style := DEFAULT_CROSSHAIR_STYLE) -> void:
		sound_fx_volume = p_sound_fx_volume
		dialog_volume = p_dialog_volume
		music_volume = p_music_volume
		mouse_sensitivity = p_mouse_sensitivity
		invert_mouse = p_invert_mouse
		crosshair_style = p_crosshair_style

	func copy() -> State:
		return State.new(sound_fx_volume, dialog_volume, music_volume,
				mouse_sensitivity, invert_mouse, crosshair_style)


signal changed(state: State)

var _state: State


func _init() -> void:
	_state = _load_state()


## A detached snapshot: callers edit it and hand it back to update().
func current() -> State:
	return _state.copy()


## Normalize, atomically persist, apply the device settings, then publish the
## new detached state. Options are intentionally immediate: Accept/Back only
## navigate and never roll values back.
func update(state: State) -> void:
	if state == null:
		return
	_state = _normalized(state)
	ConfigStore.update(CONFIG_PATH, func(config: ConfigFile) -> void:
		config.set_value(AUDIO_SECTION, SOUND_FX_VOLUME_KEY,
				_state.sound_fx_volume)
		config.set_value(AUDIO_SECTION, DIALOG_VOLUME_KEY,
				_state.dialog_volume)
		config.set_value(AUDIO_SECTION, MUSIC_VOLUME_KEY,
				_state.music_volume)
		config.set_value(CONTROLS_SECTION, MOUSE_SENSITIVITY_KEY,
				_state.mouse_sensitivity)
		config.set_value(CONTROLS_SECTION, INVERT_MOUSE_KEY,
				_state.invert_mouse)
		config.set_value(PLAYER_SECTION, CROSSHAIR_STYLE_KEY,
				_state.crosshair_style)
	)
	apply()
	changed.emit(current())


## Apply device-global audio and, when present, the live local-player mouse
## settings. A newly constructed Simulation is passed here at world-load time.
func apply(simulation: Simulation = null) -> void:
	for bus_name: StringName in SOUND_FX_BUSES:
		_set_bus_volume(bus_name, _state.sound_fx_volume)
	_set_bus_volume(DIALOG_BUS, _state.dialog_volume)
	_set_bus_volume(MUSIC_BUS, _state.music_volume)
	if simulation != null:
		simulation.set_local_player_mouse(
				_state.mouse_sensitivity, _state.invert_mouse)


func _load_state() -> State:
	return _normalized(State.new(
			int(ConfigStore.read(CONFIG_PATH, AUDIO_SECTION,
					SOUND_FX_VOLUME_KEY, DEFAULT_VOLUME)),
			int(ConfigStore.read(CONFIG_PATH, AUDIO_SECTION,
					DIALOG_VOLUME_KEY, DEFAULT_VOLUME)),
			int(ConfigStore.read(CONFIG_PATH, AUDIO_SECTION,
					MUSIC_VOLUME_KEY, DEFAULT_VOLUME)),
			int(ConfigStore.read(CONFIG_PATH, CONTROLS_SECTION,
					MOUSE_SENSITIVITY_KEY, DEFAULT_MOUSE_SENSITIVITY)),
			bool(ConfigStore.read(CONFIG_PATH, CONTROLS_SECTION,
					INVERT_MOUSE_KEY, false)),
			int(ConfigStore.read(CONFIG_PATH, PLAYER_SECTION,
					CROSSHAIR_STYLE_KEY, DEFAULT_CROSSHAIR_STYLE))))


static func _normalized(state: State) -> State:
	return State.new(
			clampi(state.sound_fx_volume, MIN_VOLUME, MAX_VOLUME),
			clampi(state.dialog_volume, MIN_VOLUME, MAX_VOLUME),
			clampi(state.music_volume, MIN_VOLUME, MAX_VOLUME),
			clampi(state.mouse_sensitivity,
					MIN_MOUSE_SENSITIVITY, MAX_MOUSE_SENSITIVITY),
			state.invert_mouse,
			clampi(state.crosshair_style,
					MIN_CROSSHAIR_STYLE, MAX_CROSSHAIR_STYLE))


static func _set_bus_volume(bus_name: StringName, volume: int) -> void:
	var bus := AudioServer.get_bus_index(bus_name)
	if bus >= 0:
		AudioServer.set_bus_volume_db(bus,
				SoundSelector.volume_db_from_255(volume))
