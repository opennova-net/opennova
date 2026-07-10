class_name ArmoryMenuHost
extends RefCounted

# Drives the JO in-game armory — weapon.mnu's WEAPON screen — by control NAME, as a
# companion the game-agnostic NovaMenuHost (menu_shell.gd) delegates to (the
# mp_menu_host / player_info_menu_host pattern). The original registers exactly these
# controls on the "WEAPON" screen [orig: WeaponDef_RegisterUICallbacks @0x567020]:
#   PLAYER_CLASS (spinlist)                  -> handle_team_class_selection @0x566f60
#   PRIMARY / SECONDARY / ACCESSORY (combos) -> UI_OnPrimaryWeaponTypeChanged @0x5662d0 /
#                                               UI_OnSecondaryWeaponChanged @0x566670 /
#                                               ui_on_weapon_ammo_slot_changed @0x566a10
#   PRIMARY_AMMO1/2, SECONDARY_AMMO1/2, ACCESSORY_AMMO1/2, GRENADE_AMMO1..3,
#   *_AMMO1_TYPE (combos)                    -> populate_ammo_combo_boxes @0x55def0
#   ACCEPT / CANCEL (buttons)                -> WeaponLoadout_ApplyFromBuffer @0x565cd0
# The screen opens in-match from input action 218 while the player stands in a type-6
# armory volume (entity Flags 0x400000) [orig: Input_HandleActionBinding @0x49b848] —
# main_game owns that key + gate and the ACCEPT apply (sim + FP viewmodel rebuild).
#
# Slot population is the witnessed class/team filter [orig: populate_weapon_slot_lists
# @0x560430 via NovaWeaponDatabase.get_slot_weapons]; the weight readout is the
# witnessed sum [orig: calculate_loadout_weight @0x55f1f0 — weaponweight + clips *
# clipweight per selected slot]. Deferred (tracked in the armory RE notes): the icon
# swaps [orig: update_player_info_weight_and_weapon_icons @0x55f480], the exact ammo
# row text of @0x55def0, and the *_AMMO1_TYPE round-type cascade.

var _menu: Node                      # the built NovaMnuMenu
var _root: NovaResourceRoot
var _weapons: NovaWeaponDatabase
var _team := 0                       # 0 = blue/good, 1 = red/evil (host stamps before open)
var _populating := false
# Selected weapon dicts per slot control name ("" row 0 = NONE).
var _slot_rows := {}                 # control name -> Array[Dictionary] (row-1 aligned)

signal loadout_accepted(loadout: Dictionary)
signal armory_closed


# weapon.mnu's WEAPON screen: PLAYER_CLASS (spinlist) + PRIMARY_AMMO1 are unique to it
# (player.mnu uses PLAYERCLASS, no ammo combos).
func owns_menu(menu: Node) -> bool:
	if menu == null:
		return false
	return menu.find_child("PLAYER_CLASS", true, false) != null \
		and menu.find_child("PRIMARY_AMMO1", true, false) != null


func set_player_team(team: int) -> void:
	_team = team


# Inject a pre-loaded weapon.def database (ADR 0018 seam: tests and hosts that
# already carry the db hand it in; on_menu_built otherwise loads it from the root).
func set_weapon_database(weapons: NovaWeaponDatabase) -> void:
	_weapons = weapons


func on_menu_built(menu: Node, _file: String, _screen: String, root: NovaResourceRoot) -> void:
	_menu = menu
	_root = root
	_ensure_weapons()
	_populate_classes()
	_connect_spin("PLAYER_CLASS", _on_class_changed)
	_populate_slots()
	for slot_name in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		_connect_combo(slot_name, _on_slot_selected.bind(slot_name))
	_connect_pressed("ACCEPT", _on_accept)   # [orig: @0x5671f6 arg 0]
	_connect_pressed("CANCEL", _on_cancel)   # [orig: @0x567214 arg 1 skips the apply]
	_update_weight()


func _ensure_weapons() -> void:
	if _weapons != null or _root == null:
		return
	_weapons = NovaWeaponDatabase.new()
	if _weapons.load_from_resource_root(_root, "weapon.def") != OK or not _weapons.is_loaded():
		push_warning("ArmoryMenuHost: weapon.def not loaded (%s); armory lists stay empty"
			% _weapons.get_last_error())
		_weapons = null


