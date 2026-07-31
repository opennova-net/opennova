class_name ArmoryMenuCompanion
extends RefCounted

const GRENADE_CONTROLS := ["GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"]

# Drives the JO in-game armory — weapon.mnu's WEAPON screen — by control NAME, as a
# companion the game-agnostic NovaMenuShell (nova_menu_shell.gd) delegates to (the
# mp_menu_companion / player_info_menu_companion pattern). The original registers exactly these
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
# Slot population is the witnessed class/team/availability filter of the
# WEAPON-screen populate [orig: populate_three_category_lists @0x566db0 via
# NovaWeaponDatabase.get_slot_weapons + the g_armoryWeaponAvailability term
# @0x566e6b — value semantics in net-re §5.63], rows sorted case-insensitively
# ascending [orig: ListWidget_SortRows -> cmp @0x6448a0, mode (string, asc)]
# under NONE at row 0; every slot reselects from the canonical parent tuples.
# Deferred (tracked in the armory RE notes): the per-class 2048-byte loadout
# buffer MEMORY (save-on-flip + remembered counts) [orig:
# g_armoryLoadoutBufferByClass @0x25DD740 -> populate_ammo_type_combo_boxes
# @0x564930], the icon swaps [orig: @0x565640 tail, icon table @0x2540D70], the
# *_AMMO2 controls, and the *_AMMO1_TYPE round-type cascade.

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
# the WEAPON on-show handler @0x567370]. SP leaves this false.
var _class_selection_enabled := false
# The current kit's canonical parent weapon.def ids; each slot reselects its row
# [orig: g_armoryLoadoutBufferByClass -> select-by-adm-index sub_645240 in
# populate_ammo_type_combo_boxes @0x564930]. Per-class buffer MEMORY stays deferred.
var _current_primary := ""
var _current_secondary := ""
var _current_accessory := ""
var _current_grenades: Array = []
# Canonical parent clip counts keyed by PRIMARY / SECONDARY / ACCESSORY. The
# visible row is zero-based while the buffer value is one-based; -1 selects the
# authored maximum [orig: @0x564c7d..0x564ce4].
var _current_parent_clips := {}
# Availability lookup (name -> value); banned (0) weapons drop from the lists
# [orig: the g_armoryWeaponAvailability term @0x566e6b]. Invalid = allow all.
var _availability_lookup := Callable()
var _populating := false
# Selected weapon dicts per slot control name ("" row 0 = NONE).
var _slot_rows := {}                 # control name -> Array[Dictionary] (row-1 aligned)
# The grenade definitions assigned to GRENADE_AMMO1..3 in weapon.def table order.
# Each authored control selects a count for its definition rather than a weapon row
# [orig: sub_566D70 registration args 0/1/2 @0x567020].
var _grenade_rows: Array = []
# The ACCEPT-hotkey debounce: the opener press that showed the screen must release
# once before the key acts as ACCEPT — the open stamps it, only the row's KEYUP
# arms it [orig: g_weaponScreenOpenDebounce = 1 at the open @0x4e0b21; cleared by
# Input_HandleMenuKeyRelease @0x4de2d0].
var _accept_hotkey_armed := false

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


## The current canonical parent tuples; each slot pre-selects its row on populate.
## The per-class loadout-buffer MEMORY (remembered ammo counts, save-on-class-flip)
## stands deferred; initial selection reads the active authoritative tuple buffer
## [orig: populate_ammo_type_combo_boxes @0x564930 select-by-adm-index].
func set_current_loadout(primary: String, secondary: String = "",
		accessory: String = "", grenades: Array = [],
		parent_clips: Dictionary = {}) -> void:
	_current_primary = primary
	_current_secondary = secondary
	_current_accessory = accessory
	_current_grenades = grenades.duplicate(true)
	_current_parent_clips = {
		"PRIMARY": int(parent_clips.get("PRIMARY", -1)),
		"SECONDARY": int(parent_clips.get("SECONDARY", -1)),
		"ACCESSORY": int(parent_clips.get("ACCESSORY", -1)),
	}


## Availability lookup (weapon name -> 0 banned / 1 allowed / 2 armory-zone-only /
## 3 mission-allowed); banned weapons drop from every slot list
## [orig: the g_armoryWeaponAvailability term of populate_three_category_lists
## @0x566e6b — any nonzero value lists]. Unset = everything allowed.
func set_availability_lookup(lookup: Callable) -> void:
	_availability_lookup = lookup


