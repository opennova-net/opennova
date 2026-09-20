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
# Guards a companion's value-changed cascade against programmatic-fill re-entry.
var _populating := false
# The loadout screens' weapon icons: PRIMARY/SECONDARY/ACCESSORY -> TextureRect
# mounted as a frame child over the blank authored *_ICON window (the compiled
# frame owns no per-widget Control nodes); freed and rebuilt per menu build.
const WEAPON_ICON_SLOTS: Array[String] = ["PRIMARY", "SECONDARY", "ACCESSORY"]
var _icon_mounts: Dictionary = {}


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


# Fill a combo and pre-select the first row without firing the cascade (the fill is
# programmatic; user selections come through widget_value_changed). select_row with
# emit=false suppresses the relay; the _populating guard covers any incidental emit.
func _set_combo_items(combo: int, rows: PackedStringArray) -> void:
	_populating = true
	_driver.set_widget_items(combo, rows)
	if rows.size() > 0:
		_driver.select_row(combo, 0, false)
	_populating = false


# --- Weapon-slot icon mounts (the two loadout companions) -----------------------

# Mount (once) and refresh one icon per weapon slot. `selected_for` maps a slot
# name to its selected WeaponDef (null when the slot is empty); `name_suffix`
# names the mounted node ("<SLOT><suffix>").
func _update_weapon_icons(name_suffix: String, selected_for: Callable) -> void:
	var frame := _driver.get_frame() if _driver != null else null
	if frame == null:
		return
	for control in WEAPON_ICON_SLOTS:
		var holder := _id(control + "_ICON")
		if holder < 0:
			continue
		var icon_rect: TextureRect = _icon_mounts.get(control)
		if icon_rect == null or not is_instance_valid(icon_rect):
			icon_rect = TextureRect.new()
			icon_rect.name = control + name_suffix
			icon_rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
			icon_rect.stretch_mode = TextureRect.STRETCH_SCALE
			icon_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
			frame.add_child(icon_rect)
			_icon_mounts[control] = icon_rect
		_place_mount(icon_rect, holder)
		var selected: WeaponDef = selected_for.call(control)
		var icon_name := selected.icon if selected != null else ""
		if icon_name.is_empty() or _root == null:
			icon_rect.texture = null
		else:
			icon_rect.texture = _root.load_texture(
					icon_name, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)


func _clear_icon_mounts() -> void:
	for control in _icon_mounts:
		var mount: TextureRect = _icon_mounts[control]
		if mount != null and is_instance_valid(mount):
			mount.queue_free()
	_icon_mounts.clear()


func _reposition_icon_mounts() -> void:
	if _driver == null:
		return
	for control in _icon_mounts:
		var mount: TextureRect = _icon_mounts[control]
		if mount == null or not is_instance_valid(mount):
			continue
		var holder := _id(String(control) + "_ICON")
		if holder >= 0:
			_place_mount(mount, holder)


# Place a mount over its widget: widget_frame_rect is the design rect scaled to
# the frame's current size, and a zero rect means the widget is not on the
# configured screen, so the mount hides with it.
func _place_mount(mount: Control, id: int) -> void:
	var rect := _driver.widget_frame_rect(id)
	mount.position = rect.position
	mount.size = rect.size
	mount.visible = rect.size.x > 0.0 and rect.size.y > 0.0
