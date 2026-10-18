class_name PlayerInfoMenuCompanion
extends MenuCompanion

# Drives the JO PLAYER_INFO screen (player.mnu) by control NAME: fills the
# NATIONALITY / DIVISION / COMBO_LIST / PLAYERVOICE comboboxes from Avatars.def and
# runs the nationality -> division -> combo cascade plus the SIDE_BLUE/SIDE_RED team
# filter. It is a companion the game-agnostic MenuShell (menu_shell.gd) delegates
# to -- the same pattern as mp_menu_companion.gd -- claimed by the NATIONALITY + COMBO_LIST
# controls unique to this screen, riding the shell's MenuDriver over the compiled
# MenuFrame surface (widgets addressed by document id via _id/widget_id).
#
# Avatar cascade, voice rows and preview-trigger selection live in the native
# MenuDriver controller. This companion supplies resources and persisted picks,
# then applies the requested preview update to its mounted Godot nodes.
const ATBL_SECTION := "Avatars"
const VOICE_PREVIEW_CONTROL := "TESTPLAYERVOICE"
const AUTO_RELOAD_CONTROL := "OPTIONS_AUTORELOAD"
const AUTO_MEDIC_CONTROL := "OPTIONS_AUTOMEDIC"

# The 3D character preview (compatible head/body .3di composited). Mounted over
# the PLAYER_PREVIEW widget rect and fed the
# resolved combo; it plays the witnessed raw-.bad idle when those assets resolve.
const AvatarPreviewScript := preload("res://game/avatar/avatar_preview.gd")
const PlayerCharacterSelectionStateScript := preload(
		"res://game/player_character_selection_state.gd")

const PARENT_SLOTS := {
	"PRIMARY": WeaponDatabase.SLOT_PRIMARY,
	"SECONDARY": WeaponDatabase.SLOT_SECONDARY,
	"ACCESSORY": WeaponDatabase.SLOT_ACCESSORY,
}
# Only PRIMARY/SECONDARY author an ammo-TYPE combo (player.mnu statics FMJ/AP/SP,
# values 0/1/2); ACCESSORY and the grenades have none.
const TYPE_SLOTS := ["PRIMARY", "SECONDARY"]
const GRENADE_CONTROLS := ["GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"]

# The voice table, the DEFAULT_VOICE value and the kit page's order, fillers,
# knives, medpack and medic class are the engine's (runtime/menu/
# player_info_kit.h, player_info_loadout.h).

var _db: AvatarDatabase
var _team := 0                          # 0 = blue/good, 1 = red/evil (SIDE_BLUE default CHECKED)
# The loadout panel's state, the engine's (runtime/menu/player_info_loadout.h
# carries the witnesses): the three category lists and the grenade positions
# as filled, the saved count pairs by weapon.def row, the ammo-type byte per
# list per team, the grenade slots and the class page the last kit restore left.
var _loadout := PlayerInfoLoadout.new()
var _preview                            # AvatarPreview mounted over PLAYER_PREVIEW (null until wired)
var _preview_id := -1                   # PLAYER_PREVIEW doc id, for the hover-zoom filter
# NAME (upper) -> Callable(row, value), dispatched by combo value changes.
var _combo_handlers: Dictionary = {}
var _character_state := PlayerCharacterSelectionStateScript.new()
# While a build or a side switch restores the shown side, the cascade's voice
# results are held here and only the last, the fill against the restored
# selection, is written (_populate_side).
var _restoring_side := false
var _restored_voice := 0
signal avatar_chosen(profile: Dictionary)


## Install the active + per-side weapon.sav character snapshot before the next build.
func set_persisted_profile(profile: Dictionary) -> void:
	_character_state.set_persisted_profile(profile)


# True for the JO PLAYER_INFO screen's unique avatar controls.
func owns_menu(driver: MenuDriver) -> bool:
	if driver == null:
		return false
	return driver.has_widget("NATIONALITY") and driver.has_widget("COMBO_LIST")


# Losing the document to another (or no) companion is the only teardown edge
# for the frame mounts — the preview is a child of the PERSISTENT MenuFrame and
# would otherwise keep rendering over the next document's widgets.
func on_menu_released() -> void:
	super()
	_clear_mounts()


