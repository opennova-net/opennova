class_name NovaMenuHost
extends Control

# Runtime menu shell: drives a live NovaMnuMenu (the same engine node the ONED
# Menus workspace previews, here with edit_mode off so it is fully interactive)
# and a NovaMusicDirector, loading the game's .mnu menu set + audio from the
# user's resource directory. It is the runtime counterpart to NovaWorld: NovaWorld
# turns a resource dir into a playable world, this turns it into the playable
# menu front-end, and main_game.gd hands off between the two.
#
# The menu itself owns intra-.mnu navigation, window show/hide, the back stack,
# and per-screen music (it pushes each screen's MUSICVAR into the director). The
# host services the policy the menu leaves to it: cross-.mnu file jumps
# (menu_requested), quit (quit_requested), and the gameplay launch. Shipped JO
# menus carry no "launch" action verb; the engine wires those by well-known
# control NAME (START_GAME, ACCEPT, EXIT, ...), so the host scans the built tree
# for those names and connects them. The control-name sets are exported so a
# different game's menu set can be pointed at the same shell.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

# Menu var index the director uses for the current-screen MUSICVAR (matches the
# menu's set_music_var_index default and the engine tests' set_var(0, ...) path).
const MUSIC_VAR_INDEX := 0

# Friendly labels for known expansions. The list item + persisted key stay the raw
# folder name (e.g. "jox01"); unknown expansions display their raw folder name.
const EXPANSION_DISPLAY_NAMES := {"jox01": "Kendari"}

# Asset names resolved from the resource dir. JO defaults; override per game. A
# blank discovery name falls back to the first file of that kind in the dir.
@export var main_menu_file := "main.mnu"
@export var ingame_menu_file := "game.mnu"
@export var menu_text_file := "menutxt.BIN"
# The menu stylesheet has a fixed canonical name the original engine looks for
# ("named menu_style.mns for the game to find it"). It is usually PFF-archived and
# is NOT a "recognized kind", so list_files(".mns") never surfaces it; load it by
# name through the VFS instead. A blank value falls back to the first .mns found.
@export var menu_stylesheet_file := "menu_style.mns"
@export var menu_sound_bank_file := ""   # "" -> a .sbf whose name contains "menu", else first
# Menu SFX profile: the .lwf the widgets' <SOUND> elements reference (hover/click).
# "" -> a .lwf whose name contains "menu" (i.e. menu.lwf), else the first .lwf found.
@export var menu_sound_profile_file := ""
@export var menu_music_file := ""        # "" -> a .mus/.bin whose name contains "menu", else first
@export var game_music_file := ""        # "" -> a .mus/.bin whose name contains "game", else first

# Well-known control names (the JO "wired by convention" launch/quit controls).
# A button found by one of these names gets its `pressed` connected to the host.
@export var start_control_names := PackedStringArray([
	"START_GAME", "ACCEPT", "LAUNCH", "GO", "HOST_GAME", "LAN_HOSTGAME",
])
@export var exit_control_names := PackedStringArray([
	"EXIT", "QUIT", "QUIT_GAME", "QUIT_TO_DESKTOP",
])
@export var return_control_names := PackedStringArray([
	"QUIT_TO_MENU", "MAIN_MENU", "ABORT", "ABORT_MISSION",
])
# List widgets the host fills with the resource dir's missions (.bms).
@export var mission_list_names := PackedStringArray([
	"MISSION_LIST", "MISSIONLIST", "MISSIONS", "IA_LIST", "MAP_LIST",
])
# List widgets the host fills with the expansions discoverable under the resource
# dir (Options -> Mods). Activating one mounts it over the base game.
@export var mod_list_names := PackedStringArray([
	"AVAIL_LIST", "MOD_LIST", "MODLIST", "EXPANSION_LIST",
])
# Readonly text widgets that show the selected expansion's description/name.
@export var mod_desc_names := PackedStringArray([
	"MOD_DESC", "MOD_DESCRIPTION",
])

# Host -> main_game intents. The host never loads a world or quits the app
# itself; it translates menu activity into these and lets main_game decide.
signal start_requested(bms_name: String)
signal exit_to_desktop_requested()
signal return_to_menu_requested()
signal resume_requested()
# Emitted when the player activates an expansion/mod in Options. The choice is also
# mounted onto the live root and persisted (read back at the next launch/world load
# by main_game.gd), so it affects gameplay, not just the menu.
signal expansion_selected(name: String)

var _menu: NovaMnuMenu
var _director: NovaMusicDirector
var _root: NovaResourceRoot
var _text: RtxtStringFile
var _style: MnsStyleSheet
var _sound_bank: NovaSbfBank
var _sound_profile: NovaLwfData
var _menu_music: NovaMusicScript
var _game_music: NovaMusicScript

