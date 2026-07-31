extends MarginContainer

# Inspector for the Mission workspace. Two regions stacked in a scroll:
#
#   _edit_box  — the editable panel for the selected entity (position, rotation,
#                team, group). Built ONCE and only repopulated in place, because the
#                controller fires `changed` on every edit and a torn-down SpinBox
#                would lose focus / caret mid-keystroke. Hidden when nothing is
#                selected.
#   _box       — the read-only mission summary (metadata, world, object counts).
#                Cheap to rebuild, so it is torn down and rebuilt on every `changed`.
#
# All edits go through the controller (never NovaMissionData directly); the controller
# moves the in-world object and writes the record. Referenced via preload (no
# class_name), the same convention as the controller and placer.

# Preloaded only for its Mode enum (the tab <-> mode map); the live controller is injected via
# setup() and used untyped, the same no-class_name convention as the rest of the workspace.
const MissionController = preload("res://modtools/mission/mission_controller.gd")
# The extracted panel sections under inspectors/ (RefCounted, no class_name; base
# inspector_section.gd): each owns its widgets + handlers and reaches shared
# inspector state via `_inspector`. Created once in setup(), before the builds.
const ScriptingInspector = preload("res://modtools/mission/inspectors/scripting_inspector.gd")
const PropertiesInspector = preload("res://modtools/mission/inspectors/properties_inspector.gd")
const LoadoutGroupsInspector = preload("res://modtools/mission/inspectors/loadout_groups_inspector.gd")
const WaypointsInspector = preload("res://modtools/mission/inspectors/waypoints_inspector.gd")
const ZonesInspector = preload("res://modtools/mission/inspectors/zones_inspector.gd")
const PlacePaletteInspector = preload("res://modtools/mission/inspectors/place_palette_inspector.gd")
const ObjectsBrowserInspector = preload("res://modtools/mission/inspectors/objects_browser_inspector.gd")
const TEAM_OPTIONS := [
	{"id": 0, "label": "Neutral"},
	{"id": 1, "label": "Good / blue"},
	{"id": 2, "label": "Evil / red"},
]

var _controller  # MissionController (preloaded, no class_name)
var _root: VBoxContainer
var _edit_box: VBoxContainer
var _box: VBoxContainer
# Extracted panel sections (see the inspectors/ preloads above).
var _scripting  # ScriptingInspector
var _properties  # PropertiesInspector
var _loadout_groups  # LoadoutGroupsInspector
var _waypoints  # WaypointsInspector
var _zones  # ZonesInspector
var _palette  # PlacePaletteInspector
var _browser  # ObjectsBrowserInspector

# --- Right-dock split (browser left, editor right) ----------------------------
# The inspector owns every widget but parents the per-selection editors + the mission-global
# form under a TabContainer (Selection | Mission) that lives in the shell's right dock
# (%AssetDock), keeping the left pane to just the mode tabs + the current mode's list/palette.
# `_detail_root` is the one owned container reparented between the dock and `_root`: when the
# workspace forwards a dock mount it mounts in the dock; with no mount (headless / GUT tests) it
# falls back under `_root`, so the whole tree stays a descendant of `self` and find_child /
# is_visible_in_tree assertions keep working unchanged.
var _detail_mount: Control       # the %AssetDock PanelContainer, or null (tests / no dock)
var _detail_mount_inner: Control # cached Margin scaffold built once inside the dock
var _detail_root: VBoxContainer # owned; holds _detail_tabs; reparented dock <-> _root
var _detail_tabs: TabContainer
var _sel_content: VBoxContainer     # Selection tab page content (per-mode editors)
var _mission_content: VBoxContainer # Mission tab page content (header / loadout / groups / summary)
var _sel_empty: Label               # shown when nothing is selected in the current mode
# Per-mode editor boxes, split off their left-pane list boxes and parented under _sel_content.
var _wp_detail_box: VBoxContainer
var _at_detail_box: VBoxContainer
var _sc_detail_box: VBoxContainer

# True while programmatically repopulating the edit widgets, so the value_changed
# handlers ignore the echo and do not re-commit (and re-emit) what they just read.
var _loading: bool = false

var _identity_label: Label  # heading: the selected model's name (or kind + index)
var _identity_sub: Label    # muted subline: kind + index, shown when a name resolved
var _identity_graphic_row: HBoxContainer
var _identity_graphic: ResourceRefWidget # read-only items.def graphic link, when resolved
var _user_points_check: CheckBox
var _animated_note: Label
var _behavior_flags_box: VBoxContainer  ## container for the AI-attribute checkboxes (built lazily)
var _behavior_flag_checks: Array = []  ## [{ "bit": int, "check": CheckBox }]
var _behavior_flag_syncing: bool = false
var _pos_spins: Array = []  # [x, y, z]
var _rot_spins: Array = []  # [pitch, yaw, roll]
var _team_option: OptionButton
# Team is a fixed faction picker for the known ids; Group is a "pick an available squad" dropdown
# (was a blind 0-255 spin): Ungrouped / each used group / a New-group entry. Both are bound via the
# FieldBinder so any out-of-range authored value is shown as one fallback row instead of rewritten.
var _group_option: OptionButton
# Collapsible "Behavior" section: the per-entity AI + waypoint fields the format carries
# beyond team / group. Bound through a FieldBinder (its own reentrancy guard), so a
# programmatic repopulate never echoes back as an edit. The "Waypoint path" field here is
# what links a unit to a path authored in Waypoints mode.
var _behavior_toggle: CheckButton
var _behavior_box: VBoxContainer
var _behavior_binder: FieldBinder
# "Waypoint path" is a dropdown of the mission's real paths (was a blind 0-127 spin): None / each
# populated path, refilled each refresh from controller.get_waypoint_path_options(); bound via the binder.
var _waypoint_option: OptionButton
var _delete_button: Button