# Wire and populate from scratch for each document build.
func _wire(_file: String, _screen: String) -> void:
	_combo_handlers.clear()
	_clear_mounts()
	_ensure_db()
	_character_state.set_database(_db)
	# Combo selections relay through the driver's aggregate value-changed signal.
	if not _driver.widget_value_changed.is_connected(_on_widget_value_changed):
		_driver.widget_value_changed.connect(_on_widget_value_changed)
	# Mousing over the preview button drives the zoom + sway, like the original
	# [orig: PlayerInfo_UpdatePlayerPreviewAnimation active test @ 0x55dba0].
	if not _driver.widget_hover_changed.is_connected(_on_widget_hover_changed):
		_driver.widget_hover_changed.connect(_on_widget_hover_changed)
	# Re-place icon/preview mounts when the 800x600 design surface changes.
	if not _driver.screen_changed.is_connected(_on_screen_changed):
		_driver.screen_changed.connect(_on_screen_changed)
	var frame := _driver.get_frame()
	if frame != null and not frame.resized.is_connected(_reposition_mounts):
		frame.resized.connect(_reposition_mounts)
	_wire_team_radios()
	_connect_combo("NATIONALITY", _on_nat_selected)
	_connect_combo("DIVISION", _on_div_selected)
	_connect_combo("COMBO_LIST", _on_combo_selected)
	_connect_combo("PLAYERVOICE", _on_voice_selected)
	_connect_pressed(VOICE_PREVIEW_CONTROL, _preview_voice)
	# Prefer persisted team; otherwise retain player.mnu's authored SIDE_BLUE default.
	var authored_team := 1 if _radio_checked("SIDE_RED") else 0
	_team = _character_state.initial_team(authored_team)
	var blue_radio := _id("SIDE_BLUE")
	var red_radio := _id("SIDE_RED")
	if blue_radio >= 0:
		_driver.set_widget_checked(blue_radio, _team == 0)
	if red_radio >= 0:
		_driver.set_widget_checked(red_radio, _team == 1)
	_populate_side()
	_wire_preview()
	# Loadout: PLAYERCLASS drives the class mask, the team radios the team mask; both
	# filter the weapon slot lists [orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430].
	_ensure_player_info_weapons()
	_connect_combo("PLAYERCLASS", _on_class_selected)
	# The witnessed per-control recompute graph [orig: PlayerInfo_RegisterAllControls
	# @ 0x561470]: weapon selects refill that slot's ammo + weight/icons; ammo/type/
	# grenade selects record the pick and refresh weight/icons.
	for control in PARENT_SLOTS:
		_connect_combo(control, _on_weapon_slot_selected.bind(control))
		_connect_combo(control + "_AMMO1", _on_ammo1_selected.bind(control))
		_connect_combo(control + "_AMMO2", _on_ammo2_selected.bind(control))
	for control in TYPE_SLOTS:
		_connect_combo(control + "_AMMO1_TYPE", _on_type_selected.bind(control))
	for i in GRENADE_CONTROLS.size():
		_connect_combo(GRENADE_CONTROLS[i], _on_grenade_selected.bind(i))
	_restore_player_class()
	_populate_loadout()
	_populate_name()
	# The auto-reload / auto-medic boxes show the current record's words
	# (engine profile_controls.h).
	var auto_reload := _id(AUTO_RELOAD_CONTROL)
	if auto_reload >= 0:
		_driver.set_widget_checked(auto_reload, PlayerProfile.store().is_auto_reload_checked())
	var auto_medic := _id(AUTO_MEDIC_CONTROL)
	if auto_medic >= 0:
		_driver.set_widget_checked(auto_medic, PlayerProfile.store().is_auto_medic_checked())
	# OK saves the chosen avatar; the .mnu's own ACTION still navigates back to main.mnu.
	_connect_pressed("ACCEPT", commit)  # [orig: PlayerInfo_SaveFromDialog @ 0x55ee10]


# --- Avatars.def loading (best-effort; degrade to empty combos) ----------------

func _ensure_db() -> void:
	if _db != null or _root == null:
		return
	_db = AvatarDatabase.new()
	if _db.load_from_resource_root(_root, "Avatars.def") != OK or not _db.is_loaded():
		push_warning("PlayerInfoMenuCompanion: Avatars.def not loaded (%s); avatar combos stay empty"
			% _db.get_last_error())
		_db = null


