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
# The three loadout categories in the order the kit page serializes them.
# retail: the category_names[] array built at 0x55e4bc-0x55e4cc.
const KIT_SLOT_ORDER := ["PRIMARY", "SECONDARY", "ACCESSORY"]

# The voice table, the DEFAULT_VOICE value and the kit page's fillers,
# knives, medpack and medic class are the engine's (WeaponDatabase over
# runtime/menu/player_info_kit.h).

var _db: AvatarDatabase
var _weapons: WeaponDatabase     # weapon.def loadout table (PRIMARY/SECONDARY/ACCESSORY)
var _slot_rows: Dictionary = {}         # control name -> Array[WeaponDef], row-aligned (null = NONE)
var _team := 0                          # 0 = blue/good, 1 = red/evil (SIDE_BLUE default CHECKED)
# Per-def picked clip counts, keyed by weapon-table index; -1/absent = the def
# default (the maxclips row). The interleaved saved-count pair of the original
# [orig: g_PlayerInfoAmmoPriCounts @ 0x25DC560 / g_PlayerInfoAmmoSecCounts
# @ 0x25DC564 — handlers store selected_row + 1].
var _ammo_pri: Dictionary = {}
var _ammo_sec: Dictionary = {}
# The ammo-TYPE byte per team per slot (0=FMJ 1=AP 2=SP)
# [orig: g_PlayerInfoAmmoTypePri/Sec[teamIndex] @ 0x25DCD64/0x25DCD68].
var _ammo_type: Dictionary = {}         # "PRIMARY"/"SECONDARY" -> {team -> int}
var _grenade_rows: Array[WeaponDef] = []  # the first 3 class-3 defs, table order
var _preview                            # AvatarPreview mounted over PLAYER_PREVIEW (null until wired)
var _preview_id := -1                   # PLAYER_PREVIEW doc id, for the hover-zoom filter
# NAME (upper) -> Callable(row, value), dispatched by combo value changes.
var _combo_handlers: Dictionary = {}
var _character_state := PlayerCharacterSelectionStateScript.new()
# The persisted voice override per side, 0 = DEFAULT_VOICE.
# retail: the profile bytes g_CurPlayerProfile[teamIndex + 1532] @ 0x25510FC.
var _voice_override: Dictionary = {}
signal avatar_chosen(profile: Dictionary)


## Install the active + per-side weapon.sav character snapshot before the next build.
func set_persisted_profile(profile: Dictionary) -> void:
	_character_state.set_persisted_profile(profile)
	# Retail keeps one override BYTE per side (profile+1532 and profile+1533);
	# our saved profile carries a single "voice", so both sides start from it
	# and then diverge per side exactly as retail's pair does. The per-side
	# persistence of the pair is the remaining D-PLAYERINFO-9 residual.
	var voice := int(profile.get("voice", WeaponDatabase.DEFAULT_VOICE_VALUE))
	_voice_override = {0: voice, 1: voice}


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
	_populate_nationalities()  # cascades into divisions -> combos -> voice
	_restore_character_selection(_team)
	_wire_preview()
	# Loadout: PLAYERCLASS drives the class mask, the team radios the team mask; both
	# filter the weapon slot lists [orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430].
	_ensure_weapons()
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
	var saved_name := _character_state.persisted_name()
	var name_edit := _id("PLAYERNAME")
	if name_edit >= 0 and not saved_name.is_empty():
		_driver.set_widget_text(name_edit, saved_name)
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
	# The remembered per-side class (weapon.sav's side block, or an edit made
	# before a team switch); nothing remembered keeps player.mnu's authored row
	# [orig: PlayerInfo_PopulateAllControls @0x5606f0 selects PLAYERCLASS from
	# g_CharSelClass].
	var player_class := _character_state.player_class(_team)
	if player_class < 5 or player_class > 9:
		return
	for row in _driver.item_count(combo):
		if _driver.item_value(combo, row) == str(player_class):
			_driver.select_row(combo, row, false)
			return


# --- Loadout (weapon slot lists) ----------------------------------------------

## Inject a weapon database for tests or another shell-owned resource mount. Keeping
## this as a public seam lets callers exercise the same menu population path without
## reaching into companion internals. The recorded ammo picks are keyed by table index,
## so a table swap invalidates them — clear rather than misapply.
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


func set_weapon_database(weapons: WeaponDatabase) -> void:
	_weapons = weapons
	_ammo_pri.clear()
	_ammo_sec.clear()
	if _driver != null:
		_populate_loadout()


