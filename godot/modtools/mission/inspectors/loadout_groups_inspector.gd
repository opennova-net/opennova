extends "res://modtools/mission/inspectors/inspector_section.gd"


# --- Weapon loadout + groups (mission-global collapsibles) --------------------
# Like the header form, these are shown whenever a mission is loaded, in any mode, and built
# ONCE. The loadout is a list of kit tuples {name, ammo_primary, ammo_secondary, flags}
# (net-re §5.63): the ammo fields are requested clip counts and flags is the per-ammo
# damage-class request (1 = x0.9, 2 = x1.1, anything else neutral); -1 everywhere means
# "weapon default". Groups are 64 fixed records with three editable ints each. Selection is
# inspector-local (no in-world interaction). The *_syncing guards stop a programmatic
# repopulate from echoing back as an edit. Name/value edits commit on Enter / focus-out (not
# per keystroke) to keep the caret; group spins commit on change. Every commit replaces the
# whole loadout / writes one group = one undo step.
var _loadout_toggle: CheckButton
var _loadout_box: VBoxContainer
var _loadout_status: Label
var _loadout_list: ItemList
var _loadout_name: LineEdit
var _loadout_ammo_pri: LineEdit
var _loadout_ammo_sec: LineEdit
var _loadout_damage: LineEdit
var _loadout_delete: Button
var _loadout_selected: int = -1
var _loadout_syncing: bool = false

var _groups_toggle: CheckButton
var _groups_box: VBoxContainer
var _groups_list: ItemList
var _group_spins: Array = []  # [flags, value, constant10]
var _groups_selected: int = -1
var _groups_syncing: bool = false


# --- Weapon loadout (mission-global collapsible) ------------------------------

func _build_loadout_panel() -> void:
	_loadout_toggle = CheckButton.new()
	_loadout_toggle.name = "MissionLoadoutToggle"
	_loadout_toggle.text = "Weapon loadout"
	_loadout_toggle.tooltip_text = "The weapons the player spawns with (single player only). An empty list means the game's default kit."
	_loadout_toggle.button_pressed = false
	_loadout_toggle.visible = false
	_inspector._mission_content.add_child(_loadout_toggle)

	_loadout_box = VBoxContainer.new()
	_loadout_box.name = "MissionLoadoutBox"
	_loadout_box.add_theme_constant_override("separation", 4)
	_loadout_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_loadout_box.visible = false
	_inspector._mission_content.add_child(_loadout_box)
	# Repopulate on expand (the per-`changed` refresh skips the list rebuild while collapsed).
	_loadout_toggle.toggled.connect(func(on: bool) -> void:
		_loadout_box.visible = on
		if on:
			_refresh_loadout_panel())

	_loadout_status = InspectorForms.add_muted_label(_loadout_box, "")
	_loadout_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	var add_button := Button.new()
	add_button.name = "MissionLoadoutAdd"
	add_button.text = "Add weapon"
	add_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_loadout_box.add_child(add_button)
	add_button.pressed.connect(_on_loadout_add_pressed)

	_loadout_list = ItemList.new()
	_loadout_list.name = "MissionLoadoutList"
	_loadout_list.select_mode = ItemList.SELECT_SINGLE
	_loadout_list.custom_minimum_size = Vector2(0, 120)
	_loadout_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_loadout_box.add_child(_loadout_list)
	_loadout_list.item_selected.connect(_on_loadout_row_selected)

	_loadout_box.add_child(HSeparator.new())
	_loadout_name = _add_loadout_line("MissionLoadoutName", "Name",
		"Weapon name, e.g. WPN_CAR15AUTO. Matched against the game's weapon table; unknown names are ignored at load.")
	_loadout_ammo_pri = _add_loadout_line("MissionLoadoutAmmoPrimary", "Primary ammo",
		"Clips requested for the weapon's main ammo. -1 = the weapon's default fill.")
	_loadout_ammo_sec = _add_loadout_line("MissionLoadoutAmmoSecondary", "Secondary ammo",
		"Clips requested for the weapon's alternate ammo (like launcher rounds), when it has one. -1 = the weapon's default fill.")
	_loadout_damage = _add_loadout_line("MissionLoadoutDamageClass", "Damage class",
		"Damage adjustment for this weapon's ammo: 1 = 10% less damage, 2 = 10% more. Anything else (or -1) = normal damage. Weapons sharing the same ammo share this setting.")

	_loadout_box.add_child(HSeparator.new())
	_loadout_delete = Button.new()
	_loadout_delete.name = "MissionLoadoutDelete"
	_loadout_delete.text = "Delete weapon"
	_loadout_delete.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_loadout_box.add_child(_loadout_delete)
	_loadout_delete.pressed.connect(_on_loadout_delete_pressed)