# --- Edit-mode tabs + Waypoints panel (P7) ------------------------------------
# A segmented Objects / Waypoints switch at the top drives the controller's mode; the
# inspector shows the object panels in Objects mode and the waypoint panel in Waypoints
# mode. The waypoint panel lists the paths and reports the selected marker (authoring
# buttons land in later phases). _mode_syncing / _wp_syncing guard programmatic updates.
var _mode_tabs: TabBar
var _mode_syncing: bool = false

# --- Cached option lists (group / waypoint-path / entity pickers) --------------
# get_group_options / get_waypoint_path_options / get_all_entities each walk + marshal every entity
# (~1600 Dictionaries across the C++ boundary). They feed the Faction Group + Behavior Waypoint-path
# pickers (refilled on every _refresh_edit_panel) and the scripting ENTITY param picker, so calling
# them on each `changed` (every edit commit / drag release) is the dominant per-edit cost. Cache them
# and rebuild only when the controller's membership revision (entity set + group membership) changes.
var _options_rev: int = -1
var _options_mission: NovaMissionData
var _cached_group_options: Array = []
var _cached_waypoint_options: Array = []
var _cached_all_entities: Array = []

# The mission object last seen by _refresh. When it changes (a new mission opened, or cleared to
# null) the per-list selections above are stale -- the same row index is a different weapon / group /
# event in another document -- so they reset. open_mission builds a fresh NovaMissionData; edits and
# undo/redo reuse the same object, so this only flips on an actual document swap.
var _last_mission: NovaMissionData = null

# Signature of the values the read-only summary last rendered. _refresh (and thus _rebuild_summary)
# fires on every model `changed` -- each spin nudge, drag-commit, and placement -- but the summary
# only moves on a handful of aggregate values, so it skips the (node-churning) rebuild when this is
# unchanged. Empty until the first build.
var _summary_sig: Array = []


# `detail_mount` is the shell's right dock (%AssetDock), forwarded by the workspace adapter; the
# editor panels + the Mission form mount there. It defaults to null so the existing one-arg test
# calls (`inspector.setup(fake)`) keep building the whole tree under `_root` unchanged.
func setup(controller, detail_mount: Control = null) -> void:
	_controller = controller
	# Create the extracted panel sections once, before any build call below.
	if _scripting == null:
		_scripting = ScriptingInspector.new(self)
		_properties = PropertiesInspector.new(self)
		_loadout_groups = LoadoutGroupsInspector.new(self)
		_waypoints = WaypointsInspector.new(self)
		_zones = ZonesInspector.new(self)
		_palette = PlacePaletteInspector.new(self)
		_browser = ObjectsBrowserInspector.new(self)
	add_theme_constant_override("margin_left", 12)
	add_theme_constant_override("margin_top", 12)
	add_theme_constant_override("margin_right", 12)
	add_theme_constant_override("margin_bottom", 12)
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	if _root == null:
		var scroll := ScrollContainer.new()
		scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
		scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
		add_child(scroll)
		_root = VBoxContainer.new()
		_root.add_theme_constant_override("separation", 8)
		_root.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		scroll.add_child(_root)
		# Build the dock-resident container first, so the editor / Mission builders below can add
		# straight into the Selection / Mission tab pages.
		_detail_mount = detail_mount
		_ensure_detail_root()
		_build_mode_tabs()           # LEFT
		_build_edit_panel()          # DOCK: Selection
		_browser._build_object_browser()      # LEFT
		_palette._build_place_panel()         # LEFT
		_waypoints._build_waypoint_panel()      # LEFT list + DOCK detail
		_zones._build_area_trigger_panel()  # LEFT list + DOCK detail
		_scripting._build_scripting_panel()     # LEFT list + DOCK detail
		_build_selection_empty()     # DOCK: Selection standby label (last in the page)
		_properties._build_props_panel()         # DOCK: Mission
		_properties._build_reground_button()     # DOCK: Mission
		_loadout_groups._build_loadout_panel()       # DOCK: Mission
		_loadout_groups._build_groups_panel()        # DOCK: Mission
		_box = VBoxContainer.new()
		_box.add_theme_constant_override("separation", 6)
		_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_mission_content.add_child(_box)
	else:
		# Already built (a re-setup): just re-point the dock subtree.
		set_detail_mount(detail_mount)
	if _controller != null and not _controller.changed.is_connected(_refresh):
		_controller.changed.connect(_refresh)
	_refresh()


# --- Right-dock plumbing ------------------------------------------------------
# `_detail_root` is built once and reparented between the dock (real mount) and `_root` (no mount).
# Each per-mode editor / Mission panel adds into `_sel_content` or `_mission_content`. The tree
# stays owned by `self`, so a refresh writes into the same widget references regardless of where
# the subtree currently lives.

func _ensure_detail_root() -> void:
	if _detail_root != null:
		return
	_detail_root = VBoxContainer.new()
	_detail_root.name = "MissionDetailRoot"
	_detail_root.add_theme_constant_override("separation", 8)
	_detail_root.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_detail_root.size_flags_vertical = Control.SIZE_EXPAND_FILL

	_detail_tabs = TabContainer.new()
	_detail_tabs.name = "MissionDetailTabs"
	_detail_tabs.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_detail_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_detail_root.add_child(_detail_tabs)

	_sel_content = _add_detail_page("Selection")
	_mission_content = _add_detail_page("Mission")
	# Selection is page 0, the default current tab: this is what keeps the edit panel (and its
	# Delete button) is_visible_in_tree() in the null-mount test path.
	_detail_tabs.current_tab = 0
	_attach_detail_root()