# PLAYER_CLASS carries the five MP soldier classes 5..9; the host fills the spinlist
# (weapon.mnu authors it empty) with the CHARTYPE display names [orig: the class walk
# in handle_team_class_selection @0x566f60; values witnessed 5..9 net-re §5.56].
const CLASS_VALUES := [5, 6, 7, 8, 9]
const CLASS_KEYS := ["CHARTYPE_DELTA_MEDIC", "CHARTYPE_DELTA_SNIPER", "CHARTYPE_DELTA_GUNNER",
	"CHARTYPE_DELTA_RIFLEMAN", "CHARTYPE_DELTA_ENGINEER"]
const CLASS_FALLBACKS := ["Medic", "Sniper", "Gunner", "Rifleman", "Engineer"]

func _populate_classes() -> void:
	var spin := _spin("PLAYER_CLASS")
	if spin == null:
		return
	var rows := PackedStringArray()
	for i in CLASS_KEYS.size():
		rows.append(_menu_text(CLASS_KEYS[i], CLASS_FALLBACKS[i]))
	_populating = true
	spin.set_values(rows)
	spin.set_value_index(3)  # rifleman default [orig: PlayerSpawn player_class default 8]
	_populating = false


func _selected_class() -> int:
	var spin := _spin("PLAYER_CLASS")
	if spin == null:
		return 8
	var idx: int = clampi(spin.get_value_index(), 0, CLASS_VALUES.size() - 1)
	return CLASS_VALUES[idx]


# The class mask bit [orig: PlayerInfo_SetTeamAndClassMask @0x55de60: 5->1 ... 9->16].
func _class_mask() -> int:
	return 1 << (_selected_class() - 5)


func _on_class_changed(_index: int, _value: String) -> void:
	if _populating:
		return
	_populate_slots()  # the class re-filters every slot list [orig: @0x566f60]
	_update_weight()


# --- Slot lists -----------------------------------------------------------------

func _populate_slots() -> void:
	if _weapons == null:
		return
	var team_mask := 2 if _team == 0 else 1  # [orig: g_playerInfoTeamMask = 2 - (team != 0)]
	_fill_slot("PRIMARY", NovaWeaponDatabase.SLOT_PRIMARY, team_mask)
	_fill_slot("SECONDARY", NovaWeaponDatabase.SLOT_SECONDARY, team_mask)
	_fill_slot("ACCESSORY", NovaWeaponDatabase.SLOT_ACCESSORY, team_mask)
	for slot_name in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		_populate_ammo(slot_name)


func _fill_slot(control: String, slot: int, team_mask: int) -> void:
	var combo := _combo(control)
	if combo == null:
		return
	var rows := PackedStringArray()
	rows.append(_menu_text("NONE", "None"))  # NONE at index 0 [orig: @0x560430]
	var dicts: Array = _weapons.get_slot_weapons(slot, _class_mask(), team_mask)
	for w in dicts:
		rows.append(_weapon_label(w))
	_slot_rows[control] = dicts
	_set_combo_items(combo, rows)
	# Pre-select the first real weapon so ACCEPT always carries a loadout.
	if rows.size() > 1:
		combo.select_silent(1)


func _weapon_label(w: Dictionary) -> String:
	var textid := String(w.get("display_textid", ""))
	if not textid.is_empty():
		var t: RtxtStringFile = NovaStrings.get_table("gametext")
		if t != null and t.has_string_in_section("WepDes", textid):
			return t.get_string_in_section("WepDes", textid)
	return String(w.get("name", ""))


# The slot's selected weapon dict (the NovaWeaponDatabase transport dict; {} = NONE).
# Public read seam (ADR 0018): tests and diagnostics read the selection here.
func selected_weapon(control: String) -> Dictionary:
	var combo := _combo(control)
	if combo == null:
		return {}
	var row := combo.get_selected()
	var dicts: Array = _slot_rows.get(control, [])
	if row <= 0 or row > dicts.size():
		return {}  # NONE
	return dicts[row - 1]


func _on_slot_selected(_row: int, _value: String, control: String) -> void:
	if _populating:
		return
	_populate_ammo(control)
	_update_weight()


