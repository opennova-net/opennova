class_name MenuShell
extends Control

# Runtime menu shell: drives the compiled menu surface — a MenuFrame (the
# engine draw-list/pump Control) orchestrated by MenuDriver,
# loading the game's .mnu menu set + audio from the user's resource directory.
# Music streams through the shared MusicService autoload (one context at a
# time, like the original AudioVM): the shell opens the MENU context; GameWorld
# opens the GAME context at mission start. It is the runtime counterpart to
# GameWorld: GameWorld turns a resource dir into a playable world, this turns it
# into the playable menu front-end, and main_game.gd hands off between the two.
#
# The driver owns intra-.mnu navigation, widget interaction, sounds, and the
# per-screen MUSICVAR push. The shell services the policy the driver leaves to
# it: cross-.mnu file jumps (menu_requested) and their history (pop_requested),
# the named back/quit Command, and the gameplay launch. Shipped JO menus carry no "launch" action verb; the engine
# wires those by well-known control NAME (START_GAME, ACCEPT, EXIT, ...), so
# the shell resolves those names against the loaded document and routes the
# driver's activation signal. The control-name sets are exported so a
# different game's menu set can be pointed at the same shell.

const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")

# The director var the current screen's MUSICVAR lands in is
# MusicDirector.MENU_MUSIC_VAR_SLOT — the witness lives at the engine home,
# engine/runtime/audio audio/music_policy.h kMenuMusicVarSlot (the menumus MUS
# script reads its section discriminator there; at index 0 it was inert and
# the VM always ran the var2=0 path instead of the screen's section — golden
# test tests/mus/mus_vm_test.cpp). setup() pushes it synchronously (open_menu
# rebuilds in place) before the director's first _process tick, so the VM
# starts in the right section.

# The list item and the persisted key stay the raw folder name (e.g. "jox01");
# the description text is cached per name once read.
var _expansion_descriptions: Dictionary = {}  # folder name -> MOD_DESC text

# Asset names resolved from the resource dir. JO defaults; override per game. A
# blank discovery name falls back to the first file of that kind in the dir.
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
# Interactive music: the engine hardcodes two bank+script pairs -- MENUMUS.SBF/.BIN
# (menu) and GAMEMUS.SBF/.BIN (game), renamed to M<n>/G<n> forms when expansion <n>
# is active [orig: Expansion_LoadAssets @ 0x4a4798]. Blank = that witnessed
# resolution (see resolve_menu_music_pair / resolve_game_music_pair); an
# explicit value wins (loose dev override).
@export var menu_sound_bank_file := ""   # "" -> MENUMUS.SBF (M<n>.sbf under an expansion)
# Menu SFX profile: the .lwf the widgets' <SOUND> elements reference (hover/click).
# "" -> a .lwf whose name contains "menu" (i.e. menu.lwf), else the first .lwf found.
@export var menu_sound_profile_file := ""
@export var menu_music_file := ""        # "" -> MENUMUS.BIN (M<n>.bin under an expansion)

# Well-known control names (the JO "wired by convention" launch/quit controls).
# An activated control matching one of these routes to the shell handler.
@export var start_control_names := PackedStringArray([
	"START_GAME", "ACCEPT", "LAUNCH", "GO", "HOST_GAME", "LAN_HOSTGAME",
])
@export var exit_control_names := PackedStringArray([
	"EXIT", "QUIT", "QUIT_GAME", "QUIT_TO_DESKTOP",
])
# The mission-exit Command seam. Retail's in-game ABORT button does NOT leave
# the mission: its authored actions raise the CONFIRM_EXIT "Are you sure?"
# panel (SHOW CONFIRM_EXIT + HIDE MAIN_WRAPPER in the shipped game.mnu), and
# the exit itself is the engine Command registered on CONFIRM_YES — the same
# per-(screen,control) seam as the other named controls (docs/mnu/menu-re.md,
# "The in-game exit confirmation"). Retail's ABORT command arms a
# confirm-pending latch that CONFIRM_YES checks; here the latch is modeled by
# reachability — the panel is only shown by ABORT's authored actions, hidden
# widgets are unclickable, and the engine hotkey scan skips hidden subtrees —
# so a CONFIRM_YES activation implies an armed ABORT. CONFIRM_NO needs no
# binding: its authored actions restore MAIN_WRAPPER. Binding ABORT here was
# the bug that skipped the confirmation (the teardown swapped the document
# under the emit, so the authored SHOW never dispatched).
@export var return_control_names := PackedStringArray([
	"CONFIRM_YES",
])
# The in-game RESTART command (game.mnu INGAME), the engine's
# World::ingame_restart_command behind the shell's restart_requested.
@export var restart_control_names := PackedStringArray([
	"RESTART",
])
# The generic BACK command seam: the actionless named button the original
# engine's shell binds by name (game.mnu's ESC-hotkeyed HIDDEN_BACK is the ONLY
# resume affordance the shipped in-game menu has — there is no visible RESUME
# button). Witnessed names only: an invented alias could double-dispatch
# against a shipped or modded button of the same name that carries a real
# action. Routed through the top-level back/quit logic: cross-.mnu back first,
# then resume (in-game) or exit-to-desktop (main menu).
@export var back_control_names := PackedStringArray([
	"HIDDEN_BACK",
])
# List widgets the shell fills with the resource dir's missions (.bms).
@export var mission_list_names := PackedStringArray([
	"MISSION_LIST", "MISSIONLIST", "MISSIONS", "IA_LIST", "CA_MISSION_LIST",
	"MAP_LIST",
])
# The engine menu flow recognizes these SP lists and owns their selection gate.
@export var sp_mission_list_names := PackedStringArray([
	"IA_LIST", "CA_MISSION_LIST",
])
# The SP briefing pane a selection fills (cleared on every populate).
@export var briefing_pane_names := PackedStringArray([
	"BRIEFING",
])
# The SP confirm controls receive the engine selection gate.
@export var sp_accept_control_names := PackedStringArray([
	"ACCEPT",
])
# List widgets the shell fills with the expansions discoverable under the resource
# dir (Options -> Mods). Activating one mounts it over the base game.
@export var mod_list_names := PackedStringArray([
	"AVAIL_LIST", "MOD_LIST", "MODLIST", "EXPANSION_LIST",
])
# Readonly text widgets that show the selected expansion's description/name.
@export var mod_desc_names := PackedStringArray([
	"MOD_DESC", "MOD_DESCRIPTION",
])

