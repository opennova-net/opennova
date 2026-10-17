class_name ArmoryMenuCompanion
extends MenuCompanion

# Drives the JO in-game armory — weapon.mnu's WEAPON screen — by control NAME, as a
# companion the game-agnostic MenuShell (menu_shell.gd) delegates to (the
# mp_menu_companion / player_info_menu_companion pattern), riding a MenuDriver
# over the compiled MenuFrame surface. The original registers exactly these
# controls on the "WEAPON" screen [orig: WeaponDef_RegisterUICallbacks @0x567020,
# run once from the screen's INIT event @0x567250]:
#   PLAYER_CLASS (spinlist)                  -> UI_HandleTeamClassSelection @0x566f60
#   PRIMARY / SECONDARY / ACCESSORY (combos) -> UI_OnPrimaryWeaponTypeChanged @0x5662d0 /
#                                               UI_OnSecondaryWeaponChanged @0x566670 /
#                                               UI_OnWeaponAmmoSlotChanged @0x566a10
#   PRIMARY_AMMO1/2, SECONDARY_AMMO1/2, ACCESSORY_AMMO1/2, GRENADE_AMMO1..3,
#   *_AMMO1_TYPE (combos)                    -> PlayerInfo_PopulateAmmoComboBoxes @0x55def0
#   ACCEPT / CANCEL (buttons)                -> WeaponLoadout_ApplyFromBuffer @0x565cd0
# The screen opens in-match from the USE-ITEM key (action 177 "useitem", retail default
# SHIFT per the shipped KeyChart) while the player stands in a type-6 armory volume
# (entity Flags 0x400000) [orig: Input_HandleActionBinding_0 case 0xB1 @0x4e0b3f;
# the parallel action 218 @0x49b8e3 ships with no binding row] — the presenter owns
# that key + gate and the ACCEPT apply (sim + FP viewmodel rebuild).
#
# Slot population is the engine's (runtime/menu/loadout_screen.h
# loadout_fill_slot): the witnessed class/team/availability filter of the
# WEAPON-screen populate (the availability values' semantics in net-re §5.63),
# the rows sorted case-insensitively ascending under NONE at row 0; every slot
# reselects from the canonical parent tuples (loadout_select_weapon_name).
# Deferred (tracked in the armory RE notes): the per-class 2048-byte loadout
# buffer MEMORY (save-on-flip + remembered counts) [orig:
# g_ArmoryLoadoutBufferByClass @0x25DD740 -> UI_PopulateAmmoTypeComboBoxes
# @0x564930], the *_AMMO2 controls, and the *_AMMO1_TYPE round-type cascade.

var _team := 0                       # 0 = blue/good, 1 = red/evil (host stamps before open)
# The local player's class + the host's class-allow mask feeding the witnessed
# open-time class resolution [orig: Armory_ResolveSelectedClass @0x5642f0].
# 0 = unclassed (SP spawn before any armory apply); the no-restriction mask
# default lives at engine world/player_loadout.h kClassAllowMaskAll
# [orig: g_HostClassAllowMask default 0x3FF].
var _player_class := 0
var _class_allow_mask := WeaponDatabase.CLASS_ALLOW_ALL
# Class selection is interactive only in an MP session — the original disables the
# PLAYER_CLASS spin (and its label) outside one [orig: the is_in_session branch of
# the WEAPON on-show handler @0x567370]. SP leaves this false.
var _class_selection_enabled := false
# The current kit's canonical parent weapon.def ids; each slot reselects its row
# [orig: g_ArmoryLoadoutBufferByClass -> select-by-adm-index UIList_SelectByValue @0x645240 in
# UI_PopulateAmmoTypeComboBoxes @0x564930]. Per-class buffer MEMORY stays deferred.
var _current_primary := ""
var _current_secondary := ""
var _current_accessory := ""
var _current_grenades: Array = []
# Canonical parent clip counts keyed by PRIMARY / SECONDARY / ACCESSORY. The
# visible row is zero-based while the buffer value is one-based; -1 selects the
# authored maximum [orig: @0x564c7d..0x564ce4].
var _current_parent_clips := {}
# Availability lookup (name -> value); banned (0) weapons drop from the lists
# [orig: the g_ArmoryWeaponAvailability term @0x566e6b]. Invalid = allow all.
var _availability_lookup := Callable()
# Each category list's weapon.def rows by list row as its fill left them (row 0
# NONE, -1; engine loadout_screen.h loadout_fill_slot).
var _slot_rows := {}                 # control name -> PackedInt32Array
# The grenade definitions assigned to GRENADE_AMMO1..3 in weapon.def table order.
# Each authored control selects a count for its definition rather than a weapon row
# [orig: WeaponDef_UISlotSelectCallback registration args 0/1/2 @0x567020].
var _grenade_rows: Array[WeaponDef] = []

