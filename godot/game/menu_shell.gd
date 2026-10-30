class_name MenuShell
extends Control

# The menu front end's device half: the compiled menu surface (a MenuFrame, the
# engine draw-list/pump Control), the backdrop movies under it, the widget
# sounds, the menu music context and the input routing, over the engine's menu
# shell (menu::MenuShell, engine/runtime/menu/menu_shell.h, which MenuDriver
# runs and the editor's Try mode runs too). The engine owns the menu mode's
# enter and leave, each open's wiring (the commands bound by control name, the
# lists the game fills, the Options screens, the update gate, the screen
# controller that owns a document), the cross-file jumps and their history, the
# back and quit, the URL rows, the updater's launch and the Mods pick's reload;
# this script answers its device requests and relays what leaves the menu to
# main_game.gd. Music streams through the shared MusicService autoload (one
# context at a time, like the original AudioVM): the shell opens the MENU
# context; GameWorld opens the GAME context at mission start.

# The director var the current screen's MUSICVAR lands in is
# MusicDirector.MENU_MUSIC_VAR_SLOT — the witness lives at the engine home,
# engine/runtime/audio audio/music_policy.h kMenuMusicVarSlot (golden test
# tests/mus/mus_vm_test.cpp). setup() pushes it synchronously (the open
# rebuilds in place) before the director's first _process tick, so the VM
# starts in the right section.

# Asset names resolved from the resource dir. JO defaults; override per game.
@export var main_menu_file := "main.mnu"
@export var ingame_menu_file := "game.mnu"
@export var menu_text_file := "menutxt.BIN"
# The gametext table (the original's g_TextGameText — in-game strings + the "WepDes"
# weapon names the HUD/armory/killfeed resolve) [orig: Game_InitSubsystems @0x4a6cd0
# loads "gametext.bin"].
@export var game_text_file := "gametext.bin"
# The menu shell's OWN text resource (options/menu strings + the "Avatars" section)
# [orig: the menu boot loads "game.bin" @0x552510 -> the menu resource @0x25510F8 —
# a SEPARATE table from g_TextGameText; the two were conflated pre-#226].
@export var menu_ui_text_file := "Game.bin"
# Interactive music: blank = the witnessed MENUMUS / M<n> resolution
# (MusicService.resolve_menu_music_pair); an explicit value wins (loose dev override).
@export var menu_sound_bank_file := ""
@export var menu_music_file := ""

## True while a screen over the menu holds the keyboard; main_game hands it its
## join screen's is_open. Retail's join screen is its own mode, so the menu it
## opens over (the LAN screen binds the same Return and Escape) takes no keys
## while it is up.
var keyboard_covered := Callable()  # () -> bool

# Shell -> main_game intents (the engine's menu::ShellExit). The shell never
# loads a world or quits the app itself.
signal start_requested(bms_name: String)
# `reason` names what ends the process, for the exit's log line.
signal exit_to_desktop_requested(reason: String)
signal return_to_menu_requested()
signal restart_requested()
signal resume_requested()
# The player chose NovaWorld (online multiplayer) from the menu.
signal novaworld_requested()
# The Mods list's pick switched the game (the root remounted in place and the
# menu opened anew): main_game reads the player's profile again.
signal game_reloaded()
# The in-game options dialog's Accept wrote the controls words and the key
# bindings into the player profile's current record: main_game applies them
# to the live bindings and the running session, then saves the profile
# (docs/playerinfo/player-sav-re.md "The controls words").
signal ingame_controls_accepted()
# The in-game options dialog's Accept on a dialog holding XHAIR_APPEARANCE
# (engine menu::OptionsScreen::ReloadCrosshairAlpha, D-HUD-48).
signal ingame_crosshair_reload_requested()
# A screen the driver shows (its activation): main_game runs the main menu's
# (STARTUP's) transport reset off it.
signal screen_activated(screen_name: String)


