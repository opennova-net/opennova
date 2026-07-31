class_name PlayerInfoMenuCompanion
extends MenuCompanion

# Drives the JO PLAYER_INFO screen (player.mnu) by control NAME: fills the
# NATIONALITY / DIVISION / COMBO_LIST / PLAYERVOICE comboboxes from Avatars.def and
# runs the nationality -> division -> combo cascade plus the SIDE_BLUE/SIDE_RED team
# filter. It is a companion the game-agnostic NovaMenuShell (nova_menu_shell.gd) delegates
# to -- the same pattern as mp_menu_companion.gd -- claimed by the NATIONALITY + COMBO_LIST
# controls unique to this screen.
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

# The 3D character preview (compatible head/body .3di composited), reused from the
# ONED Avatars workspace. Mounted into the PLAYER_PREVIEW widget rect and fed the
# resolved combo; it plays the witnessed raw-.bad idle when those assets resolve.
const AvatarPreviewScript := preload("res://engine/avatar/avatar_preview.gd")

const PARENT_SLOTS := {
	"PRIMARY": NovaWeaponDatabase.SLOT_PRIMARY,
	"SECONDARY": NovaWeaponDatabase.SLOT_SECONDARY,
	"ACCESSORY": NovaWeaponDatabase.SLOT_ACCESSORY,
}
# Only PRIMARY/SECONDARY author an ammo-TYPE combo (player.mnu statics FMJ/AP/SP,
# values 0/1/2); ACCESSORY and the grenades have none.
const TYPE_SLOTS := ["PRIMARY", "SECONDARY"]
const GRENADE_CONTROLS := ["GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"]

var _db: NovaAvatarDatabase
var _weapons: NovaWeaponDatabase     # weapon.def loadout table (PRIMARY/SECONDARY/ACCESSORY)
var _slot_rows: Dictionary = {}         # control name -> row-aligned weapon transport dicts
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
var _grenade_rows: Array[Dictionary] = []  # the first 3 class-3 defs, table order
var _nat_db_index: Array[int] = []      # NATIONALITY visible row -> nationality DB index
var _sel_nat := -1
var _sel_div := -1
var _populating := false                # guards the cascade against programmatic-fill re-entry
var _preview                            # AvatarPreview mounted in PLAYER_PREVIEW (null until wired)

# The current selection, for the ACCEPT/commit seam (Phase 5). main_game persists it.
signal avatar_chosen(profile: Dictionary)


# True when this is the JO PLAYER_INFO screen, so the shell delegates to us. Keyed on
# the NATIONALITY + COMBO_LIST controls unique to player.mnu's AVATARS block.
func owns_menu(menu: Node) -> bool:
	if menu == null:
		return false
	return menu.find_child("NATIONALITY", true, false) != null \
		and menu.find_child("COMBO_LIST", true, false) != null


# The screen's controls are freshly built children on each (re)build, so we wire and
# populate from scratch each time.
func _wire(_file: String, _screen: String) -> void:
	_ensure_db()
	_wire_team_radios()
	_connect_combo("NATIONALITY", _on_nat_selected)
	_connect_combo("DIVISION", _on_div_selected)
	_connect_combo("COMBO_LIST", _on_combo_selected)
	_connect_pressed(VOICE_PREVIEW_CONTROL, _preview_voice)
	# SIDE_BLUE is CHECKED in player.mnu; team follows whichever radio is set.
	_team = 1 if _radio_checked("SIDE_RED") else 0
	_populate_nationalities()  # cascades into divisions -> combos -> voice
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
	_populate_loadout()
	# OK saves the chosen avatar; the .mnu's own ACTION still navigates back to main.mnu.
	_connect_pressed("ACCEPT", commit)  # [orig: save_player_info_from_dialog @ 0x55ee10]


# --- Avatars.def loading (best-effort; degrade to empty combos) ----------------

func _ensure_db() -> void:
	if _db != null or _root == null:
		return
	_db = NovaAvatarDatabase.new()
	if _db.load_from_resource_root(_root, "Avatars.def") != OK or not _db.is_loaded():
		push_warning("PlayerInfoMenuCompanion: Avatars.def not loaded (%s); avatar combos stay empty"
			% _db.get_last_error())
		_db = null


