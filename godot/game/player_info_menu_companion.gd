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
# Faithful to the witnessed original (docs/playerinfo/avatars-re.md, "Screen
# orchestration", D-PLAYERINFO-5/7):
#   - team 0 = blue/good, 1 = red/evil; a nationality is shown only when
#     (alignment != 0) == (team != 0)
#     [orig: PlayerInfo_PopulateNationalityList @ 0x55d8c0]
#   - selecting a nationality resets the division and refills division + combo;
#     selecting a division refills the combo list
#     [orig: PlayerInfo_HandleNationalitySelect @ 0x560600,
#            PlayerInfo_HandleDivisionSelect @ 0x560690]
#   - the COMBO_LIST label is "<head display> - <body display>" resolved through the
#     "Avatars" RTXT section
#     [orig: populate_avatar_combo_list @ 0x560210]
# The in-world avatar appearance (D-PLAYERINFO-1) and full profile persistence are
# later phases; snapshot() exposes the current selection for the ACCEPT seam.

# The "Avatars" RTXT section the nationality/division/combo display keys resolve
# against [orig: TextResource_GetStringWithFallback(resource, "Avatars", nameKey)].
const ATBL_SECTION := "Avatars"

# TESTPLAYERVOICE is a semantic screen command, not the button's generic click sound.
# Retail resolves the trigger from the current avatar and plays it from the dedicated
# menu bank. [orig: PlayerInfo_PreviewVoice @ 0x55ff70]
const VOICE_PREVIEW_CONTROL := "TESTPLAYERVOICE"
const VOICE_PREVIEW_BANK := "menu.lwf"
const VOICE_PREVIEW_TRIGGER_FORMAT := "VOICE_%d"

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
# [orig: g_playerInfoAmmoPriCounts @ 0x25DC560 / g_playerInfoAmmoSecCounts
# @ 0x25DC564 — handlers store selected_row + 1].
var _ammo_pri: Dictionary = {}
var _ammo_sec: Dictionary = {}
# The ammo-TYPE byte per team per slot (0=FMJ 1=AP 2=SP)
# [orig: g_playerInfoAmmoTypePri/Sec[teamIndex] @ 0x25DCD64/0x25DCD68].
var _ammo_type: Dictionary = {}         # "PRIMARY"/"SECONDARY" -> {team -> int}
var _grenade_rows: Array[WeaponDef] = []  # the first 3 class-3 defs, table order
var _nat_db_index: Array[int] = []      # NATIONALITY visible row -> nationality DB index
var _sel_nat := -1
var _sel_div := -1
var _preview                            # AvatarPreview mounted over PLAYER_PREVIEW (null until wired)
var _preview_id := -1                   # PLAYER_PREVIEW doc id, for the hover-zoom filter
# NAME (upper) -> Callable(row, value), dispatched by combo value changes.
var _combo_handlers: Dictionary = {}
var _character_state := PlayerCharacterSelectionStateScript.new()
# PLAYERVOICE row -> the row's VALUE (0 for DEFAULT_VOICE, else the CHARVOICE id);
# the list is filled at runtime, so the values live here the way _nat_db_index
# carries the nationality list's.
var _voice_values: Array[int] = []
# The persisted voice override per side, 0 = DEFAULT_VOICE.
# retail: the profile bytes g_curPlayerProfile[teamIndex + 1532] @ 0x25510FC.
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
	# [orig: update_player_preview_animation active test @ 0x55dba0].
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
	# filter the weapon slot lists [orig: populate_weapon_slot_lists @ 0x560430].
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
	_connect_pressed("ACCEPT", commit)  # [orig: save_player_info_from_dialog @ 0x55ee10]


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
	var visible_nat_row := _nat_db_index.find(nat_index)
	var nat_combo := _id("NATIONALITY")
	if visible_nat_row < 0 or nat_combo < 0:
		return
	_driver.select_row(nat_combo, visible_nat_row, false)
	_sel_nat = nat_index
	_populate_divisions()
	var div_index := saved.division
	var div_combo := _id("DIVISION")
	if div_index < 0 or div_index >= _db.get_division_count(nat_index) or div_combo < 0:
		return
	_driver.select_row(div_combo, div_index, false)
	_sel_div = div_index
	_populate_combos()
	var combo_index := saved.combo
	var combo := _id("COMBO_LIST")
	if combo_index < 0 or combo_index >= _db.get_combo_count(nat_index, div_index) \
			or combo < 0:
		return
	_driver.select_row(combo, combo_index, false)
	_populate_voices()
	_refresh_preview()


