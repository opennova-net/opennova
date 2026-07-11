class_name NovaArmoryHost
extends Node

## The in-world armory surface BOTH hosts mount — the game shell (main_game) and
## ONED play-in-editor (mission_play_controller) — so the armory is literally one
## code path (editor-runtime parity: instantiate the runtime node, never
## reimplement the surface for the editor).
##
## Owns a live NovaMnuMenu over the gameplay view showing weapon.mnu's WEAPON
## screen, driven by the ArmoryMenuHost companion, zone-gated on the type-6
## armory volume contact flag the collision resolver maintains. The world keeps
## ticking underneath — the witnessed armory has no world-stop leg.
## [orig: input action 218 -> UI_OpenMenuScreen("weapon.mnu", "WEAPON") @0x49b8e3,
##  gated on entity Flags & 0x400000 @0x49b848; ACCEPT applies
##  WeaponLoadout_ApplyFromBuffer @0x565cd0 -> the WeaponSlot rebuild chain +
##  Player_MountWeaponSlot @0x4dfa40]

const MENU_FILE := "weapon.mnu"
const MENU_SCREEN := "WEAPON"
const STYLESHEET_FILE := "menu_style.mns"  # the canonical name (menu_shell's default)

signal opened
signal closed

var _world = null          # GameWorld
var _player_host = null    # LocalPlayerHost (viewmodel rebuild on ACCEPT)
var _ui_parent: Node = null
var _team := 0

var _menu: NovaMnuMenu = null
var _armory := ArmoryMenuHost.new()


func _init() -> void:
	_armory.loadout_accepted.connect(_on_loadout_accepted)
	_armory.armory_closed.connect(close)


## Wire the host to a world + player host and the control the menu overlays
## (the HUD layer in the game shell; the play viewport container in ONED).
func setup(world, player_host, ui_parent: Node) -> void:
	_world = world
	_player_host = player_host
	_ui_parent = ui_parent


func set_player_team(team: int) -> void:
	_team = team
	_armory.set_player_team(team)


func is_open() -> bool:
	return _menu != null and is_instance_valid(_menu) and _menu.visible


## The armory key: open weapon.mnu's WEAPON screen over LIVE play when the player
## stands in an armory zone. Returns false when out of zone or the menu cannot
## build (the key is then ignored, matching the original's silent gate).
func try_open() -> bool:
	if is_open() or _world == null or _ui_parent == null:
		return false
	var sim = _world.get_sim() if _world.has_method("get_sim") else null
	if sim == null or not sim.has_method("local_player_in_armory_zone"):
		return false
	if not sim.local_player_in_armory_zone():
		return false  # [orig: Flags & 0x400000 gate @0x49b848]
	if not _ensure_menu():
		return false
	_armory.set_player_team(_team)
	_menu.visible = true
	opened.emit()
	return true


func close() -> void:
	if not is_open():
		return
	_menu.visible = false
	closed.emit()


func teardown() -> void:
	if _menu != null and is_instance_valid(_menu):
		_menu.queue_free()
	_menu = null


# Build the runtime menu node over the gameplay view: the same NovaMnuMenu the
# menu shell drives (edit_mode off), fed weapon.mnu from the WORLD's mounted
# resource root, with the canonical stylesheet and the menutxt/gametext tables
# registered when no front-end shell did it already (ONED play has none).
func _ensure_menu() -> bool:
	if _menu != null and is_instance_valid(_menu):
		return true
	var root: NovaResourceRoot = _world.get_resource_root() \
			if _world.has_method("get_resource_root") else null
	if root == null:
		return false
	var bytes := root.read_file(MENU_FILE)
	if bytes.is_empty():
		push_warning("NovaArmoryHost: %s not found in the resource root" % MENU_FILE)
		return false
	var doc := NovaMnuDocument.new()
	if doc.load_from_bytes(bytes) != OK:
		push_warning("NovaArmoryHost: %s did not parse" % MENU_FILE)
		return false
	_register_text_tables(root)
	_menu = NovaMnuMenu.new()
	_menu.name = "ArmoryMenu"
	_menu.build_on_ready = false
	_menu.set_edit_mode(false)
	_menu.set_resource_root(root)
	var style := _load_style(root)
	if style != null:
		_menu.set_stylesheet(style)
	_ui_parent.add_child(_menu)
	_menu.set_menu_file(MENU_FILE)
	_menu.menu = doc
	_menu.show_screen(MENU_SCREEN)
	_armory.set_weapon_database(_world.get_weapon_database()
			if _world.has_method("get_weapon_database") else null)
	_armory.on_menu_built(_menu, MENU_FILE, MENU_SCREEN, root)
	return true


# Armory ACCEPT: stamp the sim entity (equipped_adm_index + player_class), rebuild
# the FP viewmodel + action FSM around the new primary, and resume play. SP-local
# apply — the MP client path rides the 0x2F/0x5A loadout service instead.
# [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 -> WeaponSlot rebuild chain +
#  Player_SelectWeaponSlot @0x4dd680 / Player_MountWeaponSlot @0x4dfa40]
func _on_loadout_accepted(loadout: Dictionary) -> void:
	var primary := String(loadout.get("primary", ""))
	if not primary.is_empty() and _world != null:
		var sim = _world.get_sim() if _world.has_method("get_sim") else null
		if sim != null and sim.has_method("apply_local_player_loadout"):
			sim.apply_local_player_loadout(primary, int(loadout.get("player_class", 0)))
		if _world.has_method("set_local_player_weapon_by_name") \
				and _world.set_local_player_weapon_by_name(primary) \
				and _player_host != null:
			_player_host.refresh_viewmodel()
	close()


# The armory's text lookups (WepDes weapon names, WEIGHT, CHARTYPE_* class rows)
# ride the shared NovaStrings registry; the game shell registers these at boot,
# ONED play does not — fill only the missing tables.
# [orig: Game_InitSubsystems @0x4a6cd0 loads menutxt/gametext at boot]
func _register_text_tables(root: NovaResourceRoot) -> void:
	for spec in [["menutxt", "menutxt.BIN"], ["gametext", "Game.bin"]]:
		if NovaStrings.get_table(spec[0]) != null:
			continue
		var bytes := root.read_file(spec[1])
		if bytes.is_empty():
			continue
		var t := RtxtStringFile.new()
		if t.load_from_byte_array(bytes) == OK:
			NovaStrings.register_table(spec[0], t)


# The canonical menu stylesheet name the original engine looks for.
func _load_style(root: NovaResourceRoot) -> MnsStyleSheet:
	var bytes := root.read_file(STYLESHEET_FILE)
	if bytes.is_empty():
		return null
	var s := MnsStyleSheet.new()
	return s if s.load_from_bytes(bytes) == OK else null