# Resolve a nationality/division/combo display key against the gametext table's "Avatars"
# section -- the table the original consults for these names
# [orig: GameText_GetStringWithFallback @ 0x51eb90 / g_TextGameText @ 0xB4C2AC; "Avatars"
# section, docs/playerinfo/avatars-re.md]. The shell registers gametext (Game.bin) into the
# shared NovaStrings registry at boot. A miss falls back to the raw key (the witnessed
# fallback; not the "??section:key??" debug marker NovaStrings.lookup would return).
func _display_name(key: String) -> String:
	if key.is_empty():
		return ""
	var t: RtxtStringFile = NovaStrings.get_table("gameui")
	if t != null and t.has_string_in_section(ATBL_SECTION, key):
		return t.get_string_in_section(ATBL_SECTION, key)
	return key


# --- Loadout (weapon slot lists) ----------------------------------------------

## Inject a weapon database for tests or another shell-owned resource mount. Keeping
## this as a public seam lets callers exercise the same menu population path without
## reaching into companion internals. The recorded ammo picks are keyed by table index,
## so a table swap invalidates them — clear rather than misapply.
func set_weapon_database(weapons: NovaWeaponDatabase) -> void:
	_weapons = weapons
	_ammo_pri.clear()
	_ammo_sec.clear()
	if _menu != null:
		_populate_loadout()


# Load weapon.def into the loadout table (best-effort; absent -> empty slot lists).
func _ensure_weapons() -> void:
	if _weapons != null or _root == null:
		return
	_weapons = NovaWeaponDatabase.new()
	if _weapons.load_from_resource_root(_root, "weapon.def") != OK or not _weapons.is_loaded():
		push_warning("PlayerInfoMenuCompanion: weapon.def not loaded (%s); loadout combos stay empty"
			% _weapons.get_last_error())
		_weapons = null


# Fill PRIMARY/SECONDARY/ACCESSORY for the selected class + team, each led by a "NONE" row,
# then the ammo combos, weight readout, and icons that hang off the selections.
# [orig: populate_weapon_slot_lists @ 0x560430 -> populate_weapon_accessory_ammo_ui
#  @ 0x55e8b0 -> update_player_info_weight_and_weapon_icons @ 0x55f480]
func _populate_loadout() -> void:
	if _weapons == null:
		return
	var class_mask := _selected_class_mask()
	var team_mask := 2 if _team == 0 else 1  # [orig: g_playerInfoTeamMask = 2 - (team != 0)]
	_fill_weapon_slot("PRIMARY", NovaWeaponDatabase.SLOT_PRIMARY, class_mask, team_mask)
	_fill_weapon_slot("SECONDARY", NovaWeaponDatabase.SLOT_SECONDARY, class_mask, team_mask)
	_fill_weapon_slot("ACCESSORY", NovaWeaponDatabase.SLOT_ACCESSORY, class_mask, team_mask)
	for control in PARENT_SLOTS:
		_populate_slot_ammo(control)
	_populate_grenades(class_mask, team_mask)
	_update_weight()
	_update_icons()


func _fill_weapon_slot(control: String, slot: int, class_mask: int, team_mask: int) -> void:
	var combo := _combo(control)
	if combo == null:
		return
	var rows := PackedStringArray()
	var defs: Array[Dictionary] = []
	rows.append(_menu_text("NONE", "None"))  # NONE at index 0 [orig: @ 0x560430]
	defs.append({})
	for w in _weapons.get_slot_weapons(slot, class_mask, team_mask):
		rows.append(_weapon_label(w))
		defs.append(w)
	_slot_rows[control] = defs
	_set_combo_items(combo, rows)


# Return the selected weapon.def transport row for a loadout control. NONE and an
# absent control both resolve to an empty dictionary.
func _selected_weapon(control: String) -> Dictionary:
	var combo := _combo(control)
	var defs: Array = _slot_rows.get(control, [])
	if combo == null:
		return {}
	var row := combo.get_selected()
	if row < 0 or row >= defs.size():
		return {}
	return (defs[row] as Dictionary).duplicate(true)


# Weapon display name = loadout_menu_textid resolved in gametext's "WepDes" section, else the
# raw weapon id [orig: populate_weapon_slot_lists @ 0x560430: entry+40 textid else entry+0].
func _weapon_label(w: Dictionary) -> String:
	var textid := String(w.get("display_textid", ""))
	if not textid.is_empty():
		var t: RtxtStringFile = NovaStrings.get_table("gametext")
		if t != null and t.has_string_in_section("WepDes", textid):
			return t.get_string_in_section("WepDes", textid)
	return String(w.get("name", ""))