# Controls that open NovaWorld (online multiplayer). The shipped JO main menu
# carries an NW_MULTI_PLAYER button and jo_mp.mnu a NOVAWORLD window/screen.
@export var novaworld_control_names := PackedStringArray([
	"NW_MULTI_PLAYER", "NOVAWORLD", "NOVAWORLD_LOGIN", "INTERNET_GAME",
])

# Shell -> main_game intents. The shell never loads a world or quits the app
# itself; it translates menu activity into these and lets main_game decide.
signal start_requested(bms_name: String)
signal exit_to_desktop_requested()
signal return_to_menu_requested()
signal restart_requested()
signal resume_requested()
# The player chose NovaWorld (online multiplayer) from the menu. main_game
# opens the NovaWorld panel; the shell stays out of the networking itself.
signal novaworld_requested()


var _driver: MenuDriver
var _frame: MenuFrame
var _underlay: MenuVideoUnderlay
var _audio: MenuAudio
var _root: ResourceRoot
var _text: RtxtStringFile
var _style: MnsStyleSheet
var _sound_profile: LwfData
var _frame_stats: FrameStats = null
var _player_options: PlayerOptions = null
var _options_controller: OptionsMenuController = null
# The window sounds the menu played, the last 16 oldest first, each numbered: what the game MCP's game_menu reads
# of them (DI-34: the editor's menu preview plays the same pump).
var _sounds_played: Array[Dictionary] = []
var _sound_count := 0

var _menu_cache: Dictionary = {}            # filename -> MnuDocument
# The screen history across .mnu files and a mission is the driver's
# (MenuDriver.push_screen_history / pop_screen_history, engine
# menu/screen_history.h): one back stack for every file, kept while a mission runs.
var _current_file := ""
var _selected_expansion := ""
var _in_game := false
# True from setup() until a mission's start leaves the menu (leave_menu_mode):
# the next setup() on the same root is then the menu's re-entry.
var _menu_mode := false
# Named-control routing rebuilt per open_menu: NAME (upper) -> Callable.
var _named_handlers: Dictionary = {}
# Optional delegates that own game-specific menus the generic shell does not handle
# (the JO multiplayer menu — mp_menu_companion.gd; the PLAYER_INFO character screen —
# player_info_menu_companion.gd). Empty for a plain shell. The first whose owns_menu()
# claims a loaded menu drives it; otherwise the shell's generic wiring runs.
var _companions: Array = []
# The companion wired to the current document (null when the shell's generic
# wiring runs); the next document swap releases it if it loses ownership.
var _wired_companion: MenuCompanion = null



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


## The menu mode's per-frame update: the widget clock, the menu-portrait model
## advance, and -- at the tail, the way the original orders it -- the deferred
## expansion remount. Public so a caller (and the GUT suite) can step the menu
## tick the way the mode loop does.
## retail: Menu_UpdateFrame @ 0x5528a0 (the mode struct's update slot @ 0x83b404).
func update_menu_frame(delta: float = 0.0) -> void:
	# The blink/marquee clock rides the OS tick like the original's
	# GetTickCount gate.
	if _driver != null:
		_driver.tick(GameWorld.current_frame_clock_ms())
	# The one model runtime-frame driver outside a live mission: menu portraits
	# (avatar previews) are ObjectModels, which no longer self-clock. The static
	# advance is per-frame-guarded, so a mission's own driver takes precedence.
	ObjectModel.advance_awake_frame(delta)
	_consume_expansion_reload_request()


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