func _add_loadout_line(node_name: String, label: String, tooltip: String) -> LineEdit:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_loadout_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.tooltip_text = tooltip
	lbl.clip_text = true
	lbl.custom_minimum_size = Vector2(InspectorForms.LABEL_COL_WIDTH, 0)
	row.add_child(lbl)
	var line := LineEdit.new()
	line.name = node_name
	line.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(line)
	# Commit on Enter / focus-out (not per keystroke) so the caret survives the post-commit refresh.
	line.text_submitted.connect(func(_t: String) -> void: _commit_loadout_editors())
	line.focus_exited.connect(_commit_loadout_editors)
	return line


func _on_loadout_add_pressed() -> void:
	if _inspector._controller == null:
		return
	var entries: Array = _inspector._controller.get_weapon_loadout()
	# Seed a non-empty name: an empty name serializes to a leading NUL the loader treats as the chunk
	# terminator (dropping this row and any after it). The user renames it (the name field grabs focus).
	entries.append({ "name": "WPN_NEW", "ammo_primary": "-1", "ammo_secondary": "-1", "flags": "-1" })
	_loadout_selected = entries.size() - 1
	_inspector._controller.set_weapon_loadout(entries)
	_loadout_name.grab_focus()


func _on_loadout_delete_pressed() -> void:
	if _inspector._controller == null or _loadout_selected < 0:
		return
	var entries: Array = _inspector._controller.get_weapon_loadout()
	if _loadout_selected >= entries.size():
		return
	entries.remove_at(_loadout_selected)
	_loadout_selected = mini(_loadout_selected, entries.size() - 1)
	_inspector._controller.set_weapon_loadout(entries)


func _on_loadout_row_selected(row: int) -> void:
	if _loadout_syncing:
		return
	# Flush a pending edit to the previously-selected row before switching (so a click-away keeps it).
	_commit_loadout_editors()
	_loadout_selected = row
	_refresh_loadout_panel()


func _commit_loadout_editors() -> void:
	if _loadout_syncing or _inspector._controller == null or _loadout_selected < 0:
		return
	var entries: Array = _inspector._controller.get_weapon_loadout()
	if _loadout_selected >= entries.size():
		return
	var entry: Dictionary = entries[_loadout_selected]
	# No-op edits add no undo step.
	if String(entry.get("name", "")) == _loadout_name.text \
			and String(entry.get("ammo_primary", "")) == _loadout_ammo_pri.text \
			and String(entry.get("ammo_secondary", "")) == _loadout_ammo_sec.text \
			and String(entry.get("flags", "")) == _loadout_damage.text:
		return
	# A weapon cannot be nameless: the .bms loadout chunk uses an empty name as its terminator, so a blank
	# would drop the weapon (set_weapon_loadout rejects "" too — same is_empty() test, so the two layers
	# agree). Reject the whole edit: revert ALL four fields to the stored entry (not just the name, so a
	# simultaneous value edit can't be half-applied or left visually stale) and warn in the panel's own
	# status. Delete weapon removes it.
	if _loadout_name.text.is_empty():
		_loadout_syncing = true
		_loadout_name.text = String(entry.get("name", ""))
		_loadout_ammo_pri.text = String(entry.get("ammo_primary", ""))
		_loadout_ammo_sec.text = String(entry.get("ammo_secondary", ""))
		_loadout_damage.text = String(entry.get("flags", ""))
		_loadout_syncing = false
		_loadout_status.text = "A weapon needs a name. Use Delete weapon to remove it."
		return
	entry["name"] = _loadout_name.text
	entry["ammo_primary"] = _loadout_ammo_pri.text
	entry["ammo_secondary"] = _loadout_ammo_sec.text
	entry["flags"] = _loadout_damage.text
	entries[_loadout_selected] = entry
	_inspector._controller.set_weapon_loadout(entries)


func _refresh_loadout_panel() -> void:
	if _loadout_toggle == null:
		return
	var mission: NovaMissionData = _inspector._controller.get_mission() if _inspector._controller != null else null
	if mission == null:
		_loadout_toggle.visible = false
		_loadout_box.visible = false
		return
	_loadout_toggle.visible = true
	# Skip the (relatively heavy) list/editor rebuild while collapsed; the toggle handler refreshes
	# on expand, so an open panel is always current.
	if not _loadout_box.visible:
		return

	var entries: Array = _inspector._controller.get_weapon_loadout()
	if _loadout_selected >= entries.size():
		_loadout_selected = entries.size() - 1
	_loadout_syncing = true
	_loadout_list.clear()
	for i in entries.size():
		var e := entries[i] as Dictionary
		_loadout_list.add_item("%s   (%s, %s, %s)" % [String(e.get("name", "")),
			String(e.get("ammo_primary", "")), String(e.get("ammo_secondary", "")),
			String(e.get("flags", ""))])
		if i == _loadout_selected:
			_loadout_list.select(i)
	_loadout_syncing = false

	var has_sel := _loadout_selected >= 0 and _loadout_selected < entries.size()
	var sel: Dictionary = entries[_loadout_selected] if has_sel else {}
	_loadout_syncing = true
	_inspector._sync_line(_loadout_name, String(sel.get("name", "")))
	_inspector._sync_line(_loadout_ammo_pri, String(sel.get("ammo_primary", "")))
	_inspector._sync_line(_loadout_ammo_sec, String(sel.get("ammo_secondary", "")))
	_inspector._sync_line(_loadout_damage, String(sel.get("flags", "")))
	_loadout_syncing = false
	_loadout_name.editable = has_sel
	_loadout_ammo_pri.editable = has_sel
	_loadout_ammo_sec.editable = has_sel
	_loadout_damage.editable = has_sel
	_loadout_delete.disabled = not has_sel

	if entries.is_empty():
		_loadout_status.text = "No mission kit authored (the game uses its default). Add a weapon to build one."
	elif not has_sel:
		_loadout_status.text = "Select a weapon to edit its name and values."
	else:
		_loadout_status.text = "%d weapon%s in the loadout." % [entries.size(), "" if entries.size() == 1 else "s"]