func _restore_character_selection(side: int) -> void:
	if _db == null or side < 0 or side > 1:
		return
	var saved := _character_state.side_selection(side)
	if saved == null:
		return
	var nat_index := saved.nationality
	var visible_nat_row := nationality_rows().find(nat_index)
	var nat_combo := _id("NATIONALITY")
	if visible_nat_row < 0 or nat_combo < 0:
		return
	_driver.select_row(nat_combo, visible_nat_row, false)
	_update_avatars(MenuDriver.AVATAR_NATIONALITY, visible_nat_row)
	var div_index := saved.division
	var div_combo := _id("DIVISION")
	if div_index < 0 or div_index >= _db.get_division_count(nat_index) or div_combo < 0:
		return
	_driver.select_row(div_combo, div_index, false)
	_update_avatars(MenuDriver.AVATAR_DIVISION, div_index)
	var combo_index := saved.combo
	var combo := _id("COMBO_LIST")
	if combo_index < 0 or combo_index >= _db.get_combo_count(nat_index, div_index) \
			or combo < 0:
		return
	_driver.select_row(combo, combo_index, false)
	_update_avatars(MenuDriver.AVATAR_COMBO, 0)


func _restore_player_class() -> void:
	var combo := _id("PLAYERCLASS")
	if combo < 0:
		return
	# The current record's class byte of the BLUE side, whichever side is shown
	# (the dialog's save writes the class to both)
	# [orig: PlayerInfo_PopulateAllControls @0x5606f0 selects PLAYERCLASS from
	# g_CharSelClass with no side term, @0x560782].
	var player_class := PlayerProfile.store().character_summary().blue.player_class
	if player_class < 5 or player_class > 9:
		return
	for row in _driver.item_count(combo):
		if _driver.item_value(combo, row) == str(player_class):
			_driver.select_row(combo, row, false)
			return


# --- Loadout (weapon slot lists) ----------------------------------------------

## Inject the avatar table directly (a unit without a resource root).
func set_database(db: AvatarDatabase) -> void:
	_db = db


## The NATIONALITY list's visible rows as nationality DB indices, in row order.
func nationality_rows() -> Array[int]:
	var rows: Array[int] = []
	if _driver != null:
		rows.assign(_driver.player_info_nationality_rows())
	return rows


## The selected team: 0 = blue/good, 1 = red/evil.
func team() -> int:
	return _team


## Inject a weapon database for tests or another shell-owned resource mount. Keeping
## this as a public seam lets callers exercise the same menu population path without
## reaching into companion internals. The recorded ammo picks are keyed by table index,
## so a table swap invalidates them — clear rather than misapply.
func set_weapon_database(weapons: WeaponDatabase) -> void:
	_set_weapons(weapons)
	_loadout.clear_counts()
	if _driver != null:
		_populate_loadout()


# The loadout table off the root (MenuCompanion._ensure_weapons). A table read
# again drops the picks: they are keyed by the old table's rows, and the game
# resets every count pair at each populate anyway (the engine's restore).
func _ensure_player_info_weapons() -> void:
	if _ensure_weapons("PlayerInfoMenuCompanion", "loadout combos stay empty"):
		_loadout.clear_counts()


# Fill PRIMARY/SECONDARY/ACCESSORY for the selected class + team, each led by a "NONE" row,
# then the ammo combos, weight readout, and icons that hang off the selections.
# [orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430 -> PlayerInfo_PopulateWeaponAccessoryAmmoUI
#  @ 0x55e8b0 -> PlayerInfo_UpdateWeightAndWeaponIcons @ 0x55f480]
func _populate_loadout() -> void:
	if _weapons == null:
		return
	var class_mask := _selected_class_mask()
	# (engine: runtime/world/player_loadout.cpp)
	var team_mask := WeaponDatabase.player_info_team_mask(_team)
	# The three lists in the engine's row order under NONE (engine:
	# runtime/menu/loadout_screen.h loadout_fill_slot).
	_populating = true
	_loadout.fill_slots(_driver, _weapons, class_mask, team_mask, _menu_text("NONE", "None"),
			Strings.get_table(Strings.TABLE_GAMETEXT))
	_populating = false
	_restore_kit()
	for control in PARENT_SLOTS:
		_populate_slot_ammo(control)
	_populate_grenades(class_mask, team_mask)
	_update_weight()
	_update_icons()