signal loadout_accepted(loadout: Dictionary)
signal armory_closed


# weapon.mnu's WEAPON screen: PLAYER_CLASS (spinlist) + PRIMARY_AMMO1 are unique to it
# (player.mnu uses PLAYERCLASS, no ammo combos).
func owns_menu(driver: MenuDriver) -> bool:
	if driver == null:
		return false
	return driver.has_widget("PLAYER_CLASS") and driver.has_widget("PRIMARY_AMMO1")


func set_player_team(team: int) -> void:
	_team = team


## The player's current class (5..9; 0 = unclassed) — the class the screen opens on
## [orig: Armory_ResolveSelectedClass @0x5642f0 starts from entity playerClass].
func set_player_class(player_class: int) -> void:
	_player_class = player_class


## Host class policy received in S2C 0x76. Keep the complete wire u16: retail's
## resolver indexes the class-id bits directly (5..9), while malformed 0x76 sets 0.
## [orig: NapiNPClientMsg_HandleClassAllowMask @0x42d540;
## Armory_ResolveSelectedClass @0x5642f0]
func set_class_allow_mask(mask: int) -> void:
	_class_allow_mask = mask & 0xFFFF


## The current canonical parent tuples; each slot pre-selects its row on populate.
## The per-class loadout-buffer MEMORY (remembered ammo counts, save-on-class-flip)
## stands deferred; initial selection reads the active authoritative tuple buffer
## [orig: UI_PopulateAmmoTypeComboBoxes @0x564930 select-by-adm-index].
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
## [orig: the g_ArmoryWeaponAvailability term of UI_PopulateThreeCategoryLists
## @0x566e6b — any nonzero value lists]. Unset = everything allowed.
func set_availability_lookup(lookup: Callable) -> void:
	_availability_lookup = lookup


## MP hosts enable the class spin; SP leaves it disabled like the original
## [orig: @0x567370 enables PLAYER_CLASS + STATIC_PLAYER_CLASS only in-session].
func set_class_selection_enabled(enabled: bool) -> void:
	_class_selection_enabled = enabled


# Inject a pre-loaded weapon.def database (ADR 0018 seam: tests and owners that
# already carry the db hand it in; on_menu_built otherwise loads it from the root).
func set_weapon_database(weapons: WeaponDatabase) -> void:
	_set_weapons(weapons)


# The presenter's close() releases the companion: every open rebuilds via
# on_menu_built, so the hidden frame keeps no icon mounts between shows.
func on_menu_released() -> void:
	super()
	_clear_icon_mounts()


