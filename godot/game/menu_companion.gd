class_name MenuCompanion
extends RefCounted

# Base for the game-specific menu drivers ("companions") NovaMenuShell delegates
# whole menus to (add_companion): the JO multiplayer menu (MpMenuCompanion) and the
# PLAYER_INFO character screen (PlayerInfoMenuCompanion). When a companion's
# owns_menu() claims a freshly built menu, the shell hands it the whole
# named-control wiring through on_menu_built() instead of running its generic
# launch/mission wiring. Subclasses override owns_menu + _wire and share the
# by-NAME control helpers below.

var _menu: Node  # the built NovaMnuMenu; typed Node so we depend only on its tree + signals
var _root: NovaResourceRoot


## True when this companion drives `menu`. Keyed on control names unique to the
## owned screens rather than a screen name, since a document's screens are all
## built at once.
func owns_menu(_menu_node: Node) -> bool:
	return false


## Called by NovaMenuShell after each open_menu (re)build of a menu this
## companion owns. The screen nodes are freshly built children, so prior
## connections died with the old tree; _wire rescans by name.
func on_menu_built(menu: Node, file: String, screen: String, root: NovaResourceRoot) -> void:
	_menu = menu
	_root = root
	if menu == null:
		return
	_wire(file, screen)


## Subclass hook: wire the owned screens' controls (by name) off _menu/_root.
func _wire(_file: String, _screen: String) -> void:
	pass


# --- By-NAME control helpers ----------------------------------------------------

func _find(name: String) -> Node:
	return _menu.find_child(name, true, false) if _menu != null else null


func _connect_pressed(name: String, handler: Callable) -> void:
	var node := _find(name)
	if node is BaseButton and not (node as BaseButton).pressed.is_connected(handler):
		(node as BaseButton).pressed.connect(handler)


func _edit_text(name: String, default_value := "") -> String:
	var node := _find(name)
	if node is LineEdit:  # NovaMnuEdit extends LineEdit
		return (node as LineEdit).text
	return default_value
