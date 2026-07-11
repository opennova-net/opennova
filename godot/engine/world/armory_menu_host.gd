class_name ArmoryMenuHost
extends RefCounted

# Drives the JO in-game armory — weapon.mnu's WEAPON screen — by control NAME, as a
# companion the game-agnostic NovaMenuHost (menu_shell.gd) delegates to (the
# mp_menu_host / player_info_menu_host pattern). The original registers exactly these
# controls on the "WEAPON" screen [orig: WeaponDef_RegisterUICallbacks @0x567020,
# run once from the screen's INIT event @0x567250]:
#   PLAYER_CLASS (spinlist)                  -> handle_team_class_selection @0x566f60
#   PRIMARY / SECONDARY / ACCESSORY (combos) -> UI_OnPrimaryWeaponTypeChanged @0x5662d0 /
#                                               UI_OnSecondaryWeaponChanged @0x566670 /
#                                               ui_on_weapon_ammo_slot_changed @0x566a10
#   PRIMARY_AMMO1/2, SECONDARY_AMMO1/2, ACCESSORY_AMMO1/2, GRENADE_AMMO1..3,
#   *_AMMO1_TYPE (combos)                    -> populate_ammo_combo_boxes @0x55def0
#   ACCEPT / CANCEL (buttons)                -> WeaponLoadout_ApplyFromBuffer @0x565cd0
# The screen opens in-match from the USE-ITEM key (action 177 "useitem", retail default
# SHIFT per the shipped KeyChart) while the player stands in a type-6 armory volume
# (entity Flags 0x400000) [orig: Input_HandleActionBinding_0 case 0xB1 @0x4e0b3f;
# the parallel action 218 @0x49b8e3 ships with no binding row] — the shell owns that
# key + gate and the ACCEPT apply (sim + FP viewmodel rebuild).
#
# Slot population is the witnessed class/team filter of the WEAPON-screen populate
# [orig: populate_three_category_lists @0x566db0 via NovaWeaponDatabase
# .get_slot_weapons], rows sorted case-insensitively ascending [orig:
# ListWidget_SortRows -> cmp @0x6448a0, mode (string, asc)] under NONE at row 0.
# Deferred (tracked in the armory RE notes): the per-weapon availability table
# [orig: g_armoryWeaponAvailability @0x24D5600 — mission/S2C/host-authored], the
# per-class 2048-byte loadout buffers + reselect [orig: g_armoryLoadoutBufferByClass
# @0x25DD740 -> populate_ammo_type_combo_boxes @0x564930], the icon swaps
# [orig: @0x565640 tail, icon table @0x2540D70], the exact ammo row model
# [orig: (row+1) clips @0x565490], and the *_AMMO1_TYPE round-type cascade.

var _menu: Node                      # the built NovaMnuMenu
var _root: NovaResourceRoot
var _weapons: NovaWeaponDatabase
var _team := 0                       # 0 = blue/good, 1 = red/evil (host stamps before open)
# The local player's class + the host's class-allow mask feeding the witnessed
# open-time class resolution [orig: Armory_ResolveSelectedClass @0x5642f0].
# 0 = unclassed (SP spawn before any armory apply); mask default = no restriction.
var _player_class := 0
var _class_allow_mask := 0x3FF
# Class selection is interactive only in an MP session — the original disables the
# PLAYER_CLASS spin (and its label) outside one [orig: the is_in_session branch of
# the WEAPON on-show handler @0x567370]. SP/PIE shells leave this false.
var _class_selection_enabled := false
# The currently equipped primary (weapon.def id); stands in for the per-class loadout
# buffer's reselect until that buffer is ported [orig: select-by-adm-index
# sub_645240 in populate_ammo_type_combo_boxes @0x564930].
var _current_primary := ""
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


## The player's current class (5..9; 0 = unclassed) — the class the screen opens on
## [orig: Armory_ResolveSelectedClass @0x5642f0 starts from entity playerClass].
func set_player_class(player_class: int) -> void:
	_player_class = player_class


## The currently equipped primary; PRIMARY pre-selects its row on populate (the
## per-class loadout-buffer reselect stands deferred; see the header).
func set_current_loadout(primary: String) -> void:
	_current_primary = primary


## MP hosts enable the class spin; SP/PIE leave it disabled like the original
## [orig: @0x567370 enables PLAYER_CLASS + STATIC_PLAYER_CLASS only in-session].
func set_class_selection_enabled(enabled: bool) -> void:
	_class_selection_enabled = enabled


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
# (weapon.mnu authors it empty) with the "Menu" section CHARCLASS_* names, ids 5..9
# [orig: UI_InitWeaponClassSelection @0x567250 — CHARCLASS_MEDIC..ENGINEER, values
# 5..9 via spin_list_insert_item].
const CLASS_VALUES := [5, 6, 7, 8, 9]
const CLASS_KEYS := ["CHARCLASS_MEDIC", "CHARCLASS_SNIPER", "CHARCLASS_GUNNER",
	"CHARCLASS_RIFLEMAN", "CHARCLASS_ENGINEER"]
const CLASS_FALLBACKS := ["Medic", "Sniper", "Gunner", "Rifleman", "Engineer"]

# The g_armorySelectedClass mirror: the class the filters + ACCEPT run against. The
# spin row only writes it through _on_class_changed (MP); an unclassed resolve keeps
# it outside 5..9 while the spin merely SHOWS row 0.
var _selected_class_value := 0