# Load weapon.def into the loadout table (best-effort; absent -> empty slot lists).
func _ensure_weapons() -> void:
	if _weapons == null and _root != null:
		_weapons = LoadoutWeaponTable.load_weapon_database(_root, "PlayerInfoMenuCompanion",
				"loadout combos stay empty")


# Fill PRIMARY/SECONDARY/ACCESSORY for the selected class + team, each led by a "NONE" row,
# then the ammo combos, weight readout, and icons that hang off the selections.
# [orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430 -> PlayerInfo_PopulateWeaponAccessoryAmmoUI
#  @ 0x55e8b0 -> PlayerInfo_UpdateWeightAndWeaponIcons @ 0x55f480]
func _populate_loadout() -> void:
	if _weapons == null:
		return
	var class_mask := _selected_class_mask()
	# [orig: g_PlayerInfoTeamMask = 2 - (team != 0) @0x55de60 — native policy]
	var team_mask := WeaponDatabase.player_info_team_mask(_team)
	_fill_weapon_slot("PRIMARY", WeaponDatabase.SLOT_PRIMARY, class_mask, team_mask)
	_fill_weapon_slot("SECONDARY", WeaponDatabase.SLOT_SECONDARY, class_mask, team_mask)
	_fill_weapon_slot("ACCESSORY", WeaponDatabase.SLOT_ACCESSORY, class_mask, team_mask)
	for control in PARENT_SLOTS:
		_populate_slot_ammo(control)
	_populate_grenades(class_mask, team_mask)
	_update_weight()
	_update_icons()


func _fill_weapon_slot(control: String, slot: int, class_mask: int, team_mask: int) -> void:
	var combo := _id(control)
	if combo < 0:
		return
	var rows := PackedStringArray()
	var defs: Array[WeaponDef] = []
	rows.append(_menu_text("NONE", "None"))  # NONE at index 0 [orig: @ 0x560430]
	defs.append(null)
	for w: WeaponDef in _weapons.get_slot_weapons(slot, class_mask, team_mask):
		rows.append(_weapons.weapon_label(w.index, Strings.get_table(Strings.TABLE_GAMETEXT)))
		defs.append(w)
	_slot_rows[control] = defs
	_set_combo_items(combo, rows)


# The selected weapon.def row for a loadout control. NONE and an absent
# control both resolve to null.
func _selected_weapon(control: String) -> WeaponDef:
	var combo := _id(control)
	var defs: Array = _slot_rows.get(control, [])
	if combo < 0:
		return null
	var row := _driver.selected_row(combo)
	if row < 0 or row >= defs.size():
		return null
	return defs[row]


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


func _on_class_selected(_row: int, _value: String) -> void:
	if _populating:
		return
	_populate_loadout()  # the class mask re-filters the weapon slot lists


# --- Loadout ammo combos (D-PLAYERINFO-11) --------------------------------------

# Populate through the native menu runtime; this shell carries the saved picks.
func _populate_slot_ammo(control: String) -> void:
	var w := _selected_weapon(control)
	var index := w.index if w != null else -1
	var types := _slot_type_store(control)
	_populating = true
	types[_team] = _driver.fill_player_info_ammo(_weapons, control, index,
			int(_ammo_pri.get(index, -1)), int(_ammo_sec.get(index, -1)),
			int(types.get(_team, 0)), Strings.get_table(Strings.TABLE_GAMETEXT))
	_populating = false


func _populate_grenades(class_mask: int, team_mask: int) -> void:
	_populating = true
	_grenade_rows = _driver.fill_player_info_grenades(_weapons, class_mask, team_mask,
			_ammo_pri, Strings.get_table(Strings.TABLE_GAMETEXT))
	_populating = false


func _slot_type_store(control: String) -> Dictionary:
	if not _ammo_type.has(control):
		_ammo_type[control] = {}
	return _ammo_type[control]


## The recorded clip pick for a parent slot's selected weapon, -1 = untouched (the
## def default). Public read seam (ADR 0018), mirroring the original's saved-count
## array rather than the combo position — retail serializes -1 until the user picks.
func selected_clips(control: String) -> int:
	var w := _selected_weapon(control)
	if w == null:
		return -1
	return int(_ammo_pri.get(w.index, -1))


## The ammo-TYPE byte for a slot on the current team (0=FMJ 1=AP 2=SP).
func selected_ammo_type(control: String) -> int:
	return int(_slot_type_store(control).get(_team, 0))


