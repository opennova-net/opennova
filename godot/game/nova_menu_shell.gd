class_name NovaMenuShell
extends Control

# Runtime menu shell: drives a live NovaMnuMenu (the same engine node the ONED
# Menus workspace previews, here with edit_mode off so it is fully interactive),
# loading the game's .mnu menu set + audio from the user's resource directory.
# Music streams through the shared NovaMusicService autoload (one context at a
# time, like the original AudioVM): the shell opens the MENU context; GameWorld
# opens the GAME context at mission start. It is the runtime counterpart to
# GameWorld: GameWorld turns a resource dir into a playable world, this turns it
# into the playable menu front-end, and main_game.gd hands off between the two.
#
# The menu itself owns intra-.mnu navigation, window show/hide, the back stack,
# and per-screen music (it pushes each screen's MUSICVAR into the director). The
# shell services the policy the menu leaves to it: cross-.mnu file jumps
# (menu_requested), quit (quit_requested), and the gameplay launch. Shipped JO
# menus carry no "launch" action verb; the engine wires those by well-known
# control NAME (START_GAME, ACCEPT, EXIT, ...), so the shell scans the built tree
# for those names and connects them. The control-name sets are exported so a
# different game's menu set can be pointed at the same shell.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

# Var index the director sets to the current screen's MUSICVAR. The menumus MUS
# script reads its section discriminator at var INDEX 2 (golden test
# tests/mus/mus_vm_test.cpp drives "jo_menumus.bin" via var 2; gamemus uses var 1),
# so the screen MUSICVAR must land at var2 — at index 0 it was inert and the VM
# always ran the var2=0 path (P1,P2 then a P0 loop) instead of the screen's section
# (the main menu's MUSICVAR=1 selects the P2..P8 theme). The original stores the
# active screen's MUSICVAR to Var2 on every screen event [orig:
# UI_DispatchScreenEvent @ 0x54e6a0, store @ 0x54eff4 -> AudioVM_SetVariable(2, v)].
# setup() pushes it synchronously (open_menu rebuilds in-tree) before the
# director's first _process tick, so the VM starts in the right section.
const MUSIC_VAR_INDEX := 2

# Menus are authored in a fixed 800x600 virtual design space and scaled to the
# screen by independent X/Y factors (anamorphic fill, no letterbox, origin 0,0):
# the original computes scaleX = screenW/800, scaleY = screenH/600 and applies it
# to every widget rect at draw [orig: CUIScene_SetScreenScale @ 0x639480, constants
# 0.00125 = 1/800 and 0.0016666667 = 1/600; recomputed on resolution change in
# apply_video_mode_change @ 0x55a590]. We reproduce it by scaling the menu root
# CanvasItem; authored coords stay in 800x600 space.
const DESIGN_SIZE := Vector2(800, 600)

# Friendly labels for known expansions. The list item + persisted key stay the raw
# folder name (e.g. "jox01"); unknown expansions display their raw folder name.
const EXPANSION_DISPLAY_NAMES := {"jox01": "Kendari"}

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
# The menu stylesheet has a fixed canonical name the original engine looks for
# ("named menu_style.mns for the game to find it"). It is usually PFF-archived;
# .mns is indexed as the "menu_style" kind (so list_files and the editor's
# browsers surface it), but the engine contract stays the canonical NAME loaded
# through the VFS. A blank value falls back to the first .mns found.
@export var menu_stylesheet_file := "menu_style.mns"
# Interactive music: the engine hardcodes two bank+script pairs -- MENUMUS.SBF/.BIN
# (menu) and GAMEMUS.SBF/.BIN (game), renamed to M<n>/G<n> forms when expansion <n>
# is active [orig: Expansion_LoadAssets @ 0x4a4798]. Blank = that witnessed
# resolution (see resolve_music_pair); an explicit value wins (loose dev override).
@export var menu_sound_bank_file := ""   # "" -> MENUMUS.SBF (M<n>.sbf under an expansion)
# Menu SFX profile: the .lwf the widgets' <SOUND> elements reference (hover/click).
# "" -> a .lwf whose name contains "menu" (i.e. menu.lwf), else the first .lwf found.
@export var menu_sound_profile_file := ""
@export var menu_music_file := ""        # "" -> MENUMUS.BIN (M<n>.bin under an expansion)