# The shown side's saved kit page, the one its class byte picks in the current
# record, into the lists and the picks the ammo fills then read: the count
# pairs, each category list's selection, the team's ammo-type bytes and the
# grenade slots (engine menu::player_info_restore_kit and
# PlayerInfoLoadout::restore).
func _restore_kit() -> void:
	_loadout.restore(_driver, PlayerProfile.store().restore_player_info_kit(_team, _weapons), _team)


# The selected weapon.def row for a loadout control. NONE and an absent
# control both resolve to null.
func _selected_weapon(control: String) -> WeaponDef:
	if _weapons == null:
		return null
	return _weapons.get_weapon(_loadout.selected_weapon(_driver, control))


# PLAYERCLASS carries values 5..9 (Medic..Engineer); the class mask is the matching
# power-of-two bit — native policy [orig: PlayerInfo_SetTeamAndClassMask
# @ 0x55de60: 5->1,6->2,7->4,8->8,9->16].
func _selected_class_mask() -> int:
	var combo := _id("PLAYERCLASS")
	if combo < 0:
		# No class control -> show every class's weapons (defensive; the
		# five-class union mask lives at engine world/player_loadout.h
		# kClassFilterMaskAll).
		return WeaponDatabase.CLASS_MASK_ALL
	# The authored row's value= attr is the class id (the get_selected_value read).
	var row := _driver.selected_row(combo)
	var val := _driver.item_value(combo, row) if row >= 0 else ""
	var cls := int(val) if val.is_valid_int() else 0
	return WeaponDatabase.player_info_class_mask(cls)


func _selected_player_class() -> int:
	var combo := _id("PLAYERCLASS")
	if combo >= 0:
		var row := _driver.selected_row(combo)
		var value := _driver.item_value(combo, row) if row >= 0 else ""
		if value.is_valid_int():
			var selected := int(value)
			if selected >= 5 and selected <= 9:
				return selected
	var fallback := _character_state.player_class(_team)
	return fallback if fallback >= 5 and fallback <= 9 else 8


# A class pick: the dialog saved into the record, the picked class to both
# sides and the kit into the page of the class it leaves, then the screen
# populated from the record, the picked class's page restored.
func _on_class_selected(_row: int, _value: String) -> void:
	if _populating:
		return
	_save_from_dialog()
	_populate_all_controls()


# --- Loadout ammo combos (D-PLAYERINFO-11) --------------------------------------

# Populate through the native menu runtime from the panel's saved picks.
func _populate_slot_ammo(control: String) -> void:
	_populating = true
	_loadout.fill_ammo(_driver, _weapons, control, _team, Strings.get_table(Strings.TABLE_GAMETEXT))
	_populating = false


func _populate_grenades(class_mask: int, team_mask: int) -> void:
	_populating = true
	_loadout.fill_grenades(_driver, _weapons, class_mask, team_mask,
			Strings.get_table(Strings.TABLE_GAMETEXT))
	_populating = false


## The recorded clip pick for a parent slot's selected weapon, -1 = untouched (the
## def default). Public read seam (ADR 0018), mirroring the original's saved-count
## array rather than the combo position — retail serializes -1 until the user picks.
func selected_clips(control: String) -> int:
	return _loadout.selected_clips(_driver, control)


## The ammo-TYPE byte for a slot on the current team (0=FMJ 1=AP 2=SP).
func selected_ammo_type(control: String) -> int:
	return _loadout.selected_ammo_type(control, _team)


func _on_weapon_slot_selected(_row: int, _value: String, control: String) -> void:
	if _populating:
		return
	# [orig: the PRIMARY/SECONDARY/ACCESSORY handlers @ 0x55f710/0x55f790/0x55f810]
	_populate_slot_ammo(control)
	_update_weight()
	_update_icons()


# The handlers' picks are the engine's (PlayerInfoLoadout carries the
# witnesses): AMMO1 / AMMO2 store the row + 1 for the selected weapon, the type
# list the row's authored value byte for the team (the driver's aggregate
# signal carries the display text, the statics keep their authored items), a
# grenade list its row; then the combined weight and icon refresh
# [orig: @ 0x55f480].
func _on_ammo1_selected(row: int, _value: String, control: String) -> void:
	if _populating:
		return
	_loadout.pick_clips(_driver, control, false, row)
	_update_weight()
	_update_icons()


