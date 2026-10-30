class_name ArmoryMenuCompanion
extends MenuCompanion

# The device half of weapon.mnu's WEAPON screen: the slot icon mounts over the blank
# *_ICON windows (the compiled frame owns no per-widget Controls). The screen itself, its
# on-show, picks, class pages and ACCEPT / CANCEL, is the engine's controller
# (ArmoryScreen over engine menu::ArmoryScreen, runtime/menu/armory_screen.h), which says
# when the icons change (icons_changed) and which weapon each slot shows. A DFX2 DEATH
# screen's loadout picks run the same controller over that screen's driver; its icons
# mount over that screen's own frame.

var screen := ArmoryScreen.new()
# The DEATH screen's weapon icons (refresh_screen_icons), mounted over its own frame.
var _screen_icon_mounts: Dictionary = {}


func _init() -> void:
	screen.icons_changed.connect(_show_icons)


## The screen's frame and root, before the claim's on-show refreshes the icons.
func mount(driver: MenuDriver, root: ResourceRoot) -> void:
	_driver = driver
	_root = root
	_clear_icon_mounts()
	var frame := driver.get_frame() if driver != null else null
	if frame != null and not frame.resized.is_connected(_reposition_icon_mounts):
		frame.resized.connect(_reposition_icon_mounts)
	if driver != null and not driver.screen_changed.is_connected(_on_screen_changed):
		driver.screen_changed.connect(_on_screen_changed)


# The presenter's close releases the mounts: the next show refreshes them.
func on_menu_released() -> void:
	super()
	_clear_icon_mounts()


## The DEATH screen's icon mounts freed and forgotten: they ride that screen's frame, which its
## presenter frees at each mission's teardown, while this companion lives on with its presenter.
func release_screen_icon_mounts() -> void:
	_free_icon_mounts(_screen_icon_mounts)


## The DEATH screen's weapon icons over its own frame (`root` the screen's resource root),
## hidden while their windows do not draw, from the controller's selections on that screen.
func refresh_screen_icons(driver: MenuDriver, root: ResourceRoot) -> void:
	if driver == null:
		return
	var selected_for := func(control: String) -> WeaponDef:
		return screen.selected_weapon_on(driver, control)
	_mount_weapon_icons(driver, root, _screen_icon_mounts, "DeathIcon", selected_for, true)


func _show_icons() -> void:
	_update_weapon_icons("ArmoryIcon", screen.selected_weapon)


func _on_screen_changed(_screen_name: String) -> void:
	_reposition_icon_mounts()