# Clip-count rows for the slot's ammo combo, capped by the selected weapon's maxclips
# [orig: populate_ammo_combo_boxes @0x55def0 fills per-weapon; the ACCEPT clamp is
# clips <= adm[83] @0x565cd0]. The exact original row text is unwitnessed (tracked);
# plain counts carry the same value the collect reads back.
func _populate_ammo(control: String) -> void:
	var combo := _combo(control + "_AMMO1")
	if combo == null:
		return
	var w := selected_weapon(control)
	var rows := PackedStringArray()
	var maxclips := int(w.get("maxclips", 0))
	for i in range(0, maxclips + 1):
		rows.append(str(i))
	if rows.is_empty():
		rows.append("0")
	_set_combo_items(combo, rows)
	combo.select_silent(rows.size() - 1)  # full clips default
	_update_weight()


# The slot's selected clip count. Public read seam (ADR 0018), paired with
# selected_weapon.
func selected_clips(control: String) -> int:
	var combo := _combo(control + "_AMMO1")
	if combo == null or combo.get_selected() < 0:
		return -1  # -1 = the weapon's default clips [orig: -1 -> adm[23] @0x565cd0]
	return combo.get_selected()  # rows are the counts 0..maxclips; index == count


# --- Weight ----------------------------------------------------------------------

# Total loadout weight = sum over the selected slots of weaponweight + clips *
# clipweight [orig: calculate_loadout_weight @0x55f1f0]. Rendered into
# STATIC_TOTAL_WEIGHT beside its WEIGHT label.
func _update_weight() -> void:
	var total := 0.0
	for slot_name in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		var w := selected_weapon(slot_name)
		if w.is_empty():
			continue
		var clips := selected_clips(slot_name)
		if clips < 0:
			clips = int(w.get("maxclips", 0))
		total += float(w.get("weight", 0.0)) + clips * float(w.get("clip_weight", 0.0))
	var node := _find("STATIC_TOTAL_WEIGHT")
	if node != null and node is Label:
		(node as Label).text = "%s %.1f" % [_menu_text("WEIGHT", "Weight"), total]


# --- ACCEPT / CANCEL ---------------------------------------------------------------

# Collect the selection the way the original serializes it before applying/sending
# [orig: WeaponLoadout_SerializeToBufferTeamBased @0x5658b0 {name, ammoPri, ammoSec,
# flags} per slot]. The host applies primary to the sim + viewmodel; the full
# multi-slot inventory is the runtime's tracked gap.
func _on_accept() -> void:
	var loadout := {
		"player_class": _selected_class(),
		"team": _team,
		"primary": String(selected_weapon("PRIMARY").get("name", "")),
		"primary_clips": selected_clips("PRIMARY"),
		"secondary": String(selected_weapon("SECONDARY").get("name", "")),
		"secondary_clips": selected_clips("SECONDARY"),
		"accessory": String(selected_weapon("ACCESSORY").get("name", "")),
		"accessory_clips": selected_clips("ACCESSORY"),
	}
	loadout_accepted.emit(loadout)


func _on_cancel() -> void:
	armory_closed.emit()  # [orig: CANCEL skips the apply, clears g_WeaponScreenOpen]


# --- Helpers -----------------------------------------------------------------------

func _combo(name: String) -> NovaMnuCombo:
	return _find(name) as NovaMnuCombo


func _spin(name: String) -> NovaMnuSpinList:
	return _find(name) as NovaMnuSpinList


func _set_combo_items(combo: NovaMnuCombo, rows: PackedStringArray) -> void:
	_populating = true
	combo.set_items(rows)
	if rows.size() > 0:
		combo.select_silent(0)
	_populating = false


func _connect_combo(name: String, handler: Callable) -> void:
	var combo := _combo(name)
	if combo != null and not combo.item_selected.is_connected(handler):
		combo.item_selected.connect(handler)


func _connect_spin(name: String, handler: Callable) -> void:
	var spin := _spin(name)
	if spin != null and not spin.value_changed.is_connected(handler):
		spin.value_changed.connect(handler)


func _connect_pressed(name: String, handler: Callable) -> void:
	var node := _find(name)
	if node is BaseButton and not (node as BaseButton).pressed.is_connected(handler):
		(node as BaseButton).pressed.connect(handler)


func _menu_text(key: String, fallback: String) -> String:
	for spec in [["menutxt", "Menu"], ["gametext", "Menu"]]:
		var t: RtxtStringFile = NovaStrings.get_table(spec[0])
		if t != null and t.has_string_in_section(spec[1], key):
			return t.get_string_in_section(spec[1], key)
	return fallback


func _find(name: String) -> Node:
	return _menu.find_child(name, true, false) if _menu != null else null