# Install a companion that owns game-specific menus the generic shell does not handle
# (the JO multiplayer menu — mp_menu_companion.gd; the PLAYER_INFO screen). The shell can
# drive several: companions are tried in install order, and the first whose
# owns_menu() claims the loaded menu drives it (see _wire_named_controls).
func add_companion(companion: MenuCompanion) -> void:
	if companion != null and not _companions.has(companion):
		_companions.append(companion)


# Build the shell against a resource root and open the main menu. Idempotent on
# the asset/driver wiring (only assembled once). The menu's re-entry after a
# mission (leave_menu_mode at its start, the same root) shows again the screen
# the mission was started from, the screen history under it kept (the driver's
# return_to_menu_mode; D-MNU-28 in docs/mnu/menu-re.md); any other entry opens
# the main menu's first screen with an empty history. A different root (the
# bundled placeholder menu handing over to a picked retail install, ADR 0048)
# reloads everything read from the root. Returns false when the main menu
# cannot be resolved/loaded (an empty/incomplete resource dir).
func setup(root: ResourceRoot) -> bool:
	var root_changed := root != _root
	var reentry := _driver != null and not root_changed and not _menu_mode
	_root = root
	if _driver == null:
		_assemble_assets()
	elif root_changed:
		_load_root_assets()
	_enter_menu_music()
	_in_game = false
	_menu_mode = true
	# Menu-mode enter (fresh boot AND return-from-game) recreates the
	# backdrop slots [orig: Menu_InitShellResources @ 0x552500 calls
	# UI_CreateMenuBinkVideos on both branches].
	_refresh_underlay()
	if not reentry:
		_driver.clear_screen_history()
		return open_menu(main_menu_file, "")
	var row := _driver.return_to_menu_mode()
	if row.size() == 2 and open_menu(row[0], row[1]):
		return true
	return open_menu(main_menu_file, "")


## A mission's start leaves the menu: the screen it was started from is kept on
## the screen history, under a mark, for the re-entry's setup() to show again.
## Once per stay in the menu (a restart or a map change starts no new mark).
func leave_menu_mode() -> void:
	if not _menu_mode or _driver == null:
		return
	_menu_mode = false
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
	# playback is the MenuAudio device leg. The SBF stays on the music
	# director only; it is not the menu's SFX source.
	_audio = MenuAudio.new()
	_audio.name = "MenuAudio"
	add_child(_audio)
	_load_root_assets()

	_driver = MenuDriver.new()
	_driver.attach(_frame, _audio)
	# The one music context lives on the MusicService autoload (the original
	# streams one AudioVM context at a time); the driver pushes each screen's
	# MUSICVAR into its director at the menumus discriminator index.
	_driver.set_music_director(MusicService.director())
	_driver.set_music_var_index(MusicDirector.MENU_MUSIC_VAR_SLOT)

	# Connect once on the persistent driver (screens reconfigure under it;
	# these aggregate signals survive).
	_driver.screen_changed.connect(_on_screen_changed)
	_driver.menu_requested.connect(_on_menu_requested)
	_driver.pop_requested.connect(_on_pop_requested)
	_driver.widget_value_changed.connect(_on_widget_value_changed)
	_driver.url_requested.connect(_on_url_requested)
	_driver.widget_activated.connect(_on_widget_activated)
	_driver.list_activated.connect(_on_list_activated)
	_driver.sound_requested.connect(_on_sound_requested)
	if _player_options == null:
		_player_options = PlayerOptions.new()
	_options_controller = OptionsMenuController.new()
	_options_controller.setup(_driver, _player_options)
	set_process(true)


# Everything the shell reads from its resource root: the cached documents and
# expansion descriptions, the text tables, the stylesheet and the menu SFX
# profile.
func _load_root_assets() -> void:
	_menu_cache.clear()
	_expansion_descriptions.clear()
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
	_sound_profile = _load_sound_profile(_discover_name(menu_sound_profile_file, ".lwf", "menu"))
	_audio.set_resource_root(_root)
	_audio.set_sound_profile(_sound_profile)


# --- Input routing (the frame is a passive surface; the shell samples) --------

func _gui_input(event: InputEvent) -> void:
	if _driver == null or not visible:
		return
	if _options_controller != null and _options_controller.consume_input(event):
		accept_event()
		return
	if event is InputEventMouseMotion:
		var motion := event as InputEventMouseMotion
		_driver.process_mouse(motion.position,
				(motion.button_mask & MOUSE_BUTTON_MASK_LEFT) != 0)
	elif event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.button_index == MOUSE_BUTTON_LEFT:
			_driver.process_mouse(button.position, button.pressed)
			accept_event()
		elif button.pressed and (button.button_index == MOUSE_BUTTON_WHEEL_DOWN \
				or button.button_index == MOUSE_BUTTON_WHEEL_UP):
			# One notch = one row tick (D-MNU-18 deliberate divergence).
			if _driver.process_wheel(button.position,
					1 if button.button_index == MOUSE_BUTTON_WHEEL_DOWN else -1):
				accept_event()