func _restore_player_class() -> void:
	var combo := _id("PLAYERCLASS")
	if combo < 0:
		return
	# The remembered per-side class (weapon.sav's side block, or an edit made
	# before a team switch); nothing remembered keeps player.mnu's authored row
	# [orig: PlayerInfo_PopulateAllControls @0x5606f0 selects PLAYERCLASS from
	# g_charSelClass].
	var player_class := _character_state.player_class(_team)
	if player_class < 5 or player_class > 9:
		return
	for row in _driver.item_count(combo):
		if _driver.item_value(combo, row) == str(player_class):
			_driver.select_row(combo, row, false)
			return


# Resolve a nationality/division/combo display key against the gametext table's "Avatars"
# section -- the table the original consults for these names
# [orig: GameText_GetStringWithFallback @ 0x51eb90 / g_TextGameText @ 0xB4C2AC; "Avatars"
# section, docs/playerinfo/avatars-re.md]. The shell registers gametext (Game.bin) into the
# shared Strings registry at boot. A miss falls back to the raw key (the witnessed
# fallback; not the "??section:key??" debug marker Strings.lookup would return).
func _display_name(key: String) -> String:
	return Strings.lookup_or(Strings.TABLE_GAMEUI, ATBL_SECTION, key, key)


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
	return _nat_db_index.duplicate()


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
		_weapons = LoadoutLabels.load_weapon_database(_root, "PlayerInfoMenuCompanion",
				"loadout combos stay empty")


# Fill PRIMARY/SECONDARY/ACCESSORY for the selected class + team, each led by a "NONE" row,
# then the ammo combos, weight readout, and icons that hang off the selections.
# [orig: populate_weapon_slot_lists @ 0x560430 -> populate_weapon_accessory_ammo_ui
#  @ 0x55e8b0 -> update_player_info_weight_and_weapon_icons @ 0x55f480]
func _populate_loadout() -> void:
	if _weapons == null:
		return
	var class_mask := _selected_class_mask()
	# [orig: g_playerInfoTeamMask = 2 - (team != 0) @0x55de60 — native policy]
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
		rows.append(LoadoutLabels.weapon_label(w))
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