var _driver: MenuDriver
var _frame: MenuFrame
var _underlay: MenuVideoUnderlay
var _audio: MenuAudio
var _root: ResourceRoot
var _text: RtxtStringFile
var _style: MnsStyleSheet
var _frame_stats: FrameStats = null
var _player_options: PlayerOptions = null
# The window sounds the menu played, the last 16 oldest first, each numbered: what the game MCP's game_menu reads
# of them (DI-34: the editor's menu preview plays the same pump).
var _sounds_played: Array[Dictionary] = []
var _sound_count := 0
# The screen controllers' device halves (the multiplayer menu, PLAYER_INFO), each
# registered with the driver by its binding (MenuCompanion.screen()).
var _companions: Array = []


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_PASS
	set_process(false)


func _process(delta: float) -> void:
	var stats_on := _frame_stats != null and _frame_stats.is_capture_active()
	var started := Time.get_ticks_usec() if stats_on else 0
	update_menu_frame(delta)
	if stats_on:
		_frame_stats.add(FrameStats.FRAME_MENU_SHELL,
				Time.get_ticks_usec() - started)


## The menu mode's per-frame update: the pump, the widget clock, the
## menu-portrait model advance and, at the tail, the engine shell's deferred
## Mods reload. Public so a caller (and the GUT suite) can step the menu tick.
## retail: Menu_UpdateFrame @ 0x5528a0 (the mode struct's update slot @ 0x83b404).
func update_menu_frame(_delta: float = 0.0) -> void:
	if not is_in_game():
		pump_screen_pass()
	if _driver != null:
		_driver.tick(GameWorld.current_frame_clock_ms())
	# The one model runtime-frame driver outside a live mission: menu portraits
	# (avatar previews) are ObjectModels, which no longer self-clock.
	ObjectModel.advance_awake_frame(GameWorld.current_frame_clock_ms())
	if _driver != null:
		_driver.frame_tail()


## The screen pass's mouse pump: hover, the click and the window sounds of the
## mouse as the events left it. The menu mode's update runs it once a frame; over
## a running mission the game loop's frame runs it at its head, ahead of the
## frame's net frame and tick (MainGame._process).
## retail: Game_ProcessMainFrame @ 0x5263f0 - UI_ProcessMenuScreenFrame @ 0x54e650 (the call
## @ 0x52667b) -> CUIScene_EndFrame @ 0x63e600 (the call @ 0x54e668) ->
## CWnd_ProcessMouseEvent @ 0x647a00, ahead of
## Client_ProcessNetworkFrame @ 0x526692; Menu_UpdateFrame @ 0x5528a0 in the menus.
func pump_screen_pass() -> void:
	if _driver != null and is_visible_in_tree():
		_driver.pump_mouse()


## Whether the shell is the in-game overlay over a kept-loaded mission.
func is_in_game() -> bool:
	return _driver != null and _driver.is_in_game()


## MainGame installs its process-lifetime owner before setup; standalone shells
## receive a private owner during asset assembly.
func set_player_options(options: PlayerOptions) -> void:
	if options == null:
		return
	if _driver != null:
		push_warning("set_player_options after setup is ignored; the shell " +
				"already assembled around its owner")
		return
	_player_options = options


func set_frame_stats(board: FrameStats) -> void:
	if board == _frame_stats:
		return
	if _frame_stats != null:
		var old_edge := _on_frame_stats_capture_changed
		if _frame_stats.capture_changed.is_connected(old_edge):
			_frame_stats.capture_changed.disconnect(old_edge)
	_frame_stats = board
	if _frame_stats != null:
		var edge := _on_frame_stats_capture_changed
		if not _frame_stats.capture_changed.is_connected(edge):
			_frame_stats.capture_changed.connect(edge)
	_on_frame_stats_capture_changed(
			_frame_stats != null and _frame_stats.is_capture_active())


func _on_frame_stats_capture_changed(active: bool) -> void:
	if _underlay != null:
		_underlay.set_runtime_profiling_enabled(active)


func consume_video_process_us() -> int:
	return _underlay.consume_process_us() if _underlay != null else 0


## Install a screen controller's device half (the JO multiplayer menu, the
## PLAYER_INFO screen): its binding (companion.screen()) is a delegate the engine
## shell hands every document whose owner names it.
func add_companion(companion: MenuCompanion) -> void:
	if companion == null or _companions.has(companion):
		return
	_companions.append(companion)
	if _driver != null:
		_driver.add_screen(companion.screen())