func _unhandled_key_input(event: InputEvent) -> void:
	# A backgrounded menu must not steal Esc from the world.
	if _driver == null or not is_visible_in_tree():
		return
	if _options_controller != null and _options_controller.consume_input(event):
		get_viewport().set_input_as_handled()
		return
	if event is InputEventKey and _driver.handle_key_input(event as InputEventKey):
		get_viewport().set_input_as_handled()


# --- Menu loading + navigation ------------------------------------------------

# Load a .mnu (cached) and show it, optionally jumping to target_screen. Fails
# soft (logs, keeps the current screen) when the file can't be resolved, so a
# partial resource dir does not crash the shell on a cross-.mnu jump.
func open_menu(file: String, target_screen: String) -> bool:
	var doc := _load_doc(file)
	if doc == null:
		push_warning("MenuShell: could not load menu '%s'" % file)
		return false
	_current_file = file
	_driver.clear_selected_mission()
	# Shipped same-file screen jumps name their own file (mp.mnu does); the
	# driver routes them as in-menu navigation by comparing against this
	# basename.
	if not _driver.open_document(doc, _root, _style, Strings.get_override_table(),
			file.get_file(), target_screen):
		push_warning("MenuShell: menu '%s' has no screens" % file)
		# The driver may already have swapped its document: nothing parked on
		# the shared frame by the previous companion can be rebuilt now.
		if _wired_companion != null:
			_wired_companion.on_menu_released()
			_wired_companion = null
		return false
	if _options_controller != null:
		_options_controller.prepare_document()
	_wire_named_controls()
	return true


func show_menu() -> void:
	visible = true
	set_process(_driver != null)


func hide_menu() -> void:
	visible = false
	set_process(false)


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
		_frame.configure(null, "", null, null, null)


# Marks that the menu is now the in-game/pause overlay (a kept-loaded world sits
# behind it), so the top-level back/quit resumes play instead of exiting.
func open_ingame_menu() -> bool:
	_in_game = true
	# The in-game screens' rows from an earlier visit go, as the original's close
	# of an in-game screen drops them (engine MenuRuntime::trim_screen_history);
	# the screen the mission was started from stays under its mark, which a back
	# here never pops past.
	if _driver != null:
		_driver.trim_screen_history()
	# Menu movies never tick in-game [orig: BinkVideo_UpdateAllSlots
	# @ 0x5676f0 has exactly one caller, Menu_RenderFrame].
	if _underlay != null:
		_underlay.stop()
	return open_menu(ingame_menu_file, "")


# (Re)create the backdrop movie slots from the live root + expansion. The
# in-game overlay never shows them.
func _refresh_underlay() -> void:
	if _underlay == null:
		return
	if _in_game or _root == null:
		_underlay.stop()
		return
	_underlay.set_source(_root.get_root_dir(), _current_expansion())


func _on_screen_changed(screen_name: String) -> void:
	if _underlay != null:
		_underlay.set_screen(screen_name)


# After each open, resolve the launch/quit controls and seed mission/mod lists.
# The driver keeps per-document widget state, so this rescans names against the
# freshly opened document.
#
# The "OK" control (named ACCEPT in JO) is overloaded: it launches on a play screen
# but is a plain confirm on Options/loadout/etc. The original engine dispatches it
# per-screen (CUIScene_RegisterControlCallback @0x63c060 registers callbacks keyed by screen+control), so the same
# ACCEPT means different things on different screens. We can't hardcode JO's screen
# names (this shell is game-agnostic), so we scope by the screen's ROLE inferred from
# its content: a mission list -> launch screen (ACCEPT/START_GAME launch the mission);
# else a mod list -> Mods screen (ACCEPT applies the highlighted expansion); else the
# start controls are left to the menu's own actions. Binding them globally is what
# made OK on Options launch the first mission.
func _wire_named_controls() -> void:
	_named_handlers.clear()
	_driver.clear_mission_rows()
	_driver.set_mission_controls(sp_mission_list_names, briefing_pane_names,
			sp_accept_control_names)

	# A companion (e.g. the multiplayer menu driver, or the PLAYER_INFO character screen)
	# can own a whole menu: when one claims this one, hand it the named-control wiring and
	# skip the generic launch/mission wiring, so e.g. START_GAME means "host a game" rather
	# than "launch the first mission". The first claimant wins. The previously
	# wired companion is RELEASED when it loses the document: mounts it parked
	# on the persistent frame (the PLAYER_INFO avatar preview) would otherwise
	# outlive their screen, since no later on_menu_built reaches it.
	var claimant: MenuCompanion = null
	for companion in _companions:
		if companion != null and companion.owns_menu(_driver):
			claimant = companion
			break
	if _wired_companion != null and _wired_companion != claimant:
		_wired_companion.on_menu_released()
	_wired_companion = claimant
	if claimant != null:
		claimant.on_menu_built(_driver, _current_file,
				_driver.get_current_screen(), _root)
		return
	var has_mission_list := false
	for list_name in mission_list_names:
		var id := _driver.widget_id(list_name)
		if id >= 0 and _driver.widget_kind_of(id) in _LIST_KINDS:
			has_mission_list = true
			_seed_mission_list(id)
	var has_mod_list := false
	for mod_name in mod_list_names:
		var id := _driver.widget_id(mod_name)
		if id >= 0 and _driver.widget_kind_of(id) in _LIST_KINDS:
			has_mod_list = true
			_seed_mod_list(id)

	if has_mission_list:
		_connect_named(start_control_names, _on_start_control)
	elif has_mod_list:
		_connect_named(start_control_names, _on_apply_selected_mod)
	_connect_named(exit_control_names, _on_exit_control)
	_connect_named(return_control_names, _on_return_control)
	_connect_named(restart_control_names, _on_restart_control)
	_connect_named(novaworld_control_names, _on_novaworld_control)
	_connect_named(back_control_names, _on_quit_requested)