# PLAYERCLASS carries values 5..9 (Medic..Engineer); the class mask is the matching
# power-of-two bit [orig: PlayerInfo_SetTeamAndClassMask @ 0x55de60: 5->1,6->2,7->4,8->8,9->16].
func _selected_class_mask() -> int:
	var combo := _combo("PLAYERCLASS")
	if combo == null:
		return 0x1F  # no class control -> show every class's weapons (defensive)
	var val := combo.get_selected_value()
	var cls := int(val) if val.is_valid_int() else 0
	return (1 << (cls - 5)) if cls >= 5 and cls <= 9 else 0


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
	var index := int(w.get("index", -1))
	var clipsize := int(w.get("clipsize", 0))
	var ammo1 := _combo(control + "_AMMO1")
	var type_combo := _combo(control + "_AMMO1_TYPE")
	var ammo2 := _combo(control + "_AMMO2")
	var has_ammo := not w.is_empty() and clipsize > 0
	if ammo1 != null:
		ammo1.visible = has_ammo
		if has_ammo:
			var maxclips := int(w.get("maxclips", 0))
			var rows := PackedStringArray()
			# Rows 1..maxclips: "%d - %s" = rounds + round label; row value = the
			# clip count (retail keys rows by the def index; ours by position).
			for clips in range(1, maxclips + 1):
				rows.append(_ammo_row_label(w, clips))
			_set_combo_items(ammo1, rows)
			# Saved count selects its row; -1/absent = the maxclips row (full
			# default) [orig: the `saved == i || (saved == -1 && i == maxclips)`
			# select in both fills].
			var saved := int(_ammo_pri.get(index, -1))
			ammo1.select_silent((clampi(saved, 1, maxclips) if saved > 0 else maxclips) - 1)
	if type_combo != null:
		# The TYPE combo keeps its authored FMJ/AP/SP statics; shown with AMMO1,
		# selection = the saved per-team type byte (-1 -> 0). flags2 NOAMMOTYPES
		# locks it non-interactive and resets the saved type
		# [orig: @ 0x55def0 — the +188 & 0x40 gate -> UIWidget_SetInteractiveRecursive].
		type_combo.visible = has_ammo
		if has_ammo:
			var locked := (int(w.get("flags2", 0)) & NovaWeaponDatabase.FLAG2_NOAMMOTYPES) != 0
			type_combo.disabled = locked
			if locked:
				_slot_type_store(control)[_team] = 0
			# Select by the row's authored VALUE (0/1/2), not its position — the
			# saved byte is the value [orig: the @ 0x55def0 row select].
			var saved_type := str(int(_slot_type_store(control).get(_team, 0)))
			for row in type_combo.get_item_count():
				if type_combo.get_item_value(row) == saved_type:
					type_combo.select_silent(row)
					break
	if ammo2 != null:
		var sub := _subclass_weapon(w)
		var sub_ok := has_ammo and not sub.is_empty() and int(sub.get("clipsize", 0)) > 0
		ammo2.visible = sub_ok
		if sub_ok:
			var sub_max := int(sub.get("maxclips", 0))
			var rows2 := PackedStringArray()
			for clips in range(1, sub_max + 1):
				rows2.append(_ammo_row_label(sub, clips))
			_set_combo_items(ammo2, rows2)
			var saved2 := int(_ammo_sec.get(index, -1))
			ammo2.select_silent((clampi(saved2, 1, sub_max) if saved2 > 0 else sub_max) - 1)


# The sub-weapon behind *_AMMO2: walk the parent's following table entries, bounded
# by loadout_subclasses, skipping entries whose round_type equals the parent's; the
# first DIFFERING entry is the sub-weapon (satchel -> detonator)
# [orig: the stricmp walk over entry+192.. in @ 0x55def0 / @ 0x55e8b0 / @ 0x55f1f0].
func _subclass_weapon(parent: Dictionary) -> Dictionary:
	if parent.is_empty() or _weapons == null:
		return {}
	var parent_index := int(parent.get("index", -1))
	var parent_round := String(parent.get("round_type", ""))
	var subclasses := int(parent.get("loadout_subclasses", 0))
	for k in range(1, subclasses + 1):
		var cand := _weapons.get_weapon(parent_index + k)
		if cand.is_empty():
			return {}
		if String(cand.get("round_type", "")).nocasecmp_to(parent_round) != 0:
			return cand
	return {}