# Clip-count rows for one parent slot's AMMO1/TYPE/AMMO2 combos
# [orig: populate_ammo_combo_boxes @ 0x55def0; the ACCESSORY leg is the same logic
#  inlined in populate_weapon_accessory_ammo_ui @ 0x55e8b0].
func _populate_slot_ammo(control: String) -> void:
	var w := _selected_weapon(control)
	var index := w.index if w != null else -1
	var has_ammo := w != null and w.clipsize > 0
	var ammo1 := _id(control + "_AMMO1")
	var type_combo := _id(control + "_AMMO1_TYPE")
	var ammo2 := _id(control + "_AMMO2")
	if ammo1 >= 0:
		_driver.set_widget_shown(ammo1, has_ammo)
		if has_ammo:
			var maxclips := w.maxclips
			var rows := PackedStringArray()
			# Rows 1..maxclips: "%d - %s" = rounds + round label; row value = the
			# clip count (retail keys rows by the def index; ours by position).
			for clips in range(1, maxclips + 1):
				rows.append(LoadoutLabels.ammo_row_label(w, clips))
			_set_combo_items(ammo1, rows)
			# Saved count selects its row; -1/absent = the maxclips row (full
			# default) [orig: the `saved == i || (saved == -1 && i == maxclips)`
			# select in both fills — native default_clip_row].
			var saved := int(_ammo_pri.get(index, WeaponDatabase.CLIP_COUNT_DEF_DEFAULT))
			_driver.select_row(ammo1,
					WeaponDatabase.default_clip_row(saved, maxclips) - 1, false)
	if type_combo >= 0:
		# The TYPE combo keeps its authored FMJ/AP/SP statics; shown with AMMO1,
		# selection = the saved per-team type byte (-1 -> 0). flags2 NOAMMOTYPES
		# locks it non-interactive and resets the saved type
		# [orig: @ 0x55def0 — the +188 & 0x40 gate -> UIWidget_SetInteractiveRecursive].
		_driver.set_widget_shown(type_combo, has_ammo)
		if has_ammo:
			var locked := (w.flags2 & WeaponDatabase.FLAG2_NOAMMOTYPES) != 0
			_driver.set_widget_disabled(type_combo, locked)
			if locked:
				_slot_type_store(control)[_team] = 0
			# Select by the row's authored VALUE (0/1/2), not its position — the
			# saved byte is the value [orig: the @ 0x55def0 row select].
			var saved_type := str(int(_slot_type_store(control).get(_team, 0)))
			for row in _driver.item_count(type_combo):
				if _driver.item_value(type_combo, row) == saved_type:
					_driver.select_row(type_combo, row, false)
					break
	if ammo2 >= 0:
		var sub := _subclass_weapon(w)
		var sub_ok := has_ammo and sub != null and sub.clipsize > 0
		_driver.set_widget_shown(ammo2, sub_ok)
		if sub_ok:
			var sub_max := sub.maxclips
			var rows2 := PackedStringArray()
			for clips in range(1, sub_max + 1):
				rows2.append(LoadoutLabels.ammo_row_label(sub, clips))
			_set_combo_items(ammo2, rows2)
			var saved2 := int(_ammo_sec.get(index, WeaponDatabase.CLIP_COUNT_DEF_DEFAULT))
			_driver.select_row(ammo2,
					WeaponDatabase.default_clip_row(saved2, sub_max) - 1, false)


# The sub-weapon behind *_AMMO2 — the native def-table walk
# (engine/formats/def def_subclass_weapon_index
# [orig: the stricmp walk over entry+192.. in @ 0x55def0 / @ 0x55e8b0 / @ 0x55f1f0]).
func _subclass_weapon(parent: WeaponDef) -> WeaponDef:
	if parent == null or _weapons == null:
		return null
	var index := int(_weapons.subclass_weapon_index(parent.index))
	return _weapons.get_weapon(index) if index >= 0 else null


# The first three selectable class-3 defs passing the class+team masks own
# GRENADE_AMMO1..3 in table order; leftover widgets hide. Rows 0..maxclips
# INCLUDING the zero row [orig: the >= 3 leg of populate_ammo_combo_boxes
# @ 0x55def0 — filter, table order, hidden leftovers, zero row].
func _populate_grenades(class_mask: int, team_mask: int) -> void:
	_grenade_rows = []
	if _weapons != null:
		var defs := _weapons.get_slot_weapons(
				WeaponDatabase.SLOT_GRENADE, class_mask, team_mask)
		for i in mini(defs.size(), GRENADE_CONTROLS.size()):
			_grenade_rows.append(defs[i])
	for i in GRENADE_CONTROLS.size():
		var combo := _id(GRENADE_CONTROLS[i])
		if combo < 0:
			continue
		if i >= _grenade_rows.size():
			_driver.set_widget_shown(combo, false)
			continue
		_driver.set_widget_shown(combo, true)
		var w := _grenade_rows[i]
		var maxclips := w.maxclips
		var rows := PackedStringArray()
		for clips in range(0, maxclips + 1):
			rows.append(LoadoutLabels.ammo_row_label(w, clips))
		_set_combo_items(combo, rows)
		var saved := int(_ammo_pri.get(w.index, -1))
		_driver.select_row(combo,
				maxclips if saved < 0 else clampi(saved, 0, maxclips), false)


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

