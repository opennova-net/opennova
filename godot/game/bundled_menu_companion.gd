class_name BundledMenuCompanion
extends MenuCompanion

# Drives the bundled assets/ main menu (OpenNova's placeholder, ADR 0048) by
# control NAME: PLAY_RETAIL hands the session over to the player's retail
# install, CHANGE_FOLDER picks another one. The shell (MainGame) owns the
# mount, the folder picker and the saved folder; this companion only relays
# the two buttons. EXIT is the document's own quit action.

signal play_retail_requested
signal change_folder_requested


# Keyed on the control unique to the bundled menu.
func owns_menu(driver: MenuDriver) -> bool:
	return driver != null and driver.has_widget("PLAY_RETAIL")


func _wire(_file: String, _screen: String) -> void:
	_connect_pressed("PLAY_RETAIL", play_retail_requested.emit)
	_connect_pressed("CHANGE_FOLDER", change_folder_requested.emit)