# Well-known control names (the JO "wired by convention" launch/quit controls).
# A button found by one of these names gets its `pressed` connected to the shell.
@export var start_control_names := PackedStringArray([
	"START_GAME", "ACCEPT", "LAUNCH", "GO", "HOST_GAME", "LAN_HOSTGAME",
])
@export var exit_control_names := PackedStringArray([
	"EXIT", "QUIT", "QUIT_GAME", "QUIT_TO_DESKTOP",
])
@export var return_control_names := PackedStringArray([
	"QUIT_TO_MENU", "MAIN_MENU", "ABORT", "ABORT_MISSION",
])
# List widgets the shell fills with the resource dir's missions (.bms).
@export var mission_list_names := PackedStringArray([
	"MISSION_LIST", "MISSIONLIST", "MISSIONS", "IA_LIST", "MAP_LIST",
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
# The Options -> Controls key-binding table, and the device radios that switch it
# (Keyboard/Mouse/Joystick). The shell fills the table from the libs/controls catalog.
@export var control_table_names := PackedStringArray([
	"CONTROL_MAPPING",
])
@export var control_device_names := PackedStringArray([
	"KEYBOARD", "MOUSE", "JOYSTICK",
])
# Spin lists that select the retail crosshair art (cross01.tga through cross25.tga).
@export var crosshair_style_control_names := PackedStringArray([
	"XHAIR_APPEARANCE",
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
signal resume_requested()
# The player chose NovaWorld (online multiplayer) from the menu. main_game
# opens the NovaWorld panel; the shell stays out of the networking itself.
signal novaworld_requested()
# Emitted after the Options spin list changes so an active HUD can reload its art.
signal crosshair_style_changed(style: int)

var _menu: NovaMnuMenu
var _root: NovaResourceRoot
var _text: RtxtStringFile
var _style: MnsStyleSheet
var _sound_profile: NovaLwfData

var _menu_cache: Dictionary = {}            # filename -> NovaMnuDocument
var _menu_stack: Array[Dictionary] = []     # [{file, screen}] cross-.mnu back stack
var _current_file := ""
var _selected_mission := ""
var _selected_expansion := ""
var _in_game := false
var _ready_done := false
# Optional delegates that own game-specific menus the generic shell does not handle
# (the JO multiplayer menu — mp_menu_companion.gd; the PLAYER_INFO character screen —
# player_info_menu_companion.gd). Empty for a plain shell. The first whose owns_menu()
# claims a loaded menu drives it; otherwise the shell's generic wiring runs.
var _companions: Array = []
# Lazily-built Options -> Controls key-binding catalog (libs/controls).
var _controls_model: NovaControlsModel = null


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_PASS
	if not resized.is_connected(_recompute_fit):
		resized.connect(_recompute_fit)


# Install a companion that owns game-specific menus the generic shell does not handle
# (the JO multiplayer menu — mp_menu_companion.gd; the PLAYER_INFO screen). The shell can
# drive several: companions are tried in install order, and the first whose
# owns_menu() claims the loaded menu drives it (see _wire_named_controls).
func add_companion(companion) -> void:
	if companion != null and not _companions.has(companion):
		_companions.append(companion)


# Build the shell against a resource root and open the main menu. Idempotent on
# the asset/menu/director wiring (only assembled once); show_menu() returns to
# the main menu on later entries. Returns false when the main menu
# cannot be resolved/loaded (an empty/incomplete resource dir).
func setup(root: NovaResourceRoot) -> bool:
	_root = root
	if _menu == null:
		_assemble_assets()
	_enter_menu_music()
	_in_game = false
	_menu_stack.clear()
	return open_menu(main_menu_file, "")


# --- Asset assembly -----------------------------------------------------------

func _assemble_assets() -> void:
	_text = _load_text(menu_text_file)
	# Register the engine text tables into the shared NovaStrings registry, the way the
	# original loads its TextResource globals: menutxt (UI/voice labels), gametext =
	# gametext.bin (g_TextGameText — the "WepDes" weapon names + in-game strings
	# [orig: Game_InitSubsystems @0x4a6cd0]), and gameui = Game.bin (the menu shell's
	# own resource: options/menu + "Avatars" sections [orig: the menu boot @0x552510
	# -> the menu resource @0x25510F8]).
	if _text != null:
		NovaStrings.register_table("menutxt", _text)
	var gametext := _load_text(game_text_file)
	if gametext != null:
		NovaStrings.register_table("gametext", gametext)
	var gameui := _load_text(menu_ui_text_file)
	if gameui != null:
		NovaStrings.register_table("gameui", gameui)
	_style = _load_style(_discover_name(menu_stylesheet_file, ".mns", ""))
	_sound_profile = _load_sound_profile(_discover_name(menu_sound_profile_file, ".lwf", "menu"))

	_menu = NovaMnuMenu.new()
	_menu.name = "Menu"
	_menu.build_on_ready = false
	_menu.set_edit_mode(false)
	_menu.set_resource_root(_root)
	if _style != null:
		_menu.set_stylesheet(_style)
	if _text != null:
		_menu.set_text_resource(_text)
	# Menu hover/click SFX come from the .lwf profile (set-by-trigger -> .wav). The
	# SBF stays on the music director only; it is not the menu's SFX source.
	if _sound_profile != null:
		_menu.set_sound_profile(_sound_profile)
	# The one music context lives on the NovaMusicService autoload (the original
	# streams one AudioVM context at a time); the menu pushes each screen's
	# MUSICVAR into its director at the menumus discriminator index.
	_menu.set_music_director(NovaMusicService.director())
	_menu.set_music_var_index(MUSIC_VAR_INDEX)
	# Pin the menu root at the top-left, sized to the 800x600 design space; the
	# anamorphic scale is applied per-resize in _recompute_fit. Top-left anchors keep
	# the explicit size from being overridden, so PRESET_FULL_RECT screens fill 800x600.
	_menu.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_menu.size = DESIGN_SIZE
	add_child(_menu)

	# Connect once on the persistent menu node (the child screen tree is rebuilt
	# per open_menu; these aggregate signals survive the rebuilds).
	_menu.menu_requested.connect(_on_menu_requested)
	_menu.quit_requested.connect(_on_quit_requested)
	_menu.widget_value_changed.connect(_on_widget_value_changed)
	_menu.action_dispatched.connect(_on_action_dispatched)
	_menu.url_requested.connect(_on_url_requested)


# --- Menu loading + navigation ------------------------------------------------

# Load a .mnu (cached) and show it, optionally jumping to target_screen. Fails
# soft (logs, keeps the current screen) when the file can't be resolved, so a
# partial resource dir does not crash the shell on a cross-.mnu jump.
func open_menu(file: String, target_screen: String) -> bool:
	var doc := _load_doc(file)
	if doc == null:
		push_warning("NovaMenuShell: could not load menu '%s'" % file)
		return false
	_current_file = file
	_selected_mission = ""
	# Shipped same-file screen jumps name their own file (mp.mnu does); the menu
	# routes them as in-menu navigation by comparing against its own basename.
	_menu.set_menu_file(file.get_file())
	_menu.menu = doc  # in-tree -> rebuilds synchronously, fires screen/music signals
	if not target_screen.is_empty():
		_menu.show_screen(target_screen)
	_wire_named_controls()
	_recompute_fit()
	return true


func show_menu() -> void:
	visible = true


func hide_menu() -> void:
	visible = false


# Marks that the menu is now the in-game/pause overlay (a kept-loaded world sits
# behind it), so the top-level back/quit resumes play instead of exiting.
func open_ingame_menu() -> bool:
	_in_game = true
	_menu_stack.clear()
	return open_menu(ingame_menu_file, "")


# After each (re)build, connect the launch/quit controls and seed mission/mod lists.
# The screen nodes are freshly built children, so prior connections died with the
# old tree; we just rescan.
#
# The "OK" control (named ACCEPT in JO) is overloaded: it launches on a play screen
# but is a plain confirm on Options/loadout/etc. The original engine dispatches it
# per-screen (sub_63C060 registers callbacks keyed by screen+control), so the same
# ACCEPT means different things on different screens. We can't hardcode JO's screen
# names (this shell is game-agnostic), so we scope by the screen's ROLE inferred from
# its content: a mission list -> launch screen (ACCEPT/START_GAME launch the mission);
# else a mod list -> Mods screen (ACCEPT applies the highlighted expansion); else the
# start controls are left to the menu's own actions. Binding them globally is what
# made OK on Options launch the first mission.
func _wire_named_controls() -> void:
	_seed_crosshair_style_controls()
	# A companion (e.g. the multiplayer menu driver, or the PLAYER_INFO character screen)
	# can own a whole menu: when one claims this one, hand it the named-control wiring and
	# skip the generic launch/mission wiring, so e.g. START_GAME means "host a game" rather
	# than "launch the first mission". The first claimant wins.
	for companion in _companions:
		if companion != null and companion.owns_menu(_menu):
			companion.on_menu_built(_menu, _current_file, _menu.current_screen, _root)
			return
	var has_mission_list := false
	for list_name in mission_list_names:
		var list := _menu.find_child(list_name, true, false)
		if list is NovaMnuList:
			has_mission_list = true
			_seed_mission_list(list as NovaMnuList)
	var has_mod_list := false
	for mod_name in mod_list_names:
		var mod_list := _menu.find_child(mod_name, true, false)
		if mod_list is NovaMnuList:
			has_mod_list = true
			_seed_mod_list(mod_list as NovaMnuList)
	for table_name in control_table_names:
		var ctl_table := _menu.find_child(table_name, true, false)
		if ctl_table is NovaMnuTable:
			_seed_control_mapping(ctl_table as NovaMnuTable)
	if has_mission_list:
		_connect_named(start_control_names, _on_start_control)
	elif has_mod_list:
		_connect_named(start_control_names, _on_apply_selected_mod)
	_connect_named(exit_control_names, _on_exit_control)
	_connect_named(return_control_names, _on_return_control)
	_connect_named(novaworld_control_names, _on_novaworld_control)


func _seed_crosshair_style_controls() -> void:
	var persisted := ResourceDirSettings.get_crosshair_style()
	for control_name in crosshair_style_control_names:
		var spin := _menu.find_child(control_name, true, false)
		if spin is NovaMnuSpinList:
			(spin as NovaMnuSpinList).set_value_index(persisted)


func _connect_named(names: PackedStringArray, handler: Callable) -> void:
	for n in names:
		var node := _menu.find_child(n, true, false)
		if node is BaseButton and not (node as BaseButton).pressed.is_connected(handler):
			(node as BaseButton).pressed.connect(handler)


func _seed_mission_list(list: NovaMnuList) -> void:
	list.set_items(MissionCatalog.mission_names(_root))
	if not list.item_activated.is_connected(_on_mission_activated):
		list.item_activated.connect(_on_mission_activated)


# --- Controls remap table (Options -> Controls) -------------------------------

# Fill the CONTROL_MAPPING table with the key-binding catalog and wire the
# Keyboard/Mouse/Joystick device radios to repopulate it. Read-only for now: the
# rows show the byte-exact default bindings; double-click rebinding is not wired
# (see docs/mnu/menu-re.md D-CTRL-*). The radio nodes are rebuilt with the menu, so
# the connections are re-made fresh each open without duplicating.
func _seed_control_mapping(table: NovaMnuTable) -> void:
	if _controls_model == null:
		_controls_model = NovaControlsModel.new()
	_fill_control_mapping(table, NovaControlsModel.DEVICE_KEYBOARD)
	for i in control_device_names.size():
		var radio := _menu.find_child(control_device_names[i], true, false)
		if radio is BaseButton:
			var device := i  # 0=keyboard, 1=mouse, 2=joystick (NovaControlsModel.Device)
			(radio as BaseButton).pressed.connect(func() -> void:
				_fill_control_mapping(table, device))


func _fill_control_mapping(table: NovaMnuTable, device: int) -> void:
	if _controls_model == null:
		return
	table.clear_rows()
	table.add_rows(_controls_model.get_rows(device))


# --- Expansion / mod selection (Options -> Mods) ------------------------------

# Fill a mod list with the expansions discoverable under the resource root, mirror
# the persisted current selection, and wire activation. list_expansions scans
# <root>/expansion/<name>/<name>.pff and is independent of the mounted root.
func _seed_mod_list(list: NovaMnuList) -> void:
	if _root == null:
		return
	var expansions := _root.list_expansions(_root.get_root_dir())
	list.set_items(expansions)
	var current := _current_expansion()
	var sel := expansions.find(current)
	if sel >= 0:
		list.select(sel)
	_update_mod_desc(current if sel >= 0 else "")
	if not list.item_activated.is_connected(_on_mod_activated):
		list.item_activated.connect(_on_mod_activated)


func _on_mod_activated(index: int) -> void:
	var list := _find_mod_list()
	if list == null or index < 0 or index >= list.item_count:
		return
	_apply_expansion(list.get_item_text(index))


# OK/ACCEPT on a Mods screen: mount + persist the highlighted expansion rather than
# launching a mission. Wired (instead of the launch handler) by _wire_named_controls
# when the screen has a mod list but no mission list. Reads the live ItemList
# selection (NovaMnuList extends ItemList), so it also covers the entry _seed_mod_list
# pre-selected. A no-op when nothing is highlighted or it is already the current mod.
func _on_apply_selected_mod() -> void:
	var list := _find_mod_list()
	if list == null:
		return
	var sel := list.get_selected_items()
	if sel.is_empty():
		return
	var idx := sel[0]
	if idx >= 0 and idx < list.item_count:
		_apply_expansion(list.get_item_text(idx))


# Mount the chosen expansion onto the live root, refresh the content that depends on
# it, and persist the choice. The persisted key is read at the next launch/world load
# by main_game.gd, so the selection affects gameplay too. A failed mount clears the
# root, so the previous expansion is re-mounted to recover.
func _apply_expansion(name: String) -> void:
	if _root == null or name.is_empty() or name == _current_expansion():
		return
	# Expansions layer packed archives, so only a runtime (PFF) mount can switch
	# them. A loose-root play-test mount (ADR 0025) lists no expansions to begin
	# with; this guard keeps a hand-driven selection from remounting the loose
	# root through mount_runtime and clearing it on the inevitable failure.
	if not _root.is_runtime_mount():
		push_warning("NovaMenuShell: expansions need a packed game install; the loose mount stands")
		return
	var dir := _root.get_root_dir()
	var prev := _current_expansion()
	# A full context reload clears the AudioVM globals. Preserve the active
	# screen selector so the expansion's newly selected M<n> script enters the
	# same menu section [orig: Expansion_ReloadAllAssets @ 0x568370 followed by
	# UI_DispatchScreenEvent @ 0x54e6a0 -> AudioVM_SetVariable(2, MUSICVAR)].
	var active_music_var := NovaMusicService.get_var(MUSIC_VAR_INDEX)
	if _root.mount_runtime(dir, name, NovaLaunchFlags.loose_override_enabled()) != OK:
		push_warning("NovaMenuShell: could not mount expansion '%s': %s" % [name, _root.get_last_error()])
		_root.mount_runtime(dir, prev, NovaLaunchFlags.loose_override_enabled())  # rollback
		return
	ResourceDirSettings.set_expansion(name)
	_selected_expansion = name
	_enter_menu_music()
	NovaMusicService.set_var(MUSIC_VAR_INDEX, active_music_var)
	_refresh_dependent_content()
	_update_mod_desc(name)


# After a mount change, re-fill anything seeded from the resource dir so the
# expansion's maps/missions appear; the prior mission pick is now stale.
func _refresh_dependent_content() -> void:
	_selected_mission = ""
	for list_name in mission_list_names:
		var list := _menu.find_child(list_name, true, false)
		if list is NovaMnuList:
			_seed_mission_list(list as NovaMnuList)


func _update_mod_desc(name: String) -> void:
	var desc := _find_mod_desc()
	if desc != null:
		desc.text = _describe(name)


# Names-only is all the VFS exposes today; show a friendly label when we know one,
# else the raw folder name. TODO(expansion-desc): read a description from the .pff.
func _describe(name: String) -> String:
	if name.is_empty():
		return ""
	var label := String(EXPANSION_DISPLAY_NAMES.get(name, name))
	return "%s\n\n(expansion: %s)" % [label, name]


func _current_expansion() -> String:
	return ResourceDirSettings.get_expansion()


# --- Menu signal handlers -----------------------------------------------------

func _on_menu_requested(file: String, target_screen: String) -> void:
	# Cross-.mnu forward jump: remember where we are so the back stack can return.
	var previous := {"file": _current_file, "screen": _menu.current_screen}
	if open_menu(file, target_screen):
		_menu_stack.push_back(previous)


func _on_quit_requested() -> void:
	# Top-level back/quit. Cross-.mnu back first; then it means resume (in-game)
	# or exit-to-desktop (main menu).
	if not _menu_stack.is_empty():
		var prev: Dictionary = _menu_stack.pop_back()
		open_menu(String(prev.get("file", main_menu_file)), String(prev.get("screen", "")))
	elif _in_game:
		resume_requested.emit()
	else:
		exit_to_desktop_requested.emit()


func _on_widget_value_changed(widget_name: String, kind: String, index: int, value: String) -> void:
	if kind == "spinlist" and _is_crosshair_style_control(widget_name):
		ResourceDirSettings.set_crosshair_style(index)
		crosshair_style_changed.emit(ResourceDirSettings.get_crosshair_style())
	elif kind == "list" and _is_mission_list(widget_name):
		_selected_mission = value
	elif kind == "list" and _is_mod_list(widget_name):
		# Single click previews the description; activation (double-click) mounts it.
		_update_mod_desc(value)


func _on_action_dispatched(_type: String, _target: String) -> void:
	pass  # informational; intra-menu actions are handled by the menu itself.


func _on_url_requested(url: String) -> void:
	# Shipped menus open website/marketing links (e.g. the splash PREORDER button)
	# via <ACTION type="URL">. Hand them to the OS browser, adding a scheme if the
	# authored link is bare (e.g. "www.novalogic.com/...").
	if url.is_empty():
		return
	var target := url
	if not (target.begins_with("http://") or target.begins_with("https://")):
		target = "https://" + target
	OS.shell_open(target)


# --- Named-control handlers (shell policy: launch / quit by control name) -------

func _on_start_control() -> void:
	var mission := _selected_mission
	if mission.is_empty():
		# No explicit pick: fall back to the first available mission so a menu
		# without a list (or before a selection) can still start something.
		mission = MissionCatalog.first_mission_name(_root)
	if mission.is_empty():
		push_warning("NovaMenuShell: start pressed with no mission available")
		return
	start_requested.emit(mission)


func _on_exit_control() -> void:
	exit_to_desktop_requested.emit()


func _on_return_control() -> void:
	return_to_menu_requested.emit()


func _on_novaworld_control() -> void:
	novaworld_requested.emit()


func _on_mission_activated(index: int) -> void:
	var list := _find_mission_list()
	if list != null and index >= 0 and index < list.item_count:
		_selected_mission = list.get_item_text(index)
	_on_start_control()


# --- Audio --------------------------------------------------------------------

# Open the MENU music context on the shared NovaMusicService (the original opens
# it once at boot [orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60] and re-opens
# it when the front end returns; the GAME context is the world's to open at
# mission start [orig: Game_StartMission @ 0x525598]).
func _enter_menu_music() -> void:
	NovaMusicService.open_menu_context(_root, menu_music_file, menu_sound_bank_file)


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


# The witnessed music-pair resolution (base MENUMUS/GAMEMUS names, expansion
# M<n>/G<n> forms, complete-pair-or-base fallback) lives on NovaMusicService;
# this seam keeps it queryable against the shell's root (ADR 0018 — tests and
# diagnostics read it here, not the privates).
func resolve_music_pair(prefix: String, base_stem: String) -> MusicPair:
	return NovaMusicService.resolve_music_pair(_root, prefix, base_stem)


# The visual menu assets (.mnu document, .mns stylesheet, RTXT text) load through
# the VFS by name so they resolve from PFF archives at runtime; menu textures and
# fonts resolve through the resource root the menu is given (set_resource_root).
func _load_doc(file: String) -> NovaMnuDocument:
	if _menu_cache.has(file):
		return _menu_cache[file]
	if _root == null or file.is_empty():
		return null
	var bytes := _root.read_file(file)
	if bytes.is_empty():
		return null
	var doc := NovaMnuDocument.new()
	if doc.load_from_bytes(bytes) != OK:
		return null
	_menu_cache[file] = doc
	return doc


func _load_text(file: String) -> RtxtStringFile:
	if _root == null or file.is_empty():
		return null
	var bytes := _root.read_file(file)
	if bytes.is_empty():
		return null
	var t := RtxtStringFile.new()
	return t if t.load_from_byte_array(bytes) == OK else null


func _load_style(file: String) -> MnsStyleSheet:
	if _root == null or file.is_empty():
		return null
	var bytes := _root.read_file(file)
	if bytes.is_empty():
		return null
	var s := MnsStyleSheet.new()
	return s if s.load_from_bytes(bytes) == OK and s.is_runtime_valid() else null


# The menu SFX profile (menu.lwf) loads by name through the VFS so it resolves
# from PFF archives too; its members point at loose .wav files the menu resolves
# on demand. Degrades to null (silent menu SFX) when absent.
func _load_sound_profile(name: String) -> NovaLwfData:
	if _root == null or name.is_empty():
		return null
	var d := NovaLwfData.new()
	if d.open_from_resource_root(_root, name) != OK:
		return null
	return d if d.is_loaded() and d.get_set_count() > 0 else null


# --- Layout (anamorphic fill of the 800x600 design space to the window) --------
#
# Faithful to the original: the 800x600 design space is stretched to fill the whole
# window with independent X/Y factors (no aspect preservation, no letterbox bars,
# origin 0,0). On a widescreen display the 4:3 menu is stretched horizontally, as in
# the retail game [orig: CUIScene_SetScreenScale @ 0x639480].
func _recompute_fit(_unused: Variant = null) -> void:
	if _menu == null:
		return
	if size.x <= 1.0 or size.y <= 1.0:
		return
	_menu.position = Vector2.ZERO
	_menu.size = DESIGN_SIZE
	_menu.scale = Vector2(size.x / DESIGN_SIZE.x, size.y / DESIGN_SIZE.y)


# --- Misc helpers / accessors -------------------------------------------------

func _is_mission_list(widget_name: String) -> bool:
	for n in mission_list_names:
		if n == widget_name:
			return true
	return false


func _is_mod_list(widget_name: String) -> bool:
	for n in mod_list_names:
		if n == widget_name:
			return true
	return false


func _is_crosshair_style_control(widget_name: String) -> bool:
	for n in crosshair_style_control_names:
		if n == widget_name:
			return true
	return false


func _find_mod_list() -> NovaMnuList:
	for n in mod_list_names:
		var node := _menu.find_child(n, true, false)
		if node is NovaMnuList:
			return node as NovaMnuList
	return null


# MOD_DESC builds as NovaMnuMultilineEdit (a TextEdit); set_text works while READONLY.
func _find_mod_desc() -> TextEdit:
	for n in mod_desc_names:
		var node := _menu.find_child(n, true, false)
		if node is TextEdit:
			return node as TextEdit
	return null


func _find_mission_list() -> NovaMnuList:
	for n in mission_list_names:
		var node := _menu.find_child(n, true, false)
		if node is NovaMnuList:
			return node as NovaMnuList
	return null


# Accessors for owners / tests.
func get_menu() -> NovaMnuMenu:
	return _menu


func get_music_director() -> NovaMusicDirector:
	return NovaMusicService.director()


func get_current_menu_file() -> String:
	return _current_file


func get_selected_mission() -> String:
	return _selected_mission


func get_selected_expansion() -> String:
	return _selected_expansion


func get_crosshair_style() -> int:
	return ResourceDirSettings.get_crosshair_style()


func get_menu_stack_depth() -> int:
	return _menu_stack.size()
