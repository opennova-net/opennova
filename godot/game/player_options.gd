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
const CROSSHAIR_COLOR_KEY := "crosshair_color"
const CROSSHAIR_SPREAD_KEY := "crosshair_spread"

# The slider ranges are the engine's witnessed Options ranges
# (options_policy.h kOptionsScrollRanges through MenuFrame), read by control
# name so the clamp can never drift from what the sliders seed.
const SOUND_FX_VOLUME_CONTROL := "SOUNDFXVOLUME"
const DIALOG_VOLUME_CONTROL := "DIALOGVOLUME"
const MUSIC_VOLUME_CONTROL := "MUSICVOLUME"
const MOUSE_SENSITIVITY_CONTROL := "MOUSE_SENSITIVITY"
# The starting values are not witnessed profile defaults (retail's live in
# the player profile, not game.cfg): full volume and mid sensitivity.
const DEFAULT_VOLUME := 255
const DEFAULT_MOUSE_SENSITIVITY := 128
# One home for the crosshair art range, colour default / mask and spread
# default: the native HudOverlay binding over the engine's HudLayout
# (Config_SetDefaults @0x54d461 / @0x54d472 via hud_frame.h).
const MIN_CROSSHAIR_STYLE := HudOverlay.MIN_CROSSHAIR_STYLE
const MAX_CROSSHAIR_STYLE := HudOverlay.MAX_CROSSHAIR_STYLE
const DEFAULT_CROSSHAIR_STYLE := 0
const CROSSHAIR_COLOR_MASK := HudOverlay.CROSSHAIR_COLOR_MASK
const DEFAULT_CROSSHAIR_COLOR := HudOverlay.DEFAULT_CROSSHAIR_COLOR
const DEFAULT_CROSSHAIR_SPREAD := HudOverlay.DEFAULT_CROSSHAIR_SPREAD != 0

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
	var crosshair_color: int
	var crosshair_spread: bool

	func _init(p_sound_fx_volume := DEFAULT_VOLUME,
			p_dialog_volume := DEFAULT_VOLUME,
			p_music_volume := DEFAULT_VOLUME,
			p_mouse_sensitivity := DEFAULT_MOUSE_SENSITIVITY,
			p_invert_mouse := false,
			p_crosshair_style := DEFAULT_CROSSHAIR_STYLE,
			p_crosshair_color := DEFAULT_CROSSHAIR_COLOR,
			p_crosshair_spread := DEFAULT_CROSSHAIR_SPREAD) -> void:
		sound_fx_volume = p_sound_fx_volume
		dialog_volume = p_dialog_volume
		music_volume = p_music_volume
		mouse_sensitivity = p_mouse_sensitivity
		invert_mouse = p_invert_mouse
		crosshair_style = p_crosshair_style
		crosshair_color = p_crosshair_color
		crosshair_spread = p_crosshair_spread

	func copy() -> State:
		return State.new(sound_fx_volume, dialog_volume, music_volume,
				mouse_sensitivity, invert_mouse, crosshair_style,
				crosshair_color, crosshair_spread)


signal changed(state: State)

var _state: State


func _init() -> void:
	_state = _load_state()


## A detached snapshot: callers edit it and hand it back to update().
func current() -> State:
	return _state.copy()


## Normalize, atomically persist, apply the device settings, then publish the
## new detached state. Live application is retail's preview behavior; the
## pause dialog's OPT_CANCEL rolls the model back to its entry snapshot
## through OptionsMenuController (docs/mnu/menu-re.md "The in-game options
## dialog").
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
		config.set_value(PLAYER_SECTION, CROSSHAIR_COLOR_KEY,
				_state.crosshair_color)
		config.set_value(PLAYER_SECTION, CROSSHAIR_SPREAD_KEY,
				_state.crosshair_spread)
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
	apply_mouse(simulation)


## The live local-player mouse settings alone — what a `changed` listener
## with a running Simulation pushes (update() already applied the audio).
func apply_mouse(simulation: Simulation) -> void:
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
					CROSSHAIR_STYLE_KEY, DEFAULT_CROSSHAIR_STYLE)),
			int(ConfigStore.read(CONFIG_PATH, PLAYER_SECTION,
					CROSSHAIR_COLOR_KEY, DEFAULT_CROSSHAIR_COLOR)),
			bool(ConfigStore.read(CONFIG_PATH, PLAYER_SECTION,
					CROSSHAIR_SPREAD_KEY, DEFAULT_CROSSHAIR_SPREAD))))


static func _normalized(state: State) -> State:
	return State.new(
			_clamp_to_control(state.sound_fx_volume, SOUND_FX_VOLUME_CONTROL),
			_clamp_to_control(state.dialog_volume, DIALOG_VOLUME_CONTROL),
			_clamp_to_control(state.music_volume, MUSIC_VOLUME_CONTROL),
			_clamp_to_control(state.mouse_sensitivity, MOUSE_SENSITIVITY_CONTROL),
			state.invert_mouse,
			clampi(state.crosshair_style,
					MIN_CROSSHAIR_STYLE, MAX_CROSSHAIR_STYLE),
			state.crosshair_color & CROSSHAIR_COLOR_MASK,
			state.crosshair_spread)


## The engine's witnessed range for an Options slider, by control name.
static func scroll_range_of(control: String) -> Dictionary:
	for range: Dictionary in MenuFrame.options_scroll_ranges():
		if String(range["control"]) == control:
			return range
	return {}


static func _clamp_to_control(value: int, control: String) -> int:
	var range := scroll_range_of(control)
	if range.is_empty():
		return value
	return clampi(value, int(range["minimum"]), int(range["maximum"]))


static func _set_bus_volume(bus_name: StringName, volume: int) -> void:
	var bus := AudioServer.get_bus_index(bus_name)
	if bus >= 0:
		AudioServer.set_bus_volume_db(bus,
				SoundSelector.volume_db_from_255(volume))