# Build one TabContainer page (ScrollContainer with horizontal scroll disabled, a content VBox),
# title it, and return the content VBox.
func _add_detail_page(title: String) -> VBoxContainer:
	var page := ScrollContainer.new()
	page.name = title
	page.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	page.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	page.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_detail_tabs.add_child(page)
	_detail_tabs.set_tab_title(_detail_tabs.get_tab_count() - 1, title)
	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 8)
	content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	page.add_child(content)
	return content


# Parent `_detail_root` under the dock (when a mount is set) or under `_root` (no mount). Reparents
# without freeing, so the live editor widgets keep their state and signal connections.
func _attach_detail_root() -> void:
	if _detail_root == null or not is_instance_valid(_detail_root):
		return
	var target: Control = _detail_mount_box() if (_detail_mount != null and is_instance_valid(_detail_mount)) else _root
	if target == null:
		return
	var current := _detail_root.get_parent()
	if current == target:
		return
	if current != null:
		current.remove_child(_detail_root)
	target.add_child(_detail_root)


# Lazily build a margin scaffold inside the bare %AssetDock PanelContainer and cache it. The dock
# pages scroll their own content, so the scaffold is just a padded mount. The shell owns the dock's
# lifetime (it remove_child + frees the scaffold on switch-away), so we never free it ourselves.
func _detail_mount_box() -> Control:
	if _detail_mount == null or not is_instance_valid(_detail_mount):
		return null
	if _detail_mount_inner != null and is_instance_valid(_detail_mount_inner) and _detail_mount_inner.get_parent() == _detail_mount:
		return _detail_mount_inner
	var margin := MarginContainer.new()
	margin.name = "MissionDockMargin"
	margin.add_theme_constant_override("margin_left", 10)
	margin.add_theme_constant_override("margin_top", 10)
	margin.add_theme_constant_override("margin_right", 10)
	margin.add_theme_constant_override("margin_bottom", 10)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_detail_mount.add_child(margin)
	_detail_mount_inner = margin
	return _detail_mount_inner


# Re-point the dock subtree at a new mount (or null to evacuate it back under `_root` before the
# shell clears the dock on a workspace switch). Idempotent: the same mount already mounted is a no-op,
# which absorbs the shell re-asserting the dock on every editor-state sync without thrashing focus.
func set_detail_mount(detail_mount: Control) -> void:
	if detail_mount == _detail_mount and _detail_root != null and is_instance_valid(_detail_root) and _detail_root.get_parent() != null:
		return
	_detail_mount = detail_mount
	if _detail_root == null:
		return  # not built yet; setup() attaches on first build
	_attach_detail_root()
	_refresh()


# Public API: the workspace injects the shell's resolve/pick/jump services after
# the form is built; the Mission-properties section owns the widgets.
func set_reference_services(services: Dictionary) -> void:
	_properties.set_reference_services(services)


func _refresh() -> void:
	# Defensively clear the FieldBinder reentrancy guards before this pass. GDScript has no try/finally,
	# so if a bound getter/setter ever errors mid-sync the guard would stay stuck true and silently
	# no-op every field write; clearing here bounds that to a single refresh (changed fires often).
	if _behavior_binder != null:
		_behavior_binder.reset_guard()
	if _properties._props_binder != null:
		_properties._props_binder.reset_guard()
	# Reset the per-list selections when the mission identity flips (open / clear): a kept row index
	# would otherwise bind to a different weapon / group / event in the newly opened document.
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission != _last_mission:
		_last_mission = mission
		_loadout_groups._loadout_selected = -1
		_loadout_groups._groups_selected = -1
		_scripting._sc_trigger_selected = -1
		_scripting._sc_action_selected = -1
	_refresh_option_caches()
	_refresh_mode_tabs()
	_refresh_edit_panel()
	_browser._refresh_object_browser()
	_palette._refresh_place_panel()
	_waypoints._refresh_waypoint_panel()
	_zones._refresh_area_trigger_panel()
	_scripting._refresh_scripting_panel()
	_properties._refresh_props_panel()
	_properties._refresh_reground_button()
	_loadout_groups._refresh_loadout_panel()
	_loadout_groups._refresh_groups_panel()
	# Mode gating: the object panels show only in Objects mode; the waypoint / trigger panels
	# hide themselves outside their mode. The read-only summary shows in every mode.
	if _controller != null and not _controller.is_objects_mode():
		_edit_box.visible = false
		_palette._place_box.visible = false
		_browser._objects_box.visible = false
	_refresh_selection_empty()
	_rebuild_summary()


# The Selection tab's standby label: shown when the current mode has no editor on screen (no
# mission, or Objects mode with nothing selected — the other modes always show their editor with
# disabled fields). Each per-mode editor box has already set its own visibility by this point.
func _build_selection_empty() -> void:
	_sel_empty = InspectorForms.add_muted_label(_sel_content, "")
	_sel_empty.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART


func _refresh_selection_empty() -> void:
	if _sel_empty == null:
		return
	# Shown only when the current mode has no editor on screen (no mission, or Objects mode with
	# nothing selected — the other modes always show their editor with disabled fields).
	var any_editor := _edit_box.visible or _wp_detail_box.visible or _at_detail_box.visible or _sc_detail_box.visible
	_sel_empty.visible = not any_editor
	if any_editor:
		return
	var has_mission := _controller != null and _controller.get_mission() != null
	if has_mission:
		_sel_empty.text = "Select an object in the viewport, or pick one from the palette to place it."
	else:
		_sel_empty.text = "Open a mission, then select an object, zone, or event to edit it here."


# --- Editable selection panel (persistent) ------------------------------------

