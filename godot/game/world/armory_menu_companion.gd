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
#   *_AMMO1_TYPE (combos)                    -> the count and type writes
#   ACCEPT / CANCEL (buttons)                -> WeaponLoadout_ApplyFromBuffer @0x565cd0
# The screen opens in-match from the USE-ITEM key (action 177 "useitem", retail default
# SHIFT per the shipped KeyChart) while the player stands in a type-6 armory volume
# (entity Flags 0x400000) [orig: Input_HandleActionBinding_0 case 0xB1 @0x4e0b3f;
# the parallel action 218 @0x49b8e3 ships with no binding row] — the presenter owns
# that key + gate and the ACCEPT apply (sim + FP viewmodel rebuild).
#
# The screen's state is the engine's ArmoryLoadout (runtime/menu/armory_loadout.h,
# loadout_screen.h): the per-class kit pages, staged from the profile's weapon.sav
# side and the spawn buffer, the screen's counts and ammo-type bytes, and the lists and
# grenade positions; it populates the screen from the selected class's page, takes the
# handlers' writes, saves the page a class flip leaves and the page the ACCEPT applies.
# It lives as long as this companion, which the presenter holds for the process.

# The local player's entity team (1 / 3 blue, 2 / 4 red): the stage's side, the
# weapon filter's team mask and the knife the page leads with (engine
# menu::armory_team_mask). The presenter stamps it before each open.
var _entity_team := 1
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
# The per-class memory and the screen's state (the engine's ArmoryLoadout), and the
# on-show stage's sources: the profile store whose current weapon.sav record the
# pages come from (null: an empty record) and the spawn buffer (the sim's kit).
var _loadout := ArmoryLoadout.new()
var _profiles: PlayerProfiles = null
var _spawn_kit: Array[WeaponKitEntry] = []
# Availability lookup (name -> value); banned (0) weapons drop from the lists
# [orig: the g_ArmoryWeaponAvailability term @0x566e6b]. Invalid = allow all.
var _availability_lookup := Callable()

signal loadout_accepted(kit: Array[WeaponKitEntry], player_class: int)
signal armory_closed


func set_entity_team(entity_team: int) -> void:
	_entity_team = entity_team


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


## The on-show stage's sources: the profile store (its current weapon.sav record) and
## the spawn buffer (engine menu::ArmoryLoadoutBuffers::stage_on_show).
func set_stage_sources(profiles: PlayerProfiles, spawn_kit: Array[WeaponKitEntry]) -> void:
	_profiles = profiles
	_spawn_kit = spawn_kit


## The screen's state, the class pages among it (ADR 0018 read seam; the presenter's
## mission-start stage runs through it).
func get_loadout() -> ArmoryLoadout:
	return _loadout


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
	# The on-show order: the class resolve and the spin, the stage under its team
	# gate, then the populate (engine menu::ArmoryLoadoutBuffers::stage_on_show).
	_populate_classes()
	_loadout.stage_on_show(_profiles, _entity_team, _spawn_kit)
	_populate_slots()
	# The screen's ACCEPT / CANCEL do the commands the game binds on it (engine
	# menu_commands.h: WEAPON ACCEPT -> apply_loadout, CANCEL -> cancel_loadout)
	# [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 -- ACCEPT's arg 0 @0x5671f6,
	# CANCEL's arg 1 @0x567214 skips the apply].
	driver.wire_commands(true)
	for control in ["ACCEPT", "CANCEL"]:
		match driver.screen_command(screen, control):
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

# The g_ArmorySelectedClass mirror: the class the filters, the shown page + ACCEPT run
# against. The spin row only writes it through _on_class_changed (MP); an unclassed
# resolve keeps it outside 5..9 while the spin merely SHOWS row 0.
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


# The catalog row carrying a class VALUE, -1 when no row does (the resolved
# class can sit outside 5..9 on an unclassed resolve).
func _class_row_for_value(class_value: int) -> int:
	for i in _class_catalog.size():
		if _class_catalog[i].value == class_value:
			return i
	return -1


# The flip saves the page it leaves and shows the page of the class it picks (engine
# menu::ArmoryLoadout::flip_class); the spin is live offline too (D-MNU-10).
func _on_class_changed(index: int) -> void:
	if _populating:
		return
	var row := clampi(index, 0, _class_catalog.size() - 1)
	var from_class := _selected_class_value
	_selected_class_value = _class_catalog[row].value
	if _weapons != null:
		_populating = true
		_loadout.flip_class(_driver, _weapons, _entity_team, from_class, _selected_class_value,
				_availability_lookup, Strings.menu_text("NONE", "None"),
				Strings.get_table(Strings.TABLE_GAMETEXT))
		_populating = false
	_update_weight()


# --- Slot lists -----------------------------------------------------------------

# The three category lists, the selected class's page over them and the grenade
# positions (engine menu::ArmoryLoadout::populate).
func _populate_slots() -> void:
	if _weapons == null:
		return
	_populating = true
	_loadout.populate(_driver, _weapons, _entity_team, _selected_class_value,
			_availability_lookup, Strings.menu_text("NONE", "None"),
			Strings.get_table(Strings.TABLE_GAMETEXT))
	_populating = false


# The slot's selected weapon.def row (null = NONE). Public read seam (ADR
# 0018): tests and diagnostics read the selection here.
func selected_weapon(control: String) -> WeaponDef:
	if _weapons == null or _driver == null:
		return null
	return _weapons.get_weapon(_loadout.selected_weapon(_driver, control))


func _on_slot_selected(control: String) -> void:
	if _populating:
		return
	_populating = true
	_loadout.pick_weapon(_driver, _weapons, control, Strings.get_table(Strings.TABLE_GAMETEXT))
	_populating = false
	_update_weight()


# The slot's AMMO1 row + 1, -1 with no row (engine loadout_screen.h
# armory_selected_clips carries the witness). Public read seam (ADR 0018),
# paired with selected_weapon.
func selected_clips(control: String) -> int:
	return _loadout.selected_clips(_driver, control) if _driver != null else -1


# --- Weight ----------------------------------------------------------------------

func _update_weight() -> void:
	if _weapons == null:
		_update_icons()
		return
	var total := _loadout.weight(_driver, _weapons)
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

# The page the screen shows, serialized into the selected class's page, is the kit the
# presenter applies (engine menu::ArmoryLoadout::accept; the page's order, the team's
# knife first, is armory_serialize_page's).
func _on_accept() -> void:
	var kit: Array[WeaponKitEntry] = []
	if _driver != null:
		kit = _loadout.accept(_driver, _weapons, _entity_team, _selected_class_value)
	loadout_accepted.emit(kit, _selected_class_value)


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
		_:
			# The count and type controls' writes (engine menu::ArmoryLoadout::pick_count).
			if _loadout.pick_count(_driver, widget_name, index):
				_update_weight()


func _on_screen_changed(_screen_name: String) -> void:
	_reposition_icon_mounts()