const _LIST_KINDS := [MnuDocument.TYPE_LIST, MnuDocument.TYPE_LAN_LIST]


func _connect_named(names: PackedStringArray, handler: Callable) -> void:
	for n in names:
		if _driver.has_widget(n) and not _named_handlers.has(n.to_upper()):
			_named_handlers[n.to_upper()] = handler


func _on_widget_activated(_id: int, widget_name: String) -> void:
	var handler: Callable = _named_handlers.get(widget_name.to_upper(), Callable())
	if handler.is_valid():
		handler.call()


# The catalog is the resource seam; filtering, selection and ACCEPT live in the engine.
func _seed_mission_list(id: int) -> void:
	_driver.seed_mission_list(id, MissionCatalog.rows(_root))


func _on_sound_requested(file: String, trigger: String) -> void:
	_sound_count += 1
	_sounds_played.append({"seq": _sound_count, "file": file, "trigger": trigger})
	if _sounds_played.size() > 16:
		_sounds_played.pop_front()


func _on_list_activated(id: int, row: int) -> void:
	# Double-click activation: launch on the mission list, mount on the mod
	# list (the ItemList item_activated flows).
	var widget_name := _driver.widget_name_of(id)
	if _is_mission_list(widget_name):
		_driver.activate_mission(id, row)
		_on_start_control()
	elif _is_mod_list(widget_name):
		if row >= 0 and row < _driver.item_count(id):
			_apply_expansion(_driver.item_text(id, row))


# --- Expansion / mod selection (Options -> Mods) ------------------------------

# Fill a mod list with the expansions discoverable under the resource root, mirror
# the persisted current selection, and wire activation. list_expansions scans
# <root>/expansion/<name>/<name>.pff and is independent of the mounted root.
func _seed_mod_list(id: int) -> void:
	if _root == null:
		return
	var expansions := _root.list_expansions(_root.get_root_dir())
	_driver.set_widget_items(id, expansions)
	var current := _current_expansion()
	var sel := expansions.find(current)
	if sel >= 0:
		_driver.select_row(id, sel, false)
	_update_mod_desc(current if sel >= 0 else "")


# OK/ACCEPT on a Mods screen: mount + persist the highlighted expansion rather than
# launching a mission. Wired (instead of the launch handler) by _wire_named_controls
# when the screen has a mod list but no mission list. Reads the driver's live
# selection, so it also covers the entry _seed_mod_list pre-selected. A no-op
# when nothing is highlighted or it is already the current mod.
func _on_apply_selected_mod() -> void:
	var id := _find_mod_list()
	if id < 0:
		return
	var idx := _driver.selected_row(id)
	if idx >= 0 and idx < _driver.item_count(id):
		_apply_expansion(_driver.item_text(id, idx))


# The engine queues the pick; the shell applies its resource request at the frame tail.
func _apply_expansion(name: String) -> void:
	if _root == null:
		return
	if not _driver.request_expansion(name, _current_expansion(), _root.is_runtime_mount()):
		push_warning("MenuShell: expansions need a packed game install; the loose mount stands")


func has_pending_expansion_reload() -> bool:
	return _driver != null and _driver.has_pending_expansion_reload()


func _consume_expansion_reload_request() -> void:
	if _driver == null:
		return
	var pending := _driver.take_expansion_reload()
	if not pending.is_empty():
		_mount_expansion(pending)


# Mount the chosen expansion onto the live root, refresh the content that depends on
# it, and persist the choice. The persisted key is read at the next launch/world load
# by main_game.gd, so the selection affects gameplay too. A failed mount clears the
# root, so the previous expansion is re-mounted to recover.
func _mount_expansion(name: String) -> void:
	if _root == null:
		return
	var dir := _root.get_root_dir()
	var prev := _current_expansion()
	# A full context reload clears the AudioVM globals. Preserve the active
	# screen selector so the expansion's newly selected M<n> script enters the
	# same menu section [orig: Expansion_ReloadAllAssets @ 0x568370 followed by
	# UI_DispatchScreenEvent @ 0x54e6a0 -> AudioVM_SetVariable(slot, MUSICVAR);
	# the slot witness lives at the engine home, audio/music_policy.h
	# kMenuMusicVarSlot].
	var active_music_var := MusicService.get_var(MusicDirector.MENU_MUSIC_VAR_SLOT)
	if _root.mount_runtime(dir, name, LaunchFlags.loose_override_enabled()) != OK:
		push_warning("MenuShell: could not mount expansion '%s': %s" % [name, _root.get_last_error()])
		_root.mount_runtime(dir, prev, LaunchFlags.loose_override_enabled())  # rollback
		return
	ResourceDirSettings.set_expansion(name)
	_selected_expansion = name
	_enter_menu_music()
	MusicService.set_var(MusicDirector.MENU_MUSIC_VAR_SLOT, active_music_var)
	_refresh_dependent_content()
	_update_mod_desc(name)
	# The expansion's movie overrides take effect with the remount [orig:
	# UI_CreateMenuBinkVideos @ 0x54b590 expansion preference].
	_refresh_underlay()


