class_name PlayerInfoMenuHost
extends RefCounted

# Drives the JO PLAYER_INFO screen (player.mnu) by control NAME: fills the
# NATIONALITY / DIVISION / COMBO_LIST / PLAYERVOICE comboboxes from Avatars.def and
# runs the nationality -> division -> combo cascade plus the SIDE_BLUE/SIDE_RED team
# filter. It is a companion the game-agnostic NovaMenuHost (menu_shell.gd) delegates
# to -- the same pattern as mp_menu_host.gd -- claimed by the NATIONALITY + COMBO_LIST
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

# The 3D character preview (head/body/arms .3di composited), reused from the ONED
# Avatars workspace. Mounted into the PLAYER_PREVIEW widget rect and fed the resolved
# combo; static at rest (no .adm bound) behind the D-PLAYERINFO-1 seam.
const AvatarPreviewScript := preload("res://modtools/avatar/avatar_preview.gd")

var _menu: Node                         # the built NovaMnuMenu (typed Node: only its tree is used)
var _root: NovaResourceRoot
var _text: RtxtStringFile               # the "Avatars" string table (from menutxt.BIN)
var _db: NovaAvatarDatabase
var _team := 0                          # 0 = blue/good, 1 = red/evil (SIDE_BLUE default CHECKED)
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


# Called by NovaMenuHost after each open_menu (re)build of a menu we own. The screen's
# controls are freshly built children, so we wire and populate from scratch each time.
func on_menu_built(menu: Node, _file: String, _screen: String, root: NovaResourceRoot) -> void:
	_menu = menu
	_root = root
	_ensure_db()
	_ensure_text()
	_wire_team_radios()
	_connect_combo("NATIONALITY", _on_nat_selected)
	_connect_combo("DIVISION", _on_div_selected)
	_connect_combo("COMBO_LIST", _on_combo_selected)
	# SIDE_BLUE is CHECKED in player.mnu; team follows whichever radio is set.
	_team = 1 if _radio_checked("SIDE_RED") else 0
	_populate_nationalities()  # cascades into divisions -> combos -> voice
	_wire_preview()


# --- Avatars.def + RTXT loading (best-effort; degrade to empty combos) ---------

func _ensure_db() -> void:
	if _db != null or _root == null:
		return
	_db = NovaAvatarDatabase.new()
	if _db.load_from_resource_root(_root, "Avatars.def") != OK or not _db.is_loaded():
		push_warning("PlayerInfoMenuHost: Avatars.def not loaded (%s); avatar combos stay empty"
			% _db.get_last_error())
		_db = null


# Load the menu string table ourselves from the VFS so we stay decoupled from the
# shell internals; the nationality/division/combo display keys resolve against its
# "Avatars" section. Absent -> names fall back to their raw keys (still populates).
func _ensure_text() -> void:
	if _text != null or _root == null:
		return
	var bytes: PackedByteArray = _root.read_file("menutxt.BIN")
	if bytes.is_empty():
		return
	var t := RtxtStringFile.new()
	if t.load_from_byte_array(bytes) == OK:
		_text = t


func _display_name(key: String) -> String:
	if key.is_empty():
		return ""
	if _text != null and _text.has_string_in_section(ATBL_SECTION, key):
		return _text.get_string_in_section(ATBL_SECTION, key)
	return key  # faithful fallback: the raw key shows when there is no string entry


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


# --- 3D character preview (PLAYER_PREVIEW) ------------------------------------

# Mount the head/body/arms 3D preview into the PLAYER_PREVIEW widget rect (a custom
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
	_preview.set_resource_root(_root)
	_refresh_preview()


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


# --- ACCEPT seam (Phase 5) ----------------------------------------------------

# The current selection, for main_game to persist on ACCEPT. The in-world avatar
# (D-PLAYERINFO-1) and on-disk profile format are later phases; this does not invent
# one, it just reports the chosen indices + name.
func snapshot() -> Dictionary:
	var combo := _combo("COMBO_LIST")
	var voice := _combo("PLAYERVOICE")
	return {
		"name": _edit_text("PLAYERNAME"),
		"team": _team,
		"nationality": _sel_nat,
		"division": _sel_div,
		"combo": combo.get_selected() if combo != null else -1,
		"voice": voice.get_selected() if voice != null else -1,
	}


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
	# DEFAULT_VOICE / CHARVOICE_%d are menu UI strings; try the common sections, else
	# the readable fallback. Voice labels are cosmetic, so a miss never blocks population.
	if _text != null:
		for section in ["Menu", ATBL_SECTION]:
			if _text.has_string_in_section(section, key):
				return _text.get_string_in_section(section, key)
	return fallback


func _find(name: String) -> Node:
	return _menu.find_child(name, true, false) if _menu != null else null


func _connect_pressed(name: String, handler: Callable) -> void:
	var node := _find(name)
	if node is BaseButton and not (node as BaseButton).pressed.is_connected(handler):
		(node as BaseButton).pressed.connect(handler)


func _edit_text(name: String) -> String:
	var node := _find(name)
	return (node as LineEdit).text if node is LineEdit else ""
