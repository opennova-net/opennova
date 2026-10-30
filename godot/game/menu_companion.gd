class_name MenuCompanion
extends RefCounted

# Base for the device half of an engine screen controller (the JO multiplayer
# menu MpMenuCompanion, the PLAYER_INFO screen PlayerInfoMenuCompanion, the
# armory's icons ArmoryMenuCompanion): the controller itself, its populate, its
# cascades and its commands, is the engine's (menu::MenuScreenController behind a
# MenuScreenBinding, which MenuShell.add_companion hands MenuDriver.add_screen);
# this keeps only the mounts it asks for over the compiled frame and the
# weapon.def table it reads, riding the shell's MenuDriver.

var _driver: MenuDriver
var _root: ResourceRoot
# The loadout screens' weapon icons: PRIMARY/SECONDARY/ACCESSORY -> TextureRect
# mounted as a frame child over the blank authored *_ICON window (the compiled
# frame owns no per-widget Control nodes); freed and rebuilt per menu build.
const WEAPON_ICON_SLOTS: Array[String] = ["PRIMARY", "SECONDARY", "ACCESSORY"]
var _icon_mounts: Dictionary = {}
# The loadout screens' weapon.def table and the mount it was read from
# (LoadoutWeaponTable.mount_key): a loadout companion lives the whole session,
# so a CHANGE FOLDER or a Mods switch reads weapon.def again; an injected table
# stands (_set_weapons).
var _weapons: WeaponDatabase = null
var _weapons_mount := ""
var _weapons_injected := false


## The binding of the engine screen controller this companion is the device half
## of (MenuShell.add_companion hands it to MenuDriver.add_screen); null for none.
func screen() -> MenuScreenBinding:
	return null


## The driver its screen opens on and the root its tables come from: the menu
## shell hands them over after its assembly and again when its root changes.
func attach(driver: MenuDriver, root: ResourceRoot) -> void:
	_driver = driver
	_root = root


## The screen let go of its document: the mounts parked on the shared frame go.
func on_menu_released() -> void:
	pass


# --- The loadout screens' weapon.def table ------------------------------------

# Hold an injected table (tests, another shell-owned mount); null hands the
# load back to the root.
func _set_weapons(weapons: WeaponDatabase) -> void:
	_weapons = weapons
	_weapons_injected = weapons != null


# Read weapon.def off the root unless a table was injected or the current one
# came from this mount. A missing or empty file is an empty table, so each list
# shows only NONE (D-MNU-27). True when this call read the table.
func _ensure_weapons(owner_name: String, consequence: String) -> bool:
	if _weapons_injected or _root == null:
		return false
	var mount := LoadoutWeaponTable.mount_key(_root)
	if _weapons != null and mount == _weapons_mount:
		return false
	_weapons = LoadoutWeaponTable.load_weapon_database(_root, owner_name, consequence)
	_weapons_mount = mount
	return true


# --- By-NAME helpers ----------------------------------------------------------

func _id(name: String) -> int:
	return _driver.widget_id(name) if _driver != null else -1


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
			icon_rect.texture = _root.load_texture(icon_name,
					ResourceRoot.TEXTURE_LOADER_MENU, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)


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