var _menu_cache: Dictionary = {}            # filename -> NovaMnuDocument
var _menu_stack: Array[Dictionary] = []     # [{file, screen}] cross-.mnu back stack
var _current_file := ""
var _selected_mission := ""
var _selected_expansion := ""
var _menu_size := Vector2(640, 480)
var _in_game := false
var _ready_done := false


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_PASS
	if not resized.is_connected(_recompute_fit):
		resized.connect(_recompute_fit)


# Build the shell against a resource root and open the main menu. Idempotent on
# the asset/menu/director wiring (only assembled once); call reset_to_root() to
# return to the main menu on later entries. Returns false when the main menu
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
	_director = NovaMusicDirector.new()
	_director.name = "MusicDirector"
	_director.auto_start = false
	add_child(_director)

	_text = _load_text(menu_text_file)
	_style = _load_style(_discover_name(menu_stylesheet_file, ".mns", ""))
	_sound_bank = _load_bank(_discover_path(menu_sound_bank_file, ".sbf", "menu"))
	_sound_profile = _load_sound_profile(_discover_name(menu_sound_profile_file, ".lwf", "menu"))
	_menu_music = _load_music(_discover_music(menu_music_file, "menu"))
	_game_music = _load_music(_discover_music(game_music_file, "game"))
	if _sound_bank != null:
		_director.set_bank(_sound_bank)

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
	_menu.set_music_director(_director)
	_menu.set_music_var_index(MUSIC_VAR_INDEX)
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
		push_warning("NovaMenuHost: could not load menu '%s'" % file)
		return false
	_current_file = file
	_selected_mission = ""
	_menu_size = Vector2(doc.get_menu_size())
	_menu.menu = doc  # in-tree -> rebuilds synchronously, fires screen/music signals
	if not target_screen.is_empty():
		_menu.show_screen(target_screen)
	_wire_named_controls()
	_recompute_fit()
	return true


# Return to the main menu from anywhere (e.g. quit-to-main from the pause menu).
func reset_to_root() -> void:
	_in_game = false
	_menu_stack.clear()
	_enter_menu_music()
	open_menu(main_menu_file, "")


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
	if has_mission_list:
		_connect_named(start_control_names, _on_start_control)
	elif has_mod_list:
		_connect_named(start_control_names, _on_apply_selected_mod)
	_connect_named(exit_control_names, _on_exit_control)
	_connect_named(return_control_names, _on_return_control)


func _connect_named(names: PackedStringArray, handler: Callable) -> void:
	for n in names:
		var node := _menu.find_child(n, true, false)
		if node is BaseButton and not (node as BaseButton).pressed.is_connected(handler):
			(node as BaseButton).pressed.connect(handler)


func _seed_mission_list(list: NovaMnuList) -> void:
	var missions := _root.list_files(".bms") if _root != null else PackedStringArray()
	var names := PackedStringArray()
	for m in missions:
		names.append(String(m).get_file())
	list.set_items(names)
	if not list.item_activated.is_connected(_on_mission_activated):
		list.item_activated.connect(_on_mission_activated)


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
# it, persist the choice, and announce it. The persisted key is read at the next
# launch/world load by main_game.gd, so the selection affects gameplay too. A failed
# mount clears the root, so the previous expansion is re-mounted to recover.
func _apply_expansion(name: String) -> void:
	if _root == null or name.is_empty() or name == _current_expansion():
		return
	var dir := _root.get_root_dir()
	var prev := _current_expansion()
	if _root.mount_runtime(dir, name, NovaLaunchFlags.loose_override_enabled()) != OK:
		push_warning("NovaMenuHost: could not mount expansion '%s': %s" % [name, _root.get_last_error()])
		_root.mount_runtime(dir, prev, NovaLaunchFlags.loose_override_enabled())  # rollback
		return
	ResourceDirSettings.set_expansion(name)
	_selected_expansion = name
	_refresh_dependent_content()
	_update_mod_desc(name)
	expansion_selected.emit(name)


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
	_menu_stack.push_back({"file": _current_file, "screen": _menu.current_screen})
	open_menu(file, target_screen)


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


func _on_widget_value_changed(widget_name: String, kind: String, _index: int, value: String) -> void:
	if kind == "list" and _is_mission_list(widget_name):
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


# --- Named-control handlers (host policy: launch / quit by control name) -------