func _on_ammo2_selected(row: int, _value: String, control: String) -> void:
	if _populating:
		return
	_loadout.pick_clips(_driver, control, true, row)
	_update_weight()
	_update_icons()


func _on_type_selected(row: int, _value: String, control: String) -> void:
	if _populating:
		return
	_loadout.pick_ammo_type(_driver, control, _team, row)
	_update_weight()
	_update_icons()


func _on_grenade_selected(row: int, _value: String, i: int) -> void:
	if _populating:
		return
	_loadout.pick_grenade(i, row)
	_update_weight()
	_update_icons()


# --- Weight readout + weapon icons (D-PLAYERINFO-11) ----------------------------

func _update_weight() -> void:
	if _weapons == null:
		return
	var total := _loadout.weight(_driver, _weapons)
	var label := _id("STATIC_TOTAL_WEIGHT")
	if label >= 0:
		# The readout's format, bands and menu tokens are the engine's
		# (loadout_labels.h loadout_weight_line).
		_driver.set_widget_text(label, WeaponDatabase.loadout_weight_line(total,
				Strings.get_table(Strings.TABLE_MENUTXT), Strings.get_table(Strings.TABLE_GAMEUI)))


# Texture the PRIMARY/SECONDARY/ACCESSORY_ICON windows from the selected def's
# loadout_menu_icon (+144); NONE clears. GRENADE_ICON keeps its authored static
# [orig: @ 0x55f480 — icons from weapon +144; GRENADE_ICON untouched; retail's
#  no-selection resolves to the blank entry-0 icon, our NONE row clears].
# The icon TextureRects mount as frame children over each *_ICON widget rect
# (the compiled frame has no per-widget Controls to parent into).
func _update_icons() -> void:
	_update_weapon_icons("LoadoutIcon", _selected_weapon)


# --- Population (the cascade) -------------------------------------------------

func _populate_nationalities() -> void:
	_update_avatars(MenuDriver.AVATAR_TEAM, 0)


# The shown side's lists filled and its saved selection restored, as the
# screen's build and a side switch do. The cascade runs through the first rows
# on its way to the restored ones; each of its legs reads the profile's byte as
# it stands, and only the last leg's voice, the fill against the restored
# selection, is written (engine profile::set_voice_override).
func _populate_side() -> void:
	_restoring_side = true
	_restored_voice = selected_voice()
	_populate_nationalities()  # cascades into divisions -> combos -> voice
	_restore_character_selection(_team)
	_restoring_side = false
	PlayerProfile.store().set_voice(_team, _restored_voice)


# The voice the cascade leaves selected (a pick, or the fill's reset of a value
# the list does not carry) is the shown side's override byte in the current
# profile record, written at once for a user's pick (engine
# profile::set_voice_override); a side's restore writes only its last leg's.
func _update_avatars(change: int, value: int) -> void:
	_populating = true
	var voice := _driver.update_player_info_avatars(_db, change, value, _team,
			selected_voice(), Strings.get_table(Strings.TABLE_GAMEUI),
			Strings.get_table(Strings.TABLE_MENUTXT))
	_populating = false
	if _restoring_side:
		_restored_voice = voice
	else:
		PlayerProfile.store().set_voice(_team, voice)
	if _driver.player_info_avatar_preview_changed():
		_refresh_preview()


## The shown side's voice override in the current profile record; 0 =
## DEFAULT_VOICE, otherwise the CHARVOICE id the PLAYERVOICE list carries as
## that row's value.
func selected_voice() -> int:
	return PlayerProfile.store().get_voice(_team)


func _on_voice_selected(row: int, _value: String) -> void:
	if not _populating:
		_update_avatars(MenuDriver.AVATAR_VOICE, row)


func _preview_voice() -> void:
	if _driver != null:
		_driver.preview_player_info_voice(_db, selected_voice())


# --- 3D character preview (PLAYER_PREVIEW) ------------------------------------