# After a mount change, re-fill anything seeded from the resource dir so the
# expansion's maps/missions appear; the prior mission pick is now stale.
func _refresh_dependent_content() -> void:
	_driver.clear_selected_mission()
	for list_name in mission_list_names:
		var id := _driver.widget_id(list_name)
		if id >= 0 and _driver.widget_kind_of(id) in _LIST_KINDS:
			_seed_mission_list(id)


func _update_mod_desc(name: String) -> void:
	var desc := _find_mod_desc()
	if desc >= 0:
		_driver.set_widget_text(desc, _describe(name))


# The expansion's own name and description, read by the engine the way the
# retail scan reads them (ResourceRoot.expansion_name / expansion_description:
# <n>.bin's EXP_NAME / EXP_DESC with their fallbacks, resolved independently
# of the mounted stack, so a not-yet-mounted expansion previews the same way).
func _describe(name: String) -> String:
	if name.is_empty() or _root == null:
		return ""
	if not _expansion_descriptions.has(name):
		var dir := _root.get_root_dir()
		_expansion_descriptions[name] = "%s\n\n%s" % [
				_root.expansion_name(dir, name), _root.expansion_description(dir, name)]
	return _expansion_descriptions[name]


func _current_expansion() -> String:
	return ResourceDirSettings.get_expansion()


# --- Driver signal handlers ---------------------------------------------------

func _on_menu_requested(file: String, target_screen: String) -> void:
	# Cross-.mnu forward jump: remember where we are so the back stack can return.
	# A file that does not load, or a target screen it does not hold, changes
	# nothing: retail's select finds no such screen and keeps the current one,
	# its history untouched (docs/mnu/menu-re.md, "Activation and the ACTION
	# walk").
	var doc := _load_doc(file)
	if doc == null:
		push_warning("MenuShell: could not load menu '%s'" % file)
		return
	if not target_screen.is_empty() and not _doc_has_screen(doc, target_screen):
		push_warning("MenuShell: menu '%s' has no screen '%s'" % [file, target_screen])
		return
	var previous_file := _current_file
	var previous_screen := _driver.get_current_screen()
	if open_menu(file, target_screen):
		_driver.push_screen_history(previous_file, previous_screen)


func _doc_has_screen(doc: MnuDocument, screen_name: String) -> bool:
	for screen_id in doc.get_screen_ids():
		if doc.get_screen_name(screen_id).nocasecmp_to(screen_name) == 0:
			return true
	return false


# POP_SCREEN with the document's own history empty: the previous file's screen,
# the one history retail keeps across every loaded file; with none, nothing
# happens (retail pops only a non-empty history; docs/mnu/menu-re.md).
func _on_pop_requested() -> void:
	# In a mission the history's top is the mark on the screen the mission was
	# started from, which pops nothing (D-MNU-28).
	var prev := _driver.pop_screen_history()
	if prev.size() == 2:
		open_menu(prev[0] if not prev[0].is_empty() else main_menu_file, prev[1])


# The named back/quit Command (HIDDEN_BACK, the retail per-control callback):
# cross-.mnu back first, then resume (in-game) or exit to the desktop.
func _on_quit_requested() -> void:
	# Top-level back/quit. Cross-.mnu back first; then it means resume (in-game)
	# or exit-to-desktop (main menu). In a mission the history's top is the mark
	# on the screen the mission was started from, which pops nothing.
	var prev := _driver.pop_screen_history()
	if prev.size() == 2:
		open_menu(prev[0] if not prev[0].is_empty() else main_menu_file, prev[1])
	elif _in_game:
		resume_requested.emit()
	else:
		exit_to_desktop_requested.emit()


func _on_widget_value_changed(widget_name: String, kind: String, index: int, value: String) -> void:
	if kind == "list" and _is_mission_list(widget_name):
		_driver.select_mission(_driver.widget_id(widget_name), index, value)
	elif kind == "list" and _is_mod_list(widget_name):
		# Single click previews the description; activation (double-click) mounts it.
		_update_mod_desc(value)


func _on_url_requested(url: String, external: bool) -> void:
	# Shipped menus open website/marketing links (e.g. the splash PREORDER button)
	# via <ACTION type="URL" EXTERNAL_BROWSER>. Hand them to the OS browser, adding
	# a scheme if the authored link is bare (e.g. "www.novalogic.com/..."). A URL
	# without the external browser is retail's in-game page fetch (docs/mnu/
	# menu-re.md), which the shell does not host: it opens nothing.
	if url.is_empty() or not external:
		return
	var target := url
	if not (target.begins_with("http://") or target.begins_with("https://")):
		target = "https://" + target
	OS.shell_open(target)