## MP hosts enable the class spin; SP leaves it disabled like the original
## [orig: @0x567370 enables PLAYER_CLASS + STATIC_PLAYER_CLASS only in-session].
func set_class_selection_enabled(enabled: bool) -> void:
	_class_selection_enabled = enabled


# Inject a pre-loaded weapon.def database (ADR 0018 seam: tests and owners that
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
		_connect_combo(slot_name + "_AMMO1", _on_ammo_selected)
	for control in GRENADE_CONTROLS:
		_connect_combo(control, _on_ammo_selected)
	_connect_pressed("ACCEPT", _on_accept)   # [orig: @0x5671f6 arg 0]
	_connect_pressed("CANCEL", _on_cancel)   # [orig: @0x567214 arg 1 skips the apply]
	# The on-show re-registers the ACCEPT hotkeys and the open re-stamps the
	# debounce [orig: CUIWidget_ResetScreenHotkeys/AddScreenHotkey @0x567483..
	# 0x5674c0; g_weaponScreenOpenDebounce = 1 @0x4e0b21].
	_accept_hotkey_armed = false
	_update_weight()


func _ensure_weapons() -> void:
	if _weapons != null or _root == null:
		return
	_weapons = NovaWeaponDatabase.new()
	if _weapons.load_from_resource_root(_root, "weapon.def") != OK or not _weapons.is_loaded():
		push_warning("ArmoryMenuCompanion: weapon.def not loaded (%s); armory lists stay empty"
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
	# has no runtime disable; parking the subtree's processing is the shell-side stand-in.
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
	_populate_grenades(team_mask)
	for slot_name in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		_populate_ammo(slot_name)


func _available_slot_weapons(slot: int, team_mask: int) -> Array:
	var dicts: Array = _weapons.get_slot_weapons(slot, _class_mask(), team_mask)
	if _availability_lookup.is_valid():
		dicts = dicts.filter(func(w):
			return int(_availability_lookup.call(String(w.get("name", "")))) != 0)
	return dicts


func _fill_slot(control: String, slot: int, team_mask: int) -> void:
	var combo := _combo(control)
	if combo == null:
		return
	var dicts: Array = _available_slot_weapons(slot, team_mask)
	# The map availability term: banned (0) weapons never list; every nonzero value
	# (allowed / armory-zone-only / mission-allowed) does
	# [orig: the !g_armoryWeaponAvailability[i] skip @0x566e6b].
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
	# Each slot re-selects its row from the canonical parent tuples [orig: sub_645240
	# select-by-adm-index in @0x564930]; the per-class buffer MEMORY stays deferred.
	var current := ""
	match control:
		"PRIMARY": current = _current_primary
		"SECONDARY": current = _current_secondary
		"ACCESSORY": current = _current_accessory
	if not current.is_empty():
		for i in dicts.size():
			if String(dicts[i].get("name", "")).nocasecmp_to(current) == 0:
				combo.select_silent(i + 1)
				break


# weapon.mnu has no parent GRENADE weapon combo: its three authored controls are
# count selectors for the class/team/selectable grenade definitions in table
# order. Availability is applied only after a definition owns its control, so a
# banned grenade retains that position with a zero-only row. Their
# registered callback args 0/1/2 address the same three positions
# [orig: WeaponDef_RegisterUICallbacks @0x567020 -> sub_566D70].
func _populate_grenades(team_mask: int) -> void:
	# Unlike the three parent lists, availability does not remove a grenade def:
	# retail assigns class/team/selectable category-3 defs to controls first, then
	# an unavailable def stops after its zero row [orig: @0x5647a4..0x5648a6].
	var dicts: Array = _weapons.get_slot_weapons(
			NovaWeaponDatabase.SLOT_GRENADE, _class_mask(), team_mask)
	_grenade_rows = []
	for i in mini(dicts.size(), GRENADE_CONTROLS.size()):
		_grenade_rows.append(dicts[i])
	for i in GRENADE_CONTROLS.size():
		var combo := _combo(GRENADE_CONTROLS[i])
		if combo == null:
			continue
		var w: Dictionary = _grenade_rows[i] if i < _grenade_rows.size() else {}
		var allowed := not w.is_empty()
		if allowed and _availability_lookup.is_valid():
			allowed = int(_availability_lookup.call(String(w.get("name", "")))) != 0
		var maxclips := int(w.get("maxclips", 0)) if allowed else 0
		var rows := PackedStringArray()
		for clips in range(0, maxclips + 1):
			rows.append(_ammo_row_label(w, clips))
		_set_combo_items(combo, rows)
		if not w.is_empty():
			combo.select_silent(_current_grenade_clips(
					String(w.get("name", "")), maxclips))
	_update_weight()


func _current_grenade_clips(weapon_name: String, maxclips: int) -> int:
	for value in _current_grenades:
		var row := value as Dictionary
		if String(row.get("name", "")).nocasecmp_to(weapon_name) != 0:
			continue
		var clips := int(row.get("ammo_primary", -1))
		return maxclips if clips < 0 else clampi(clips, 0, maxclips)
	return 0


func _weapon_label(w: Dictionary) -> String:
	var textid := String(w.get("display_textid", ""))
	if not textid.is_empty():
		var t: RtxtStringFile = NovaStrings.get_table("gametext")
		if t != null and t.has_string_in_section("WepDes", textid):
			return t.get_string_in_section("WepDes", textid)
	return String(w.get("name", ""))


func _ammo_row_label(w: Dictionary, clips: int) -> String:
	var round_label := String(w.get("round_type", ""))
	if not round_label.is_empty():
		var gametext: RtxtStringFile = NovaStrings.get_table("gametext")
		if gametext != null and gametext.has_string_in_section("WepDes", round_label):
			round_label = gametext.get_string_in_section("WepDes", round_label)
	return "%d - %s" % [
		clips * int(w.get("clipsize", 0)),
		round_label,
	]


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


func _on_ammo_selected(_row: int, _value: String) -> void:
	if _populating:
		return
	_update_weight()


# Clip-count rows for the slot's ammo combo. Retail row 0 means one clip, shows
# clipsize rounds, and serializes as 1; row maxclips-1 is the full load
# [orig: @0x564c7d..0x564ce4, sprintf "%d - %s"].
func _populate_ammo(control: String) -> void:
	var combo := _combo(control + "_AMMO1")
	if combo == null:
		return
	var w := selected_weapon(control)
	var rows := PackedStringArray()
	var maxclips := int(w.get("maxclips", 0))
	for clips in range(1, maxclips + 1):
		rows.append(_ammo_row_label(w, clips))
	_set_combo_items(combo, rows)
	if not rows.is_empty():
		var clips := int(_current_parent_clips.get(control, -1))
		var current_name := ""
		match control:
			"PRIMARY": current_name = _current_primary
			"SECONDARY": current_name = _current_secondary
			"ACCESSORY": current_name = _current_accessory
		if (current_name.is_empty()
				or String(w.get("name", "")).nocasecmp_to(current_name) != 0
				or clips < 0):
			clips = maxclips
		combo.select_silent(clampi(clips, 1, maxclips) - 1)
	_update_weight()


# The slot's selected clip count. Public read seam (ADR 0018), paired with
# selected_weapon.
func selected_clips(control: String) -> int:
	var combo := _combo(control + "_AMMO1")
	if combo == null or combo.get_selected() < 0:
		# -1 = the def default; the original's main leg takes adm[23] RAW as the total
		# (only the sub-weapon leg multiplies by clipsize) [orig: @0x565cd0 0x566166].
		return -1
	return combo.get_selected() + 1


func _selected_grenade_loadout() -> Array[Dictionary]:
	var selected: Array[Dictionary] = []
	for i in _grenade_rows.size():
		var combo := _combo(GRENADE_CONTROLS[i])
		var clips := combo.get_selected() if combo != null else 0
		if clips <= 0:
			continue
		selected.append({
			"name": String((_grenade_rows[i] as Dictionary).get("name", "")),
			"ammo_primary": clips,
			"ammo_secondary": -1,
			"flags": -1,
		})
	return selected


# --- Weight ----------------------------------------------------------------------

# Total loadout weight = weaponweight + clips * clipweight per selected slot (the
# WEAPON screen weighs the selected ammo TYPE's own def — deferred with the type
# combos) [orig: calculate_equipped_weapons_weight @0x565490 — adm[85]/65536 +
# (row+1) * ammoDef[84]/65536]. Rendered into STATIC_TOTAL_WEIGHT as
# "<TOTAL_WEIGHT> <w> <LBS> (<encumbrance>)" with the witnessed encumbrance bands
# <33.3 LIGHT / <66.6 NORMAL / else HEAVY [orig: update_weapon_weight_display
# @0x565640 — sprintf "%s %.1f %s (%s)"].
# The parent-slot sum and the encumbrance bands ride the libs/def port shared
# with PLAYER_INFO (NovaWeaponDatabase.loadout_weight/encumbrance_class —
# [orig: calculate_loadout_weight @0x55f1f0 sibling]; ctest def_loadout_weight
# pins the formula and the exact thresholds).

func _update_weight() -> void:
	if _weapons == null:
		return  # weapon.def absent: the screen degrades with empty slot lists
	var indices := PackedInt32Array()
	var counts := PackedInt32Array()
	for slot_name in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		var w := selected_weapon(slot_name)
		if w.is_empty():
			continue
		indices.append(int(w.get("index", -1)))
		counts.append(selected_clips(slot_name))  # -1 = the def default (maxclips)
	var total := _weapons.loadout_weight(indices, counts)
	for i in _grenade_rows.size():
		var combo := _combo(GRENADE_CONTROLS[i])
		var clips := combo.get_selected() if combo != null else 0
		if clips <= 0:
			continue
		var grenade := _grenade_rows[i] as Dictionary
		# The category-3 controls are extra-ammo legs, not parent weapon slots:
		# retail adds selected_row * adm[84] only [orig: @0x5655c9..0x56561c].
		total += clips * float(grenade.get("clip_weight", 0.0))
	var band := _weapons.encumbrance_class(total)
	var encumbrance := _menu_text("LIGHT_ENCUMBRANCE", "Light")
	if band == NovaWeaponDatabase.ENCUMBRANCE_HEAVY:
		encumbrance = _menu_text("HEAVY_ENCUMBRANCE", "Heavy")
	elif band == NovaWeaponDatabase.ENCUMBRANCE_NORMAL:
		encumbrance = _menu_text("NORMAL_ENCUMBRANCE", "Normal")
	var node := _find("STATIC_TOTAL_WEIGHT")
	var label := node as Label
	if label == null and node != null:
		label = node.find_child("Label", false, false) as Label
	if label != null:
		label.text = "%s %.1f %s (%s)" % [
			_menu_text("TOTAL_WEIGHT", "Total Weight"), total,
			_menu_text("LBS", "lbs"), encumbrance]


# --- ACCEPT / CANCEL ---------------------------------------------------------------

# Collect the selection the way the original serializes it before applying/sending
# [orig: WeaponLoadout_SerializeSelectionsToBuffer @0x5658b0 {name, ammoPri, ammoSec,
# flags} per slot, into the PER-CLASS 2048-byte buffer @0x25DD740 + 2048*class]. The
# host applies the full canonical kit to the sim; the selected slot then remounts
# its first-person viewmodel.
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
		"grenades": _selected_grenade_loadout(),
	}
	loadout_accepted.emit(loadout)