# --- Groups (mission-global collapsible) --------------------------------------

func _build_groups_panel() -> void:
	_groups_toggle = CheckButton.new()
	_groups_toggle.name = "MissionGroupsToggle"
	_groups_toggle.text = "Groups"
	_groups_toggle.tooltip_text = "Per-group data (64 groups)."
	_groups_toggle.button_pressed = false
	_groups_toggle.visible = false
	_inspector._mission_content.add_child(_groups_toggle)

	_groups_box = VBoxContainer.new()
	_groups_box.name = "MissionGroupsBox"
	_groups_box.add_theme_constant_override("separation", 4)
	_groups_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_groups_box.visible = false
	_inspector._mission_content.add_child(_groups_box)
	_groups_toggle.toggled.connect(func(on: bool) -> void:
		_groups_box.visible = on
		if on:
			_refresh_groups_panel())

	_groups_list = ItemList.new()
	_groups_list.name = "MissionGroupsList"
	_groups_list.select_mode = ItemList.SELECT_SINGLE
	_groups_list.custom_minimum_size = Vector2(0, 140)
	_groups_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_groups_box.add_child(_groups_list)
	_groups_list.item_selected.connect(_on_group_row_selected)

	_groups_box.add_child(HSeparator.new())
	InspectorForms.add_section_heading(_groups_box, "Fields")
	const INT32_MIN := -2147483648.0
	const INT32_MAX := 2147483647.0
	_group_spins = [
		InspectorForms.add_spin_row(_groups_box, "MissionGroupFlags", "Flags", 0.0, 3.0, 1.0),
		InspectorForms.add_spin_row(_groups_box, "MissionGroupValue", "Value", INT32_MIN, INT32_MAX, 1.0),
		InspectorForms.add_spin_row(_groups_box, "MissionGroupConstant", "Constant", 10.0, 10.0, 1.0),
	]
	for spin in [_group_spins[0], _group_spins[1]]:
		spin.value_changed.connect(_on_group_spin_changed)
	_group_spins[2].editable = false


func _on_group_row_selected(row: int) -> void:
	if _groups_syncing:
		return
	_groups_selected = row
	_refresh_groups_panel()


func _on_group_spin_changed(_value: float) -> void:
	if _groups_syncing or _inspector._controller == null or _groups_selected < 0:
		return
	_inspector._controller.set_group(_groups_selected,
		int(_group_spins[0].value), int(_group_spins[1].value), int(_group_spins[2].value))


func _refresh_groups_panel() -> void:
	if _groups_toggle == null:
		return
	var mission: NovaMissionData = _inspector._controller.get_mission() if _inspector._controller != null else null
	if mission == null:
		_groups_toggle.visible = false
		_groups_box.visible = false
		return
	_groups_toggle.visible = true
	if not _groups_box.visible:
		return

	var groups: Array = _inspector._controller.get_groups()
	_groups_syncing = true
	_groups_list.clear()
	for i in groups.size():
		var g := groups[i] as Dictionary
		_groups_list.add_item("Group %d   (flags %d, value %d)" % [int(g.get("index", i)), int(g.get("field0", 0)), int(g.get("field8", 0))])
		if i == _groups_selected:
			_groups_list.select(i)
	_groups_syncing = false

	var has_sel := _groups_selected >= 0 and _groups_selected < groups.size()
	var sel: Dictionary = groups[_groups_selected] if has_sel else {}
	_groups_syncing = true
	_inspector._sync_spin(_group_spins[0], float(int(sel.get("field0", 0))))
	_inspector._sync_spin(_group_spins[1], float(int(sel.get("field8", 0))))
	_inspector._sync_spin(_group_spins[2], float(int(sel.get("field12", 0))))
	_group_spins[0].editable = has_sel
	_group_spins[1].editable = has_sel
	_group_spins[2].editable = false
	_groups_syncing = false