# --- Named-control handlers (shell policy: launch / quit by control name) -------

func _on_start_control() -> void:
	# The retail ACCEPT handler launches only through a SHOWN mission list's
	# current entry (or the IA_LIST's selected value); with nothing selected it
	# does nothing (docs/mnu/menu-re.md, SINGLE_PLAYER). No catalog fallback.
	var mission := get_selected_mission()
	if mission.is_empty():
		push_warning("MenuShell: start pressed with no mission selected")
		return
	start_requested.emit(mission)


func _on_exit_control() -> void:
	exit_to_desktop_requested.emit()


func _on_return_control() -> void:
	# CONFIRM_YES is registered per (screen, control) in retail — the INGAME,
	# STAT and DEATH screens each bind their own exit handler
	# (docs/mnu/menu-re.md "The in-game exit confirmation"). The shell binds
	# the name, so it answers only while a mission is running; a front-end
	# document that happened to author the name never leaves a mission that
	# is not there.
	if not _in_game:
		return
	return_to_menu_requested.emit()


# RESTART is the in-game screen's own command; like CONFIRM_YES it answers only
# while a mission is running.
func _on_restart_control() -> void:
	if not _in_game:
		return
	restart_requested.emit()


func _on_novaworld_control() -> void:
	novaworld_requested.emit()


# --- Audio --------------------------------------------------------------------

# Open the MENU music context on the shared MusicService (the original opens
# it once at boot [orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60] and re-opens
# it when the front end returns; the GAME context is the world's to open at
# mission start [orig: Game_StartMission @ 0x525598]).
func _enter_menu_music() -> void:
	MusicService.open_menu_context(_root, menu_music_file, menu_sound_bank_file)


# --- Asset resolution helpers (all best-effort, degrade to null) --------------

# Resolve an explicit file, else discover one by extension (preferring a name
# containing `prefer`). Returns the winning entry's logical basename (loadable
# through the VFS by name via _root.read_file) rather than a loose disk path, so
# discovery works for PFF-archived assets too.
func _discover_name(explicit: String, suffix: String, prefer: String) -> String:
	if not explicit.is_empty():
		return explicit
	if _root == null:
		return ""
	var files := _root.list_files(suffix)
	if files.is_empty():
		return ""
	if not prefer.is_empty():
		for f in files:
			if String(f).get_file().to_lower().contains(prefer):
				return String(f).get_file()
	return String(files[0]).get_file()


# The witnessed music-pair resolution (engine-derived names via
# MusicDirector.resolve_*_music_pair + the VFS/loose fallback orchestration)
# lives on MusicService; these seams keep it queryable against the
# shell's root (ADR 0018 — tests and diagnostics read it here, not the
# privates).
func resolve_menu_music_pair() -> MusicPair:
	return MusicService.resolve_menu_music_pair(_root)


func resolve_game_music_pair() -> MusicPair:
	return MusicService.resolve_game_music_pair(_root)


# The visual menu assets (.mnu document, .mns stylesheet, RTXT text) load through
# the VFS by name so they resolve from PFF archives at runtime; menu textures and
# fonts resolve through the resource root the frame is given.
func _load_doc(file: String) -> MnuDocument:
	if _menu_cache.has(file):
		return _menu_cache[file]
	if _root == null or file.is_empty():
		return null
	var bytes := _root.read_file(file)
	if bytes.is_empty():
		return null
	var doc := MnuDocument.new()
	if doc.load_from_bytes(bytes) != OK:
		return null
	_menu_cache[file] = doc
	return doc


func _load_text(file: String) -> RtxtStringFile:
	if file.is_empty():
		return null
	return Strings.load_rtxt(_root, file)


# The menu SFX profile (menu.lwf) loads by name through the VFS so it resolves
# from PFF archives too; its members point at loose .wav files resolved on
# demand. Degrades to null (silent menu SFX) when absent.
func _load_sound_profile(name: String) -> LwfData:
	if _root == null or name.is_empty():
		return null
	var d := LwfData.new()
	if d.open_from_resource_root(_root, name) != OK:
		return null
	return d if d.is_loaded() and d.get_set_count() > 0 else null


# --- Misc helpers / accessors -------------------------------------------------

func _is_mission_list(widget_name: String) -> bool:
	for n in mission_list_names:
		if n.nocasecmp_to(widget_name) == 0:
			return true
	return false


func _is_mod_list(widget_name: String) -> bool:
	for n in mod_list_names:
		if n.nocasecmp_to(widget_name) == 0:
			return true
	return false


func _find_mod_list() -> int:
	for n in mod_list_names:
		var id := _driver.widget_id(n)
		if id >= 0 and _driver.widget_kind_of(id) in _LIST_KINDS:
			return id
	return -1