func _build_edit_panel() -> void:
	_edit_box = VBoxContainer.new()
	_edit_box.add_theme_constant_override("separation", 4)
	_edit_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_sel_content.add_child(_edit_box)

	# Build the shared field binder up front: both the Faction "Group" picker (below) and the
	# Behavior fields bind to it, and the Faction section is built before _build_behavior_section.
	_behavior_binder = FieldBinder.new()

	# The identity line is the section heading: it reads the selected model's name (resolved
	# from items.def) prominently, with a muted kind + index subline beneath, so the user
	# sees "Humvee" rather than just "Item #42".
	_identity_label = InspectorForms.add_section_heading(_edit_box, "Selected entity")
	_identity_sub = InspectorForms.add_muted_label(_edit_box, "")
	_identity_graphic_row = HBoxContainer.new()
	_identity_graphic_row.name = "MissionSelectedGraphicRow"
	_identity_graphic_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_edit_box.add_child(_identity_graphic_row)
	var graphic_label := Label.new()
	graphic_label.text = "Graphic"
	graphic_label.tooltip_text = "Graphic declared by this entity's items.def row."
	graphic_label.clip_text = true
	graphic_label.custom_minimum_size = Vector2(InspectorForms.LABEL_COL_WIDTH, 0)
	_identity_graphic_row.add_child(graphic_label)
	_identity_graphic = ResourceRefWidget.new()
	_identity_graphic.name = "MissionSelectedGraphic"
	_identity_graphic.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_identity_graphic_row.add_child(_identity_graphic)
	_configure_selected_graphic_ref()
	_user_points_check = CheckBox.new()
	_user_points_check.name = "MissionUserPointsCheck"
	_user_points_check.text = "User points"
	_user_points_check.tooltip_text = "Show this model's named userpoints in the viewport."
	_edit_box.add_child(_user_points_check)

	InspectorForms.add_section_heading(_edit_box, "Position")
	_pos_spins = [
		InspectorForms.add_spin_row(_edit_box, "MissionPosX", "X", -1000000.0, 1000000.0, 0.001),
		InspectorForms.add_spin_row(_edit_box, "MissionPosY", "Y", -1000000.0, 1000000.0, 0.001),
		InspectorForms.add_spin_row(_edit_box, "MissionPosZ", "Z", -1000000.0, 1000000.0, 0.001),
	]

	InspectorForms.add_section_heading(_edit_box, "Rotation")
	_rot_spins = [
		InspectorForms.add_spin_row(_edit_box, "MissionRotPitch", "Pitch", -360.0, 360.0, 1.0),
		InspectorForms.add_spin_row(_edit_box, "MissionRotYaw", "Yaw", -360.0, 360.0, 1.0),
		InspectorForms.add_spin_row(_edit_box, "MissionRotRoll", "Roll", -360.0, 360.0, 1.0),
	]

	# team / group are stored on every entity kind by the format, so they are shown for
	# all selectable objects; in practice they drive organics (units) at runtime.
	InspectorForms.add_section_heading(_edit_box, "Faction")
	_team_option = InspectorForms.add_id_option_row(_edit_box, "MissionTeam", "Team", [])
	_team_option.tooltip_text = "Faction for this entity."
	_behavior_binder.bind_option(_team_option,
		func(info): return int(info.get("team", 0)),
		func(value: int) -> void: _set_team(value),
		func(): return TEAM_OPTIONS,
		func(value: int) -> String: return "Team %d" % value)
	# Group is chosen from the mission's actual squads (Ungrouped / each used group / a New group
	# entry), not typed as a raw number; the option list is refilled in _refresh_edit_panel.
	_group_option = _add_entity_option_row(_edit_box, "group", "Group",
		"Which squad this unit belongs to. Lists groups already in use, plus a new one.",
		func(): return _cached_group_options)

	_build_behavior_section()

	_animated_note = InspectorForms.add_muted_label(_edit_box, "Animated object.")

	# Delete sits at the bottom of the edit panel as the one destructive action; the whole
	# panel is hidden when nothing is selected, so the button only shows with a selection.
	# Removing an entity is not saved to the .bms until Save Mission, so an accidental
	# delete is recovered by reopening the mission rather than a modal confirm here.
	_edit_box.add_child(HSeparator.new())
	_delete_button = Button.new()
	_delete_button.name = "MissionDeleteEntity"
	_delete_button.text = "Delete object"
	_delete_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_edit_box.add_child(_delete_button)

	for axis in 3:
		_pos_spins[axis].value_changed.connect(_on_position_axis.bind(axis))
		_rot_spins[axis].value_changed.connect(_on_rotation_axis.bind(axis))
	# The Team and Group dropdowns are wired through the FieldBinder, not here.
	# Behavior fields likewise wire themselves through the FieldBinder in _build_behavior_section.
	_user_points_check.toggled.connect(_on_user_points_toggled)
	_delete_button.pressed.connect(_on_delete_pressed)


func _configure_selected_graphic_ref() -> void:
	if _identity_graphic == null or not is_instance_valid(_identity_graphic):
		return
	_identity_graphic.configure("object_model", "Graphic", _properties._ref_services)
	_identity_graphic.name_edit.editable = false
	_identity_graphic.name_edit.focus_mode = Control.FOCUS_NONE
	_identity_graphic.name_edit.tooltip_text = "Graphic declared by this entity's items.def row."
	# The selected entity's graphic is derived from items.def, not authored on the
	# mission record. Keep browse/clear hidden so the row reads as a jump target.
	_identity_graphic.browse_button.visible = false
	_identity_graphic.clear_button.visible = false


