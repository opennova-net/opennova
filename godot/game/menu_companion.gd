class_name MenuCompanion
extends RefCounted

# Base for the game-specific menu drivers ("companions") MenuShell delegates
# whole menus to (add_companion): the JO multiplayer menu (MpMenuCompanion) and the
# PLAYER_INFO character screen (PlayerInfoMenuCompanion). When a companion's
# owns_menu() claims a freshly opened document, the shell hands it the whole
# named-control wiring through on_menu_built() instead of running its generic
# launch/mission wiring. Subclasses override owns_menu + _wire and share the
# by-NAME helpers below, all riding the shell's MenuDriver (widgets are
# addressed by document id via widget_id — the find_child successor).

var _driver: MenuDriver
var _root: ResourceRoot
# NAME (upper) -> Callable, rebuilt per _wire; dispatched off the driver's
# widget_activated. Only fires while the wired document is still loaded.
var _activation_handlers: Dictionary = {}
var _wired_file := ""


## True when this companion drives the loaded document. Keyed on control names
## unique to the owned screens rather than a screen name, since a document's
## screens are all addressable at once.
func owns_menu(_driver_candidate: MenuDriver) -> bool:
	return false


## Called by MenuShell after each open_menu of a document this companion owns.
func on_menu_built(driver: MenuDriver, file: String, screen: String, root: ResourceRoot) -> void:
	_driver = driver
	_root = root
	_activation_handlers.clear()
	_wired_file = driver.get_menu_file() if driver != null else ""
	if driver == null:
		return
	if not driver.widget_activated.is_connected(_on_widget_activated):
		driver.widget_activated.connect(_on_widget_activated)
	_wire(file, screen)


## Called by MenuShell when a freshly opened document is NOT this companion's
## while it was the wired one: the companion must drop anything it parked on
## the shared frame (mounts, previews), because no later on_menu_built is
## coming. Subclasses override and call super.
func on_menu_released() -> void:
	_activation_handlers.clear()
	_wired_file = ""


## Subclass hook: wire the owned screens' controls (by name) off _driver/_root.
func _wire(_file: String, _screen: String) -> void:
	pass


# --- By-NAME helpers ----------------------------------------------------------

func _id(name: String) -> int:
	return _driver.widget_id(name) if _driver != null else -1


func _connect_pressed(name: String, handler: Callable) -> void:
	if _driver != null and _driver.has_widget(name):
		_activation_handlers[name.to_upper()] = handler


func _on_widget_activated(_id_activated: int, widget_name: String) -> void:
	# The shell swaps documents under the shared driver; a stale companion's
	# name matches must not double-dispatch against the new document.
	if _driver == null or _driver.get_menu_file() != _wired_file:
		return
	var handler: Callable = _activation_handlers.get(widget_name.to_upper(), Callable())
	if handler.is_valid():
		handler.call()


func _edit_text(name: String, default_value := "") -> String:
	var id := _id(name)
	if id < 0:
		return default_value
	return _driver.get_widget_text(id)


func _is_checked(name: String, default_value := false) -> bool:
	var id := _id(name)
	if id < 0:
		return default_value
	return _driver.is_widget_checked(id)