# MOD_DESC is authored MULTI_EDIT (readonly); the compiled path wraps its text.
func _find_mod_desc() -> int:
	for n in mod_desc_names:
		var id := _driver.widget_id(n)
		if id >= 0:
			return id
	return -1


# Accessors for owners / tests.
func get_driver() -> MenuDriver:
	return _driver


func get_frame() -> MenuFrame:
	return _frame


func get_stylesheet() -> MnsStyleSheet:
	return _style


func get_resource_root() -> ResourceRoot:
	return _root


func get_current_menu_file() -> String:
	return _current_file


func get_selected_mission() -> String:
	return _driver.get_selected_mission() if _driver != null else ""


func get_selected_expansion() -> String:
	return _selected_expansion


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
		"file": _current_file,
		"screen": _driver.get_current_screen(),
		"screens": _driver.get_screen_names(),
		"in_game": _in_game,
		"stack_depth": _driver.get_screen_history_depth(),
		"sounds": _sounds_played.duplicate(true),
		"underlay": {
			"active_slots": _underlay.get_active_slot_count() if _underlay != null else 0,
			"startup_layout": _underlay.is_startup_layout() if _underlay != null else false,
			"failed": _underlay.get_failed_count() if _underlay != null else 0,
		},
	}
	if include_widgets:
		var rows: Array[Dictionary] = []
		for i in _frame.widget_count():
			var rect := _frame.widget_rect(i)
			rows.append({
				"index": i,
				"name": _frame.widget_name(i),
				"kind": _frame.widget_kind(i),
				"disabled": _frame.is_widget_disabled(i),
				"rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y],
				"text": _frame.get_widget_text(i),
				"items": _frame.item_count(i),
			})
		snapshot["widgets"] = rows
	return snapshot


func _design_to_local(design_pos: Vector2) -> Vector2:
	var size := _frame.get_size()
	return Vector2(design_pos.x * size.x / MenuFrame.DESIGN_WIDTH,
			design_pos.y * size.y / MenuFrame.DESIGN_HEIGHT)


# Press+release through the real pump at the widget's design-rect center;
# click activation follows the pump's claim rules exactly. Resolution is
# frame-side (pre-order index) — the driver's doc-id space is a DIFFERENT
# addressing and must not index frame rects.
func menu_press(widget_name: String) -> bool:
	if _driver == null or _frame == null or not is_visible_in_tree():
		return false
	for i in _frame.widget_count():
		if _frame.widget_name(i) != widget_name or _frame.is_widget_disabled(i):
			continue
		var local := _design_to_local(_frame.widget_rect(i).get_center())
		_driver.process_mouse(local, true)
		_driver.process_mouse(local, false)
		return true
	return false


# One raw pump sample at design coords with the button held or not, as a
# motion is: the claim's hover and the window sounds it plays. Returns the hit
# widget index (-1 for none).
func menu_move_at(design_pos: Vector2, down: bool) -> int:
	if _driver == null or _frame == null or not is_visible_in_tree():
		return -1
	var local := _design_to_local(design_pos)
	var hit := _frame.hit_test(local)
	_driver.process_mouse(local, down)
	return hit


# Raw pump press+release at design coords (list rows, combo popups, spin
# arrows). Returns the hit widget index (-1 for none).
func menu_press_at(design_pos: Vector2) -> int:
	if _driver == null or _frame == null or not is_visible_in_tree():
		return -1
	var local := _design_to_local(design_pos)
	var hit := _frame.hit_test(local)
	_driver.process_mouse(local, true)
	_driver.process_mouse(local, false)
	return hit


# A REAL left click at design coords: press + release fed through Godot's input
# dispatch (mouse filters, companion mounts, GUI focus all apply), unlike the
# pump-direct press ops -- the seam that observes whether a mounted Control
# steals a click from the frame. The events are buffered and land on the next
# frame; callers re-read the menu state after a frame.
func menu_click_at(design_pos: Vector2) -> bool:
	if _driver == null or _frame == null or not is_visible_in_tree():
		return false
	var window_pos := get_global_transform_with_canvas() * _design_to_local(design_pos)
	for pressed in [true, false]:
		var click := InputEventMouseButton.new()
		click.button_index = MOUSE_BUTTON_LEFT
		click.pressed = pressed
		click.position = window_pos
		click.global_position = window_pos
		Input.parse_input_event(click)
	return true


func menu_key(keycode: int, unicode: int = 0) -> bool:
	# The same guard as the real input paths: a hidden menu (a world is up)
	# must not receive synthetic menu input either.
	if _driver == null or not is_visible_in_tree():
		return false
	var ev := InputEventKey.new()
	ev.keycode = keycode as Key
	ev.physical_keycode = keycode as Key
	ev.unicode = unicode
	ev.pressed = true
	# Same order as the real path: an armed remap capture consumes keys first.
	if _options_controller != null and _options_controller.consume_input(ev):
		return true
	return _driver.handle_key_input(ev)


func menu_show_screen(name: String) -> bool:
	if _driver == null:
		return false
	return _driver.show_screen(name)