# Build the collapsed-by-default "Behavior" section: a toggle that shows / hides a box of
# the per-entity AI + waypoint fields. Each field is a FieldBinder-bound SpinBox whose
# setter routes through controller.set_selected_property; sync happens in
# _refresh_edit_panel via _behavior_binder.sync_from. Ranges follow the format's field
# widths so a real value is never clamped on display.
func _build_behavior_section() -> void:
	# _behavior_binder is created in _build_edit_panel (the Faction Group picker binds to it too).
	_behavior_toggle = CheckButton.new()
	_behavior_toggle.name = "MissionBehaviorToggle"
	_behavior_toggle.text = "Behavior"
	_behavior_toggle.tooltip_text = "Per-unit AI and waypoint settings (these mainly drive organic units at runtime)."
	_behavior_toggle.button_pressed = false
	_edit_box.add_child(_behavior_toggle)

	_behavior_box = VBoxContainer.new()
	_behavior_box.add_theme_constant_override("separation", 4)
	_behavior_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_behavior_box.visible = false
	_edit_box.add_child(_behavior_box)
	_behavior_toggle.toggled.connect(func(on: bool) -> void: _behavior_box.visible = on)

	# Waypoint path (waypoint_id) is the bridge: it names which authored path a unit follows. A
	# dropdown of the mission's real paths (None / each populated path), not a blind 0-127 number.
	# waypoint_id is a fixed path NUMBER (0..127; 0 = None), not an index into the populated-path list. The
	# dropdown lists only paths that have markers, so a unit pointed at an empty slot (common for units that
	# man a gun / ride a vehicle and never path-follow) has no matching row; label that case as the real
	# (empty) path slot rather than the generic "Value N".
	_waypoint_option = _add_entity_option_row(_behavior_box, "waypoint_id", "Waypoint path",
		"Which authored path this unit follows (0-127; 0 = None). Units manning a gun or riding a vehicle don't follow a path -- set None.",
		func(): return _cached_waypoint_options,
		func(v: int) -> String: return ("Path %d (no markers)" % v) if (v >= 1 and v <= 127) else ("Value %d" % v))
	# The plain numeric rows + their section headings come from one ordered table (the field set +
	# ranges live in MissionEntityFields, matching the libs/mission name->member map). The picker /
	# text / flag rows below are not plain spins, so they stay explicit.
	for entry in MissionEntityFields.SPIN_FIELDS:
		if entry.has("section"):
			InspectorForms.add_section_heading(_behavior_box, String(entry["section"]))
			continue
		var spin := _add_behavior_spin(String(entry["property"]), String(entry["label"]),
			float(entry["min"]), float(entry["max"]))
		if entry.has("tip"):
			spin.tooltip_text = String(entry["tip"])
	_add_behavior_line("name1", "AI class",
		func(info) -> String: return String(info.get("name1", "")),
		func(text: String) -> void: _behavior_set_string("name1", text),
		"AI class name (iai_name), max 7 chars. Press Enter to apply.")
	_add_behavior_line("name2", "AI script",
		func(info) -> String: return String(info.get("name2", "")),
		func(text: String) -> void: _behavior_set_string("name2", text),
		"AI script file (ai_textfile), max 7 chars. Press Enter to apply.")
	InspectorForms.add_section_heading(_behavior_box, "Flags")
	# The named AI-attribute bits are authored as checkboxes, generated lazily from the engine's bit list
	# (a mission must be loaded for the controller to answer). The hex field below stays as an advanced
	# editor + escape hatch: it shows the full 32-bit value, so bits the editor does not name (preserved
	# verbatim on round-trip) remain visible and editable.
	_behavior_flags_box = VBoxContainer.new()
	_behavior_flags_box.name = "MissionBehFlags"
	_behavior_flags_box.add_theme_constant_override("separation", 2)
	_behavior_box.add_child(_behavior_flags_box)
	_add_behavior_line("ai_flags", "AI flags (hex)",
		func(info) -> String: return "0x%08X" % (int(info.get("ai_flags", 0)) & 0xFFFFFFFF),
		func(text: String) -> void: _ai_flags_set(text),
		"Full 32-bit AI flags in hex. The checkboxes cover the named bits; use this for any others. Press Enter to apply.")


func _add_behavior_spin(property: String, label: String, min_value: float, max_value: float) -> SpinBox:
	var spin := InspectorForms.add_spin_row(_behavior_box, "MissionBeh_" + property, label, min_value, max_value, 1.0)
	_behavior_binder.bind_spin(spin,
		func(info): return float(int(info.get(property, 0))),
		func(value: float) -> void: _behavior_set(property, value))
	return spin


# A labelled OptionButton "pick from available options" row for an entity field (waypoint path /
# group), bound through the behaviour FieldBinder. The binder OWNS the row: each sync_from refills the
# items from `options_getter` ({ id, label }, cached in _refresh_option_caches), selects the entity's
# current value, adds a single out-of-range fallback row if needed, and its guard stops the
# programmatic repopulate echoing back as an edit. Item ids carry the model value, so a user pick
# commits through set_selected_property(property, id). One populator only -- do not also call
# populate_id_option on these (that double-population left a duplicate/untagged fallback row).
func _add_entity_option_row(parent: Control, property: String, label: String, tooltip: String = "", options_getter := Callable(), fallback_label := Callable()) -> OptionButton:
	var option := InspectorForms.add_id_option_row(parent, "MissionOpt_" + property, label, [])
	if not tooltip.is_empty():
		option.tooltip_text = tooltip
	# The binder OWNS the list (refills it from options_getter each sync) as well as the selection +
	# out-of-range fallback, so it is the single populator -- _refresh_edit_panel must not also call
	# populate_id_option on these (that double-population left a duplicate/untagged fallback row).
	_behavior_binder.bind_option(option,
		func(info): return int(info.get(property, 0)),
		func(value: int) -> void: _behavior_set(property, value),
		options_getter, fallback_label)
	return option