# The first three selectable class-3 defs passing the class+team masks own
# GRENADE_AMMO1..3 in table order; leftover widgets hide. Rows 0..maxclips
# INCLUDING the zero row [orig: the >= 3 leg of populate_ammo_combo_boxes
# @ 0x55def0 — filter, table order, hidden leftovers, zero row].
func _populate_grenades(class_mask: int, team_mask: int) -> void:
	_grenade_rows = []
	if _weapons != null:
		var dicts: Array = _weapons.get_slot_weapons(
				NovaWeaponDatabase.SLOT_GRENADE, class_mask, team_mask)
		for i in mini(dicts.size(), GRENADE_CONTROLS.size()):
			_grenade_rows.append(dicts[i] as Dictionary)
	for i in GRENADE_CONTROLS.size():
		var combo := _combo(GRENADE_CONTROLS[i])
		if combo == null:
			continue
		if i >= _grenade_rows.size():
			combo.visible = false
			continue
		combo.visible = true
		var w := _grenade_rows[i]
		var maxclips := int(w.get("maxclips", 0))
		var rows := PackedStringArray()
		for clips in range(0, maxclips + 1):
			rows.append(_ammo_row_label(w, clips))
		_set_combo_items(combo, rows)
		var saved := int(_ammo_pri.get(int(w.get("index", -1)), -1))
		combo.select_silent(maxclips if saved < 0 else clampi(saved, 0, maxclips))


# "<rounds> - <round label>" [orig: sprintf "%d - %s" with i*clipsize + round_type
# in every ammo fill; the label resolves through gametext "WepDes"].
func _ammo_row_label(w: Dictionary, clips: int) -> String:
	var round_label := String(w.get("round_type", ""))
	if not round_label.is_empty():
		var gametext: RtxtStringFile = NovaStrings.get_table("gametext")
		if gametext != null and gametext.has_string_in_section("WepDes", round_label):
			round_label = gametext.get_string_in_section("WepDes", round_label)
	return "%d - %s" % [clips * int(w.get("clipsize", 0)), round_label]


func _slot_type_store(control: String) -> Dictionary:
	if not _ammo_type.has(control):
		_ammo_type[control] = {}
	return _ammo_type[control]


## The recorded clip pick for a parent slot's selected weapon, -1 = untouched (the
## def default). Public read seam (ADR 0018), mirroring the original's saved-count
## array rather than the combo position — retail serializes -1 until the user picks.
func selected_clips(control: String) -> int:
	var w := _selected_weapon(control)
	if w.is_empty():
		return -1
	return int(_ammo_pri.get(int(w.get("index", -1)), -1))


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
	var index := int(_selected_weapon(control).get("index", -1))
	if index >= 0:
		_ammo_pri[index] = row + 1  # [orig: @ 0x55f730 — stores selected_row + 1]
	_update_weight()
	_update_icons()  # the original's combined refresh [orig: @ 0x55f480]


func _on_ammo2_selected(row: int, _value: String, control: String) -> void:
	if _populating:
		return
	var index := int(_selected_weapon(control).get("index", -1))
	if index >= 0:
		_ammo_sec[index] = row + 1  # [orig: @ 0x55f730 ctx 1 — the interleaved pair]
	_update_weight()
	_update_icons()