# Weight = parent slots through the native ported math, plus the witnessed
# clip-only terms for sub-weapons and grenades
# [orig: calculate_loadout_weight @ 0x55f1f0 — parents weaponweight +
#  (saved<=0?maxclips:saved)*clipweight; sub-weapons and grenades clip term ONLY,
#  grenades -1 -> maxclips with a saved 0 staying 0].
func _update_weight() -> void:
	if _weapons == null:
		return
	var indices := PackedInt32Array()
	var counts := PackedInt32Array()
	var total := 0.0
	for control in PARENT_SLOTS:
		var w := _selected_weapon(control)
		if w == null:
			continue
		var index := w.index
		indices.append(index)
		counts.append(int(_ammo_pri.get(index, -1)))
		# The witnessed sub-weapon term is gated on the *_AMMO2 control existing.
		var sub := _subclass_weapon(w)
		if _id(control + "_AMMO2") >= 0 and sub != null and sub.clipsize > 0:
			var saved2 := int(_ammo_sec.get(index, WeaponDatabase.CLIP_COUNT_DEF_DEFAULT))
			total += _weapons.extra_ammo_weight(sub.index,
					WeaponDatabase.CLIP_COUNT_DEF_DEFAULT if saved2 <= 0 else saved2)
	total += _weapons.loadout_weight(indices, counts)
	for i in _grenade_rows.size():
		# The witnessed grenade term is gated on the control existing AND shown.
		var combo := _id(GRENADE_CONTROLS[i]) if i < GRENADE_CONTROLS.size() else -1
		if combo < 0 or not _driver.is_widget_shown(combo):
			continue
		var g := _grenade_rows[i]
		var saved := int(_ammo_pri.get(g.index, WeaponDatabase.CLIP_COUNT_DEF_DEFAULT))
		total += _weapons.extra_ammo_weight(g.index, saved)
	var band := _weapons.encumbrance_class(total)
	var encumbrance := Strings.menu_text("LIGHT_ENCUMBRANCE", "Light")
	if band == WeaponDatabase.ENCUMBRANCE_HEAVY:
		encumbrance = Strings.menu_text("HEAVY_ENCUMBRANCE", "Heavy")
	elif band == WeaponDatabase.ENCUMBRANCE_NORMAL:
		encumbrance = Strings.menu_text("NORMAL_ENCUMBRANCE", "Normal")
	var label := _id("STATIC_TOTAL_WEIGHT")
	if label >= 0:
		# [orig: update_player_info_weight_and_weapon_icons @ 0x55f480 —
		#  sprintf "%s %.1f %s (%s)", keys TOTAL_WEIGHT / LBS / *_ENCUMBRANCE]
		_driver.set_widget_text(label, "%s %.1f %s (%s)" % [
			Strings.menu_text("TOTAL_WEIGHT", "Total Weight"), total,
			Strings.menu_text("LBS", "lbs"), encumbrance])


# Texture the PRIMARY/SECONDARY/ACCESSORY_ICON windows from the selected def's
# loadout_menu_icon (+144); NONE clears. GRENADE_ICON keeps its authored static
# [orig: @ 0x55f480 — icons from weapon +144; GRENADE_ICON untouched; retail's
#  no-selection resolves to the blank entry-0 icon, our NONE row clears].
# The icon TextureRects mount as frame children over each *_ICON widget rect
# (the compiled frame has no per-widget Controls to parent into).
func _update_icons() -> void:
	_update_weapon_icons("LoadoutIcon", _selected_weapon)