func _populate_classes() -> void:
	_selected_class_value = _resolve_selected_class()
	var spin := _spin("PLAYER_CLASS")
	if spin == null:
		return
	var rows := PackedStringArray()
	for i in CLASS_KEYS.size():
		rows.append(_menu_text(CLASS_KEYS[i], CLASS_FALLBACKS[i]))
	_populating = true
	spin.set_values(rows)
	# Select by VALUE = the resolved class; a class with no row falls back to row 0
	# [orig: SpinList_SelectItemByValue @0x64ba50 selects 0 on no match].
	spin.set_value_index(maxi(CLASS_VALUES.find(_selected_class_value), 0))
	_populating = false
	# Outside an MP session the class spin is inert [orig: @0x567370]. NovaMnuSpinList
	# has no runtime disable; parking the subtree's processing is the host-side stand-in.
	spin.process_mode = Node.PROCESS_MODE_INHERIT if _class_selection_enabled \
			else Node.PROCESS_MODE_DISABLED


# The class the screen opens on: the player's current class when the host allows it,
# else the next allowed class scanning up through 9, else gunner (7)
# [orig: Armory_ResolveSelectedClass @0x5642f0 against g_hostClassAllowMask].
func _resolve_selected_class() -> int:
	var c := _player_class
	if ((1 << c) & _class_allow_mask) == 0:
		c += 1
		while c <= 9 and ((1 << c) & _class_allow_mask) == 0:
			c += 1
		if c > 9:
			c = 7
	return c


# The class filter bit: 1 << (class - 5) for the five soldier classes; any other
# class (an unclassed SP spawn) filters NOTHING — the witnessed default is ALL
# weapons [orig: Armory_ResolveSelectedClass @0x5642f0 switch default mask = -1].
func _class_mask() -> int:
	if _selected_class_value < 5 or _selected_class_value > 9:
		return -1
	return 1 << (_selected_class_value - 5)


func _on_class_changed(index: int, _value: String) -> void:
	if _populating:
		return
	# The flip is MP-only in the original (the spin is disabled otherwise); saving the
	# outgoing class's selections into its per-class buffer is the tracked deferral.
	_selected_class_value = CLASS_VALUES[clampi(index, 0, CLASS_VALUES.size() - 1)]
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
	var dicts: Array = _weapons.get_slot_weapons(slot, _class_mask(), team_mask)
	# Rows are sorted case-insensitively ascending by display label before NONE is
	# prepended at row 0 [orig: ListWidget_SortRows -> cmp @0x6448a0 with (string, asc);
	# NONE inserted at 0 @0x566f15].
	var labeled: Array = []
	for w in dicts:
		labeled.append([_weapon_label(w), w])
	labeled.sort_custom(func(a, b): return String(a[0]).nocasecmp_to(String(b[0])) < 0)
	var rows := PackedStringArray()
	rows.append(_menu_text("NONE", "None"))
	dicts = []
	for pair in labeled:
		rows.append(pair[0])
		dicts.append(pair[1])
	_slot_rows[control] = dicts
	_set_combo_items(combo, rows)
	# PRIMARY re-selects the equipped weapon's row — the loadout-buffer reselect's
	# stand-in [orig: sub_645240 select-by-adm-index in @0x564930]; the other slots
	# stay on NONE until the per-class buffer (and multi-slot inventory) lands.
	if control == "PRIMARY" and not _current_primary.is_empty():
		for i in dicts.size():
			if String(dicts[i].get("name", "")).nocasecmp_to(_current_primary) == 0:
				combo.select_silent(i + 1)
				break


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
		# -1 = the def default; the original's main leg takes adm[23] RAW as the total
		# (only the sub-weapon leg multiplies by clipsize) [orig: @0x565cd0 0x566166].
		return -1
	return combo.get_selected()  # rows are the counts 0..maxclips; index == count


# --- Weight ----------------------------------------------------------------------

# Total loadout weight = weaponweight + clips * clipweight per selected slot (the
# WEAPON screen weighs the selected ammo TYPE's own def — deferred with the type
# combos) [orig: calculate_equipped_weapons_weight @0x565490 — adm[85]/65536 +
# (row+1) * ammoDef[84]/65536]. Rendered into STATIC_TOTAL_WEIGHT as
# "<TOTAL_WEIGHT> <w> <LBS> (<encumbrance>)" with the witnessed encumbrance bands
# <33.3 LIGHT / <66.6 NORMAL / else HEAVY [orig: update_weapon_weight_display
# @0x565640 — sprintf "%s %.1f %s (%s)"].
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
	var encumbrance := _menu_text("LIGHT_ENCUMBRANCE", "Light")
	if total >= 66.6:
		encumbrance = _menu_text("HEAVY_ENCUMBRANCE", "Heavy")
	elif total >= 33.3:
		encumbrance = _menu_text("NORMAL_ENCUMBRANCE", "Normal")
	var node := _find("STATIC_TOTAL_WEIGHT")
	if node != null and node is Label:
		(node as Label).text = "%s %.1f %s (%s)" % [
			_menu_text("TOTAL_WEIGHT", "Total Weight"), total,
			_menu_text("LBS", "lbs"), encumbrance]


# --- ACCEPT / CANCEL ---------------------------------------------------------------

# Collect the selection the way the original serializes it before applying/sending
# [orig: WeaponLoadout_SerializeSelectionsToBuffer @0x5658b0 {name, ammoPri, ammoSec,
# flags} per slot, into the PER-CLASS 2048-byte buffer @0x25DD740 + 2048*class]. The
# host applies primary to the sim + viewmodel; the full multi-slot inventory is the
# runtime's tracked gap.
func _on_accept() -> void:
	var loadout := {
		"player_class": _selected_class_value,
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