## Build the shell against a resource root and enter the menu mode. The asset and
## driver wiring assemble once; a different root (the bundled placeholder menu
## handing over to a picked retail install, ADR 0048) reloads everything read
## from the root. The engine's enter shows the screen a mission was started from
## on the return from it (D-MNU-28), else the main menu's first screen. Returns
## false when the main menu cannot be resolved/loaded.
func setup(root: ResourceRoot) -> bool:
	var root_changed := root != _root
	_root = root
	if _driver == null:
		_assemble_assets()
	elif root_changed:
		_load_root_assets()
	_enter_menu_music()
	# Menu-mode enter (fresh boot AND return-from-game) recreates the
	# backdrop slots [orig: Menu_InitShellResources @ 0x552500 calls
	# UI_CreateMenuBinkVideos on both branches].
	_refresh_underlay(false)
	_driver.set_player_profiles(PlayerProfile.store())
	return _driver.enter_menu_mode(root_changed)


## A mission's start leaves the menu: the screen it was started from is kept on
## the screen history, under a mark, for the re-entry's setup() to show again.
func leave_menu_mode() -> void:
	if _driver != null:
		_driver.leave_menu_mode()


# --- Asset assembly -----------------------------------------------------------

func _assemble_assets() -> void:
	# The movie backdrop draws UNDER the compiled surface (child order): the
	# authored custom appearances paint nothing and the movies show through
	# [orig: Menu_RenderFrame @ 0x54b7c0 — Bink update + draw BEFORE the
	# scene walk; slot policy engine-side in menu/menu_video.h].
	_underlay = MenuVideoUnderlay.new()
	_underlay.name = "MenuVideoUnderlay"
	_underlay.set_anchors_preset(Control.PRESET_FULL_RECT)
	_underlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_underlay.set_runtime_profiling_enabled(
			_frame_stats != null and _frame_stats.is_capture_active())
	add_child(_underlay)

	# The compiled surface: MenuFrame renders + pumps in the fixed 800x600
	# design space, anamorphically scaled to its own size [orig:
	# CUIScene_SetScreenScale @ 0x639480 — the scale pair lives inside the
	# frame's compile].
	_frame = MenuFrame.new()
	_frame.name = "MenuFrameSurface"
	_frame.set_anchors_preset(Control.PRESET_FULL_RECT)
	_frame.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_frame)

	# Menu hover/click SFX come from the .lwf profile (set-by-trigger -> .wav);
	# playback is the MenuAudio device leg.
	_audio = MenuAudio.new()
	_audio.name = "MenuAudio"
	add_child(_audio)

	_driver = MenuDriver.new()
	_driver.attach(_frame, _audio)
	_driver.set_menu_files(main_menu_file, ingame_menu_file)
	_load_root_assets()
	# The one music context lives on the MusicService autoload; the driver pushes
	# each screen's MUSICVAR into its director at the menumus discriminator index.
	_driver.set_music_director(MusicService.director())
	_driver.set_music_var_index(MusicDirector.MENU_MUSIC_VAR_SLOT)
	if _player_options == null:
		_player_options = PlayerOptions.new()
	_driver.set_player_words_source(
			func() -> PackedInt32Array: return _player_options.current().to_words(),
			_set_player_words)
	_driver.set_update_launcher(_launch_update)
	_driver.set_update_waiting(RuntimeUpdates.available)
	_driver.screen_changed.connect(_on_screen_changed)
	_driver.sound_requested.connect(_on_sound_requested)
	_driver.shell_exit_requested.connect(_on_shell_exit_requested)
	_driver.shell_resources_requested.connect(_on_shell_resources_requested)
	_driver.game_reloaded.connect(game_reloaded.emit)
	_driver.profile_save_requested.connect(func() -> void: PlayerProfile.save())
	_driver.controls_accepted.connect(ingame_controls_accepted.emit)
	_driver.crosshair_reload_requested.connect(ingame_crosshair_reload_requested.emit)
	if not RuntimeUpdates.availability_changed.is_connected(_on_update_availability_changed):
		RuntimeUpdates.availability_changed.connect(_on_update_availability_changed)
	for companion in _companions:
		_driver.add_screen(companion.screen())
	set_process(true)