# --- Population (the cascade) -------------------------------------------------

# Fill NATIONALITY, filtered by team alignment, then cascade into division/combo/voice.
# [orig: PlayerInfo_PopulateNationalityList @ 0x55d8c0; team filter D-PLAYERINFO-5]
func _populate_nationalities() -> void:
	var combo := _id("NATIONALITY")
	if combo < 0:
		return
	_nat_db_index.clear()
	var rows := PackedStringArray()
	if _db != null:
		for i in _db.get_nationality_count():
			var nat := _db.get_nationality(i)
			var align := nat.alignment
			# show only when (alignment != 0) == (team != 0): good->blue(0), evil->red(1)
			if (align != 0) != (_team != 0):
				continue
			_nat_db_index.append(i)
			rows.append(_display_name(nat.name_key))
	_set_combo_items(combo, rows)
	_sel_nat = _nat_db_index[0] if not _nat_db_index.is_empty() else -1
	_populate_divisions()


# [orig: PlayerInfo_PopulateDivisionList @ 0x55da50]
func _populate_divisions() -> void:
	var combo := _id("DIVISION")
	if combo < 0:
		return
	var rows := PackedStringArray()
	if _db != null and _sel_nat >= 0:
		for i in _db.get_division_count(_sel_nat):
			var div := _db.get_division(_sel_nat, i)
			rows.append(_display_name(div.name_key))
	_set_combo_items(combo, rows)
	_sel_div = 0 if rows.size() > 0 else -1
	_populate_combos()


# Each row is "<head display> - <body display>" (last - first).
# [orig: populate_avatar_combo_list @ 0x560210]
func _populate_combos() -> void:
	var combo := _id("COMBO_LIST")
	if combo < 0:
		return
	var rows := PackedStringArray()
	if _db != null and _sel_nat >= 0 and _sel_div >= 0:
		for i in _db.get_combo_count(_sel_nat, _sel_div):
			var c := _db.get_combo(_sel_nat, _sel_div, i)
			var last := _display_name(c.get_head().display_name)
			var first := _display_name(c.get_body().display_name)
			rows.append("%s - %s" % [last, first])
	_set_combo_items(combo, rows)
	_populate_voices()
	_refresh_preview()


# The voice list is avatar-derived: DEFAULT_VOICE (value 0) plus every ENABLED
# voice-table row whose sex matches the selected head's `sex` byte, each row
# valued with its CHARVOICE id. A persisted override that is not one of those
# rows is reset to DEFAULT_VOICE, and the list then selects BY VALUE.
# retail: populate_player_voice_combo @ 0x55dce0 -- clear the list, resolve the
# selected avatar's SEX through the combo record (sub_57AE90 @ 0x57ae90 returns
# combo+280, which CAvatarDefs_ParseConfigLine stores from the head part's
# sex field @ 0x57aad2; a combo the registry cannot resolve yields 0 = male),
# add DEFAULT_VOICE, walk the table, then
# `if (!found) profile[team + 1532] = 0` and
# UIList_SelectByValue(list, profile[team + 1532], 1).
func _populate_voices() -> void:
	var combo := _id("PLAYERVOICE")
	if combo < 0:
		return
	# The values (DEFAULT_VOICE first, then the enabled table rows of the
	# selected head's sex) and the persisted-override reset are the engine's;
	# this companion resolves the labels and applies the selection.
	var values := WeaponDatabase.player_info_voice_values(_selected_combo_head_sex())
	var rows := PackedStringArray()
	_voice_values.clear()
	for voice in values:
		_voice_values.append(voice)
		rows.append(_menu_text("DEFAULT_VOICE", "Default")
				if voice == WeaponDatabase.DEFAULT_VOICE_VALUE
				else _menu_text("CHARVOICE_%d" % voice, "Voice %d" % voice))
	_voice_override[_team] = WeaponDatabase.player_info_voice_selection(selected_voice(), values)
	_set_combo_items(combo, rows)
	_driver.select_row(combo, maxi(_voice_values.find(selected_voice()), 0), false)