func _on_weapon_slot_selected(_row: int, _value: String, control: String) -> void:
	if _populating:
		return
	# [orig: the PRIMARY/SECONDARY/ACCESSORY handlers @ 0x55f710/0x55f790/0x55f810]
	_populate_slot_ammo(control)
	_update_weight()
	_update_icons()


func _on_ammo1_selected(row: int, _value: String, control: String) -> void:
	if _populating:
		return
	var w := _selected_weapon(control)
	if w != null:
		_ammo_pri[w.index] = row + 1  # [orig: @ 0x55f730 — stores selected_row + 1]
	_update_weight()
	_update_icons()  # the original's combined refresh [orig: @ 0x55f480]


func _on_ammo2_selected(row: int, _value: String, control: String) -> void:
	if _populating:
		return
	var w := _selected_weapon(control)
	if w != null:
		_ammo_sec[w.index] = row + 1  # [orig: @ 0x55f730 ctx 1 — the interleaved pair]
	_update_weight()
	_update_icons()


func _on_type_selected(row: int, _value: String, control: String) -> void:
	if _populating:
		return
	# The driver's aggregate signal carries the row's display text; the semantic
	# byte is the authored value= attr of the selected row (the statics keep their
	# authored items, so the document lookup is the value source).
	var type_combo := _id(control + "_AMMO1_TYPE")
	var value := _driver.item_value(type_combo, row) if type_combo >= 0 and row >= 0 else ""
	# [orig: @ 0x55f760/0x55f7e0 — the row VALUE byte, stored per team]
	_slot_type_store(control)[_team] = int(value) if value.is_valid_int() else 0
	_update_weight()
	_update_icons()  # the original's combined refresh [orig: @ 0x55f480]


func _on_grenade_selected(row: int, _value: String, i: int) -> void:
	if _populating:
		return
	if i < _grenade_rows.size():
		# Rows start at 0, so the row IS the clip count. (Retail stores the row
		# VALUE — rounds — which equals clips only because grenade defs use
		# clipsize 1; we store clips, the consistent half of that quirk
		# [orig: @ 0x55fb70].)
		_ammo_pri[_grenade_rows[i].index] = row
	_update_weight()
	_update_icons()


# --- Weight readout + weapon icons (D-PLAYERINFO-11) ----------------------------

func _update_weight() -> void:
	if _weapons == null:
		return
	var parents: Array[WeaponDef] = []
	for control in PARENT_SLOTS:
		parents.append(_selected_weapon(control))
	var total := _driver.player_info_loadout_weight(
			_weapons, parents, _grenade_rows, _ammo_pri, _ammo_sec)
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


func _update_avatars(change: int, value: int) -> void:
	_populating = true
	_voice_override[_team] = _driver.update_player_info_avatars(_db, change, value,
			_team, selected_voice(), Strings.get_table(Strings.TABLE_GAMEUI),
			Strings.get_table(Strings.TABLE_MENUTXT))
	_populating = false
	if _driver.player_info_avatar_preview_changed():
		_refresh_preview()


## The persisted voice override for the shown side; 0 = DEFAULT_VOICE, otherwise
## the CHARVOICE id the PLAYERVOICE list carries as that row's value.
func selected_voice() -> int:
	return int(_voice_override.get(_team, WeaponDatabase.DEFAULT_VOICE_VALUE))


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


func _selected_combo_index() -> int:
	var combo := _id("COMBO_LIST")
	if combo < 0:
		return -1
	var idx := _driver.selected_row(combo)
	return idx if idx >= 0 else 0


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


# Apply the native team cascade, then restore the shell's saved side picks.
func _set_team(team: int) -> void:
	if team == _team:
		return
	_remember_current_character_selection()
	_team = team
	var blue_radio := _id("SIDE_BLUE")
	var red_radio := _id("SIDE_RED")
	if blue_radio >= 0:
		_driver.set_widget_checked(blue_radio, team == 0)
	if red_radio >= 0:
		_driver.set_widget_checked(red_radio, team == 1)
	_populate_nationalities()
	_restore_character_selection(team)
	_restore_player_class()
	_populate_loadout()  # the team mask re-filters the weapon slot lists


# --- ACCEPT seam (Phase 5) ----------------------------------------------------