func on_menu_built(driver: MenuDriver, file: String, screen: String, root: ResourceRoot) -> void:
	# The base wires _driver/_root, the by-name handler table (dispatched
	# with the stale-document guard) and the widget_activated relay.
	super(driver, file, screen, root)
	_clear_icon_mounts()
	if not driver.widget_value_changed.is_connected(_on_widget_value_changed):
		driver.widget_value_changed.connect(_on_widget_value_changed)
	if not driver.screen_changed.is_connected(_on_screen_changed):
		driver.screen_changed.connect(_on_screen_changed)
	var frame := driver.get_frame()
	if frame != null and not frame.resized.is_connected(_reposition_icon_mounts):
		frame.resized.connect(_reposition_icon_mounts)
	_ensure_weapons("ArmoryMenuCompanion", "armory lists stay empty")
	_populate_classes()
	_populate_slots()
	# The screen's ACCEPT / CANCEL do the commands the game binds on it (engine
	# menu_commands.h: WEAPON ACCEPT -> apply_loadout, CANCEL -> cancel_loadout)
	# [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 -- ACCEPT's arg 0 @0x5671f6,
	# CANCEL's arg 1 @0x567214 skips the apply].
	for control in ["ACCEPT", "CANCEL"]:
		match driver.screen_command(screen, control, true):
			"apply_loadout":
				_activation_handlers[control] = _on_accept
			"cancel_loadout":
				_activation_handlers[control] = _on_cancel
	# The on-show's ACCEPT hotkeys (the use-item keys) have no state here:
	# ArmoryPresenter routes their presses to trigger_accept past the engine's
	# armory press latch
	# [orig: the calls of CUIWidget_ResetScreenHotkeys @0x649ce0 and
	# CUIWidget_AddScreenHotkey @0x649e20 @0x567483..0x5674c0].
	_update_weight()


# PLAYER_CLASS carries the five MP soldier classes; the host fills the spinlist
# (weapon.mnu authors it empty) from the engine catalog rows
# ({value:int, text_key:String} in the authored spin order — the witness
# [orig: UI_InitWeaponClassSelection @0x567250 — CHARCLASS_MEDIC..ENGINEER,
# values 5..9 via CSpinListWnd_InsertItem] lives at engine
# world/player_loadout.h kArmoryClassCatalog).
var _class_catalog: Array[ArmoryClassRow] = WeaponDatabase.armory_class_catalog()
# Display FALLBACK strings stay godot-side, keyed by the catalog row's text_key.
const CLASS_FALLBACK_TEXT := {
	"CHARCLASS_MEDIC": "Medic",
	"CHARCLASS_SNIPER": "Sniper",
	"CHARCLASS_GUNNER": "Gunner",
	"CHARCLASS_RIFLEMAN": "Rifleman",
	"CHARCLASS_ENGINEER": "Engineer",
}

# The g_ArmorySelectedClass mirror: the class the filters + ACCEPT run against. The
# spin row only writes it through _on_class_changed (MP); an unclassed resolve keeps
# it outside 5..9 while the spin merely SHOWS row 0.
var _selected_class_value := 0

func _populate_classes() -> void:
	_selected_class_value = _resolve_selected_class()
	var spin := _id("PLAYER_CLASS")
	if spin < 0:
		return
	var rows := PackedStringArray()
	for row_v in _class_catalog:
		var text_key := row_v.text_key
		rows.append(Strings.menu_text(text_key,
				String(CLASS_FALLBACK_TEXT.get(text_key, text_key))))
	_populating = true
	_driver.set_widget_items(spin, rows)
	# The host's class mask enables each class row, then the select (the engine's
	# menu::enable_class_rows, the on-show order of UI_InitTeamClassSelection).
	_driver.enable_class_rows(spin, _class_allow_mask)
	# Select by VALUE = the resolved class; a class with no row falls back to row 0
	# [orig: SpinList_SelectItemByValue @0x64ba50 selects 0 on no match].
	_driver.select_row(spin, maxi(_class_row_for_value(_selected_class_value), 0), false)
	_populating = false
	# Outside an MP session the class spin is inert — the original disables it
	# [orig: @0x567370]; the driver's disable is the pump's visual state 1.
	_driver.set_widget_disabled(spin, not _class_selection_enabled)


# The class the screen opens on — the engine's one impl (world/player_loadout
# armory_resolve_selected_class (engine: runtime/world/player_loadout.cpp)).
func _resolve_selected_class() -> int:
	return WeaponDatabase.armory_resolve_selected_class(
			_player_class, _class_allow_mask)


# The class filter bit — the engine's one impl (world/player_loadout
# armory_class_filter_mask (engine: runtime/world/player_loadout.cpp)).
func _class_mask() -> int:
	return WeaponDatabase.armory_class_filter_mask(_selected_class_value)


# The catalog row carrying a class VALUE, -1 when no row does (the resolved
# class can sit outside 5..9 on an unclassed resolve).
func _class_row_for_value(class_value: int) -> int:
	for i in _class_catalog.size():
		if _class_catalog[i].value == class_value:
			return i
	return -1