# Everything the shell reads from its resource root: the text tables and the
# stylesheet (the engine shell reads its menus again itself). A widget's <SOUND>
# plays from the bank it names (MenuAudio), so the shell loads no sound bank.
func _load_root_assets() -> void:
	_text = _load_text(menu_text_file)
	# Register the engine text tables into the shared Strings registry, the way the
	# original loads its TextResource globals: menutxt (UI/voice labels), gametext =
	# gametext.bin (g_TextGameText — the "WepDes" weapon names + in-game strings
	# [orig: Game_InitSubsystems @0x4a6cd0]), and gameui = Game.bin (the menu shell's
	# own resource: options/menu + "Avatars" sections [orig: the menu boot @0x552510
	# -> the menu resource @0x25510F8]).
	# Registered unconditionally: a root without one of the tables unregisters
	# the previous root's (null clears) instead of keeping its strings alive.
	Strings.register_table(Strings.TABLE_MENUTXT, _text)
	Strings.register_table(Strings.TABLE_GAMETEXT, _load_text(game_text_file))
	Strings.register_table(Strings.TABLE_GAMEERR, _load_text("gameerr.bin"))
	Strings.register_table(Strings.TABLE_GAMEUI, _load_text(menu_ui_text_file))
	# The shell's stylesheets by their fixed names, usually PFF-archived:
	# menu_style.mns, then brand.mns onto it, as the game loads them (the port
	# and its witnesses: engine/runtime/menu/menu_style.h); null when neither is
	# there, so every %VAR% stays literal.
	_style = MnsStyleSheet.load_shell(_root) if _root != null else null
	_audio.set_resource_root(_root)
	_driver.set_shell_sources(_root, _style)


# --- Input routing (the frame is a passive surface; the shell samples) --------

func _gui_input(event: InputEvent) -> void:
	if _driver == null or not visible:
		return
	if _driver.consume_options_input(event) & MenuDriver.OPTIONS_CONSUMED:
		accept_event()
		return
	# Each mouse event as its message, as it arrives (the left button's press and
	# release reach the windows now); the pump is update_menu_frame's, once a frame.
	if _driver.take_mouse_event(event):
		accept_event()
	elif event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.pressed and (button.button_index == MOUSE_BUTTON_WHEEL_DOWN \
				or button.button_index == MOUSE_BUTTON_WHEEL_UP):
			# One notch = one row tick (D-MNU-18 deliberate divergence).
			if _driver.process_wheel(button.position,
					1 if button.button_index == MOUSE_BUTTON_WHEEL_DOWN else -1):
				accept_event()


func _unhandled_key_input(event: InputEvent) -> void:
	# A backgrounded menu must not steal Esc from the world, nor a covered one
	# the keys of the screen over it.
	if _driver == null or not is_visible_in_tree() or _is_keyboard_covered():
		return
	if _driver.consume_options_input(event) & MenuDriver.OPTIONS_CONSUMED:
		get_viewport().set_input_as_handled()
		return
	if event is InputEventKey and _driver.handle_key_input(event as InputEventKey):
		get_viewport().set_input_as_handled()


func _is_keyboard_covered() -> bool:
	return keyboard_covered.is_valid() and bool(keyboard_covered.call())


# --- Menu loading + navigation (the engine shell's) ---------------------------

## Open `file` at `target_screen` (empty: its first) and wire it. Fails soft (a
## warning, the current screen kept) when the file can't be resolved.
func open_menu(file: String, target_screen: String) -> bool:
	if _driver == null:
		return false
	_driver.set_player_profiles(PlayerProfile.store())
	return _driver.open_menu(file, target_screen)


func show_menu() -> void:
	visible = true
	set_process(_driver != null)


func hide_menu() -> void:
	visible = false
	set_process(false)
	# The menu's pointer (its CURSOR, or none) is the menu's alone: the shell's
	# other surfaces keep the system one (MenuDriver.get_applied_pointer).
	Input.set_custom_mouse_cursor(null, Input.CURSOR_ARROW)