# A text field bound through the FieldBinder (Enter to apply), for a property that reads
# better as text than a number. Used for the ai_flags bitfield, shown as hexadecimal.
func _add_behavior_line(property: String, label: String, getter: Callable, setter: Callable, tooltip: String = "") -> LineEdit:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_behavior_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.tooltip_text = tooltip if not tooltip.is_empty() else label
	lbl.clip_text = true
	lbl.custom_minimum_size = Vector2(InspectorForms.LABEL_COL_WIDTH, 0)
	row.add_child(lbl)
	var line := LineEdit.new()
	line.name = "MissionBeh_" + property
	line.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	if not tooltip.is_empty():
		line.tooltip_text = tooltip
	row.add_child(line)
	_behavior_binder.bind_line(line, getter, setter)
	return line


func _behavior_set(property: String, value: float) -> void:
	if _controller != null:
		_controller.set_selected_property(property, int(value))


func _behavior_set_string(property: String, value: String) -> void:
	if _controller != null:
		_controller.set_selected_string_property(property, value)


# Apply a hexadecimal ai_flags edit. Pass the value as a signed int32 so the engine's int
# parameter carries the exact 32-bit pattern (the high bit becomes negative and reads back
# as the same bits). An unparseable entry restores the field from the model rather than
# writing garbage.
func _ai_flags_set(text: String) -> void:
	if _controller == null:
		return
	var parsed := _parse_uint32(text)
	if parsed < 0:
		_behavior_binder.sync_from(_controller.get_selected_entity())
		return
	var signed: int = parsed if parsed < 0x80000000 else parsed - 0x100000000
	_controller.set_selected_property("ai_flags", signed)


# Parse a uint32 from "0x..." hex, bare hex, or decimal text. Returns -1 when the text is
# empty, malformed, or out of the 0..0xFFFFFFFF range (the caller treats -1 as "no change").
func _parse_uint32(text: String) -> int:
	var t := text.strip_edges()
	if t.is_empty():
		return -1
	var value := -1
	if t.to_lower().begins_with("0x"):
		if t.is_valid_hex_number(true):
			value = t.hex_to_int()
	elif t.is_valid_int():
		value = t.to_int()
	elif t.is_valid_hex_number(false):
		value = ("0x" + t).hex_to_int()
	if value < 0 or value > 0xFFFFFFFF:
		return -1
	return value


# Build the AI-attribute checkboxes once (from the engine's bit list) and set each from `flags`. Mirrors
# the event-flag checkbox pattern (_sc_flag_checks): one build, synced on every selection refresh.
func _sync_behavior_flags(flags: int) -> void:
	if _behavior_flag_checks.is_empty() and _controller != null and _behavior_flags_box != null:
		for entry in _controller.get_ai_flag_bits():
			var entry_dict := entry as Dictionary
			var bit := int(entry_dict.get("value", 0))
			var check := InspectorForms.add_checkbox(_behavior_flags_box, "MissionBehFlag%d" % bit, String(entry_dict.get("name", "")))
			check.toggled.connect(_on_behavior_flag_toggled)
			_behavior_flag_checks.append({ "bit": bit, "check": check })
	_behavior_flag_syncing = true
	for flag_entry in _behavior_flag_checks:
		(flag_entry["check"] as CheckBox).button_pressed = (flags & int(flag_entry["bit"])) != 0
	_behavior_flag_syncing = false


# Toggle a named AI-attribute bit. Merge against the entity's current flags so bits the checkboxes do not
# cover (e.g. DEAF / IGNORE_FOOTSTEPS, not yet decoded) are never dropped, then apply the checked bits.
func _on_behavior_flag_toggled(_pressed: bool) -> void:
	if _behavior_flag_syncing or _controller == null:
		return
	var known_mask := 0
	var checked := 0
	for flag_entry in _behavior_flag_checks:
		var bit := int(flag_entry["bit"])
		known_mask |= bit
		if (flag_entry["check"] as CheckBox).button_pressed:
			checked |= bit
	var current := int(_controller.get_selected_entity().get("ai_flags", 0)) & 0xFFFFFFFF
	var merged := (current & ~known_mask) | checked
	var signed: int = merged if merged < 0x80000000 else merged - 0x100000000
	_controller.set_selected_property("ai_flags", signed)


# Rebuild the cached group / waypoint-path / entity option lists only when the controller's membership
# revision (entity set + group membership) changes, or the mission flips. Everything that affects these
# lists -- object/marker add+remove and group edits -- bumps that revision, so a position/team/AI edit
# or a drag (which fire `changed` too) reuses the cache instead of re-marshalling every entity.
func _refresh_option_caches() -> void:
	if _controller == null:
		_cached_group_options = []
		_cached_waypoint_options = []
		_cached_all_entities = []
		return
	var mission: NovaMissionData = _controller.get_mission()
	var rev: int = _controller.get_membership_revision()
	# Rebuild only when the document or its membership revision changes. (get_group_options etc. handle
	# a null mission by returning their base list, so this is safe before a mission is loaded too.)
	if mission == _options_mission and rev == _options_rev:
		return
	_options_mission = mission
	_options_rev = rev
	_cached_group_options = _controller.get_group_options()
	_cached_waypoint_options = _controller.get_waypoint_path_options()
	_cached_all_entities = _controller.get_all_entities()


