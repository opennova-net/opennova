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

# Asset names resolved from the resource dir. JO defaults; override per game. A
# blank discovery name falls back to the first file of that kind in the dir.
@export var main_menu_file := "main.mnu"
@export var ingame_menu_file := "game.mnu"
@export var menu_text_file := "menutxt.BIN"
@export var menu_stylesheet_file := ""   # "" -> first .mns in the dir
@export var menu_sound_bank_file := ""   # "" -> a .sbf whose name contains "menu", else first
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

# Host -> main_game intents. The host never loads a world or quits the app
# itself; it translates menu activity into these and lets main_game decide.
signal start_requested(bms_name: String)
signal exit_to_desktop_requested()
signal return_to_menu_requested()
signal resume_requested()

var _menu: NovaMnuMenu
var _director: NovaMusicDirector
var _root: NovaResourceRoot
var _text: RtxtStringFile
var _style: MnsStyleSheet
var _sound_bank: NovaSbfBank
var _menu_music: NovaMusicScript
var _game_music: NovaMusicScript

var _menu_cache: Dictionary = {}            # filename -> NovaMnuDocument
var _menu_stack: Array[Dictionary] = []     # [{file, screen}] cross-.mnu back stack
var _current_file := ""
var _selected_mission := ""
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

	_text = _load_text(_resolve(menu_text_file))
	_style = _load_style(_discover_path(menu_stylesheet_file, ".mns", ""))
	_sound_bank = _load_bank(_discover_path(menu_sound_bank_file, ".sbf", "menu"))
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
	if _sound_bank != null:
		_menu.set_sound_bank(_sound_bank)
	_menu.set_music_director(_director)
	_menu.set_music_var_index(MUSIC_VAR_INDEX)
	add_child(_menu)

	# Connect once on the persistent menu node (the child screen tree is rebuilt
	# per open_menu; these aggregate signals survive the rebuilds).
	_menu.menu_requested.connect(_on_menu_requested)
	_menu.quit_requested.connect(_on_quit_requested)
	_menu.widget_value_changed.connect(_on_widget_value_changed)
	_menu.action_dispatched.connect(_on_action_dispatched)


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


# After each (re)build, connect the launch/quit controls and seed mission lists.
# The screen nodes are freshly built children, so prior connections died with the
# old tree; we just rescan.
func _wire_named_controls() -> void:
	_connect_named(start_control_names, _on_start_control)
	_connect_named(exit_control_names, _on_exit_control)
	_connect_named(return_control_names, _on_return_control)
	for list_name in mission_list_names:
		var list := _menu.find_child(list_name, true, false)
		if list is NovaMnuList:
			_seed_mission_list(list as NovaMnuList)


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


func _on_action_dispatched(_type: String, _target: String) -> void:
	pass  # informational; intra-menu actions are handled by the menu itself.


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


func _load_doc(file: String) -> NovaMnuDocument:
	if _menu_cache.has(file):
		return _menu_cache[file]
	var path := _resolve(file)
	if path.is_empty():
		return null
	var doc := NovaMnuDocument.new()
	if doc.load_from_path(path) != OK:
		return null
	_menu_cache[file] = doc
	return doc


func _load_text(path: String) -> RtxtStringFile:
	if path.is_empty():
		return null
	var t := RtxtStringFile.new()
	return t if t.load_from_path(path) == OK else null


func _load_style(path: String) -> MnsStyleSheet:
	if path.is_empty():
		return null
	var s := MnsStyleSheet.new()
	return s if s.load_from_path(path) == OK else null


func _load_bank(path: String) -> NovaSbfBank:
	if path.is_empty():
		return null
	var b := NovaSbfBank.new()
	b.load_from_path(path)
	return b if b.get_entry_count() > 0 else null


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


func get_menu_stack_depth() -> int:
	return _menu_stack.size()