## The persisted voice override for the shown side; 0 = DEFAULT_VOICE, otherwise
## the CHARVOICE id the PLAYERVOICE list carries as that row's value.
func selected_voice() -> int:
	return int(_voice_override.get(_team, WeaponDatabase.DEFAULT_VOICE_VALUE))


# The selected combo head's SEX byte, the voice list's filter key. An
# unresolvable selection yields 0 (male) the way retail's failed registry
# lookup does. retail: sub_57AE90 @ 0x57ae90 -> MinimapSlot_FindByPackedId
# @ 0x57a270, `return 0` on a miss @ 0x57aeb5.
func _selected_combo_head_sex() -> int:
	var head := _selected_combo_head()
	return head.sex if head != null else 0


# The selected combo head's own `voice` byte, the voice PREVIEW fallback.
# retail: Avatars_ResolveSelectionIndex @ 0x57ae60 reads combo+284, stored from
# the head part's voice field @ 0x57aae3.
func _selected_combo_head_voice() -> int:
	var head := _selected_combo_head()
	return head.voice if head != null else -1


func _selected_combo_head() -> AvatarPartRow:
	if _db == null or _sel_nat < 0 or _sel_div < 0:
		return null
	var combo := _id("COMBO_LIST")
	var idx := _driver.selected_row(combo) if combo >= 0 else 0
	if idx < 0:
		idx = 0
	if idx >= _db.get_combo_count(_sel_nat, _sel_div):
		return null
	return _db.get_combo(_sel_nat, _sel_div, idx).get_head()


# PLAYERVOICE selection: store the picked ROW VALUE as this side's override.
# Retail does NOT repopulate the list here.
# retail: sub_560030 @ 0x560030 -- `g_curPlayerProfile[teamIndex + 1532] =
# *(BYTE *)(eventData + 16)`, the selection notification's value byte.
func _on_voice_selected(row: int, _value: String) -> void:
	if _populating:
		return
	_voice_override[_team] = (_voice_values[row]
			if row >= 0 and row < _voice_values.size() else WeaponDatabase.DEFAULT_VOICE_VALUE)


func _preview_voice() -> void:
	if _driver == null:
		return
	# retail: PlayerInfo_PreviewVoice @ 0x55ff70 -- the persisted override when
	# NON-ZERO, else the selected combo head's own voice byte.
	var voice := selected_voice()
	if voice == WeaponDatabase.DEFAULT_VOICE_VALUE:
		voice = _selected_combo_head_voice()
	if voice < 0:
		return
	_driver.play_widget_sound(VOICE_PREVIEW_TRIGGER_FORMAT % voice, VOICE_PREVIEW_BANK)


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
# [orig: update_player_preview_animation active test @ 0x55dba0].
func _on_widget_hover_changed(id: int, hovered: bool) -> void:
	if _driver == null or _driver.get_menu_file() != _wired_file:
		return
	if _preview_id < 0 or id != _preview_id:
		return
	if _preview != null and is_instance_valid(_preview):
		_preview.set_hovered(hovered)


func _refresh_preview() -> void:
	if _preview == null or _db == null or _sel_nat < 0 or _sel_div < 0:
		return
	var idx := _selected_combo_index()
	if idx < 0 or idx >= _db.get_combo_count(_sel_nat, _sel_div):
		return
	# [orig: combo -> spawned-player model is D-PLAYERINFO-1, unwitnessed; the
	# portrait stops at the resolved part .3di geometry.]
	_preview.load_combo(_db.get_combo(_sel_nat, _sel_div, idx))


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
	if _populating:
		return
	_sel_nat = _nat_db_index[row] if row >= 0 and row < _nat_db_index.size() else -1
	_populate_divisions()  # resets the division selection and refills division + combo