func _on_start_control() -> void:
	var mission := _selected_mission
	if mission.is_empty():
		# No explicit pick: fall back to the first available mission so a menu
		# without a list (or before a selection) can still start something.
		var missions := _root.list_files(".bms") if _root != null else PackedStringArray()
		if missions.size() > 0:
			mission = String(missions[0]).get_file()
	if mission.is_empty():
		push_warning("NovaMenuHost: start pressed with no mission available")
		return
	start_requested.emit(mission)


func _on_exit_control() -> void:
	exit_to_desktop_requested.emit()


func _on_return_control() -> void:
	return_to_menu_requested.emit()


func _on_mission_activated(index: int) -> void:
	var list := _find_mission_list()
	if list != null and index >= 0 and index < list.item_count:
		_selected_mission = list.get_item_text(index)
	_on_start_control()


# --- Audio --------------------------------------------------------------------

func _enter_menu_music() -> void:
	_play_script(_menu_music)


func enter_game_music() -> void:
	_play_script(_game_music)


func _play_script(script: NovaMusicScript) -> void:
	if _director == null:
		return
	_director.stop()
	if script == null or _is_headless():
		return  # no script, or no audio device (headless tests / probes)
	_director.load_mus_script(script)
	_director.start()


# --- Asset resolution helpers (all best-effort, degrade to null) --------------

func _resolve(name: String) -> String:
	if _root == null or name.is_empty():
		return ""
	return _root.resolve_file(name)


# Resolve an explicit file, else discover one by extension (preferring a name
# containing `prefer`). Returns a loadable path, or "".
func _discover_path(explicit: String, suffix: String, prefer: String) -> String:
	if not explicit.is_empty():
		return _resolve(explicit)
	if _root == null:
		return ""
	var files := _root.list_files(suffix)
	if files.is_empty():
		return ""
	if not prefer.is_empty():
		for f in files:
			if String(f).get_file().to_lower().contains(prefer):
				return _resolve(String(f).get_file())
	return _resolve(String(files[0]).get_file())


# Like _discover_path, but returns the winning entry's logical basename (loadable
# through the VFS by name via _root.read_file) rather than a loose disk path. Used
# by the byte-based loaders so discovery works for PFF-archived assets too.
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


# Music scripts ship as .bin (e.g. menumus.bin / gamemus.bin); .bin also covers
# the menu string table (menutxt.BIN), so a music candidate must contain "mus"
# (gamemus/menumus) to avoid grabbing the text table. Prefer one also matching
# `prefer` ("menu" / "game").
func _discover_music(explicit: String, prefer: String) -> String:
	if not explicit.is_empty():
		return _resolve(explicit)
	if _root == null:
		return ""
	var candidates := PackedStringArray()
	candidates.append_array(_root.list_files(".mus"))
	candidates.append_array(_root.list_files(".bin"))
	var fallback := ""
	for f in candidates:
		var n := String(f).get_file().to_lower()
		if not n.contains("mus"):
			continue
		if n.contains(prefer):
			return _resolve(String(f).get_file())
		if fallback.is_empty():
			fallback = _resolve(String(f).get_file())
	return fallback


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
	return s if s.load_from_bytes(bytes) == OK else null


func _load_bank(path: String) -> NovaSbfBank:
	if path.is_empty():
		return null
	var b := NovaSbfBank.new()
	b.load_from_path(path)
	return b if b.get_entry_count() > 0 else null


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


# Music scripts carry an SCR encryption layer that only the registered loader
# strips, so they load via ResourceLoader (not a direct load_from_path).
func _load_music(path: String) -> NovaMusicScript:
	if path.is_empty():
		return null
	var res = ResourceLoader.load(path, "NovaMusicScript")
	return res as NovaMusicScript


# --- Layout (uniform letterbox fit of the menu's design size to the window) ----

func _recompute_fit(_unused: Variant = null) -> void:
	if _menu == null or _menu_size.x <= 0.0 or _menu_size.y <= 0.0:
		return
	if size.x <= 1.0 or size.y <= 1.0:
		return
	var fit := minf(size.x / _menu_size.x, size.y / _menu_size.y)
	fit = maxf(fit, 0.01)
	_menu.scale = Vector2(fit, fit)
	_menu.position = (size - _menu_size * fit) * 0.5


# --- Misc helpers / accessors -------------------------------------------------

func _is_headless() -> bool:
	return DisplayServer.get_name() == "headless"


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


# Accessors for hosts / tests.
func get_menu() -> NovaMnuMenu:
	return _menu


func get_music_director() -> NovaMusicDirector:
	return _director


func get_current_menu_file() -> String:
	return _current_file


func get_selected_mission() -> String:
	return _selected_mission


func get_selected_expansion() -> String:
	return _selected_expansion


func get_menu_stack_depth() -> int:
	return _menu_stack.size()