func _on_class_changed(index: int) -> void:
	if _populating:
		return
	# The flip is MP-only in the original (the spin is disabled otherwise); saving the
	# outgoing class's selections into its per-class buffer is the tracked deferral.
	var row := clampi(index, 0, _class_catalog.size() - 1)
	_selected_class_value = _class_catalog[row].value
	_populate_slots()  # the class re-filters every slot list [orig: @0x566f60]
	_update_weight()


# --- Slot lists -----------------------------------------------------------------

func _populate_slots() -> void:
	if _weapons == null:
		return
	# [orig: g_PlayerInfoTeamMask = 2 - (team != 0)] — the witness lives at
	# engine world/player_loadout.h player_info_team_mask.
	var team_mask := WeaponDatabase.player_info_team_mask(_team)
	_fill_slot("PRIMARY", WeaponDatabase.SLOT_PRIMARY, team_mask)
	_fill_slot("SECONDARY", WeaponDatabase.SLOT_SECONDARY, team_mask)
	_fill_slot("ACCESSORY", WeaponDatabase.SLOT_ACCESSORY, team_mask)
	_populate_grenades(team_mask)
	for slot_name in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		_populate_ammo(slot_name)


# The list's rows (the class/team filter, the map availability term, the
# engine's row order under NONE), then the reselect of the canonical parent
# tuple's weapon by name; the per-class buffer MEMORY stays deferred (engine
# loadout_screen.h loadout_fill_slot / loadout_select_weapon_name carry the
# witnesses).
func _fill_slot(control: String, slot: int, team_mask: int) -> void:
	if _id(control) < 0:
		return
	_populating = true
	var rows := _driver.fill_loadout_slot(_weapons, control, slot, _class_mask(), team_mask,
			_availability_lookup, Strings.menu_text("NONE", "None"),
			Strings.get_table(Strings.TABLE_GAMETEXT))
	_populating = false
	_slot_rows[control] = rows
	_driver.select_loadout_weapon_name(_weapons, control, rows, _current_weapon_name(control))


# The canonical parent tuple's weapon name for a category ("" for none).
func _current_weapon_name(control: String) -> String:
	match control:
		"PRIMARY":
			return _current_primary
		"SECONDARY":
			return _current_secondary
		"ACCESSORY":
			return _current_accessory
	return ""


func _populate_grenades(team_mask: int) -> void:
	_populating = true
	_grenade_rows = _driver.fill_armory_grenades(_weapons, _class_mask(), team_mask,
			_current_grenades, _availability_lookup, Strings.get_table(Strings.TABLE_GAMETEXT))
	_populating = false
	_update_weight()


# The slot's selected weapon.def row (null = NONE). Public read seam (ADR
# 0018): tests and diagnostics read the selection here.
func selected_weapon(control: String) -> WeaponDef:
	if _weapons == null or _driver == null:
		return null
	return _weapons.get_weapon(_driver.loadout_selected_weapon(control, _rows_of(control)))


# A category list's rows as its last fill left them (none before one).
func _rows_of(control: String) -> PackedInt32Array:
	return _slot_rows.get(control, PackedInt32Array())


func _on_slot_selected(control: String) -> void:
	if _populating:
		return
	_populate_ammo(control)
	_update_weight()


func _populate_ammo(control: String) -> void:
	if _id(control + "_AMMO1") < 0:
		return
	var w := selected_weapon(control)
	_populating = true
	_driver.fill_armory_ammo(_weapons, control, w.index if w != null else -1,
			_current_weapon_name(control), int(_current_parent_clips.get(control, -1)),
			Strings.get_table(Strings.TABLE_GAMETEXT))
	_populating = false
	_update_weight()


# The slot's selected clip count, -1 the def default (engine loadout_screen.h
# armory_selected_clips carries the witness). Public read seam (ADR 0018),
# paired with selected_weapon.
func selected_clips(control: String) -> int:
	return _driver.armory_selected_clips(control) if _driver != null else -1


# --- Weight ----------------------------------------------------------------------