# Mount the head/body 3D preview over the PLAYER_PREVIEW widget rect (a custom
# button surface in player.mnu) and feed it the current combo. Guarded: a menu
# without the widget, or without an avatar db / resource root, simply shows no preview.
func _wire_preview() -> void:
	# The authored PLAYER_PREVIEW window is a compiled widget; -1 means this screen
	# simply does not author it. The preview Control mounts as a frame child placed
	# by the widget's frame rect.
	var preview_id := _id("PLAYER_PREVIEW")
	if preview_id < 0:
		return
	var frame := _driver.get_frame()
	if frame == null:
		return
	_preview_id = preview_id
	_preview = AvatarPreviewScript.new()
	_preview.name = "PlayerInfoAvatarPreview"
	frame.add_child(_preview)
	_place_mount(_preview, preview_id)
	# Runtime PLAYER_INFO portrait: camera fixed, character facing the viewer,
	# with idle spin and hover zoom/sway handled by AvatarPreview.
	_preview.set_resource_root(_root)
	_refresh_preview()


# Hover edges arrive from the driver's pump claim; only the PLAYER_PREVIEW widget
# drives the zoom + sway, like the original
# [orig: PlayerInfo_UpdatePlayerPreviewAnimation active test @ 0x55dba0].
func _on_widget_hover_changed(id: int, hovered: bool) -> void:
	if _driver == null or _driver.get_menu_file() != _wired_file:
		return
	if _preview_id < 0 or id != _preview_id:
		return
	if _preview != null and is_instance_valid(_preview):
		_preview.set_hovered(hovered)


func _refresh_preview() -> void:
	if _preview == null:
		return
	var combo := _driver.player_info_avatar_combo(_db)
	if combo != null:
		# The portrait stops at the resolved part geometry; D-PLAYERINFO-1
		# tracks the separate spawned-player model seam.
		_preview.load_combo(combo)


# --- Frame mounts (icons + preview) ---------------------------------------------

# Free the previous build's frame-child mounts; the shell re-opens the document
# and hands us a fresh on_menu_built, so the mounts rebuild from scratch too.
func _clear_mounts() -> void:
	_clear_icon_mounts()
	if _preview != null and is_instance_valid(_preview):
		_preview.queue_free()
	_preview = null
	_preview_id = -1


func _reposition_mounts() -> void:
	# The shared driver serves every document, and the persistent frame's
	# resized signal can fire between a foreign open_document and the shell's
	# release call: stale ids from this document must never place mounts
	# against the new one (the sibling handlers carry the same guard).
	if _driver == null or _driver.get_menu_file() != _wired_file:
		return
	_reposition_icon_mounts()
	if _preview != null and is_instance_valid(_preview) and _preview_id >= 0:
		_place_mount(_preview, _preview_id)


func _on_screen_changed(_screen_name: String) -> void:
	_reposition_mounts()  # carries the stale-document guard


# --- Selection handlers (cascade edges) ---------------------------------------

func _on_nat_selected(row: int, _value: String) -> void:
	if not _populating:
		_update_avatars(MenuDriver.AVATAR_NATIONALITY, row)


func _on_div_selected(row: int, _value: String) -> void:
	if not _populating:
		_update_avatars(MenuDriver.AVATAR_DIVISION, row)


func _on_combo_selected(_row: int, _value: String) -> void:
	if not _populating:
		_update_avatars(MenuDriver.AVATAR_COMBO, 0)


# --- Team radios (SIDE_BLUE / SIDE_RED) ---------------------------------------

# The driver flips the radio checked state (with group exclusivity) on the click
# before emitting the activation, so the handlers only re-run the team cascade.
func _wire_team_radios() -> void:
	_connect_pressed("SIDE_BLUE", _on_side_blue)
	_connect_pressed("SIDE_RED", _on_side_red)


func _on_side_blue() -> void:
	_set_team(0)


func _on_side_red() -> void:
	_set_team(1)


# A side radio's click, the shown side's as well: the dialog saved into the
# record, the side it leaves with its kit page included, then the screen
# populated for the clicked side from the record.
func _set_team(team: int) -> void:
	_save_from_dialog()
	_team = team
	var blue_radio := _id("SIDE_BLUE")
	var red_radio := _id("SIDE_RED")
	if blue_radio >= 0:
		_driver.set_widget_checked(blue_radio, team == 0)
	if red_radio >= 0:
		_driver.set_widget_checked(red_radio, team == 1)
	_populate_all_controls()