func _refresh_edit_panel() -> void:
	var entity: Dictionary = _controller.get_selected_entity() if _controller != null else {}
	if entity.is_empty():
		_loading = true
		if _identity_graphic != null:
			_identity_graphic.set_value("")
		if _identity_graphic_row != null:
			_identity_graphic_row.visible = false
		_sync_user_points_check(false)
		_loading = false
		_edit_box.visible = false
		return
	_edit_box.visible = true

	# Bracket every .value write: assigning a SpinBox value fires value_changed
	# synchronously, and without the guard each repopulate would re-commit the value
	# back into the model and loop.
	_loading = true
	var kind_index := "%s #%d" % [_kind_label(int(entity.get("kind", -1))), int(entity.get("index", -1))]
	var model_name: String = _controller.get_selected_display_name() if _controller != null else ""
	if model_name.is_empty():
		_identity_label.text = kind_index
		_identity_sub.visible = false
	else:
		_identity_label.text = model_name
		_identity_sub.text = kind_index
		_identity_sub.visible = true
	var graphic_name: String = _controller.get_selected_graphic_name() if _controller != null else ""
	if graphic_name.is_empty():
		_identity_graphic.set_value("")
		_identity_graphic_row.visible = false
	else:
		_identity_graphic.set_value(graphic_name)
		_identity_graphic_row.visible = true
	_sync_user_points_check(true)
	# Use _sync_spin (focus-aware) so a refresh that lands while the user is mid-typing a position /
	# rotation value does not clobber the keystroke -- matching the zone-bounds and scripting spins.
	# The _loading bracket still suppresses the value_changed echo for the spins that do get written.
	var pos: Vector3 = entity.get("position", Vector3.ZERO)
	_sync_spin(_pos_spins[0], pos.x)
	_sync_spin(_pos_spins[1], pos.y)
	_sync_spin(_pos_spins[2], pos.z)
	var rot: Vector3 = entity.get("rotation_deg", Vector3.ZERO)
	_sync_spin(_rot_spins[0], rot.x)
	_sync_spin(_rot_spins[1], rot.y)
	_sync_spin(_rot_spins[2], rot.z)
	_loading = false

	# The Behavior fields AND the Team / Group / Waypoint-path pickers all sync through the FieldBinder here:
	# bind_option refills each picker from its cached option list (see _refresh_option_caches), selects
	# the entity's current value, and adds a single out-of-range fallback row if needed. The binder's
	# own reentrancy guard (independent of _loading) stops this programmatic sync echoing back as edits.
	_behavior_binder.sync_from(entity)
	_sync_behavior_flags(int(entity.get("ai_flags", 0)))

	var summary: Dictionary = _controller.get_selection_summary() if _controller != null else {}
	_animated_note.visible = bool(summary.get("animated", false))


func _sync_user_points_check(has_selection: bool) -> void:
	if _user_points_check == null:
		return
	var has_points := false
	var visible := false
	if has_selection and _controller != null:
		has_points = _controller.selected_has_user_points() if _controller.has_method("selected_has_user_points") else false
		visible = _controller.is_selected_user_points_visible() if _controller.has_method("is_selected_user_points_visible") else false
	_user_points_check.disabled = not has_points
	_user_points_check.button_pressed = has_points and visible


func _on_user_points_toggled(pressed: bool) -> void:
	if _loading or _controller == null:
		return
	if _controller.has_method("set_selected_user_points_visible"):
		_controller.set_selected_user_points_visible(pressed)


func _on_position_axis(value: float, axis: int) -> void:
	if _loading or _controller == null:
		return
	# Read the unchanged axes from the controller (exact), not the sibling SpinBoxes
	# (which may show a step-snapped value), so editing one axis never nudges another.
	var p: Vector3 = _controller.get_selected_position()
	match axis:
		0:
			p.x = value
		1:
			p.y = value
		2:
			p.z = value
	_controller.set_selected_position(p)


func _on_rotation_axis(value: float, axis: int) -> void:
	if _loading or _controller == null:
		return
	var r: Vector3 = _controller.get_selected_rotation()
	match axis:
		0:
			r.x = value
		1:
			r.y = value
		2:
			r.z = value
	_controller.set_selected_rotation(r)


func _set_team(value: int) -> void:
	if _controller != null:
		_controller.set_selected_team(value)


func _on_delete_pressed() -> void:
	if _controller == null:
		return
	# The controller removes the selected entity, re-bakes the world, and fires `changed`;
	# _refresh_edit_panel then hides this panel (nothing is selected after a delete).
	_controller.delete_selected()


# --- Edit-mode tabs (Objects / Waypoints) -------------------------------------
# The tabs drive the controller's mode; the controller is the single source of truth, so
# _refresh_mode_tabs syncs the current tab back from it (guarded against echo).

# Tab index <-> controller Mode. Tab order: 0 Objects, 1 Waypoints, 2 Triggers (zones), 3 Scripting.
const _TAB_TO_MODE := [
	MissionController.Mode.OBJECTS,
	MissionController.Mode.WAYPOINTS,
	MissionController.Mode.AREA_TRIGGERS,
	MissionController.Mode.SCRIPTING,
]

func _build_mode_tabs() -> void:
	_mode_tabs = TabBar.new()
	_mode_tabs.name = "MissionModeTabs"
	_mode_tabs.add_tab("Objects")
	_mode_tabs.add_tab("Waypoints")
	_mode_tabs.add_tab("Triggers")
	_mode_tabs.add_tab("Scripting")
	_mode_tabs.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_root.add_child(_mode_tabs)
	_mode_tabs.tab_changed.connect(_on_mode_tab_changed)


func _on_mode_tab_changed(tab: int) -> void:
	if _mode_syncing or _controller == null:
		return
	if tab >= 0 and tab < _TAB_TO_MODE.size():
		_controller.set_mode(_TAB_TO_MODE[tab])