## Process-exit-only release for the retail menu cursor + the compiled menu's
## GPU textures: Input keeps a process-wide cursor reference after applying it
## and the frame retains its texture set; both must drop before RenderingServer
## exits.
func release_runtime_renderer_resources() -> void:
	Input.set_custom_mouse_cursor(null, Input.CURSOR_ARROW)
	if _underlay != null:
		_underlay.stop()
	if _frame != null:
		# configure(null) wipes the retained texture/font sets.
		_frame.configure(null, "", null, null)


## The in-game/pause overlay over a kept-loaded world: the top-level back
## resumes play instead of exiting.
func open_ingame_menu() -> bool:
	# Menu movies never tick in-game [orig: BinkVideo_UpdateAllSlots
	# @ 0x5676f0 has exactly one caller, Menu_RenderFrame].
	if _underlay != null:
		_underlay.stop()
	if _driver == null:
		return false
	_driver.set_player_profiles(PlayerProfile.store())
	return _driver.open_ingame_menu()


# (Re)create the backdrop movie slots from the live root + expansion. The
# in-game overlay never shows them.
func _refresh_underlay(in_game: bool) -> void:
	if _underlay == null:
		return
	if in_game or _root == null:
		_underlay.stop()
		return
	_underlay.set_source(_root.get_root_dir(), String(_root.get_expansion()))


func _on_screen_changed(screen_name: String) -> void:
	if _underlay != null:
		_underlay.set_screen(screen_name)
	screen_activated.emit(screen_name)


func _on_update_availability_changed(available: bool) -> void:
	if _driver != null:
		_driver.set_update_waiting(available)


# What leaves the menu, as the engine shell asks it (menu::ShellExit).
func _on_shell_exit_requested(exit: int, text: String) -> void:
	match exit:
		MenuDriver.SHELL_START_MISSION:
			start_requested.emit(text)
		MenuDriver.SHELL_EXIT:
			exit_to_desktop_requested.emit(text)
		MenuDriver.SHELL_RETURN_TO_MENU:
			return_to_menu_requested.emit()
		MenuDriver.SHELL_RESTART:
			restart_requested.emit()
		MenuDriver.SHELL_RESUME:
			resume_requested.emit()
		MenuDriver.SHELL_NOVAWORLD:
			novaworld_requested.emit()


# The updater's launch (the engine quits the game once it started).
func _launch_update() -> bool:
	if RuntimeUpdates.launch_update():
		return true
	push_warning("MenuShell: the updater did not start: %s" % RuntimeUpdates.last_error)
	return false


func _set_player_words(words: PackedInt32Array) -> void:
	var state := _player_options.current()
	state.set_words(words)
	_player_options.update(state)


# The Mods pick's reload: the shell's resources read again from the new mount.
func _on_shell_resources_requested() -> void:
	_load_root_assets()
	_enter_menu_music()
	_refresh_underlay(false)


func _on_sound_requested(file: String, trigger: String) -> void:
	_sound_count += 1
	_sounds_played.append({"seq": _sound_count, "file": file, "trigger": trigger})
	if _sounds_played.size() > 16:
		_sounds_played.pop_front()


func has_pending_expansion_reload() -> bool:
	return _driver != null and _driver.has_pending_expansion_reload()


# --- Audio --------------------------------------------------------------------

# Open the MENU music context on the shared MusicService (the original opens
# it once at boot [orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60] and re-opens
# it when the front end returns; the GAME context is the world's to open at
# mission start [orig: Game_StartMission @ 0x525598]).
func _enter_menu_music() -> void:
	MusicService.open_menu_context(_root, menu_music_file, menu_sound_bank_file)


# The witnessed music-pair resolution lives on MusicService; these seams keep it
# queryable against the shell's root (ADR 0018).
func resolve_menu_music_pair() -> MusicPair:
	return MusicService.resolve_menu_music_pair(_root)


func resolve_game_music_pair() -> MusicPair:
	return MusicService.resolve_game_music_pair(_root)


func _load_text(file: String) -> RtxtStringFile:
	if file.is_empty():
		return null
	return Strings.load_rtxt(_root, file)