# The screen populated for the shown side from the record, as a side radio and a
# class pick do after their save: the side's character and voice, the class, the
# boxes and the name, then the loadout (the team and class masks re-filter the
# lists) and its kit page.
func _populate_all_controls() -> void:
	_populate_side()
	_restore_player_class()
	var auto_reload := _id(AUTO_RELOAD_CONTROL)
	if auto_reload >= 0:
		_driver.set_widget_checked(auto_reload, PlayerProfile.store().is_auto_reload_checked())
	var auto_medic := _id(AUTO_MEDIC_CONTROL)
	if auto_medic >= 0:
		_driver.set_widget_checked(auto_medic, PlayerProfile.store().is_auto_medic_checked())
	_populate_name()
	_populate_loadout()


# --- ACCEPT seam (Phase 5) ----------------------------------------------------

# The current selection, including both side records, for the dialog's save
# and the shell's next spawn. Class is stamped across both side snapshots because retail's dialog
# walks both 0x8006 blocks before serializing; character bytes remain per-side
# [orig: PlayerInfo_SaveFromDialog @0x55EE3F-0x55EF38].
func snapshot() -> Dictionary:
	var combo := _id("COMBO_LIST")
	var player_class := _selected_player_class()
	# The voice is not here: its pick already wrote the profile record.
	var profile := _character_state.snapshot(
			_team, _driver.player_info_avatar_nationality(), _driver.player_info_avatar_division(),
			_driver.selected_row(combo) if combo >= 0 else -1,
			player_class, _edit_text("PLAYERNAME"))
	# No table (no root) means there was no loadout choice to commit. Keep that
	# distinct from a loaded screen whose three selected rows are explicitly
	# NONE, as a missing or empty weapon.def's are (D-MNU-27).
	if _weapons != null and _weapons.is_loaded():
		var primary := _selected_weapon("PRIMARY")
		var secondary := _selected_weapon("SECONDARY")
		var accessory := _selected_weapon("ACCESSORY")
		# Clip counts are the recorded picks, -1 = untouched default — the kit
		# tuple's serialized semantic [orig: PlayerInfo_SerializeWeaponLoadout @ 0x55e4b0
		# writes the saved arrays; "-1" is the filler]. The type bytes are the
		# tuple's flags field [orig: g_PlayerInfoAmmoTypePri/Sec].
		profile.merge({
			"primary": primary.name if primary != null else "",
			"primary_clips": selected_clips("PRIMARY"),
			"primary_ammo_type": selected_ammo_type("PRIMARY"),
			"secondary": secondary.name if secondary != null else "",
			"secondary_clips": selected_clips("SECONDARY"),
			"secondary_ammo_type": selected_ammo_type("SECONDARY"),
			"accessory": accessory.name if accessory != null else "",
			"accessory_clips": selected_clips("ACCESSORY"),
			"kit": kit_entries(),
			# The page the populate restored, which a class pick has already left.
			"kit_class": _loadout.page_class,
		})
	return profile


## The kit page exactly as retail serializes it: consecutive entries of
## (name, primary ammo count, secondary ammo count, ammo-type/flags), in the
## witnessed order -- the side's knife, the medic's medpack, the three loadout
## categories, then ALWAYS three grenade slots. Each entry is a Dictionary with
## the playersav::KitEntry field names, so the profile writer can hand the page
## straight to the encoder.
##
## retail: PlayerInfo_SerializeWeaponLoadout @ 0x55e4b0 -- knife first (@0x55e4dc picks
## the blade off g_PlayerInfoTeamMask), the class-5 medpack block (@0x55e624),
## the PRIMARY/SECONDARY/ACCESSORY selections (@0x55e6bb), then the fixed
## g_PlayerInfoGrenadeSlots[0..2] walk (@0x55e7e0, bounded by
## g_PlayerInfoAmmoPriCounts @ 0x25DC560 = three dwords past 0x25DC554).
func kit_entries() -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	if _weapons == null or not _weapons.is_loaded():
		return out
	# The order, the per-side knife, the medic's medpack and the fixed grenade
	# slots are the engine's page builder over the panel's picks (engine:
	# runtime/menu/player_info_loadout.h kit_selection): the three category
	# selections (NONE serializes the catalog's "None" row) with their count
	# pairs and type bytes, then the three grenade slots.
	for row in _loadout.kit_entries(_driver, _weapons, _team, _selected_player_class()):
		out.append(row)
	return out