func _refresh_mode_tabs() -> void:
	if _mode_tabs == null:
		return
	var has_mission := _controller != null and _controller.get_mission() != null
	_mode_tabs.visible = has_mission
	var want := _TAB_TO_MODE.find(_controller.get_mode()) if _controller != null else 0
	if want < 0:
		want = 0
	if _mode_tabs.current_tab != want:
		_mode_syncing = true
		_mode_tabs.current_tab = want
		_mode_syncing = false


# Set a LineEdit only if the text differs AND it is not focused, so a programmatic sync (which fires on
# every `changed`, including an undo that lands while the user is mid-edit) never moves the caret or
# clobbers an uncommitted keystroke. The commit-on-Enter/focus-out path reconciles the value on exit.
func _sync_line(line: LineEdit, value: String) -> void:
	if line.text != value and not line.has_focus():
		line.text = value


# SpinBox counterpart of _sync_line: don't overwrite a value the user is mid-typing (its inner LineEdit
# holds the focus). The per-change commit has already pushed any real edit, so skipping the sync is safe.
func _sync_spin(spin: SpinBox, value: float) -> void:
	if spin.get_line_edit().has_focus():
		return
	if spin.value != value:
		spin.value = value


# --- Read-only mission summary (rebuilt) --------------------------------------

func _rebuild_summary() -> void:
	if _box == null:
		return

	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	var info := {}
	var stats := {}
	var selection := {}
	# Fingerprint everything the summary renders, then bail before any node churn when it is
	# unchanged from the last build (the common case: most refreshes touch a per-entity field the
	# summary does not show).
	var sig: Array = [null]
	if mission != null:
		info = mission.get_info()
		stats = _controller.get_stats() if _controller != null else {}
		selection = _controller.get_selection_summary() if _controller != null else {}
		sig = [
			mission.get_instance_id(),
			mission.get_mission_name(), mission.get_designer(),
			int(info.get("climate", 0)), int(info.get("weather", 0)),
			selection.is_empty(),
			int(stats.get("placed", 0)), int(stats.get("batched", 0)), int(stats.get("batches", 0)),
			int(stats.get("animated", 0)), int(stats.get("unresolved", 0)), int(stats.get("markers", 0)),
			mission.get_entity_count(NovaMissionData.KIND_ITEM),
			mission.get_entity_count(NovaMissionData.KIND_BUILDING),
			mission.get_entity_count(NovaMissionData.KIND_ORGANIC),
			mission.get_entity_count(NovaMissionData.KIND_MARKER),
		]
	if sig == _summary_sig:
		return
	_summary_sig = sig

	for child in _box.get_children():
		child.queue_free()

	if mission == null:
		_add_heading("Mission")
		_add_body("Open a mission to load its terrain, environment, and placed objects.")
		return

	_add_heading(_nonempty(mission.get_mission_name(), "Untitled mission"))
	var designer := mission.get_designer().strip_edges()
	if not designer.is_empty():
		_add_row("Designer", designer)

	# The selected entity has its own editable panel above; here, only prompt when
	# nothing is selected so the viewer always explains how to begin.
	if selection.is_empty():
		_add_separator()
		_add_heading("Selection")
		_add_body("Click an object to select it, then drag it on the terrain or edit its fields above. Save Mission to write changes.")

	_add_separator()
	_add_heading("World")
	# Terrain/environment moved from read-only rows here into editable link
	# widgets in the Mission properties form (the World section above).
	_add_row("Climate", str(int(info.get("climate", 0))))
	_add_row("Weather", str(int(info.get("weather", 0))))

	_add_separator()
	_add_heading("Objects")
	_add_row("Placed", str(int(stats.get("placed", 0))))
	_add_row("Batched", "%d in %d draw groups" % [int(stats.get("batched", 0)), int(stats.get("batches", 0))])
	_add_row("Animated", str(int(stats.get("animated", 0))))
	var unresolved := int(stats.get("unresolved", 0))
	if unresolved > 0:
		_add_row("Unresolved", str(unresolved))
	_add_row("Markers (hidden)", str(int(stats.get("markers", 0))))

	_add_separator()
	_add_heading("Entities")
	_add_row("Items", str(mission.get_entity_count(NovaMissionData.KIND_ITEM)))
	_add_row("Buildings", str(mission.get_entity_count(NovaMissionData.KIND_BUILDING)))
	_add_row("Organics", str(mission.get_entity_count(NovaMissionData.KIND_ORGANIC)))
	_add_row("Markers", str(mission.get_entity_count(NovaMissionData.KIND_MARKER)))


# --- Row builders -------------------------------------------------------------

func _add_heading(text: String) -> void:
	var label := Label.new()
	label.text = text
	label.theme_type_variation = &"Heading"
	_box.add_child(label)


func _add_body(text: String) -> void:
	var label := Label.new()
	label.text = text
	label.theme_type_variation = &"Muted"
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_box.add_child(label)


func _add_row(key: String, value: String) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var key_label := Label.new()
	key_label.text = key
	key_label.theme_type_variation = &"Muted"
	key_label.custom_minimum_size = Vector2(128, 0)
	var value_label := Label.new()
	value_label.text = value
	value_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	value_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	row.add_child(key_label)
	row.add_child(value_label)
	_box.add_child(row)


func _add_separator() -> void:
	_box.add_child(HSeparator.new())


func _nonempty(text: String, fallback: String) -> String:
	var trimmed := text.strip_edges()
	return trimmed if not trimmed.is_empty() else fallback


func _kind_label(kind: int) -> String:
	match kind:
		NovaMissionData.KIND_ITEM:
			return "Item"
		NovaMissionData.KIND_BUILDING:
			return "Building"
		NovaMissionData.KIND_ORGANIC:
			return "Organic"
		NovaMissionData.KIND_MARKER:
			return "Marker"
		_:
			return "Entity"