func _update_weight() -> void:
	if _weapons == null:
		_update_icons()
		return
	var parents: Array[WeaponDef] = []
	for slot_name in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		parents.append(selected_weapon(slot_name))
	var total := _driver.armory_loadout_weight(_weapons, parents, _grenade_rows)
	var label := _id("STATIC_TOTAL_WEIGHT")
	if label >= 0:
		_driver.set_widget_text(label, WeaponDatabase.loadout_weight_line(total,
				Strings.get_table(Strings.TABLE_MENUTXT), Strings.get_table(Strings.TABLE_GAMEUI)))
	_update_icons()


# The blank PRIMARY/SECONDARY/ACCESSORY_ICON windows receive the selected
# weapon.def row's loadout_menu_icon (+144); NONE clears the image. Retail does
# this in the same combined refresh as the weight line [orig:
# UI_UpdateWeaponWeightDisplay @0x5657a8..0x5658a3]. TextureRects are mounted as
# frame children because the compiled MenuFrame has no per-widget Control nodes.
func _update_icons() -> void:
	_update_weapon_icons("ArmoryIcon", selected_weapon)


## The rendered weight line (public read seam for tests/diagnostics).
func weight_line() -> String:
	var label := _id("STATIC_TOTAL_WEIGHT")
	return _driver.get_widget_text(label) if label >= 0 else ""


# --- ACCEPT / CANCEL ---------------------------------------------------------------

# Collect the selection the way the original serializes it before applying/sending
# [orig: WeaponLoadout_SerializeSelectionsToBuffer @0x5658b0 {name, ammoPri, ammoSec,
# flags} per slot, into the PER-CLASS 2048-byte buffer @0x25DD740 + 2048*class]. The
# host applies the full canonical kit to the sim; the selected slot then remounts
# its first-person viewmodel.
func _on_accept() -> void:
	# The selection is the engine's pick (engine loadout_screen.h
	# armory_loadout_pick); this builds the presenter's loadout record from it.
	var pick: ArmoryLoadoutPick = _driver.armory_loadout_pick(_weapons, _rows_of("PRIMARY"),
			_rows_of("SECONDARY"), _rows_of("ACCESSORY"), _grenade_rows) \
			if _driver != null else ArmoryLoadoutPick.new()
	var loadout := {
		"player_class": _selected_class_value,
		"team": _team,
		"primary": pick.names[0],
		"primary_clips": pick.clips[0],
		"secondary": pick.names[1],
		"secondary_clips": pick.clips[1],
		"accessory": pick.names[2],
		"accessory_clips": pick.clips[2],
		"grenades": pick.grenades,
	}
	loadout_accepted.emit(loadout)


## Programmatic ACCEPT — the hotkey-accelerator path. The WEAPON screen's on-show
## registers the USE-ITEM binding row's runtime keys on the ACCEPT control, so the
## armory-opener key doubles as ACCEPT while the screen is up
## [orig: UI_InitTeamClassSelection @0x567370 — control "ACCEPT" gains
##  g_UseItemBindingKey0/1 via CUIWidget_AddScreenHotkey @0x649e20, called
##  @0x5674a8/@0x5674c0].
## Same collect + apply as clicking the button.
func trigger_accept() -> void:
	_on_accept()


func _on_cancel() -> void:
	armory_closed.emit()  # [orig: CANCEL skips the apply, clears g_WeaponScreenOpen]


# --- Driver relays -----------------------------------------------------------------

func _on_widget_value_changed(widget_name: String, kind: String, index: int, _value: String) -> void:
	if _populating:
		return
	if kind == "spinlist" and widget_name.nocasecmp_to("PLAYER_CLASS") == 0:
		_on_class_changed(index)
		return
	if kind != "combo":
		return
	match widget_name.to_upper():
		"PRIMARY", "SECONDARY", "ACCESSORY":
			_on_slot_selected(widget_name.to_upper())
		"PRIMARY_AMMO1", "SECONDARY_AMMO1", "ACCESSORY_AMMO1", \
		"GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3":
			_update_weight()


func _on_screen_changed(_screen_name: String) -> void:
	_reposition_icon_mounts()


# --- Helpers -----------------------------------------------------------------------