# The ACCEPT: the dialog saved into the current record, then the selection
# handed to the shell for the next spawn.
func commit() -> void:
	avatar_chosen.emit(_save_from_dialog())


# The dialog read back into the current record in memory, as the ACCEPT, a side
# radio and a class pick each do first: the two boxes (each when authored, the
# medic box inverted; engine profile_controls.h), the name when PLAYERNAME is
# authored, the class to both sides, the character, and the kit into the page
# the last populate restored (PlayerProfile.accept_player_info).
func _save_from_dialog() -> Dictionary:
	var auto_reload := _id(AUTO_RELOAD_CONTROL)
	if auto_reload >= 0:
		PlayerProfile.store().set_auto_reload_checked(_driver.is_widget_checked(auto_reload))
	var auto_medic := _id(AUTO_MEDIC_CONTROL)
	if auto_medic >= 0:
		PlayerProfile.store().set_auto_medic_checked(_driver.is_widget_checked(auto_medic))
	var profile := snapshot()
	var saved := profile
	if _id("PLAYERNAME") < 0:
		saved = profile.duplicate()
		saved.erase("name")
	var error := PlayerProfile.accept_player_info(saved)
	if error != OK:
		push_warning("PlayerInfoMenuCompanion: the dialog was not saved into the profile (error %d)"
				% error)
	return profile


# --- Helpers ------------------------------------------------------------------

# Register a combo-select handler (row, value) for a named control, dispatched off
# the driver's aggregate widget_value_changed (kind == "combo") — the item_selected
# successor. `value` is the row's display text; handlers needing the authored
# value= attr resolve it through item_value.
func _connect_combo(name: String, handler: Callable) -> void:
	if _driver.has_widget(name):
		_combo_handlers[name.to_upper()] = handler


func _on_widget_value_changed(widget_name: String, kind: String, index: int, value: String) -> void:
	# The shell swaps documents under the shared driver; a stale companion's
	# name matches must not dispatch against the new document.
	if _driver == null or _driver.get_menu_file() != _wired_file:
		return
	if kind == "edit" and widget_name.to_upper() == "PLAYERNAME":
		_on_player_name_changed(value)
		return
	if kind != "combo":
		return
	var handler: Callable = _combo_handlers.get(widget_name.to_upper(), Callable())
	if handler.is_valid():
		handler.call(index, value)


# The PLAYERNAME edit set from the current record's name, as every populate
# sets it: the edit's text set cuts the name's first {hot} marker (engine
# MenuRuntime::set_widget_text), then its change notice runs the name rule once
# on the label the set stored, so a kept name renames the record and clears its
# no-name flag, and a refused last character is cut from the edit alone. The
# label is the set's own, never the frame's read-back (D-MNU-49). An
# unauthored edit sets nothing.
func _populate_name() -> void:
	var id := _id("PLAYERNAME")
	if id >= 0:
		_on_player_name_changed(
				_driver.set_widget_text(id, PlayerProfile.store().get_player_name()))


# The PLAYERNAME edit's change notice names the current profile record as the
# player types (and once at each populate): a label whose last character the
# edit keeps (or no text) renames it and clears its no-name flag, a refused
# last character is cut and the edit set to the cut, a text set again whose
# notice reaches no handler (engine: PlayerProfiles::name_changed).
func _on_player_name_changed(text: String) -> void:
	var kept := PlayerProfile.store().name_changed(text)
	if kept == text:
		return
	var id := _id("PLAYERNAME")
	if id >= 0:
		_driver.set_widget_text(id, kept)


func _radio_checked(name: String) -> bool:
	var id := _id(name)
	return id >= 0 and _driver.is_widget_checked(id)


func _menu_text(key: String, fallback: String) -> String:
	# DEFAULT_VOICE / CHARVOICE_%d are menu UI strings: menutxt's "Menu" then
	# gameui's "Avatars", else the readable fallback. Voice labels are cosmetic,
	# so a miss never blocks population.
	return Strings.lookup_or(Strings.TABLE_MENUTXT, Strings.SECTION_MENU, key,
			Strings.lookup_or(Strings.TABLE_GAMEUI, ATBL_SECTION, key, fallback))
