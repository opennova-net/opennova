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
const ASPECT_MODE_KEY := "display_16x9"
# The two tip options persist as retail's game.cfg words (default 1), which
# the Options rows MR_CLIPPY_KEYBOARD / MR_CLIPPY_HINTS edit (engine
# hud/tip_system.h TipOptions carries the witness).
const KEYBOARD_TIPS_KEY := "enable_keyboardtips"
const GAMEPLAY_TIPS_KEY := "enable_gameplaytips"

# The slider ranges are the engine's witnessed Options ranges
# (options_policy.h kOptionsScrollRanges through MenuFrame), read by control
# name so the clamp can never drift from what the sliders seed.
const SOUND_FX_VOLUME_CONTROL := "SOUNDFXVOLUME"
const DIALOG_VOLUME_CONTROL := "DIALOGVOLUME"
const MUSIC_VOLUME_CONTROL := "MUSICVOLUME"
const MOUSE_SENSITIVITY_CONTROL := "MOUSE_SENSITIVITY"
# Initial channel volumes come from the engine configuration defaults.
const DEFAULT_VOLUME := SoundSelector.DEFAULT_CHANNEL_VOLUME
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
# The aspect mode persists as retail's `display_16x9` cfg word: the render
# aspect-mode index (0 = 4:3, 1 = 16:10, 2 = 16:9, 3 = 5:4; any other value =
# the surface's own ratio, a cfg-only choice no authored spin row carries). The
# 0 is only the pre-seed placeholder (the config defaults' memset): a fresh
# profile seeds the word from the primary desktop's ratio ONCE and persists
# it, as retail's first-launch video test does (fresh_profile_aspect_mode;
# docs/mnu/menu-re.md, the 16x9DISPLAY paragraph).
const DEFAULT_ASPECT_MODE := 0

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
	var aspect_mode: int
	var keyboard_tips: bool
	var gameplay_tips: bool

	func _init(p_sound_fx_volume := DEFAULT_VOLUME,
			p_dialog_volume := DEFAULT_VOLUME,
			p_music_volume := DEFAULT_VOLUME,
			p_mouse_sensitivity := DEFAULT_MOUSE_SENSITIVITY,
			p_invert_mouse := false,
			p_crosshair_style := DEFAULT_CROSSHAIR_STYLE,
			p_crosshair_color := DEFAULT_CROSSHAIR_COLOR,
			p_crosshair_spread := DEFAULT_CROSSHAIR_SPREAD,
			p_aspect_mode := DEFAULT_ASPECT_MODE,
			p_keyboard_tips := true,
			p_gameplay_tips := true) -> void:
		sound_fx_volume = p_sound_fx_volume
		dialog_volume = p_dialog_volume
		music_volume = p_music_volume
		mouse_sensitivity = p_mouse_sensitivity
		invert_mouse = p_invert_mouse
		crosshair_style = p_crosshair_style
		crosshair_color = p_crosshair_color
		crosshair_spread = p_crosshair_spread
		aspect_mode = p_aspect_mode
		keyboard_tips = p_keyboard_tips
		gameplay_tips = p_gameplay_tips

	func copy() -> State:
		return State.new(sound_fx_volume, dialog_volume, music_volume,
				mouse_sensitivity, invert_mouse, crosshair_style,
				crosshair_color, crosshair_spread, aspect_mode,
				keyboard_tips, gameplay_tips)


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
		config.set_value(PLAYER_SECTION, ASPECT_MODE_KEY, _state.aspect_mode)
		config.set_value(PLAYER_SECTION, KEYBOARD_TIPS_KEY, 1 if _state.keyboard_tips else 0)
		config.set_value(PLAYER_SECTION, GAMEPLAY_TIPS_KEY, 1 if _state.gameplay_tips else 0)
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
		simulation.set_local_player_aspect_mode(_state.aspect_mode)
		simulation.set_local_player_mouse(
				_state.mouse_sensitivity, _state.invert_mouse)


## The aspect mode a fresh profile seeds. Retail's first launch runs the video
## test, which writes the cfg word from the primary desktop's ratio -- 1 (the
## spin's widescreen row) past 1.34, else 0 -- and saves the config at once, so
## the desktop is sampled once and a later window or monitor never re-seeds it.
## The rule is the engine's (renderer/aspect_ratio.h); this is its device feed.
static func fresh_profile_aspect_mode(desktop: Vector2i) -> int:
	return Simulation.fresh_profile_aspect_mode(desktop.x, desktop.y)


## The primary desktop's size: retail's SM_CXSCREEN / SM_CYSCREEN sample.
static func desktop_size() -> Vector2i:
	return DisplayServer.screen_get_size(DisplayServer.SCREEN_PRIMARY)


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
					CROSSHAIR_SPREAD_KEY, DEFAULT_CROSSHAIR_SPREAD)),
			_load_aspect_mode(),
			int(ConfigStore.read(CONFIG_PATH, PLAYER_SECTION, KEYBOARD_TIPS_KEY, 1)) != 0,
			int(ConfigStore.read(CONFIG_PATH, PLAYER_SECTION, GAMEPLAY_TIPS_KEY, 1)) != 0))


# The persisted cfg word, or the one-time desktop seed a fresh profile writes
# before anything reads it (retail saves the whole config right after the
# video test decides the word).
static func _load_aspect_mode() -> int:
	# Any stored word (the native -1 included) wins; only an absent key seeds.
	if ConfigStore.has_key(CONFIG_PATH, PLAYER_SECTION, ASPECT_MODE_KEY):
		return int(ConfigStore.read(CONFIG_PATH, PLAYER_SECTION,
				ASPECT_MODE_KEY, DEFAULT_ASPECT_MODE))
	var seeded := fresh_profile_aspect_mode(desktop_size())
	ConfigStore.write(CONFIG_PATH, PLAYER_SECTION, ASPECT_MODE_KEY, seeded)
	return seeded


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
			state.crosshair_spread, state.aspect_mode,
			state.keyboard_tips, state.gameplay_tips)


# Clamp to the engine's witnessed range for an Options slider, by control
# name (a control the engine does not range passes through).
static func _clamp_to_control(value: int, control: String) -> int:
	for range: Dictionary in MenuFrame.options_scroll_ranges():
		if String(range["control"]) == control:
			return clampi(value, int(range["minimum"]), int(range["maximum"]))
	return value


static func _set_bus_volume(bus_name: StringName, volume: int) -> void:
	var bus := AudioServer.get_bus_index(bus_name)
	if bus >= 0:
		AudioServer.set_bus_volume_db(bus,
				SoundSelector.volume_db_from_255(volume))