func _on_type_selected(_row: int, value: String, control: String) -> void:
	if _populating:
		return
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
		_ammo_pri[int(_grenade_rows[i].get("index", -1))] = row
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
		if w.is_empty():
			continue
		var index := int(w.get("index", -1))
		indices.append(index)
		counts.append(int(_ammo_pri.get(index, -1)))
		# The witnessed sub-weapon term is gated on the *_AMMO2 control existing.
		var sub := _subclass_weapon(w)
		if _combo(control + "_AMMO2") != null \
				and not sub.is_empty() and int(sub.get("clipsize", 0)) > 0:
			var saved2 := int(_ammo_sec.get(index, -1))
			var eff2 := int(sub.get("maxclips", 0)) if saved2 <= 0 else saved2
			total += eff2 * float(sub.get("clip_weight", 0.0))
	total += _weapons.loadout_weight(indices, counts)
	for i in _grenade_rows.size():
		# The witnessed grenade term is gated on the control existing AND shown.
		var combo := _combo(GRENADE_CONTROLS[i]) if i < GRENADE_CONTROLS.size() else null
		if combo == null or not combo.visible:
			continue
		var g := _grenade_rows[i]
		var saved := int(_ammo_pri.get(int(g.get("index", -1)), -1))
		var clips := int(g.get("maxclips", 0)) if saved == -1 else saved
		total += clips * float(g.get("clip_weight", 0.0))
	var band := _weapons.encumbrance_class(total)
	var encumbrance := _menu_ui_text("LIGHT_ENCUMBRANCE", "Light")
	if band == NovaWeaponDatabase.ENCUMBRANCE_HEAVY:
		encumbrance = _menu_ui_text("HEAVY_ENCUMBRANCE", "Heavy")
	elif band == NovaWeaponDatabase.ENCUMBRANCE_NORMAL:
		encumbrance = _menu_ui_text("NORMAL_ENCUMBRANCE", "Normal")
	var node := _find("STATIC_TOTAL_WEIGHT")
	var label := node as Label
	if label == null and node != null:
		label = node.find_child("Label", false, false) as Label
	if label != null:
		# [orig: update_player_info_weight_and_weapon_icons @ 0x55f480 —
		#  sprintf "%s %.1f %s (%s)", keys TOTAL_WEIGHT / LBS / *_ENCUMBRANCE]
		label.text = "%s %.1f %s (%s)" % [
			_menu_ui_text("TOTAL_WEIGHT", "Total Weight"), total,
			_menu_ui_text("LBS", "lbs"), encumbrance]


# Texture the PRIMARY/SECONDARY/ACCESSORY_ICON windows from the selected def's
# loadout_menu_icon (+144); NONE clears. GRENADE_ICON keeps its authored static
# [orig: @ 0x55f480 — icons from weapon +144; GRENADE_ICON untouched; retail's
#  no-selection resolves to the blank entry-0 icon, our NONE row clears].
func _update_icons() -> void:
	for control in PARENT_SLOTS:
		var holder := _find(control + "_ICON") as Control
		if holder == null:
			continue
		var icon_rect := holder.get_node_or_null("LoadoutIcon") as TextureRect
		if icon_rect == null:
			icon_rect = TextureRect.new()
			icon_rect.name = "LoadoutIcon"
			icon_rect.set_anchors_preset(Control.PRESET_FULL_RECT)
			icon_rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
			icon_rect.stretch_mode = TextureRect.STRETCH_SCALE
			icon_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
			holder.add_child(icon_rect)
		var icon_name := String(_selected_weapon(control).get("icon", ""))
		if icon_name.is_empty() or _root == null:
			icon_rect.texture = null
		else:
			icon_rect.texture = _root.load_texture(
					icon_name, NovaResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)


# The weight/encumbrance keys are menu-UI tokens (menutxt "Menu", else gameui
# "Menu") — a different section set than the Avatars display keys.
func _menu_ui_text(key: String, fallback: String) -> String:
	for spec in [["menutxt", "Menu"], ["gameui", "Menu"]]:
		var t: RtxtStringFile = NovaStrings.get_table(spec[0])
		if t != null and t.has_string_in_section(spec[1], key):
			return t.get_string_in_section(spec[1], key)
	return fallback


# --- Population (the cascade) -------------------------------------------------

# Fill NATIONALITY, filtered by team alignment, then cascade into division/combo/voice.
# [orig: PlayerInfo_PopulateNationalityList @ 0x55d8c0; team filter D-PLAYERINFO-5]
func _populate_nationalities() -> void:
	var combo := _combo("NATIONALITY")
	if combo == null:
		return
	_nat_db_index.clear()
	var rows := PackedStringArray()
	if _db != null:
		for i in _db.get_nationality_count():
			var nat: Dictionary = _db.get_nationality(i)
			var align := int(nat.get("alignment", 0))
			# show only when (alignment != 0) == (team != 0): good->blue(0), evil->red(1)
			if (align != 0) != (_team != 0):
				continue
			_nat_db_index.append(i)
			rows.append(_display_name(String(nat.get("name_key", ""))))
	_set_combo_items(combo, rows)
	_sel_nat = _nat_db_index[0] if not _nat_db_index.is_empty() else -1
	_populate_divisions()