func _current_character_selection() -> PlayerCharacterSelectionState.Selection:
	return _character_state.make_selection(_team,
			_driver.player_info_avatar_nationality(), _driver.player_info_avatar_division(),
			_selected_combo_index(), _selected_player_class())


func _remember_current_character_selection() -> void:
	var current := _current_character_selection()
	if current != null:
		_character_state.remember(current)


# The current selection, including both side records, for main_game to persist
# on ACCEPT. Class is stamped across both side snapshots because retail's dialog
# walks both 0x8006 blocks before serializing; character bytes remain per-side
# [orig: PlayerInfo_SaveFromDialog @0x55EE3F-0x55EF38].
func snapshot() -> Dictionary:
	var combo := _id("COMBO_LIST")
	var player_class := _selected_player_class()
	# The persisted voice is the picked row's VALUE, not its position: retail
	# stores the notification's value byte into the profile and re-selects the
	# list by that value on the next populate.
	var profile := _character_state.snapshot(
			_team, _driver.player_info_avatar_nationality(), _driver.player_info_avatar_division(),
			_driver.selected_row(combo) if combo >= 0 else -1,
			player_class, _edit_text("PLAYERNAME"), selected_voice())
	# Missing weapon.def means there was no loadout choice to commit. Keep that
	# distinct from a loaded screen whose three selected rows are explicitly NONE.
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
	# slots are the engine's page builder; this companion supplies its picks:
	# the three category selections with their recorded count pairs (-1 =
	# untouched) and the team's ammo-type byte for PRIMARY/SECONDARY, then the
	# three grenade positions (entry 0 when the class/team filter left one empty).
	var indices := PackedInt32Array()
	var pri := PackedInt32Array()
	var sec := PackedInt32Array()
	var flags := PackedInt32Array()
	for control in KIT_SLOT_ORDER:
		var index := _slot_weapon_index(control)
		indices.append(index)
		pri.append(int(_ammo_pri.get(index, -1)))
		sec.append(int(_ammo_sec.get(index, -1)))
		flags.append(selected_ammo_type(control) if control in TYPE_SLOTS else -1)
	var grenade_indices := PackedInt32Array()
	var grenade_pri := PackedInt32Array()
	var grenade_sec := PackedInt32Array()
	for i in GRENADE_CONTROLS.size():
		var index := _grenade_rows[i].index if i < _grenade_rows.size() else 0
		grenade_indices.append(index)
		grenade_pri.append(int(_ammo_pri.get(index, -1)))
		grenade_sec.append(int(_ammo_sec.get(index, -1)))
	for row in _weapons.player_info_kit_entries(_team, _selected_player_class(), indices,
			pri, sec, flags, grenade_indices, grenade_pri, grenade_sec):
		out.append(row)
	return out


# The weapon-table index a loadout list would report as its selected VALUE.
# Every weapon row carries its table index and the NONE row carries 0, so a
# NONE slot serializes weapon-table entry 0 -- retail's own quirk.
# retail: the NONE insert UIList_AddRow(list, "NONE", 0, 0, 0) @ 0x56058f in
# PlayerInfo_PopulateWeaponSlotLists @ 0x560430, read back through
# UIList_GetSelectedValue @ 0x644660 at 0x55e6e8.
func _slot_weapon_index(control: String) -> int:
	var w := _selected_weapon(control)
	return w.index if w != null else 0


func commit() -> void:
	avatar_chosen.emit(snapshot())


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


# The PLAYERNAME edit's change names the current profile record as the player
# types: text whose last character the edit keeps (or no text) renames it and
# clears its no-name flag, a refused last character is cut from the edit
# (engine: PlayerProfiles::rename).
func _on_player_name_changed(text: String) -> void:
	if PlayerProfile.store().rename(text):
		return
	var id := _id("PLAYERNAME")
	if id >= 0:
		_driver.set_widget_text(id, text.left(text.length() - 1))


func _radio_checked(name: String) -> bool:
	var id := _id(name)
	return id >= 0 and _driver.is_widget_checked(id)


func _menu_text(key: String, fallback: String) -> String:
	# DEFAULT_VOICE / CHARVOICE_%d are menu UI strings: menutxt's "Menu" then
	# gameui's "Avatars", else the readable fallback. Voice labels are cosmetic,
	# so a miss never blocks population.
	return Strings.lookup_or(Strings.TABLE_MENUTXT, Strings.SECTION_MENU, key,
			Strings.lookup_or(Strings.TABLE_GAMEUI, ATBL_SECTION, key, fallback))