## Programmatic ACCEPT — the hotkey-accelerator path. The WEAPON screen's on-show
## registers the USE-ITEM binding row's runtime keys on the ACCEPT control, so the
## armory-opener key doubles as ACCEPT while the screen is up
## [orig: UI_InitTeamClassSelection @0x567370 — control "ACCEPT" gains
##  g_useItemBindingKey0/1 via CUIWidget_AddScreenHotkey @0x5674a8/@0x5674c0].
## Same collect + apply as clicking the button.
func trigger_accept() -> void:
	_on_accept()


## One armory-hotkey edge (true = pressed). Returns true when the edge triggered
## ACCEPT. The release ARMS the key (the opener press that showed the screen must
## release once [orig: Input_HandleMenuKeyRelease @0x4de2d0 clears the open
## debounce]); an armed press is the ACCEPT accelerator [orig: the on-show
## registration @0x5674a8]. NovaArmoryPresenter routes key input here while open.
func accept_hotkey_edge(pressed: bool) -> bool:
	if not pressed:
		_accept_hotkey_armed = true
		return false
	if not _accept_hotkey_armed:
		return false
	_accept_hotkey_armed = false
	trigger_accept()
	return true


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
	# [orig: the armory's menu tokens resolve against the menu resource (game.bin)
	#  via TextResource_GetStringWithFallback(resource, "Menu", key) @0x562ee0]
	for spec in [["menutxt", "Menu"], ["gameui", "Menu"]]:
		var t: RtxtStringFile = NovaStrings.get_table(spec[0])
		if t != null and t.has_string_in_section(spec[1], key):
			return t.get_string_in_section(spec[1], key)
	return fallback


func _find(name: String) -> Node:
	return _menu.find_child(name, true, false) if _menu != null else null