func _on_div_selected(row: int, _value: String) -> void:
	if _populating:
		return
	_sel_div = row
	_populate_combos()


# retail: PlayerInfo_HandleVoiceSelect @ 0x55fe00 -- the COMBO_LIST handler
# despite its name (registered against "COMBO_LIST" @ 0x5615a6): it stores the
# picked avatar and rebuilds PLAYERVOICE for the new head.
func _on_combo_selected(_row: int, _value: String) -> void:
	if _populating:
		return
	_populate_voices()  # the voice list is avatar-derived; refresh on a combo change
	_refresh_preview()


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


# A team change re-filters the nationality list and resets the cascade
# [orig: PlayerInfo_SaveAndRepopulate @ 0x5608f0 re-runs PopulateAllControls(team)].
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
	return _character_state.make_selection(_team, _sel_nat, _sel_div,
			_selected_combo_index(), _selected_player_class())


func _remember_current_character_selection() -> void:
	var current := _current_character_selection()
	if current != null:
		_character_state.remember(current)


# The current selection, including both side records, for main_game to persist
# on ACCEPT. Class is stamped across both side snapshots because retail's dialog
# walks both 0x8006 blocks before serializing; character bytes remain per-side
# [orig: save_player_info_from_dialog @0x55EE3F-0x55EF38].
func snapshot() -> Dictionary:
	var combo := _id("COMBO_LIST")
	var player_class := _selected_player_class()
	# The persisted voice is the picked row's VALUE, not its position: retail
	# stores the notification's value byte into the profile and re-selects the
	# list by that value on the next populate.
	var profile := _character_state.snapshot(
			_team, _sel_nat, _sel_div,
			_driver.selected_row(combo) if combo >= 0 else -1,
			player_class, _edit_text("PLAYERNAME"), selected_voice())
	# Missing weapon.def means there was no loadout choice to commit. Keep that
	# distinct from a loaded screen whose three selected rows are explicitly NONE.
	if _weapons != null and _weapons.is_loaded():
		var primary := _selected_weapon("PRIMARY")
		var secondary := _selected_weapon("SECONDARY")
		var accessory := _selected_weapon("ACCESSORY")
		# Clip counts are the recorded picks, -1 = untouched default — the kit
		# tuple's serialized semantic [orig: serialize_weapon_loadout @ 0x55e4b0
		# writes the saved arrays; "-1" is the filler]. The type bytes are the
		# tuple's flags field [orig: g_playerInfoAmmoTypePri/Sec].
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
## retail: serialize_weapon_loadout @ 0x55e4b0 -- knife first (@0x55e4dc picks
## the blade off g_playerInfoTeamMask), the class-5 medpack block (@0x55e624),
## the PRIMARY/SECONDARY/ACCESSORY selections (@0x55e6bb), then the fixed
## g_playerInfoGrenadeSlots[0..2] walk (@0x55e7e0, bounded by
## g_playerInfoAmmoPriCounts @ 0x25DC560 = three dwords past 0x25DC554).
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
# populate_weapon_slot_lists @ 0x560430, read back through
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
	if kind != "combo":
		return
	var handler: Callable = _combo_handlers.get(widget_name.to_upper(), Callable())
	if handler.is_valid():
		handler.call(index, value)


func _radio_checked(name: String) -> bool:
	var id := _id(name)
	return id >= 0 and _driver.is_widget_checked(id)


func _menu_text(key: String, fallback: String) -> String:
	# DEFAULT_VOICE / CHARVOICE_%d are menu UI strings: menutxt's "Menu" then
	# gameui's "Avatars", else the readable fallback. Voice labels are cosmetic,
	# so a miss never blocks population.
	return Strings.lookup_or(Strings.TABLE_MENUTXT, Strings.SECTION_MENU, key,
			Strings.lookup_or(Strings.TABLE_GAMEUI, ATBL_SECTION, key, fallback))