# --- Misc helpers / accessors -------------------------------------------------

func get_driver() -> MenuDriver:
	return _driver


func get_frame() -> MenuFrame:
	return _frame


func get_stylesheet() -> MnsStyleSheet:
	return _style


func get_resource_root() -> ResourceRoot:
	return _root


func get_current_menu_file() -> String:
	return _driver.get_current_menu_file() if _driver != null else ""


func get_selected_mission() -> String:
	return _driver.get_selected_mission() if _driver != null else ""


## The screen history's rows, a mission's mark included.
func get_menu_stack_depth() -> int:
	return _driver.get_screen_history_depth() if _driver != null else 0


# --- MCP menu-driving seam (the game_menu tool) -------------------------------
# Typed surface for driving the compiled menu from the runtime MCP: snapshot
# the current screen's widgets, press one through the REAL mouse pump (design
# coords scale exactly like _gui_input's), feed a key event, or navigate.
# Positions are design-space (800x600); the frame scales like process_mouse.


func menu_snapshot(include_widgets: bool = true) -> Dictionary:
	if _driver == null or _frame == null:
		return {}
	var snapshot := {
		"visible": is_visible_in_tree(),
		"surface": "shell",
		"file": get_current_menu_file(),
		"screen": _driver.get_current_screen(),
		"screens": _driver.get_screen_names(),
		"in_game": is_in_game(),
		"stack_depth": _driver.get_screen_history_depth(),
		"sounds": _sounds_played.duplicate(true),
		"underlay": {
			"active_slots": _underlay.get_active_slot_count() if _underlay != null else 0,
			"startup_layout": _underlay.is_startup_layout() if _underlay != null else false,
			"failed": _underlay.get_failed_count() if _underlay != null else 0,
		},
	}
	return MenuFrameSurface.menu_state(_driver, _frame, snapshot, include_widgets)


# Press+release through the real pump at the widget's design-rect center
# (MenuFrameSurface.press_widget): resolution is frame-side (pre-order index).
func menu_press(widget_name: String) -> bool:
	if _driver == null or _frame == null or not is_visible_in_tree():
		return false
	return MenuFrameSurface.press_widget(_driver, _frame, widget_name)


# One raw pump sample at design coords with the button held or not, as a
# motion is. Returns the hit widget index (-1 for none).
func menu_move_at(design_pos: Vector2, down: bool) -> int:
	if _driver == null or _frame == null or not is_visible_in_tree():
		return -1
	return MenuFrameSurface.pump_at(_driver, _frame, design_pos, down)


# Raw pump press+release at design coords (list rows, combo popups, spin
# arrows). Returns the hit widget index (-1 for none).
func menu_press_at(design_pos: Vector2) -> int:
	if _driver == null or _frame == null or not is_visible_in_tree():
		return -1
	return MenuFrameSurface.press_at(_driver, _frame, design_pos)


# A REAL left click at design coords through Godot's input dispatch
# (MenuFrameSurface.click_at) -- the seam that observes whether a mounted
# Control steals a click from the frame. The events land on the next frame;
# callers re-read the menu state after a frame.
func menu_click_at(design_pos: Vector2) -> bool:
	if _driver == null or _frame == null or not is_visible_in_tree():
		return false
	MenuFrameSurface.click_at(_frame, design_pos)
	return true


func menu_key(keycode: int, unicode: int = 0) -> bool:
	# The same guard as the real input paths: a hidden menu (a world is up)
	# or a covered one must not receive synthetic menu input either.
	if _driver == null or not is_visible_in_tree() or _is_keyboard_covered():
		return false
	var ev := InputEventKey.new()
	ev.keycode = keycode as Key
	ev.physical_keycode = keycode as Key
	ev.unicode = unicode
	ev.pressed = true
	# Same order as the real path: an armed remap capture consumes keys first.
	if _driver.consume_options_input(ev) & MenuDriver.OPTIONS_CONSUMED:
		return true
	return _driver.handle_key_input(ev)


func menu_show_screen(name: String) -> bool:
	if _driver == null:
		return false
	return _driver.show_screen(name)