# [orig: PlayerInfo_PopulateDivisionList @ 0x55da50]
func _populate_divisions() -> void:
	var combo := _combo("DIVISION")
	if combo == null:
		return
	var rows := PackedStringArray()
	if _db != null and _sel_nat >= 0:
		for i in _db.get_division_count(_sel_nat):
			var div: Dictionary = _db.get_division(_sel_nat, i)
			rows.append(_display_name(String(div.get("name_key", ""))))
	_set_combo_items(combo, rows)
	_sel_div = 0 if rows.size() > 0 else -1
	_populate_combos()


# Each row is "<head display> - <body display>" (last - first).
# [orig: populate_avatar_combo_list @ 0x560210]
func _populate_combos() -> void:
	var combo := _combo("COMBO_LIST")
	if combo == null:
		return
	var rows := PackedStringArray()
	if _db != null and _sel_nat >= 0 and _sel_div >= 0:
		for i in _db.get_combo_count(_sel_nat, _sel_div):
			var c: Dictionary = _db.get_combo(_sel_nat, _sel_div, i)
			var head: Dictionary = c.get("head", {})
			var body: Dictionary = c.get("body", {})
			var last := _display_name(String(head.get("display_name", "")))
			var first := _display_name(String(body.get("display_name", "")))
			rows.append("%s - %s" % [last, first])
	_set_combo_items(combo, rows)
	_populate_voices()
	_refresh_preview()


# The voice list is avatar-derived: a default entry plus the selected character's
# voice. [orig: PlayerInfo_HandleVoiceSelect @ 0x55fe00 -- DEFAULT_VOICE + CHARVOICE_%d]
func _populate_voices() -> void:
	var combo := _combo("PLAYERVOICE")
	if combo == null:
		return
	var rows := PackedStringArray()
	rows.append(_menu_text("DEFAULT_VOICE", "Default"))
	var voice := _selected_combo_head_voice()
	if voice >= 0:
		rows.append(_menu_text("CHARVOICE_%d" % voice, "Voice %d" % voice))
	_set_combo_items(combo, rows)


func _selected_combo_head_voice() -> int:
	if _db == null or _sel_nat < 0 or _sel_div < 0:
		return -1
	var combo := _combo("COMBO_LIST")
	var idx := combo.get_selected() if combo != null else 0
	if idx < 0:
		idx = 0
	if idx >= _db.get_combo_count(_sel_nat, _sel_div):
		return -1
	var c: Dictionary = _db.get_combo(_sel_nat, _sel_div, idx)
	var head: Dictionary = c.get("head", {})
	return int(head.get("voice", -1))


func _preview_voice() -> void:
	var voice := _selected_combo_head_voice()
	if voice < 0 or _menu == null or not _menu.has_method("play_widget_sound"):
		return
	# The persisted profile override is owned by D-PLAYERINFO-9; until that profile
	# field exists, retail's selected-avatar fallback is the authoritative voice.
	_menu.call("play_widget_sound", VOICE_PREVIEW_TRIGGER_FORMAT % voice, VOICE_PREVIEW_BANK)


# --- 3D character preview (PLAYER_PREVIEW) ------------------------------------

# Mount the head/body 3D preview into the PLAYER_PREVIEW widget rect (a custom
# button surface in player.mnu) and feed it the current combo. Null-guarded: a menu
# without the widget, or without an avatar db / resource root, simply shows no preview.
func _wire_preview() -> void:
	var rect := _find("PLAYER_PREVIEW")
	if rect == null or not (rect is Control):
		return
	_preview = AvatarPreviewScript.new()
	_preview.name = "PlayerInfoAvatarPreview"
	_preview.set_anchors_preset(Control.PRESET_FULL_RECT)
	_preview.mouse_filter = Control.MOUSE_FILTER_IGNORE  # let the button keep its clicks
	(rect as Control).add_child(_preview)
	# Locked menu portrait: no grid/axes, camera fixed, character facing the viewer.
	# The editor Avatars workspace keeps the interactive fly camera; only this runtime
	# mount opts into the portrait (idle spin + hover zoom/sway, see AvatarPreview).
	_preview.set_menu_preview(true)
	_preview.set_resource_root(_root)
	# Mousing over the preview button drives the zoom + sway, like the original
	# [orig: update_player_preview_animation active test @ 0x55dba0].
	var preview_button := rect as Control
	if not preview_button.mouse_entered.is_connected(_on_preview_hover):
		preview_button.mouse_entered.connect(_on_preview_hover.bind(true))
		preview_button.mouse_exited.connect(_on_preview_hover.bind(false))
	_refresh_preview()


func _on_preview_hover(hovered: bool) -> void:
	if _preview != null and is_instance_valid(_preview):
		_preview.set_hovered(hovered)


func _refresh_preview() -> void:
	if _preview == null or _db == null or _sel_nat < 0 or _sel_div < 0:
		return
	var idx := _selected_combo_index()
	if idx < 0 or idx >= _db.get_combo_count(_sel_nat, _sel_div):
		return
	# [orig: combo -> spawned-player model is D-PLAYERINFO-1, unwitnessed; the preview
	# stops at the resolved part .3di geometry, as the ONED Avatars workspace does.]
	_preview.load_combo(_db.resolve_combo(_sel_nat, _sel_div, idx))


func _selected_combo_index() -> int:
	var combo := _combo("COMBO_LIST")
	if combo == null:
		return -1
	var idx := combo.get_selected()
	return idx if idx >= 0 else 0


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


func _on_combo_selected(_row: int, _value: String) -> void:
	if _populating:
		return
	_populate_voices()  # the voice list is avatar-derived; refresh on a combo change
	_refresh_preview()


# --- Team radios (SIDE_BLUE / SIDE_RED) ---------------------------------------

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
	_team = team
	_populate_nationalities()
	_populate_loadout()  # the team mask re-filters the weapon slot lists


# --- ACCEPT seam (Phase 5) ----------------------------------------------------

# The current selection, for main_game to persist on ACCEPT. The in-world avatar
# (D-PLAYERINFO-1) and on-disk profile format are later phases; this does not invent
# one, it just reports the chosen indices + name.
func snapshot() -> Dictionary:
	var combo := _combo("COMBO_LIST")
	var voice := _combo("PLAYERVOICE")
	var profile := {
		"name": _edit_text("PLAYERNAME"),
		"team": _team,
		"nationality": _sel_nat,
		"division": _sel_div,
		"combo": combo.get_selected() if combo != null else -1,
		"voice": voice.get_selected() if voice != null else -1,
	}
	var class_combo := _combo("PLAYERCLASS")
	if class_combo != null:
		var class_value := class_combo.get_selected_value()
		profile["player_class"] = int(class_value) if class_value.is_valid_int() else 0
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
			"primary": String(primary.get("name", "")),
			"primary_clips": selected_clips("PRIMARY"),
			"primary_ammo_type": selected_ammo_type("PRIMARY"),
			"secondary": String(secondary.get("name", "")),
			"secondary_clips": selected_clips("SECONDARY"),
			"secondary_ammo_type": selected_ammo_type("SECONDARY"),
			"accessory": String(accessory.get("name", "")),
			"accessory_clips": selected_clips("ACCESSORY"),
		})
	return profile


func commit() -> void:
	avatar_chosen.emit(snapshot())


# --- Helpers ------------------------------------------------------------------

func _combo(name: String) -> NovaMnuCombo:
	return _find(name) as NovaMnuCombo


# Fill a combo and pre-select the first row without firing the cascade (the fill is
# programmatic; user selections come through item_selected). select_silent suppresses
# the relay; the _populating guard covers any incidental emit from set_items.
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


func _radio_checked(name: String) -> bool:
	var node := _find(name)
	return node is BaseButton and (node as BaseButton).button_pressed


func _menu_text(key: String, fallback: String) -> String:
	# DEFAULT_VOICE / CHARVOICE_%d are menu UI strings: try menutxt's "Menu" then gametext's
	# "Avatars" in the shared registry, else the readable fallback. Voice labels are cosmetic,
	# so a miss never blocks population.
	for spec in [["menutxt", "Menu"], ["gameui", ATBL_SECTION]]:
		var t: RtxtStringFile = NovaStrings.get_table(spec[0])
		if t != null and t.has_string_in_section(spec[1], key):
			return t.get_string_in_section(spec[1], key)
	return fallback


