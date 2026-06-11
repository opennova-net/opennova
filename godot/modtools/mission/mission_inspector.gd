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

const ObjectUiHelpers = preload("res://modtools/object/ui/object_ui_helpers.gd")
# Preloaded only for its Mode enum (the tab <-> mode map); the live controller is injected via
# setup() and used untyped, the same no-class_name convention as the rest of the workspace.
const MissionController = preload("res://modtools/mission/mission_controller.gd")
const TEAM_OPTIONS := [
	{"id": 0, "label": "Neutral"},
	{"id": 1, "label": "Good / blue"},
	{"id": 2, "label": "Evil / red"},
]

var _controller  # MissionController (preloaded, no class_name)
var _root: VBoxContainer
var _edit_box: VBoxContainer
var _box: VBoxContainer

# --- Right-dock split (browser left, editor right) ----------------------------
# The inspector owns every widget but parents the per-selection editors + the mission-global
# form under a TabContainer (Selection | Mission) that lives in the shell's right dock
# (%AssetDock), keeping the left pane to just the mode tabs + the current mode's list/palette.
# `_detail_root` is the one owned container reparented between the dock and `_root`: when the
# workspace forwards a dock host it mounts in the dock; with no host (headless / GUT tests) it
# falls back under `_root`, so the whole tree stays a descendant of `self` and find_child /
# is_visible_in_tree assertions keep working unchanged.
var _detail_host: Control       # the %AssetDock PanelContainer, or null (tests / no dock)
var _detail_host_inner: Control # cached Margin scaffold built once inside the dock
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

# --- Place-object palette (persistent) ----------------------------------------
var _place_box: VBoxContainer
var _place_search: LineEdit
var _place_list: ItemList
var _place_status: Label
var _place_stop: Button
# items.def ids parallel to the currently-shown _place_list rows (the list is filtered
# by the search box, so row index != item index in the full set).
var _place_row_ids: Array = []
# The mission the palette rows were built for; the (expensive) list is only repopulated
# when this changes, not on every `changed` (which fires on each edit / placement).
var _place_built_for: NovaMissionData
# Placeable items for the current mission, cached so the palette does not re-enumerate
# the whole items.def (1000+ entries) on every refresh. Rebuilt when the mission changes.
var _placeable_cache: Array = []
var _placeable_names: Dictionary = {}  # id -> display label
# True while programmatically syncing the list selection, so item_selected echoes do
# not re-arm.
var _place_syncing: bool = false

# --- Placed-objects browser (persistent) --------------------------------------
# A searchable list of every object already placed in the mission (items / buildings / people).
# Selecting a row selects that entity AND frames the camera on it, so a named unit is found on a
# large map without hunting the viewport. Built once; the (potentially 1000+ row) cache is only
# rebuilt when the object set changes (count / mission) or item names first become resolvable,
# not on every `changed` (which fires on each edit / drag frame).
var _objects_box: VBoxContainer
var _objects_search: LineEdit
var _objects_list: ItemList
var _objects_status: Label
# Cached rows for the current mission: { kind, index, label, category, search }. `label` carries a
# duplicate-name ordinal so two "Soldier" rows are distinguishable; `search` is the lowercase match key.
var _objects_cache: Array = []
# { kind, index } parallel to the currently-shown (filtered) _objects_list rows.
var _objects_rows: Array = []
var _objects_built_for: NovaMissionData
var _objects_built_count: int = -1
# The controller membership revision the rows were built for. Gating on the count alone would miss an
# identity change at a constant total (e.g. an undo/redo that swaps an entity for a different one of the
# same kind, or a future "change item" edit); the revision bumps on every such re-bake, so include it.
var _objects_built_rev: int = -1
var _objects_db_ready: bool = false
# True while programmatically syncing the list selection, so item_selected echoes do not re-select.
var _objects_syncing: bool = false

# --- Edit-mode tabs + Waypoints panel (P7) ------------------------------------
# A segmented Objects / Waypoints switch at the top drives the controller's mode; the
# inspector shows the object panels in Objects mode and the waypoint panel in Waypoints
# mode. The waypoint panel lists the paths and reports the selected marker (authoring
# buttons land in later phases). _mode_syncing / _wp_syncing guard programmatic updates.
var _mode_tabs: TabBar
var _mode_syncing: bool = false

# Live-simulation transport (Play the mission): drives the controller's sim over the placed nodes.
var _sim_bar: HBoxContainer
var _sim_play_btn: Button
# Play-in-editor hooks, injected by the workspace (the swap is a viewport concern
# the inspector cannot own): play() -> Error, is_playing() -> bool, stop() -> void.
var _play_mission_cb := Callable()
var _is_playing_cb := Callable()
var _stop_play_cb := Callable()
# Debug-overlay hooks, same injection pattern: toggle() -> void, is_open() -> bool.
var _debug_toggle_cb := Callable()
var _debug_is_open_cb := Callable()
var _play_mission_btn: Button
var _debug_btn: Button
var _sim_pause_btn: Button
var _sim_step_btn: Button
var _sim_stop_btn: Button
var _wp_box: VBoxContainer
var _wp_status: Label
var _wp_new_path_button: Button
var _wp_list: ItemList
var _wp_marker_label: Label
var _wp_row_paths: Array = []  # path indices parallel to the _wp_list rows
var _wp_syncing: bool = false
# Active-path flag toggles (loop is the inverse of the stored DoesNotLoop bit) + the
# ordered marker sub-list. _wp_flags_syncing / _wp_marker_syncing guard programmatic sets.
var _wp_loop_check: CheckBox
var _wp_blue_check: CheckBox
var _wp_red_check: CheckBox
var _wp_flags_syncing: bool = false
var _wp_marker_list: ItemList
var _wp_marker_rows: Array = []  # marker indices parallel to the _wp_marker_list rows
var _wp_marker_syncing: bool = false
# Authoring buttons (P7d): add (a placement tool), reorder, delete, clear.
var _wp_add_button: Button
var _wp_up_button: Button
var _wp_down_button: Button
var _wp_delete_button: Button
var _wp_clear_button: Button

# --- Area-trigger (zone) panel (Phase 2) --------------------------------------
# Shown in Triggers mode: the zone list, the selected zone's six min/max spins, the two known
# flag toggles (Active / Constrain height), and Add / Delete. Built once, repopulated under
# guards so a programmatic set never echoes back as a user edit. Edits route through the
# controller (set_selected_zone_bounds / _flags, add_area_trigger_default, delete...).
var _at_box: VBoxContainer
var _at_status: Label
var _at_list: ItemList
var _at_rows: Array = []  # zone indices parallel to the _at_list rows
var _at_syncing: bool = false
var _at_min_spins: Array = []  # [x, y, z] SpinBox
var _at_max_spins: Array = []  # [x, y, z] SpinBox
var _at_active_check: CheckBox
var _at_constrain_check: CheckBox
var _at_flags_syncing: bool = false
var _at_bounds_syncing: bool = false
var _at_add_button: Button
var _at_delete_button: Button

# --- Mission properties (header) editable form --------------------------------
# A collapsible form for the mission-level header fields. Built ONCE and synced in
# place via _props_binder (FieldBinder), since LineEdits would lose their caret if torn
# down on every `changed`. Setters route through the controller's set_header_* (one undo
# step each). Mission-global, so it shows in both Objects and Waypoints mode.
var _props_toggle: CheckButton
var _props_box: VBoxContainer
var _props_binder: FieldBinder
# Link-widget services (resolve/pick/jump Callables from the shell). They arrive
# AFTER setup() builds the form (the workspace injects them post-build), so the
# setter re-configures the already-built widgets.
var _ref_services: Dictionary = {}
var _terrain_ref_widget: ResourceRefWidget
var _env_ref_widget: ResourceRefWidget
# Mission-tab bulk re-ground (B8): the manual twin of the workspace's activate-time
# "terrain changed under N objects" prompt.
var _reground_button: Button

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

# --- Weapon loadout + groups (mission-global collapsibles) --------------------
# Like the header form, these are shown whenever a mission is loaded, in any mode, and built
# ONCE. The loadout is a list of (name, value1, value2) records; groups are 64 fixed records
# with three editable ints each. Selection is inspector-local (no in-world interaction). The
# *_syncing guards stop a programmatic repopulate from echoing back as an edit. Name/value
# edits commit on Enter / focus-out (not per keystroke) to keep the caret; group spins commit
# on change. Every commit replaces the whole loadout / writes one group = one undo step.
var _loadout_toggle: CheckButton
var _loadout_box: VBoxContainer
var _loadout_status: Label
var _loadout_list: ItemList
var _loadout_name: LineEdit
var _loadout_value1: LineEdit
var _loadout_value2: LineEdit
var _loadout_delete: Button
var _loadout_selected: int = -1
var _loadout_syncing: bool = false

var _groups_toggle: CheckButton
var _groups_box: VBoxContainer
var _groups_list: ItemList
var _group_spins: Array = []  # [flags, value, constant10]
var _groups_selected: int = -1
var _groups_syncing: bool = false

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

# --- Scripting (events / triggers / actions) panel (Phase 4) ------------------
# Shown in Scripting mode (4th tab). Top: the event list + Add / Delete event. Middle: the selected
# event's flag checkboxes + reset / delay spins. Then a Triggers sub-list with a type / sub-type /
# logic-flags / 4-param editor, and an Actions sub-list with a type / sub-type / 4-param editor, each
# with Add / Remove / Move. Bottom: a diagnostics strip. Built ONCE; every sub-widget group carries its
# own *_syncing guard so a programmatic repopulate never echoes back as a user edit, and spins sync
# through _sync_spin so a refresh never clobbers a value being typed. Edits route through the controller.
# Reset / delay are the engine's 10-bit fields, so the spins clamp to 0..1023.
const SCRIPT_PARAM_MIN := -2147483648.0
const SCRIPT_PARAM_MAX := 2147483647.0
const SCRIPT_COUNTER_MAX := 1023.0
var _sc_box: VBoxContainer
var _sc_status: Label
var _sc_event_list: ItemList
var _sc_event_rows: Array = []  # event indices parallel to the event-list rows
var _sc_add_event_button: Button
var _sc_delete_event_button: Button
var _sc_event_syncing: bool = false
# Selected event's own attributes.
var _sc_flags_row: HBoxContainer  # holds the lazily-built flag checkboxes
var _sc_flag_checks: Array = []  # [{ "bit": int, "check": CheckBox }]
var _sc_reset_spin: SpinBox
var _sc_delay_spin: SpinBox
var _sc_attr_syncing: bool = false
# Triggers sub-list + per-trigger editor. _sc_trigger_selected is the local index within the event chain.
var _sc_trigger_list: ItemList
var _sc_trigger_add: Button
var _sc_trigger_remove: Button
var _sc_trigger_up: Button
var _sc_trigger_down: Button
var _sc_trigger_main: OptionButton
var _sc_trigger_sub: OptionButton
var _sc_trigger_negate: CheckBox
var _sc_trigger_or: CheckBox
var _sc_trigger_xor: CheckBox
var _sc_trigger_desc: Label  # plain-language description of the selected trigger type
var _sc_trigger_params: Array = []  # [p1, p2, p3, p4] MissionParamSlot
var _sc_trigger_selected: int = -1
var _sc_trigger_syncing: bool = false
# Actions sub-list + per-action editor.
var _sc_action_list: ItemList
var _sc_action_add: Button
var _sc_action_remove: Button
var _sc_action_up: Button
var _sc_action_down: Button
var _sc_action_type: OptionButton
var _sc_action_sub: OptionButton
var _sc_action_desc: Label  # plain-language description of the selected action type
var _sc_action_params: Array = []  # [p1, p2, p3, p4] MissionParamSlot
var _sc_action_preview_row: HBoxContainer
var _sc_action_preview: Button
var _sc_action_preview_stop: Button
var _sc_action_selected: int = -1
var _sc_action_syncing: bool = false
# The event index the trigger/action sub-selections currently belong to. The event-list click handler
# resets the sub-selections, but the controller can also switch events without a click (set_mode
# auto-focusing the first event, add_event_default). Tracking the shown event lets the refresh drop a
# stale sub-selection so the trigger/action editor never binds to a different event's chain.
var _sc_event_shown: int = -1
var _sc_event_summary: Label  # "when <conditions> then <actions>" readout for the selected event
var _sc_diagnostics: Label
# The event chain the panel was last populated from, so the sub-list handlers read the same data.
var _sc_chain: Dictionary = {}
# PLAYPARTANIM preview gate: the AI-change action family + the play-part-anim sub-type.
const _AI_CHANGE_ACTION_TYPES := [3, 12, 13, 21]
const _PLAYPARTANIM_SUB := 34


# `detail_host` is the shell's right dock (%AssetDock), forwarded by the workspace adapter; the
# editor panels + the Mission form mount there. It defaults to null so the existing one-arg test
# calls (`inspector.setup(fake)`) keep building the whole tree under `_root` unchanged.
func setup(controller, detail_host: Control = null) -> void:
	_controller = controller
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
		_detail_host = detail_host
		_ensure_detail_root()
		_build_mode_tabs()           # LEFT
		_build_edit_panel()          # DOCK: Selection
		_build_object_browser()      # LEFT
		_build_place_panel()         # LEFT
		_build_waypoint_panel()      # LEFT list + DOCK detail
		_build_area_trigger_panel()  # LEFT list + DOCK detail
		_build_scripting_panel()     # LEFT list + DOCK detail
		_build_selection_empty()     # DOCK: Selection standby label (last in the page)
		_build_props_panel()         # DOCK: Mission
		_build_reground_button()     # DOCK: Mission
		_build_loadout_panel()       # DOCK: Mission
		_build_groups_panel()        # DOCK: Mission
		_box = VBoxContainer.new()
		_box.add_theme_constant_override("separation", 6)
		_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_mission_content.add_child(_box)
	else:
		# Already built (a re-setup): just re-point the dock subtree.
		set_detail_host(detail_host)
	if _controller != null and not _controller.changed.is_connected(_refresh):
		_controller.changed.connect(_refresh)
	_refresh()


# --- Right-dock plumbing ------------------------------------------------------
# `_detail_root` is built once and reparented between the dock (real host) and `_root` (no host).
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
	# Delete button) is_visible_in_tree() in the null-host test path.
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


# Parent `_detail_root` under the dock (when a host is set) or under `_root` (no host). Reparents
# without freeing, so the live editor widgets keep their state and signal connections.
func _attach_detail_root() -> void:
	if _detail_root == null or not is_instance_valid(_detail_root):
		return
	var target: Control = _detail_host_box() if (_detail_host != null and is_instance_valid(_detail_host)) else _root
	if target == null:
		return
	var current := _detail_root.get_parent()
	if current == target:
		return
	if current != null:
		current.remove_child(_detail_root)
	target.add_child(_detail_root)


# Lazily build a margin scaffold inside the bare %AssetDock PanelContainer and cache it. The dock
# pages scroll their own content, so the scaffold is just a padded host. The shell owns the dock's
# lifetime (it remove_child + frees the scaffold on switch-away), so we never free it ourselves.
func _detail_host_box() -> Control:
	if _detail_host == null or not is_instance_valid(_detail_host):
		return null
	if _detail_host_inner != null and is_instance_valid(_detail_host_inner) and _detail_host_inner.get_parent() == _detail_host:
		return _detail_host_inner
	var margin := MarginContainer.new()
	margin.name = "MissionDockMargin"
	margin.add_theme_constant_override("margin_left", 10)
	margin.add_theme_constant_override("margin_top", 10)
	margin.add_theme_constant_override("margin_right", 10)
	margin.add_theme_constant_override("margin_bottom", 10)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_detail_host.add_child(margin)
	_detail_host_inner = margin
	return _detail_host_inner


# Re-point the dock subtree at a new host (or null to evacuate it back under `_root` before the
# shell clears the dock on a workspace switch). Idempotent: the same host already mounted is a no-op,
# which absorbs the shell re-asserting the dock on every editor-state sync without thrashing focus.
func set_detail_host(detail_host: Control) -> void:
	if detail_host == _detail_host and _detail_root != null and is_instance_valid(_detail_root) and _detail_root.get_parent() != null:
		return
	_detail_host = detail_host
	if _detail_root == null:
		return  # not built yet; setup() attaches on first build
	_attach_detail_root()
	_refresh()


func _refresh() -> void:
	# Defensively clear the FieldBinder reentrancy guards before this pass. GDScript has no try/finally,
	# so if a bound getter/setter ever errors mid-sync the guard would stay stuck true and silently
	# no-op every field write; clearing here bounds that to a single refresh (changed fires often).
	if _behavior_binder != null:
		_behavior_binder.reset_guard()
	if _props_binder != null:
		_props_binder.reset_guard()
	# Reset the per-list selections when the mission identity flips (open / clear): a kept row index
	# would otherwise bind to a different weapon / group / event in the newly opened document.
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission != _last_mission:
		_last_mission = mission
		_loadout_selected = -1
		_groups_selected = -1
		_sc_trigger_selected = -1
		_sc_action_selected = -1
	_refresh_option_caches()
	_refresh_mode_tabs()
	_refresh_edit_panel()
	_refresh_object_browser()
	_refresh_place_panel()
	_refresh_waypoint_panel()
	_refresh_area_trigger_panel()
	_refresh_scripting_panel()
	_refresh_props_panel()
	_refresh_reground_button()
	_refresh_loadout_panel()
	_refresh_groups_panel()
	# Mode gating: the object panels show only in Objects mode; the waypoint / trigger panels
	# hide themselves outside their mode. The read-only summary shows in every mode.
	if _controller != null and not _controller.is_objects_mode():
		_edit_box.visible = false
		_place_box.visible = false
		_objects_box.visible = false
	_refresh_selection_empty()
	_rebuild_summary()


# The Selection tab's standby label: shown when the current mode has no editor on screen (no
# mission, or Objects mode with nothing selected — the other modes always show their editor with
# disabled fields). Each per-mode editor box has already set its own visibility by this point.
func _build_selection_empty() -> void:
	_sel_empty = ObjectUiHelpers.add_muted_label(_sel_content, "")
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
	_identity_label = ObjectUiHelpers.add_section_heading(_edit_box, "Selected entity")
	_identity_sub = ObjectUiHelpers.add_muted_label(_edit_box, "")

	ObjectUiHelpers.add_section_heading(_edit_box, "Position")
	_pos_spins = [
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionPosX", "X", -1000000.0, 1000000.0, 0.001),
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionPosY", "Y", -1000000.0, 1000000.0, 0.001),
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionPosZ", "Z", -1000000.0, 1000000.0, 0.001),
	]

	ObjectUiHelpers.add_section_heading(_edit_box, "Rotation")
	_rot_spins = [
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionRotPitch", "Pitch", -360.0, 360.0, 1.0),
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionRotYaw", "Yaw", -360.0, 360.0, 1.0),
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionRotRoll", "Roll", -360.0, 360.0, 1.0),
	]

	# team / group are stored on every entity kind by the format, so they are shown for
	# all selectable objects; in practice they drive organics (units) at runtime.
	ObjectUiHelpers.add_section_heading(_edit_box, "Faction")
	_team_option = ObjectUiHelpers.add_id_option_row(_edit_box, "MissionTeam", "Team", [])
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

	_animated_note = ObjectUiHelpers.add_muted_label(_edit_box, "Animated object.")

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
	_delete_button.pressed.connect(_on_delete_pressed)


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
			ObjectUiHelpers.add_section_heading(_behavior_box, String(entry["section"]))
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
	ObjectUiHelpers.add_section_heading(_behavior_box, "Flags")
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
	var spin := ObjectUiHelpers.add_spin_row(_behavior_box, "MissionBeh_" + property, label, min_value, max_value, 1.0)
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
	var option := ObjectUiHelpers.add_id_option_row(parent, "MissionOpt_" + property, label, [])
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
	lbl.custom_minimum_size = Vector2(ObjectUiHelpers.LABEL_COL_WIDTH, 0)
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
			var check := ObjectUiHelpers.add_checkbox(_behavior_flags_box, "MissionBehFlag%d" % bit, String(entry_dict.get("name", "")))
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


# --- Placed-objects browser (persistent) --------------------------------------
# A searchable list of every object already in the mission. Selecting a row drives the controller
# to select that entity and frame the camera on it (find-on-map). Built once; the row cache is
# rebuilt only when the object set changes, like the palette below, so edits / drags stay cheap.

func _build_object_browser() -> void:
	_objects_box = VBoxContainer.new()
	_objects_box.add_theme_constant_override("separation", 4)
	_objects_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_root.add_child(_objects_box)

	ObjectUiHelpers.add_section_heading(_objects_box, "Placed objects")
	_objects_status = ObjectUiHelpers.add_muted_label(_objects_box, "")
	_objects_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	# Show the translate/rotate gizmo on the selected object (drag the arrows to move, the rings to
	# rotate). On by default; turn it off for a plain terrain free-drag.
	var gizmo_check := CheckBox.new()
	gizmo_check.name = "MissionGizmoCheck"
	gizmo_check.text = "Show transform gizmo"
	gizmo_check.tooltip_text = "Show the move/rotate gizmo on the selected object: drag an axis arrow to move it, a ring to rotate it."
	gizmo_check.button_pressed = _controller == null or _controller.is_gizmo_enabled()
	_objects_box.add_child(gizmo_check)
	gizmo_check.toggled.connect(func(pressed: bool) -> void:
		if _controller != null:
			_controller.set_gizmo_enabled(pressed)
	)

	# Debug: visualize the convex collision hulls that picking tests against, in world.
	var pick_debug := CheckBox.new()
	pick_debug.name = "MissionPickDebugCheck"
	pick_debug.text = "Show pick collision (debug)"
	pick_debug.tooltip_text = "Draw the convex collision hulls used for click-picking, over the placed objects."
	pick_debug.button_pressed = _controller != null and _controller.is_pick_debug()
	_objects_box.add_child(pick_debug)
	pick_debug.toggled.connect(func(pressed: bool) -> void:
		if _controller != null:
			_controller.set_pick_debug(pressed)
	)

	_objects_search = LineEdit.new()
	_objects_search.placeholder_text = "Search placed objects"
	_objects_search.clear_button_enabled = true
	_objects_search.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_objects_box.add_child(_objects_search)

	_objects_list = ItemList.new()
	_objects_list.select_mode = ItemList.SELECT_SINGLE
	_objects_list.custom_minimum_size = Vector2(0, 260)
	_objects_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_objects_box.add_child(_objects_list)

	_objects_search.text_changed.connect(_on_object_search_changed)
	_objects_list.item_selected.connect(_on_object_row_selected)


func _refresh_object_browser() -> void:
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission == null:
		_objects_box.visible = false
		_objects_built_for = null
		_objects_built_count = -1
		_objects_db_ready = false
		_objects_cache = []
		_objects_rows = []
		return
	_objects_box.visible = true
	var count: int = _controller.get_object_count()
	var rev: int = _controller.get_membership_revision()
	var db_ready: bool = _controller.has_item_database()
	var mission_changed := mission != _objects_built_for
	# Rebuild rows when the document, its object set (count), or its composition (membership revision,
	# which bumps on every entity-set re-bake) changes, or whenever the item database's readiness flips
	# in EITHER direction (names become resolvable -> replace "Item <id>" placeholders; or the resource
	# dir is cleared/repointed and they must fall back to placeholders again).
	if mission_changed or count != _objects_built_count or rev != _objects_built_rev or db_ready != _objects_db_ready:
		_objects_built_for = mission
		_objects_built_count = count
		_objects_built_rev = rev
		_objects_db_ready = db_ready
		_rebuild_object_cache()
		if mission_changed:
			_objects_search.text = ""
		_populate_object_list(_objects_search.text)
	_sync_object_selection()


# Pull the flat object list from the controller and bake per-row display labels + search keys
# once. Duplicate base names get a "(n)" ordinal so repeated units are distinguishable in the list.
func _rebuild_object_cache() -> void:
	_objects_cache = []
	var rows: Array = _controller.get_object_list() if _controller != null else []
	var name_total: Dictionary = {}
	for r in rows:
		var base := _object_base_label(r)
		name_total[base] = int(name_total.get(base, 0)) + 1
	var name_seen: Dictionary = {}
	for r in rows:
		var base := _object_base_label(r)
		var label := base
		if int(name_total.get(base, 0)) > 1:
			var n := int(name_seen.get(base, 0)) + 1
			name_seen[base] = n
			label = "%s (%d)" % [base, n]
		var category := String(r.get("category", "Object"))
		_objects_cache.append({
			"kind": int(r.get("kind", 0)),
			"index": int(r.get("index", 0)),
			"label": label,
			"category": category,
			"search": (label + " " + category).to_lower(),
		})


# A row's base label: the resolved model name, or an "Item <id>" placeholder when the name is
# not resolvable (no item database yet, or an id the database does not carry).
func _object_base_label(row: Dictionary) -> String:
	var nm := String(row.get("name", "")).strip_edges()
	if not nm.is_empty():
		return nm
	return "Item %d" % int(row.get("item_id", 0))


# Fill the list from the cache, filtered by a case-insensitive substring of the search text.
# Row order matches _objects_rows.
func _populate_object_list(filter: String) -> void:
	_objects_syncing = true
	_objects_list.clear()
	_objects_rows = []
	var needle := filter.strip_edges().to_lower()
	for entry in _objects_cache:
		if not needle.is_empty() and not String(entry["search"]).contains(needle):
			continue
		var row := _objects_list.add_item(String(entry["label"]))
		_objects_list.set_item_tooltip(row, String(entry["category"]))
		_objects_rows.append({ "kind": int(entry["kind"]), "index": int(entry["index"]) })
	_objects_syncing = false
	_update_object_status()


func _update_object_status() -> void:
	var total := _objects_cache.size()
	var shown := _objects_list.item_count
	if total == 0:
		_objects_status.text = "No objects placed yet. Use the palette below to add some."
	elif shown == 0:
		_objects_status.text = "No objects match \"%s\"." % _objects_search.text.strip_edges()
	elif shown == total:
		_objects_status.text = "%d placed object%s. Click one to jump to it." % [total, "" if total == 1 else "s"]
	else:
		_objects_status.text = "Showing %d of %d. Click one to jump to it." % [shown, total]


# Highlight the row for the controller's current selection (so a viewport pick lights up the
# matching list row) and scroll it into view. A selection of another kind (e.g. a marker) or
# nothing clears the list selection. select()/deselect_all() do not emit item_selected.
func _sync_object_selection() -> void:
	var summary: Dictionary = _controller.get_selection_summary() if _controller != null else {}
	_objects_syncing = true
	var found := -1
	if not summary.is_empty():
		var want_kind := int(summary.get("kind", -1))
		var want_index := int(summary.get("index", -1))
		for i in _objects_rows.size():
			var r: Dictionary = _objects_rows[i]
			if int(r["kind"]) == want_kind and int(r["index"]) == want_index:
				found = i
				break
	if found >= 0:
		_objects_list.select(found)
		_objects_list.ensure_current_is_visible()
	else:
		_objects_list.deselect_all()
	_objects_syncing = false


func _on_object_search_changed(text: String) -> void:
	_populate_object_list(text)
	_sync_object_selection()


func _on_object_row_selected(row: int) -> void:
	if _objects_syncing or _controller == null:
		return
	if row < 0 or row >= _objects_rows.size():
		return
	var r: Dictionary = _objects_rows[row]
	_controller.select_object(int(r["kind"]), int(r["index"]))


# --- Place-object palette (persistent) ----------------------------------------
# A searchable list of placeable items. Selecting one arms placement on the controller;
# a terrain click then places it (handled in the viewport input path). Built once and
# only repopulated when the mission changes (the list can be 1000+ rows), like the edit
# panel above; on other refreshes only the armed-state affordance is synced.

func _build_place_panel() -> void:
	_place_box = VBoxContainer.new()
	_place_box.add_theme_constant_override("separation", 4)
	_place_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_root.add_child(_place_box)

	ObjectUiHelpers.add_section_heading(_place_box, "Place object")
	_place_status = ObjectUiHelpers.add_muted_label(_place_box, "")
	_place_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	_place_search = LineEdit.new()
	_place_search.placeholder_text = "Search items"
	_place_search.clear_button_enabled = true
	_place_search.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_place_box.add_child(_place_search)

	_place_list = ItemList.new()
	_place_list.select_mode = ItemList.SELECT_SINGLE
	_place_list.custom_minimum_size = Vector2(0, 220)
	_place_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_place_box.add_child(_place_list)

	_place_stop = Button.new()
	_place_stop.text = "Stop placing"
	_place_stop.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_place_box.add_child(_place_stop)

	_place_search.text_changed.connect(_on_place_search_changed)
	_place_list.item_selected.connect(_on_place_item_selected)
	_place_stop.pressed.connect(_on_place_stop)


func _refresh_place_panel() -> void:
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission == null:
		_place_box.visible = false
		_place_built_for = null
		_placeable_cache = []
		_placeable_names = {}
		return
	_place_box.visible = true
	# Rebuild the item cache + rows when the mission changes, OR keep retrying while the
	# cache is still empty: a mission can open before its items.def is resolvable (e.g.
	# the resource directory is repointed afterwards), and the palette must not stay
	# stuck empty for the life of that open mission. The common case (cache already
	# populated, same mission) skips this entirely, so edits / placements stay cheap.
	var mission_changed := mission != _place_built_for
	if mission_changed or _placeable_cache.is_empty():
		var fresh: Array = _controller.get_placeable_items()
		if mission_changed or not fresh.is_empty():
			_place_built_for = mission
			_placeable_cache = fresh
			_placeable_names = {}
			for entry in _placeable_cache:
				var id := int(entry.get("id", 0))
				var name := String(entry.get("display_name", ""))
				_placeable_names[id] = name if not name.is_empty() else "item %d" % id
			# Only clear the search when the mission itself changes; a late-arriving item
			# database must not wipe a filter the user is mid-typing.
			if mission_changed:
				_place_search.text = ""
			_populate_palette(_place_search.text)
	_sync_place_affordance()


# Fill the list from the cached placeable items, filtered by a case-insensitive
# substring of the search text. Row order matches _place_row_ids.
func _populate_palette(filter: String) -> void:
	_place_syncing = true
	_place_list.clear()
	_place_row_ids = []
	var needle := filter.strip_edges().to_lower()
	for entry in _placeable_cache:
		var id := int(entry.get("id", 0))
		var label: String = _placeable_names.get(id, "item %d" % id)
		if not needle.is_empty() and not label.to_lower().contains(needle):
			continue
		_place_list.add_item(label)
		_place_row_ids.append(id)
	_place_syncing = false


# Reflect the controller's armed state: select the armed row, show the Stop button, and
# write a status line. Selection changes here never echo (select()/deselect_all() do
# not emit item_selected).
func _sync_place_affordance() -> void:
	var armed_id := int(_controller.get_placement_item_id()) if _controller != null else 0
	var armed := armed_id != 0
	_place_stop.visible = armed

	_place_syncing = true
	var row := _place_row_ids.find(armed_id) if armed else -1
	if row >= 0:
		_place_list.select(row)
	else:
		_place_list.deselect_all()
	_place_syncing = false

	if _placeable_cache.is_empty():
		_place_status.text = "No item database (items.def) found. Set a resource directory in the Terrain workspace, then reopen the mission."
	elif armed:
		_place_status.text = "Placing %s. Click the terrain to place it; right-click or Esc to stop." % _placeable_names.get(armed_id, "item %d" % armed_id)
	elif _place_list.item_count == 0 and not _place_search.text.strip_edges().is_empty():
		# The search filtered everything out: say so, rather than leaving the generic prompt
		# over an empty list (which reads like the mission has no items).
		_place_status.text = "No items match \"%s\". Try a different search." % _place_search.text.strip_edges()
	else:
		_place_status.text = "Pick an item, then click the terrain to place it."


func _on_place_search_changed(text: String) -> void:
	_populate_palette(text)
	_sync_place_affordance()


func _on_place_item_selected(row: int) -> void:
	if _place_syncing or _controller == null:
		return
	if row < 0 or row >= _place_row_ids.size():
		return
	_controller.arm_placement(int(_place_row_ids[row]))


func _on_place_stop() -> void:
	if _controller != null:
		_controller.disarm_placement()


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
	_build_sim_bar()


# A Play / Pause / Step / Stop row that runs the live mission simulation over the placed entities
# (the AI walks the NPCs along their authored routes). Read-only over the mission; Stop restores.
func _build_sim_bar() -> void:
	_sim_bar = HBoxContainer.new()
	_sim_bar.name = "MissionSimBar"
	_sim_bar.add_theme_constant_override("separation", 4)
	_sim_bar.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_sim_bar.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	_root.add_child(_sim_bar)

	var label := ObjectUiHelpers.add_muted_label(_sim_bar, "Simulate:")
	label.autowrap_mode = TextServer.AUTOWRAP_OFF
	label.clip_text = true
	label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	label.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	_sim_play_btn = Button.new()
	_sim_play_btn.text = "Play"
	_sim_play_btn.tooltip_text = "Promote the loaded mission and walk its AI along their routes."
	_prepare_sim_button(_sim_play_btn)
	_sim_bar.add_child(_sim_play_btn)
	_sim_play_btn.pressed.connect(func() -> void:
		if _controller != null: _controller.sim_play())

	_sim_pause_btn = Button.new()
	_sim_pause_btn.text = "Pause"
	_prepare_sim_button(_sim_pause_btn)
	_sim_bar.add_child(_sim_pause_btn)
	_sim_pause_btn.pressed.connect(func() -> void:
		if _controller != null: _controller.sim_pause())

	_sim_step_btn = Button.new()
	_sim_step_btn.text = "Step"
	_sim_step_btn.tooltip_text = "Advance the simulation one tick."
	_prepare_sim_button(_sim_step_btn)
	_sim_bar.add_child(_sim_step_btn)
	_sim_step_btn.pressed.connect(func() -> void:
		if _controller != null: _controller.sim_step())

	_sim_stop_btn = Button.new()
	_sim_stop_btn.text = "Stop"
	_sim_stop_btn.tooltip_text = "Stop and restore the authored positions."
	_prepare_sim_button(_sim_stop_btn)
	_sim_bar.add_child(_sim_stop_btn)
	_sim_stop_btn.pressed.connect(func() -> void:
		if _controller != null: _controller.sim_stop())

	_sim_bar.add_child(VSeparator.new())

	# Play-in-editor: boots the REAL game loop over the open mission in a play
	# viewport (the in-place Simulate above stays for quick in-context checks).
	_play_mission_btn = Button.new()
	_play_mission_btn.text = "Play Mission"
	_play_mission_btn.tooltip_text = "Run the open mission with the real game loop in the viewport. Esc stops."
	_prepare_sim_button(_play_mission_btn)
	_sim_bar.add_child(_play_mission_btn)
	_play_mission_btn.pressed.connect(_on_play_mission_pressed)

	# Mission debug panel: summons the engine debug overlay over the editor
	# (read-only there — the workspace locks variable edits before mounting).
	_debug_btn = Button.new()
	_debug_btn.name = "MissionDebugBtn"
	_debug_btn.toggle_mode = true
	_debug_btn.text = "Debug"
	_debug_btn.tooltip_text = "Open the mission debug panel: live units, sim transport, and script variables."
	_prepare_sim_button(_debug_btn)
	_sim_bar.add_child(_debug_btn)
	_debug_btn.toggled.connect(_on_debug_toggled)


# The workspace injects these after building the inspector; without them (tests,
# headless) the Play Mission button simply hides.
func set_play_hooks(play: Callable, is_playing: Callable, stop: Callable) -> void:
	_play_mission_cb = play
	_is_playing_cb = is_playing
	_stop_play_cb = stop
	_refresh_sim_bar()


# Same injection pattern as set_play_hooks: the Debug toggle hides until the
# workspace hands over the overlay summon + open-state query.
func set_debug_hooks(toggle: Callable, is_open: Callable) -> void:
	_debug_toggle_cb = toggle
	_debug_is_open_cb = is_open
	_refresh_sim_bar()


func _on_debug_toggled(_pressed: bool) -> void:
	if _debug_toggle_cb.is_valid():
		_debug_toggle_cb.call()
	# Re-sync from the real open state: a summon that could not mount (no shell)
	# must not leave the toggle latched on.
	_refresh_sim_bar()


func _on_play_mission_pressed() -> void:
	if _is_playing_cb.is_valid() and bool(_is_playing_cb.call()):
		if _stop_play_cb.is_valid():
			_stop_play_cb.call()
	elif _play_mission_cb.is_valid():
		_play_mission_cb.call()
	_refresh_sim_bar()


func _prepare_sim_button(button: Button) -> void:
	button.custom_minimum_size = Vector2(0, 30)
	button.size_flags_vertical = Control.SIZE_SHRINK_CENTER


func _refresh_sim_bar() -> void:
	if _sim_bar == null:
		return
	# Simulation is an optional controller capability; tolerate controllers (e.g. the test
	# doubles) that don't implement it by hiding the bar instead of erroring.
	var supported: bool = _controller != null and _controller.has_method("can_simulate")
	var can: bool = supported and _controller.can_simulate()
	var simming: bool = supported and _controller.is_simulating()
	var playing: bool = supported and _controller.is_sim_playing()
	# The bar also stays up whenever the Debug toggle is wired: it is the
	# overlay's ONLY close affordance in the editor, so it must remain reachable
	# with no mission open (the perf tab works without a sim) and while an
	# overlay is still up after the mission underneath it cleared.
	_sim_bar.visible = can or simming or _debug_toggle_cb.is_valid()
	_sim_play_btn.disabled = not can or playing
	_sim_pause_btn.disabled = not playing
	_sim_step_btn.disabled = not can or playing
	_sim_stop_btn.disabled = not simming
	if _play_mission_btn != null:
		var pie_playing := _is_playing_cb.is_valid() and bool(_is_playing_cb.call())
		_play_mission_btn.visible = _play_mission_cb.is_valid()
		_play_mission_btn.text = "Stop Playing" if pie_playing else "Play Mission"
		_play_mission_btn.disabled = not pie_playing and not can
	if _debug_btn != null:
		# Stays enabled regardless of sim state: the overlay is useful without a
		# live sim (perf tab, last mission's spans). Pressed state rides this
		# refresh (controller.changed), so no polling.
		_debug_btn.visible = _debug_toggle_cb.is_valid()
		if _debug_is_open_cb.is_valid():
			_debug_btn.set_pressed_no_signal(bool(_debug_is_open_cb.call()))


func _on_mode_tab_changed(tab: int) -> void:
	if _mode_syncing or _controller == null:
		return
	if tab >= 0 and tab < _TAB_TO_MODE.size():
		_controller.set_mode(_TAB_TO_MODE[tab])


func _refresh_mode_tabs() -> void:
	_refresh_sim_bar()
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


# --- Waypoints panel ----------------------------------------------------------
# Lists the mission's waypoint paths and reports the selected marker. Selecting a path row
# focuses it (controller.select_waypoint_path); markers are picked in the viewport. Built
# once and repopulated under the _wp_syncing guard (select() must not echo as a pick).
# Path flag editing + marker authoring buttons land in later phases.

func _build_waypoint_panel() -> void:
	_wp_box = VBoxContainer.new()
	_wp_box.add_theme_constant_override("separation", 4)
	_wp_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.visible = false
	_root.add_child(_wp_box)

	ObjectUiHelpers.add_section_heading(_wp_box, "Waypoint paths")
	_wp_status = ObjectUiHelpers.add_muted_label(_wp_box, "")
	_wp_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	# Always-available entry point: focus the first empty path so authoring works even when
	# the list is empty (a brand-new or all-cleared mission), where no row could be clicked.
	_wp_new_path_button = Button.new()
	_wp_new_path_button.name = "MissionWpNewPath"
	_wp_new_path_button.text = "New path"
	_wp_new_path_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(_wp_new_path_button)
	_wp_new_path_button.pressed.connect(_on_wp_new_path_pressed)

	_wp_list = ItemList.new()
	_wp_list.name = "MissionWaypointPaths"
	_wp_list.select_mode = ItemList.SELECT_SINGLE
	_wp_list.custom_minimum_size = Vector2(0, 140)
	_wp_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(_wp_list)
	_wp_list.item_selected.connect(_on_wp_path_selected)

	# The active path's markers in route order. Selecting a row selects that marker.
	ObjectUiHelpers.add_section_heading(_wp_box, "Markers")
	_wp_marker_list = ItemList.new()
	_wp_marker_list.name = "MissionWaypointMarkers"
	_wp_marker_list.select_mode = ItemList.SELECT_SINGLE
	_wp_marker_list.custom_minimum_size = Vector2(0, 140)
	_wp_marker_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(_wp_marker_list)
	_wp_marker_list.item_selected.connect(_on_wp_marker_row_selected)

	# Authoring buttons: Add marker (a placement tool) on its own row; reorder / delete on
	# the next; Clear path last.
	_wp_add_button = Button.new()
	_wp_add_button.name = "MissionWpAddMarker"
	_wp_add_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(_wp_add_button)
	_wp_add_button.pressed.connect(_on_wp_add_pressed)

	var reorder_row := HBoxContainer.new()
	reorder_row.add_theme_constant_override("separation", 6)
	reorder_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(reorder_row)
	_wp_up_button = _make_wp_button(reorder_row, "MissionWpMoveUp", "Move up")
	_wp_down_button = _make_wp_button(reorder_row, "MissionWpMoveDown", "Move down")
	_wp_delete_button = _make_wp_button(reorder_row, "MissionWpDeleteMarker", "Delete marker")
	_wp_up_button.pressed.connect(_on_wp_move_pressed.bind(-1))
	_wp_down_button.pressed.connect(_on_wp_move_pressed.bind(1))
	_wp_delete_button.pressed.connect(_on_wp_delete_marker_pressed)

	_wp_clear_button = Button.new()
	_wp_clear_button.name = "MissionWpClearPath"
	_wp_clear_button.text = "Clear path"
	_wp_clear_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(_wp_clear_button)
	_wp_clear_button.pressed.connect(_on_wp_clear_pressed)

	# --- Detail (dock Selection): active-path flags + selected-marker readout -----
	# These edit the selected path / marker, so they live in the Selection tab beside the other
	# per-selection editors. "Loop" is shown (not "DoesNotLoop") so the toggle reads the way the
	# route behaves; the controller inverts it back to the stored bit.
	_wp_detail_box = VBoxContainer.new()
	_wp_detail_box.add_theme_constant_override("separation", 4)
	_wp_detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_detail_box.visible = false
	_sel_content.add_child(_wp_detail_box)

	ObjectUiHelpers.add_section_heading(_wp_detail_box, "Path")
	var flags_row := HBoxContainer.new()
	flags_row.add_theme_constant_override("separation", 10)
	_wp_detail_box.add_child(flags_row)
	_wp_loop_check = ObjectUiHelpers.add_checkbox(flags_row, "MissionWpLoop", "Loop")
	_wp_blue_check = ObjectUiHelpers.add_checkbox(flags_row, "MissionWpBlue", "Blue")
	_wp_red_check = ObjectUiHelpers.add_checkbox(flags_row, "MissionWpRed", "Red")
	_wp_loop_check.toggled.connect(_on_wp_flag_toggled)
	_wp_blue_check.toggled.connect(_on_wp_flag_toggled)
	_wp_red_check.toggled.connect(_on_wp_flag_toggled)

	_wp_detail_box.add_child(HSeparator.new())
	ObjectUiHelpers.add_section_heading(_wp_detail_box, "Selected marker")
	_wp_marker_label = ObjectUiHelpers.add_muted_label(_wp_detail_box, "")
	_wp_marker_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART


func _make_wp_button(parent: Control, node_name: String, text: String) -> Button:
	var button := Button.new()
	button.name = node_name
	button.text = text
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(button)
	return button


func _on_wp_add_pressed() -> void:
	if _controller == null:
		return
	# The button toggles the add-marker tool: arm it, or disarm if already armed.
	if _controller.is_marker_placement_armed():
		_controller.disarm_marker_placement()
	else:
		_controller.arm_marker_placement()


func _on_wp_move_pressed(delta: int) -> void:
	if _controller != null:
		_controller.move_selected_marker(delta)


func _on_wp_delete_marker_pressed() -> void:
	if _controller != null:
		_controller.delete_selected_marker()


func _on_wp_clear_pressed() -> void:
	if _controller != null:
		_controller.clear_active_path()


func _on_wp_flag_toggled(_pressed: bool) -> void:
	if _wp_flags_syncing or _controller == null:
		return
	_controller.set_waypoint_flags(_wp_loop_check.button_pressed, _wp_blue_check.button_pressed, _wp_red_check.button_pressed)


func _on_wp_marker_row_selected(row: int) -> void:
	if _wp_marker_syncing or _controller == null:
		return
	if row < 0 or row >= _wp_marker_rows.size():
		return
	_controller.select_waypoint_marker(int(_wp_marker_rows[row]))


func _on_wp_path_selected(row: int) -> void:
	if _wp_syncing or _controller == null:
		return
	if row < 0 or row >= _wp_row_paths.size():
		return
	_controller.select_waypoint_path(int(_wp_row_paths[row]))


func _on_wp_new_path_pressed() -> void:
	if _controller != null:
		_controller.select_new_waypoint_path()


func _refresh_waypoint_panel() -> void:
	if _wp_box == null:
		return
	var wp: bool = _controller != null and _controller.is_waypoint_mode() and _controller.get_mission() != null
	_wp_box.visible = wp
	_wp_detail_box.visible = wp
	if not wp:
		return

	# List every populated path, plus the active path even when empty (so a freshly chosen
	# path is visible). Rebuilt each refresh: the list is small (<= 128) and changes shape
	# as paths are authored.
	var active := int(_controller.get_selected_waypoint_path_index())
	var summaries: Array = _controller.get_waypoint_summaries()
	_wp_syncing = true
	_wp_list.clear()
	_wp_row_paths = []
	for s in summaries:
		var idx := int((s as Dictionary)["index"])
		var count := int((s as Dictionary)["marker_count"])
		if count == 0 and idx != active:
			continue
		_wp_list.add_item("Path %d  -  %d markers%s" % [idx, count, _flag_suffix(int((s as Dictionary)["flags"]))])
		_wp_row_paths.append(idx)
		if idx == active:
			_wp_list.select(_wp_list.item_count - 1)
	_wp_syncing = false

	if _wp_row_paths.is_empty():
		_wp_status.text = "No waypoint paths yet. Click New path to start a route."
	elif active < 0:
		_wp_status.text = "Select a path to see its route, then click a marker in the viewport."
	else:
		_wp_status.text = "Click a marker in the viewport to select it."

	# Active-path flags + ordered marker sub-list.
	var active_path: Dictionary = _controller.get_active_waypoint_path()
	var has_active := not active_path.is_empty()
	var flags := int(active_path.get("flags", 0))
	_wp_flags_syncing = true
	_wp_loop_check.button_pressed = (flags & NovaMissionData.WP_FLAG_DOES_NOT_LOOP) == 0
	_wp_blue_check.button_pressed = (flags & NovaMissionData.WP_FLAG_BLUE_TEAM) != 0
	_wp_red_check.button_pressed = (flags & NovaMissionData.WP_FLAG_RED_TEAM) != 0
	_wp_loop_check.disabled = not has_active
	_wp_blue_check.disabled = not has_active
	_wp_red_check.disabled = not has_active
	_wp_flags_syncing = false

	var mission: NovaMissionData = _controller.get_mission()
	var marker_sel: Dictionary = _controller.get_selected_marker()
	var selected_marker_index := int(marker_sel.get("marker_index", -1)) if not marker_sel.is_empty() else -1
	var indices: PackedInt32Array = active_path.get("marker_indices", PackedInt32Array()) if has_active else PackedInt32Array()
	_wp_marker_syncing = true
	_wp_marker_list.clear()
	_wp_marker_rows = []
	for order in indices.size():
		var mi: int = indices[order]
		var pos: Vector3 = mission.get_entity(NovaMissionData.KIND_MARKER, mi).get("position", Vector3.ZERO)
		_wp_marker_list.add_item("%d.  marker #%d  (%.0f, %.0f, %.0f)" % [order + 1, mi, pos.x, pos.y, pos.z])
		_wp_marker_rows.append(mi)
		if mi == selected_marker_index:
			_wp_marker_list.select(_wp_marker_list.item_count - 1)
	_wp_marker_syncing = false

	# Authoring affordances: Add is a toggle (arm / stop); reorder + delete need a selected
	# marker; clear needs a non-empty path. Armed state also drives the status line.
	var armed: bool = _controller.is_marker_placement_armed()
	var has_marker_sel := not marker_sel.is_empty()
	_wp_add_button.text = "Stop adding markers" if armed else "Add marker"
	_wp_add_button.disabled = not has_active
	_wp_up_button.disabled = not has_marker_sel
	_wp_down_button.disabled = not has_marker_sel
	_wp_delete_button.disabled = not has_marker_sel
	_wp_clear_button.disabled = not (has_active and indices.size() > 0)
	if armed:
		_wp_status.text = "Click the terrain to add a marker to this path; right-click or Esc to stop."

	var marker: Dictionary = marker_sel
	if marker.is_empty():
		_wp_marker_label.text = "No marker selected."
	else:
		var p: Vector3 = marker.get("position", Vector3.ZERO)
		# Whole units, matching the marker list rows above (the format stores integers).
		_wp_marker_label.text = "Marker #%d  (%.0f, %.0f, %.0f)" % [int(marker["marker_index"]), p.x, p.y, p.z]


# A short "[loop, blue]"-style suffix describing a path's flags, or "" when none apply.
func _flag_suffix(flags: int) -> String:
	var parts: Array = []
	if (flags & NovaMissionData.WP_FLAG_DOES_NOT_LOOP) == 0:
		parts.append("loop")
	if (flags & NovaMissionData.WP_FLAG_BLUE_TEAM) != 0:
		parts.append("blue")
	if (flags & NovaMissionData.WP_FLAG_RED_TEAM) != 0:
		parts.append("red")
	return "  [%s]" % ", ".join(parts) if not parts.is_empty() else ""


# --- Area-trigger (zone) panel (Phase 2) --------------------------------------
# Lists the mission's zones and edits the selected one: six min/max spins (mission units), the
# two known flag toggles (Active = bit 0x01, Constrain height = bit 0x02), and Add / Delete.
# Selecting a row focuses that zone; a zone is also picked / translated in the viewport. Built
# once, repopulated under guards so a programmatic set never echoes back as a user edit.

func _build_area_trigger_panel() -> void:
	_at_box = VBoxContainer.new()
	_at_box.add_theme_constant_override("separation", 4)
	_at_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_at_box.visible = false
	_root.add_child(_at_box)

	ObjectUiHelpers.add_section_heading(_at_box, "Area triggers / zones")
	_at_status = ObjectUiHelpers.add_muted_label(_at_box, "")
	_at_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	_at_add_button = Button.new()
	_at_add_button.name = "MissionAtAddZone"
	_at_add_button.text = "Add zone"
	_at_add_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_at_box.add_child(_at_add_button)
	_at_add_button.pressed.connect(_on_at_add_pressed)

	_at_list = ItemList.new()
	_at_list.name = "MissionAreaTriggers"
	_at_list.select_mode = ItemList.SELECT_SINGLE
	_at_list.custom_minimum_size = Vector2(0, 120)
	_at_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_at_box.add_child(_at_list)
	_at_list.item_selected.connect(_on_at_row_selected)

	# --- Detail (dock Selection): the selected zone's bounds + flags + delete ------
	# The six bounds spins gain the wider dock column over the cramped left lane.
	_at_detail_box = VBoxContainer.new()
	_at_detail_box.add_theme_constant_override("separation", 4)
	_at_detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_at_detail_box.visible = false
	_sel_content.add_child(_at_detail_box)

	ObjectUiHelpers.add_section_heading(_at_detail_box, "Bounds (mission units)")
	# The format stores bounds as signed 16.16 fixed-point, so the representable range is
	# ~±32768 mission units; the spins are bounded to that (the lib also clamps on write).
	const ZONE_MIN := -32767.0
	const ZONE_MAX := 32767.0
	_at_min_spins = [
		ObjectUiHelpers.add_spin_row(_at_detail_box, "MissionAtMinX", "Min X", ZONE_MIN, ZONE_MAX, 0.001),
		ObjectUiHelpers.add_spin_row(_at_detail_box, "MissionAtMinY", "Min Y", ZONE_MIN, ZONE_MAX, 0.001),
		ObjectUiHelpers.add_spin_row(_at_detail_box, "MissionAtMinZ", "Min Z", ZONE_MIN, ZONE_MAX, 0.001),
	]
	_at_max_spins = [
		ObjectUiHelpers.add_spin_row(_at_detail_box, "MissionAtMaxX", "Max X", ZONE_MIN, ZONE_MAX, 0.001),
		ObjectUiHelpers.add_spin_row(_at_detail_box, "MissionAtMaxY", "Max Y", ZONE_MIN, ZONE_MAX, 0.001),
		ObjectUiHelpers.add_spin_row(_at_detail_box, "MissionAtMaxZ", "Max Z", ZONE_MIN, ZONE_MAX, 0.001),
	]
	for axis in 3:
		_at_min_spins[axis].value_changed.connect(_on_at_bounds_changed)
		_at_max_spins[axis].value_changed.connect(_on_at_bounds_changed)

	ObjectUiHelpers.add_section_heading(_at_detail_box, "Flags")
	var flags_row := HBoxContainer.new()
	flags_row.add_theme_constant_override("separation", 10)
	_at_detail_box.add_child(flags_row)
	_at_active_check = ObjectUiHelpers.add_checkbox(flags_row, "MissionAtActive", "Active")
	_at_constrain_check = ObjectUiHelpers.add_checkbox(flags_row, "MissionAtConstrainZ", "Constrain height")
	_at_active_check.tooltip_text = "Flags bit 0x01. NOTE: the in-zone trigger condition (*IsWithinArea) does NOT read this bit; only the mission-boundary out-of-bounds check does. Shipped missions leave it clear."
	_at_constrain_check.tooltip_text = "Limit the zone to its Z (height) range (bit 0x02). When clear, the zone is unbounded vertically (+/-16384)."
	_at_active_check.toggled.connect(_on_at_flag_toggled)
	_at_constrain_check.toggled.connect(_on_at_flag_toggled)

	_at_detail_box.add_child(HSeparator.new())
	_at_delete_button = Button.new()
	_at_delete_button.name = "MissionAtDeleteZone"
	_at_delete_button.text = "Delete zone"
	_at_delete_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_at_detail_box.add_child(_at_delete_button)
	_at_delete_button.pressed.connect(_on_at_delete_pressed)


func _on_at_add_pressed() -> void:
	if _controller != null:
		_controller.add_area_trigger_default()


func _on_at_delete_pressed() -> void:
	if _controller != null:
		_controller.delete_selected_area_trigger()


func _on_at_row_selected(row: int) -> void:
	if _at_syncing or _controller == null:
		return
	if row < 0 or row >= _at_rows.size():
		return
	_controller.select_area_trigger(int(_at_rows[row]))


func _on_at_bounds_changed(_value: float) -> void:
	if _at_bounds_syncing or _controller == null:
		return
	var mn := Vector3(_at_min_spins[0].value, _at_min_spins[1].value, _at_min_spins[2].value)
	var mx := Vector3(_at_max_spins[0].value, _at_max_spins[1].value, _at_max_spins[2].value)
	_controller.set_selected_zone_bounds(mn, mx)


func _on_at_flag_toggled(_pressed: bool) -> void:
	if _at_flags_syncing or _controller == null:
		return
	_controller.set_selected_zone_flags(_at_active_check.button_pressed, _at_constrain_check.button_pressed)


func _refresh_area_trigger_panel() -> void:
	if _at_box == null:
		return
	var on: bool = _controller != null and _controller.is_area_trigger_mode() and _controller.get_mission() != null
	_at_box.visible = on
	_at_detail_box.visible = on
	if not on:
		return

	var zones: Array = _controller.get_area_triggers()
	var selected := int(_controller.get_selected_zone_index())
	_at_syncing = true
	_at_list.clear()
	_at_rows = []
	for z in zones:
		var zd := z as Dictionary
		var zidx := int(zd.get("index", -1))
		var zmn: Vector3 = zd.get("min", Vector3.ZERO)
		var zmx: Vector3 = zd.get("max", Vector3.ZERO)
		var zactive := bool(zd.get("active", false))
		var label := "Zone %d  (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)%s" % [zidx, zmn.x, zmn.y, zmn.z, zmx.x, zmx.y, zmx.z, "" if zactive else "  [off]"]
		_at_list.add_item(label)
		_at_rows.append(zidx)
		if zidx == selected:
			_at_list.select(_at_list.item_count - 1)
	_at_syncing = false

	if zones.is_empty():
		_at_status.text = "No zones yet. Click Add zone, then drag it in the viewport or set its bounds below."
	elif selected < 0:
		_at_status.text = "Select a zone, or click one in the viewport."
	else:
		_at_status.text = "Drag the zone in the viewport to move it; set exact bounds below."

	# Selected zone's bounds + flags; the editors are disabled when nothing is selected.
	var sel_zone: Dictionary = _controller.get_selected_zone()
	var has_sel := not sel_zone.is_empty()
	var sel_mn: Vector3 = sel_zone.get("min", Vector3.ZERO)
	var sel_mx: Vector3 = sel_zone.get("max", Vector3.ZERO)
	_at_bounds_syncing = true
	# _sync_spin skips a spin whose inner LineEdit is focused, so a refresh mid-edit (e.g. an undo while
	# the user is typing a bound) does not clobber the in-flight keystroke.
	_sync_spin(_at_min_spins[0], sel_mn.x)
	_sync_spin(_at_min_spins[1], sel_mn.y)
	_sync_spin(_at_min_spins[2], sel_mn.z)
	_sync_spin(_at_max_spins[0], sel_mx.x)
	_sync_spin(_at_max_spins[1], sel_mx.y)
	_sync_spin(_at_max_spins[2], sel_mx.z)
	for axis in 3:
		_at_min_spins[axis].editable = has_sel
		_at_max_spins[axis].editable = has_sel
	_at_bounds_syncing = false

	_at_flags_syncing = true
	_at_active_check.button_pressed = bool(sel_zone.get("active", false))
	_at_constrain_check.button_pressed = bool(sel_zone.get("constrain_z", false))
	_at_active_check.disabled = not has_sel
	_at_constrain_check.disabled = not has_sel
	_at_flags_syncing = false

	_at_delete_button.disabled = not has_sel


# --- Scripting (events / triggers / actions) panel ----------------------------
# Mode-tab panel (4th tab). The event list drives the controller's selected event; the event's flags +
# reset/delay, its triggers, and its actions are edited in place. The same focus/echo guards as the
# other panels apply. All mutations go through the controller (one undo step each).

func _make_sc_button(parent: Control, node_name: String, text: String, handler: Callable) -> Button:
	var button := Button.new()
	button.name = node_name
	button.text = text
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(button)
	button.pressed.connect(handler)
	return button


func _add_sc_option(parent: Control, node_name: String, label_text: String, handler: Callable) -> OptionButton:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(row)
	var lbl := Label.new()
	lbl.text = label_text
	lbl.custom_minimum_size = Vector2(96, 0)
	row.add_child(lbl)
	var option := OptionButton.new()
	option.name = node_name
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(option)
	option.item_selected.connect(handler)
	return option


func _build_scripting_panel() -> void:
	_sc_box = VBoxContainer.new()
	_sc_box.add_theme_constant_override("separation", 4)
	_sc_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_sc_box.visible = false
	_root.add_child(_sc_box)

	ObjectUiHelpers.add_section_heading(_sc_box, "Mission scripting (events)")
	_sc_status = ObjectUiHelpers.add_muted_label(_sc_box, "")
	_sc_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	_sc_add_event_button = _make_sc_button(_sc_box, "MissionScAddEvent", "Add event", _on_sc_add_event)

	_sc_event_list = ItemList.new()
	_sc_event_list.name = "MissionScEvents"
	_sc_event_list.select_mode = ItemList.SELECT_SINGLE
	_sc_event_list.custom_minimum_size = Vector2(0, 100)
	_sc_event_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_sc_box.add_child(_sc_event_list)
	_sc_event_list.item_selected.connect(_on_sc_event_selected)

	_sc_delete_event_button = _make_sc_button(_sc_box, "MissionScDeleteEvent", "Delete event", _on_sc_delete_event)

	# --- Detail (dock Selection): the selected event's flags + triggers + actions -
	# The dense chain editor moves to the wider dock; the left pane keeps just the event browser.
	_sc_detail_box = VBoxContainer.new()
	_sc_detail_box.add_theme_constant_override("separation", 4)
	_sc_detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_sc_detail_box.visible = false
	_sel_content.add_child(_sc_detail_box)

	ObjectUiHelpers.add_section_heading(_sc_detail_box, "Event")
	# The flag checkboxes are generated from the engine's bit list on the first refresh (a mission must be
	# loaded for the controller to answer), so a new EventFlags bit appears without touching the UI code.
	_sc_flags_row = HBoxContainer.new()
	_sc_flags_row.name = "MissionScEventFlags"
	_sc_flags_row.add_theme_constant_override("separation", 10)
	_sc_detail_box.add_child(_sc_flags_row)
	_sc_reset_spin = ObjectUiHelpers.add_spin_row(_sc_detail_box, "MissionScReset", "Reset after", 0.0, SCRIPT_COUNTER_MAX, 1.0)
	_sc_delay_spin = ObjectUiHelpers.add_spin_row(_sc_detail_box, "MissionScDelay", "Delay", 0.0, SCRIPT_COUNTER_MAX, 1.0)
	_sc_reset_spin.tooltip_text = "Ticks before a Reset-after event may fire again (the engine keeps the top 10 bits)."
	_sc_delay_spin.tooltip_text = "Ticks the event waits, after its triggers pass, before running its actions."
	_sc_reset_spin.value_changed.connect(_on_sc_attr_changed)
	_sc_delay_spin.value_changed.connect(_on_sc_attr_changed)

	_sc_detail_box.add_child(HSeparator.new())
	ObjectUiHelpers.add_section_heading(_sc_detail_box, "Triggers (conditions)")
	_sc_trigger_list = ItemList.new()
	_sc_trigger_list.name = "MissionScTriggers"
	_sc_trigger_list.select_mode = ItemList.SELECT_SINGLE
	_sc_trigger_list.custom_minimum_size = Vector2(0, 76)
	_sc_trigger_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_sc_detail_box.add_child(_sc_trigger_list)
	_sc_trigger_list.item_selected.connect(_on_sc_trigger_selected)
	var trig_buttons := HBoxContainer.new()
	trig_buttons.add_theme_constant_override("separation", 6)
	_sc_detail_box.add_child(trig_buttons)
	_sc_trigger_add = _make_sc_button(trig_buttons, "MissionScTrigAdd", "Add", _on_sc_trigger_add)
	_sc_trigger_remove = _make_sc_button(trig_buttons, "MissionScTrigRemove", "Remove", _on_sc_trigger_remove)
	_sc_trigger_up = _make_sc_button(trig_buttons, "MissionScTrigUp", "Up", _on_sc_trigger_up)
	_sc_trigger_down = _make_sc_button(trig_buttons, "MissionScTrigDown", "Down", _on_sc_trigger_down)
	_sc_trigger_main = _add_sc_option(_sc_detail_box, "MissionScTrigMain", "Type", _on_sc_trigger_main_selected)
	_sc_trigger_sub = _add_sc_option(_sc_detail_box, "MissionScTrigSub", "Sub-type", _on_sc_trigger_sub_selected)
	_sc_trigger_desc = ObjectUiHelpers.add_muted_label(_sc_detail_box, "")
	_sc_trigger_desc.name = "MissionScTrigDesc"
	var trig_flags := HBoxContainer.new()
	trig_flags.add_theme_constant_override("separation", 10)
	_sc_detail_box.add_child(trig_flags)
	_sc_trigger_negate = ObjectUiHelpers.add_checkbox(trig_flags, "MissionScTrigNeg", "Negate")
	_sc_trigger_or = ObjectUiHelpers.add_checkbox(trig_flags, "MissionScTrigOr", "OR")
	_sc_trigger_xor = ObjectUiHelpers.add_checkbox(trig_flags, "MissionScTrigXor", "XOR")
	_sc_trigger_negate.tooltip_text = "Invert this condition (condition_flags bit 0)."
	# The engine takes the combine operator from THIS trigger's flags to join the NEXT condition in the
	# chain (left to right). The last trigger's OR/XOR bits are unused. [orig: sub_454050 @0x454050]
	_sc_trigger_or.tooltip_text = "Join the NEXT condition with OR instead of AND (bit 1)."
	_sc_trigger_xor.tooltip_text = "Join the NEXT condition with XOR (bit 2)."
	_sc_trigger_negate.toggled.connect(_on_sc_trigger_flag_toggled)
	_sc_trigger_or.toggled.connect(_on_sc_trigger_flag_toggled)
	_sc_trigger_xor.toggled.connect(_on_sc_trigger_flag_toggled)
	_sc_trigger_params = _build_sc_param_slots("MissionScTrigP", _on_sc_trigger_param_changed)

	_sc_detail_box.add_child(HSeparator.new())
	ObjectUiHelpers.add_section_heading(_sc_detail_box, "Actions (effects)")
	_sc_action_list = ItemList.new()
	_sc_action_list.name = "MissionScActions"
	_sc_action_list.select_mode = ItemList.SELECT_SINGLE
	_sc_action_list.custom_minimum_size = Vector2(0, 76)
	_sc_action_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_sc_detail_box.add_child(_sc_action_list)
	_sc_action_list.item_selected.connect(_on_sc_action_selected)
	var act_buttons := HBoxContainer.new()
	act_buttons.add_theme_constant_override("separation", 6)
	_sc_detail_box.add_child(act_buttons)
	_sc_action_add = _make_sc_button(act_buttons, "MissionScActAdd", "Add", _on_sc_action_add)
	_sc_action_remove = _make_sc_button(act_buttons, "MissionScActRemove", "Remove", _on_sc_action_remove)
	_sc_action_up = _make_sc_button(act_buttons, "MissionScActUp", "Up", _on_sc_action_up)
	_sc_action_down = _make_sc_button(act_buttons, "MissionScActDown", "Down", _on_sc_action_down)
	_sc_action_type = _add_sc_option(_sc_detail_box, "MissionScActType", "Type", _on_sc_action_type_selected)
	_sc_action_sub = _add_sc_option(_sc_detail_box, "MissionScActSub", "Sub-type", _on_sc_action_sub_selected)
	_sc_action_desc = ObjectUiHelpers.add_muted_label(_sc_detail_box, "")
	_sc_action_desc.name = "MissionScActDesc"
	_sc_action_params = _build_sc_param_slots("MissionScActP", _on_sc_action_param_changed)

	# Preview row: play this action's part animation on its target model in the editor viewport. Shown
	# only for PLAYPARTANIM (the AI-change "play part anim" sub-type); hidden for every other action.
	_sc_action_preview_row = HBoxContainer.new()
	_sc_action_preview_row.name = "MissionScActPreviewRow"
	_sc_action_preview_row.add_theme_constant_override("separation", 6)
	_sc_action_preview_row.visible = false
	_sc_detail_box.add_child(_sc_action_preview_row)
	_sc_action_preview = _make_sc_button(_sc_action_preview_row, "MissionScActPreview", "Preview", _on_sc_action_preview)
	_sc_action_preview.tooltip_text = "Play this part animation on the target unit in the viewport."
	_sc_action_preview_stop = _make_sc_button(_sc_action_preview_row, "MissionScActPreviewStop", "Stop", _on_sc_action_preview_stop)
	_sc_action_preview_stop.tooltip_text = "Stop the preview and return the model to rest."

	_sc_detail_box.add_child(HSeparator.new())
	ObjectUiHelpers.add_section_heading(_sc_detail_box, "Summary")
	_sc_event_summary = ObjectUiHelpers.add_muted_label(_sc_detail_box, "")
	_sc_event_summary.name = "MissionScSummary"
	_sc_event_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	ObjectUiHelpers.add_section_heading(_sc_detail_box, "Diagnostics")
	_sc_diagnostics = ObjectUiHelpers.add_muted_label(_sc_detail_box, "")
	_sc_diagnostics.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART


# Build 4 typed param slots (label + stacked spinbox/dropdown). One build, never freed; configure() per
# refresh picks the widget from the schema and set_value() syncs focus-guarded. [feature: typed pickers]
func _build_sc_param_slots(prefix: String, on_changed: Callable) -> Array:
	var slots: Array = []
	for i in 4:
		var slot := MissionParamSlot.new()
		slot.setup("%s%d" % [prefix, i + 1], "Param %d" % (i + 1), SCRIPT_PARAM_MIN, SCRIPT_PARAM_MAX)
		_sc_detail_box.add_child(slot)
		slot.committed.connect(func(): on_changed.call(0.0))
		slots.append(slot)
	return slots


func _populate_sc_option(option: OptionButton, entries: Array, selected_value: int) -> void:
	option.clear()
	var sel_idx := -1
	for entry in entries:
		var entry_dict := entry as Dictionary
		var value := int(entry_dict.get("value", 0))
		var idx := option.item_count
		option.add_item(String(entry_dict.get("name", "")))
		option.set_item_id(idx, value)
		if value == selected_value:
			sel_idx = idx
	if sel_idx >= 0:
		option.select(sel_idx)
	else:
		# The stored value is not in the named set (an unknown sub-type, say); surface it as a raw row so
		# the dropdown shows the actual value instead of silently snapping to the first entry.
		option.add_item("Value %d" % selected_value)
		option.set_item_id(option.item_count - 1, selected_value)
		option.select(option.item_count - 1)


func _refresh_scripting_panel() -> void:
	if _sc_box == null:
		return
	var on: bool = _controller != null and _controller.is_scripting_mode() and _controller.get_mission() != null
	_sc_box.visible = on
	_sc_detail_box.visible = on
	if not on:
		return

	# Build the flag checkboxes once, from the engine's bit list.
	if _sc_flag_checks.is_empty():
		for entry in _controller.get_event_flag_bits():
			var entry_dict := entry as Dictionary
			var bit := int(entry_dict.get("value", 0))
			var check := ObjectUiHelpers.add_checkbox(_sc_flags_row, "MissionScFlag%d" % bit, String(entry_dict.get("name", "")))
			check.toggled.connect(_on_sc_flag_toggled)
			_sc_flag_checks.append({ "bit": bit, "check": check })

	var events: Array = _controller.get_events()
	var selected := int(_controller.get_selected_event_index())
	_sc_event_syncing = true
	_sc_event_list.clear()
	_sc_event_rows = []
	for ev in events:
		var ev_dict := ev as Dictionary
		var eidx := int(ev_dict.get("index", -1))
		_sc_event_list.add_item("Event %d  (%d trig, %d act)" % [eidx, int(ev_dict.get("trigger_count", 0)), int(ev_dict.get("action_count", 0))])
		_sc_event_rows.append(eidx)
		if eidx == selected:
			_sc_event_list.select(_sc_event_list.item_count - 1)
	_sc_event_syncing = false

	var has_event := selected >= 0
	_sc_delete_event_button.disabled = not has_event
	if events.is_empty():
		_sc_status.text = "No events yet. Click Add event, then chain triggers (conditions) and actions (effects)."
	elif not has_event:
		_sc_status.text = "Select an event to edit its triggers and actions."
	else:
		_sc_status.text = "Triggers are the conditions; when they pass, the actions run."

	_sc_chain = _controller.get_selected_event_chain()
	var event_dict: Dictionary = _sc_chain.get("event", {})
	var triggers: Array = _sc_chain.get("triggers", [])
	var actions: Array = _sc_chain.get("actions", [])

	_sc_attr_syncing = true
	var flags := int(event_dict.get("flags", 0))
	for flag_entry in _sc_flag_checks:
		var check := flag_entry["check"] as CheckBox
		check.button_pressed = (flags & int(flag_entry["bit"])) != 0
		check.disabled = not has_event
	_sync_spin(_sc_reset_spin, float(event_dict.get("reset_after", 0)))
	_sync_spin(_sc_delay_spin, float(event_dict.get("delay", 0)))
	_sc_reset_spin.editable = has_event
	_sc_delay_spin.editable = has_event
	_sc_attr_syncing = false

	# Drop the sub-selections when the selected event changed since the last refresh through ANY path
	# (event-list click, set_mode auto-focus, add_event), so the trigger/action editor never binds to a
	# trigger/action of a different event than the one now shown. Then clamp to the (possibly shrunken) lists.
	if selected != _sc_event_shown:
		_sc_event_shown = selected
		_sc_trigger_selected = -1
		_sc_action_selected = -1
	if _sc_trigger_selected >= triggers.size():
		_sc_trigger_selected = triggers.size() - 1
	if _sc_action_selected >= actions.size():
		_sc_action_selected = actions.size() - 1

	_refresh_sc_trigger_section(triggers)
	_refresh_sc_action_section(actions)

	# Readable "when <conditions> then <actions>" summary of the whole event. [feature: event-flow readability]
	_sc_event_summary.text = _sc_summary_text(triggers, actions) if has_event else ""

	# Diagnostics: the model's reference checks (zone/event) + editor-side ref-integrity for the typed refs
	# the model doesn't cover (group/event-trigger/waypoint). [feature: validation & ref-integrity]
	var diagnostics: Array = _sc_chain.get("diagnostics", [])
	if not has_event:
		_sc_diagnostics.text = ""
	else:
		var lines: Array = []
		for diag in diagnostics:
			lines.append("⚠ " + String((diag as Dictionary).get("message", "")))
		lines.append_array(_sc_ref_integrity_lines(triggers, actions))
		_sc_diagnostics.text = "No problems detected in this event." if lines.is_empty() else "\n".join(lines)


# Compose the event's plain-language summary. Each trigger phrase comes from the schema description with its
# raw params substituted; triggers are joined left-to-right by THIS trigger's operator (engine semantics,
# sub_454050) and prefixed NOT when negated. Actions are listed in order.
func _sc_summary_text(triggers: Array, actions: Array) -> String:
	var when_part := ""
	if triggers.is_empty():
		when_part = "always"
	else:
		for i in triggers.size():
			var t := triggers[i] as Dictionary
			var phrase := _sc_trigger_phrase(t)
			if bool(t.get("negated", false)):
				phrase = "NOT (%s)" % phrase
			when_part += phrase
			if i < triggers.size() - 1:
				when_part += " %s " % String(t.get("logic_operator", "and")).to_upper()
	var then_part := ""
	if actions.is_empty():
		then_part = "(no actions)"
	else:
		var act_phrases: Array = []
		for a in actions:
			act_phrases.append(_sc_action_phrase(a as Dictionary))
		then_part = "; ".join(act_phrases)
	return "When %s, then %s." % [when_part, then_part]


func _sc_trigger_phrase(t: Dictionary) -> String:
	var schema := MissionParamSchema.trigger_slots(int(t.get("main_type", 0)), int(t.get("sub_type", 0)))
	var params := [int(t.get("param1", 0)), int(t.get("param2", 0)), int(t.get("param3", 0)), int(t.get("param4", 0))]
	var desc := _fill_desc(String(schema["desc"]), params)
	return desc if desc != "" else String(t.get("sub_type_name", t.get("main_type_name", "?")))


func _sc_action_phrase(a: Dictionary) -> String:
	var schema := MissionParamSchema.action_slots(int(a.get("action_type", 0)), int(a.get("action_sub_type", 0)))
	var params := [int(a.get("param1", 0)), int(a.get("param2", 0)), int(a.get("param3", 0)), int(a.get("param4", 0))]
	var desc := _fill_desc(String(schema["desc"]), params)
	return desc if desc != "" else String(a.get("action_type_name", "?"))


func _fill_desc(desc: String, params: Array) -> String:
	if desc == "":
		return ""
	var out := desc
	for i in 4:
		out = out.replace("{p%d}" % (i + 1), str(params[i]))
	return out


# Editor-side range checks for typed refs the C++ event-chain diagnostics don't already cover (it handles
# zone + ResetEvent refs). Conservative: only kinds with a well-defined bound (group 0..count, waypoint
# path), so it never cries wolf and never double-reports what the model already flagged.
func _sc_ref_integrity_lines(triggers: Array, actions: Array) -> Array:
	var lines: Array = []
	var group_count: int = _controller.get_group_count()
	var path_count: int = _controller.get_waypoint_summaries().size()
	var check := func(kind: int, value: int, what: String) -> void:
		match kind:
			MissionParamSchema.Kind.GROUP:
				if value < 0 or value >= group_count:
					lines.append("⚠ %s references group %d (only %d groups)." % [what, value, group_count])
			MissionParamSchema.Kind.WAYPOINT:
				if value < -1 or value >= path_count:
					lines.append("⚠ %s references waypoint path %d (only %d paths)." % [what, value, path_count])
	for ti in triggers.size():
		var t := triggers[ti] as Dictionary
		var ts := MissionParamSchema.trigger_slots(int(t.get("main_type", 0)), int(t.get("sub_type", 0)))
		var tp := [int(t.get("param1", 0)), int(t.get("param2", 0)), int(t.get("param3", 0)), int(t.get("param4", 0))]
		for i in 4:
			check.call(int((ts["params"][i] as Dictionary)["kind"]), tp[i], "Trigger %d" % ti)
	for ai in actions.size():
		var a := actions[ai] as Dictionary
		var as_ := MissionParamSchema.action_slots(int(a.get("action_type", 0)), int(a.get("action_sub_type", 0)))
		var ap := [int(a.get("param1", 0)), int(a.get("param2", 0)), int(a.get("param3", 0)), int(a.get("param4", 0))]
		for i in 4:
			check.call(int((as_["params"][i] as Dictionary)["kind"]), ap[i], "Action %d" % ai)
	return lines


func _refresh_sc_trigger_section(triggers: Array) -> void:
	_sc_trigger_syncing = true
	_sc_trigger_list.clear()
	for i in triggers.size():
		var trig := triggers[i] as Dictionary
		var negate := "!" if bool(trig.get("negated", false)) else ""
		_sc_trigger_list.add_item("%d. %s%s / %s  [%s]" % [i, negate, String(trig.get("main_type_name", "?")), String(trig.get("sub_type_name", "?")), String(trig.get("logic_operator", "and"))])
		if i == _sc_trigger_selected:
			_sc_trigger_list.select(i)
	_sc_trigger_syncing = false

	var has_sel := _sc_trigger_selected >= 0 and _sc_trigger_selected < triggers.size()
	_sc_trigger_add.disabled = _controller.get_selected_event_index() < 0 or triggers.size() >= 20
	_sc_trigger_remove.disabled = not has_sel
	_sc_trigger_up.disabled = not (has_sel and _sc_trigger_selected > 0)
	_sc_trigger_down.disabled = not (has_sel and _sc_trigger_selected < triggers.size() - 1)
	_sc_trigger_main.disabled = not has_sel
	_sc_trigger_sub.disabled = not has_sel
	_sc_trigger_negate.disabled = not has_sel
	_sc_trigger_or.disabled = not has_sel
	_sc_trigger_xor.disabled = not has_sel
	for slot in _sc_trigger_params:
		slot.set_editable(has_sel)

	_sc_trigger_syncing = true
	if not has_sel:
		_sc_trigger_main.clear()
		_sc_trigger_sub.clear()
		_sc_trigger_negate.button_pressed = false
		_sc_trigger_or.button_pressed = false
		_sc_trigger_xor.button_pressed = false
		_sc_trigger_desc.text = ""
		_sc_trigger_syncing = false
		return
	var trig := triggers[_sc_trigger_selected] as Dictionary
	var main_type := int(trig.get("main_type", 0))
	var sub_type := int(trig.get("sub_type", 0))
	_populate_sc_option(_sc_trigger_main, _controller.get_trigger_main_types(), main_type)
	_populate_sc_option(_sc_trigger_sub, _controller.get_trigger_sub_types(main_type), sub_type)
	_sc_trigger_negate.button_pressed = bool(trig.get("negated", false))
	_sc_trigger_or.button_pressed = bool(trig.get("logic_or", false))
	_sc_trigger_xor.button_pressed = bool(trig.get("logic_xor", false))
	var params := [int(trig.get("param1", 0)), int(trig.get("param2", 0)), int(trig.get("param3", 0)), int(trig.get("param4", 0))]
	var schema := MissionParamSchema.trigger_slots(main_type, sub_type)
	_sc_trigger_desc.text = String(schema["desc"]) if String(schema["desc"]) != "" else "No description yet for this trigger type; parameters are raw values."
	for i in 4:
		var slot_def := schema["params"][i] as Dictionary
		_sc_trigger_params[i].configure(slot_def, _sc_param_items(int(slot_def["kind"]), slot_def))
		_sc_trigger_params[i].set_value(params[i])
		# Disable slots this trigger type doesn't use (greyed, non-editable). `used` is false only for
		# described types past their param count; unknown / variable types keep all four editable.
		_sc_trigger_params[i].set_editable(bool(slot_def.get("used", true)))
	_sc_trigger_syncing = false


func _refresh_sc_action_section(actions: Array) -> void:
	_sc_action_syncing = true
	_sc_action_list.clear()
	for i in actions.size():
		var act := actions[i] as Dictionary
		_sc_action_list.add_item("%d. %s / %s" % [i, String(act.get("action_type_name", "?")), String(act.get("action_sub_type_name", "?"))])
		if i == _sc_action_selected:
			_sc_action_list.select(i)
	_sc_action_syncing = false

	var has_sel := _sc_action_selected >= 0 and _sc_action_selected < actions.size()
	_sc_action_add.disabled = _controller.get_selected_event_index() < 0 or actions.size() >= 20
	_sc_action_remove.disabled = not has_sel
	_sc_action_up.disabled = not (has_sel and _sc_action_selected > 0)
	_sc_action_down.disabled = not (has_sel and _sc_action_selected < actions.size() - 1)
	_sc_action_type.disabled = not has_sel
	_sc_action_sub.disabled = not has_sel
	for slot in _sc_action_params:
		slot.set_editable(has_sel)

	_sc_action_syncing = true
	if not has_sel:
		_sc_action_type.clear()
		_sc_action_sub.clear()
		_sc_action_desc.text = ""
		_sc_action_syncing = false
		_refresh_sc_preview(-1, -1)  # no action selected -> hide the row + stop any preview
		return
	var act := actions[_sc_action_selected] as Dictionary
	var action_type := int(act.get("action_type", 0))
	var action_sub := int(act.get("action_sub_type", 0))
	_populate_sc_option(_sc_action_type, _controller.get_action_types(), action_type)
	_populate_sc_option(_sc_action_sub, _controller.get_action_sub_types(action_type), action_sub)
	var params := [int(act.get("param1", 0)), int(act.get("param2", 0)), int(act.get("param3", 0)), int(act.get("param4", 0))]
	var schema := MissionParamSchema.action_slots(action_type, action_sub)
	_sc_action_desc.text = String(schema["desc"]) if String(schema["desc"]) != "" else "No description yet for this action type; parameters are raw values."
	for i in 4:
		var slot_def := schema["params"][i] as Dictionary
		_sc_action_params[i].configure(slot_def, _sc_param_items(int(slot_def["kind"]), slot_def))
		_sc_action_params[i].set_value(params[i])
		# Disable slots this action type doesn't use; AI actions (variable) + raw types stay editable.
		_sc_action_params[i].set_editable(bool(slot_def.get("used", true)))
	_sc_action_syncing = false
	_refresh_sc_preview(action_type, action_sub)


# Show the Preview/Stop row only for a PLAYPARTANIM action, and enable it only when a target model
# resolves. Leaving PLAYPARTANIM (or having no resolvable target) stops any running preview.
func _refresh_sc_preview(action_type: int, action_sub: int) -> void:
	if _sc_action_preview_row == null:
		return
	var is_ppa := action_sub == _PLAYPARTANIM_SUB and action_type in _AI_CHANGE_ACTION_TYPES
	_sc_action_preview_row.visible = is_ppa
	if not is_ppa:
		if _controller != null:
			_controller.stop_preview()
		return
	var actions: Array = _sc_chain.get("actions", [])
	var action: Dictionary = actions[_sc_action_selected] if _sc_action_selected >= 0 and _sc_action_selected < actions.size() else {}
	var can: bool = _controller != null and not action.is_empty() and _controller.can_preview_part_anim(action)
	_sc_action_preview.disabled = not can
	_sc_action_preview_stop.disabled = not can
	_sc_action_preview.tooltip_text = "Play this part animation on the target unit in the viewport." if can \
		else "Select or target an animated entity to preview this part animation."


# Build the dropdown items for a picker-kind param slot from the mission's collections. RAW kinds get [].
# An out-of-range stored value is handled by MissionParamSlot.set_value (shows it as a raw "Value N" row).
func _sc_param_items(kind: int, slot_def: Dictionary) -> Array:
	if _controller == null:
		return []
	match kind:
		MissionParamSchema.Kind.GROUP:
			var groups: Array = []
			for i in _controller.get_group_count():
				groups.append({ "value": i, "label": "Group %d" % i })
			return groups
		MissionParamSchema.Kind.ENTITY:
			# Cached (see _refresh_option_caches): get_all_entities marshals every entity, so calling it
			# per param slot on every scripting refresh would re-walk the whole scene each keystroke.
			return _cached_all_entities
		MissionParamSchema.Kind.ZONE:
			var zones: Array = []
			for z in _controller.get_area_triggers():
				var zd := z as Dictionary
				zones.append({ "value": int(zd.get("index", 0)), "label": "Zone %d (id %d)" % [int(zd.get("index", 0)), int(zd.get("id", 0))] })
			return zones
		MissionParamSchema.Kind.EVENT:
			var evs: Array = []
			for e in _controller.get_events():
				var ed := e as Dictionary
				evs.append({ "value": int(ed.get("index", 0)), "label": "Event %d" % int(ed.get("index", 0)) })
			return evs
		MissionParamSchema.Kind.WAYPOINT:
			var wps: Array = [{ "value": -1, "label": "-1 (nearest of type)" }]
			for w in _controller.get_waypoint_summaries():
				var wd := w as Dictionary
				wps.append({ "value": int(wd.get("index", 0)), "label": "Path %d" % int(wd.get("index", 0)) })
			return wps
		MissionParamSchema.Kind.BOOL:
			return [{ "value": 0, "label": "Off (0)" }, { "value": 1, "label": "On (1)" }]
		MissionParamSchema.Kind.ENUM:
			return slot_def.get("enum", [])
	return []


func _on_sc_add_event() -> void:
	if _controller == null:
		return
	_sc_trigger_selected = -1
	_sc_action_selected = -1
	_controller.add_event_default()


func _on_sc_delete_event() -> void:
	if _controller == null:
		return
	_sc_trigger_selected = -1
	_sc_action_selected = -1
	_controller.delete_selected_event()


func _on_sc_event_selected(row: int) -> void:
	if _sc_event_syncing or _controller == null:
		return
	if row < 0 or row >= _sc_event_rows.size():
		return
	# A different event has its own triggers / actions; drop the sub-selections.
	_sc_trigger_selected = -1
	_sc_action_selected = -1
	_controller.select_event(int(_sc_event_rows[row]))


func _on_sc_flag_toggled(_pressed: bool) -> void:
	if _sc_attr_syncing or _controller == null:
		return
	_commit_selected_event()


func _on_sc_attr_changed(_value: float) -> void:
	if _sc_attr_syncing or _controller == null:
		return
	_commit_selected_event()


func _commit_selected_event() -> void:
	if _controller == null:
		return
	var flags := 0
	for flag_entry in _sc_flag_checks:
		if (flag_entry["check"] as CheckBox).button_pressed:
			flags |= int(flag_entry["bit"])
	_controller.set_selected_event(flags, int(_sc_reset_spin.value), int(_sc_delay_spin.value))


func _on_sc_trigger_selected(row: int) -> void:
	if _sc_trigger_syncing or _controller == null:
		return
	_sc_trigger_selected = row
	_refresh_sc_trigger_section(_sc_chain.get("triggers", []))


func _on_sc_trigger_add() -> void:
	if _controller == null:
		return
	# The appended trigger lands at the end; pre-select that index so the post-add refresh focuses it.
	_sc_trigger_selected = int(_sc_chain.get("triggers", []).size())
	_controller.add_selected_event_trigger()


func _on_sc_trigger_remove() -> void:
	if _controller == null or _sc_trigger_selected < 0:
		return
	_controller.remove_selected_event_trigger(_sc_trigger_selected)


func _on_sc_trigger_up() -> void:
	if _controller == null or _sc_trigger_selected <= 0:
		return
	var from := _sc_trigger_selected
	_sc_trigger_selected = from - 1
	_controller.move_selected_event_trigger(from, -1)


func _on_sc_trigger_down() -> void:
	if _controller == null or _sc_trigger_selected < 0:
		return
	if _sc_trigger_selected >= int(_sc_chain.get("triggers", []).size()) - 1:
		return
	var from := _sc_trigger_selected
	_sc_trigger_selected = from + 1
	_controller.move_selected_event_trigger(from, 1)


func _on_sc_trigger_main_selected(_idx: int) -> void:
	if _sc_trigger_syncing:
		return
	# Switching the main type resets the sub-type (the old sub rarely maps onto the new type).
	_commit_selected_trigger({ "sub_type": 0 })


func _on_sc_trigger_sub_selected(_idx: int) -> void:
	if _sc_trigger_syncing:
		return
	_commit_selected_trigger()


func _on_sc_trigger_flag_toggled(_pressed: bool) -> void:
	if _sc_trigger_syncing:
		return
	_commit_selected_trigger()


func _on_sc_trigger_param_changed(_value: float) -> void:
	if _sc_trigger_syncing:
		return
	_commit_selected_trigger()


func _commit_selected_trigger(overrides: Dictionary = {}) -> void:
	if _controller == null or _sc_trigger_selected < 0:
		return
	var trigger := {
		"main_type": _sc_trigger_main.get_selected_id(),
		"sub_type": _sc_trigger_sub.get_selected_id(),
		"param1": _sc_trigger_params[0].read_value(),
		"param2": _sc_trigger_params[1].read_value(),
		"param3": _sc_trigger_params[2].read_value(),
		"param4": _sc_trigger_params[3].read_value(),
		"negated": _sc_trigger_negate.button_pressed,
		"logic_or": _sc_trigger_or.button_pressed,
		"logic_xor": _sc_trigger_xor.button_pressed,
	}
	for key in overrides:
		trigger[key] = overrides[key]
	_controller.set_selected_event_trigger(_sc_trigger_selected, trigger)


func _on_sc_action_selected(row: int) -> void:
	if _sc_action_syncing or _controller == null:
		return
	_sc_action_selected = row
	_refresh_sc_action_section(_sc_chain.get("actions", []))


func _on_sc_action_add() -> void:
	if _controller == null:
		return
	_sc_action_selected = int(_sc_chain.get("actions", []).size())
	_controller.add_selected_event_action()


func _on_sc_action_remove() -> void:
	if _controller == null or _sc_action_selected < 0:
		return
	_controller.remove_selected_event_action(_sc_action_selected)


func _on_sc_action_up() -> void:
	if _controller == null or _sc_action_selected <= 0:
		return
	var from := _sc_action_selected
	_sc_action_selected = from - 1
	_controller.move_selected_event_action(from, -1)


func _on_sc_action_down() -> void:
	if _controller == null or _sc_action_selected < 0:
		return
	if _sc_action_selected >= int(_sc_chain.get("actions", []).size()) - 1:
		return
	var from := _sc_action_selected
	_sc_action_selected = from + 1
	_controller.move_selected_event_action(from, 1)


func _on_sc_action_type_selected(_idx: int) -> void:
	if _sc_action_syncing:
		return
	_commit_selected_action({ "action_sub_type": 0 })


func _on_sc_action_sub_selected(_idx: int) -> void:
	if _sc_action_syncing:
		return
	_commit_selected_action()


func _on_sc_action_param_changed(_value: float) -> void:
	if _sc_action_syncing:
		return
	_commit_selected_action()


func _on_sc_action_preview() -> void:
	if _controller == null or _sc_action_selected < 0:
		return
	var actions: Array = _sc_chain.get("actions", [])
	if _sc_action_selected >= actions.size():
		return
	_controller.preview_part_anim(actions[_sc_action_selected])


func _on_sc_action_preview_stop() -> void:
	if _controller != null:
		_controller.stop_preview()


func _commit_selected_action(overrides: Dictionary = {}) -> void:
	if _controller == null or _sc_action_selected < 0:
		return
	var action := {
		"action_type": _sc_action_type.get_selected_id(),
		"action_sub_type": _sc_action_sub.get_selected_id(),
		"param1": _sc_action_params[0].read_value(),
		"param2": _sc_action_params[1].read_value(),
		"param3": _sc_action_params[2].read_value(),
		"param4": _sc_action_params[3].read_value(),
	}
	for key in overrides:
		action[key] = overrides[key]
	_controller.set_selected_event_action(_sc_action_selected, action)


# --- Mission properties (header) editable form --------------------------------

func _build_props_panel() -> void:
	_props_binder = FieldBinder.new()
	_props_toggle = CheckButton.new()
	_props_toggle.name = "MissionPropsToggle"
	_props_toggle.text = "Mission properties"
	_props_toggle.tooltip_text = "Mission-level header: title, world, gameplay, game modes."
	_props_toggle.button_pressed = false
	_props_toggle.visible = false
	_mission_content.add_child(_props_toggle)

	_props_box = VBoxContainer.new()
	_props_box.name = "MissionPropsBox"
	_props_box.add_theme_constant_override("separation", 4)
	_props_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.visible = false
	_mission_content.add_child(_props_box)
	_props_toggle.toggled.connect(func(on: bool) -> void: _props_box.visible = on)

	_add_props_line("mission_name", "Name", "Mission title.")
	_add_props_line("designer", "Designer", "Mission author.")
	_add_props_line("briefing", "Briefing", "Mission briefing text.")
	ObjectUiHelpers.add_section_heading(_props_box, "World")
	_terrain_ref_widget = _add_props_ref("terrain", "terrain", "Terrain",
		"The ground this mission is built on. The world reloads onto the new terrain the next time the mission is opened.")
	_env_ref_widget = _add_props_ref("environment", "environment", "Environment",
		"Sky, light, and weather for this mission. Takes effect the next time the mission is opened.")
	_add_props_option("climate", "Climate", [[0, "Desert"], [1, "Jungle"], [2, "Snow"]])
	_add_props_option("weather", "Weather", [[0, "Nice day"], [1, "Rainy"], [2, "Snow"]])
	_add_props_option("mission_type", "Type", [[1, "Normal"], [2, "Combat vehicle"], [3, "Tenth Mountain"]])
	ObjectUiHelpers.add_section_heading(_props_box, "Gameplay")
	_add_props_spin("player_health", "Player health", 0.0, 1000000.0)
	_add_props_spin("minutes_per_day", "Minutes / day", 0.0, 65535.0)
	_add_props_spin("max_saves", "Max saves", 0.0, 255.0)
	_add_props_spin("start_time", "Start time", 0.0, 65535.0)
	ObjectUiHelpers.add_section_heading(_props_box, "Game mode")
	_add_props_game_mode()
	ObjectUiHelpers.add_section_heading(_props_box, "Options")
	_add_props_flag(NovaMissionData.ATTRIB_ENABLE_NVG, "Night vision")
	_add_props_flag(NovaMissionData.ATTRIB_ROTATE_MAP_180, "Rotate map 180")
	ObjectUiHelpers.add_section_heading(_props_box, "Audio")
	_add_props_spin("music", "Music track", 0.0, 1000000.0)
	_add_props_spin("reverb", "Reverb", 0.0, 1000000.0)


# DOCK: Mission — the manual twin of the workspace's activate-time re-ground prompt
# (terrain heights edited under the mission, undo of an applied re-ground, declined
# prompt: this button reaches the same one-undo-step bulk re-ground any time).
func _build_reground_button() -> void:
	_reground_button = Button.new()
	_reground_button.name = "MissionRegroundAll"
	_reground_button.text = "Re-ground objects"
	_reground_button.tooltip_text = "Snap objects the terrain moved out from under back onto the surface (one undo step). Objects placed above the ground on purpose are left alone."
	_reground_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_reground_button.visible = false
	_mission_content.add_child(_reground_button)
	_reground_button.pressed.connect(func() -> void:
		if _controller != null and _controller.has_method("reground_drifted"):
			_controller.reground_drifted())


func _refresh_reground_button() -> void:
	if _reground_button == null:
		return
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	_reground_button.visible = mission != null and _controller.has_method("reground_drifted")
	_reground_button.disabled = _controller != null and _controller.has_method("is_simulating") \
		and _controller.is_simulating()


func _add_props_line(field: String, label: String, tooltip: String = "") -> LineEdit:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.tooltip_text = tooltip if not tooltip.is_empty() else label
	lbl.clip_text = true
	lbl.custom_minimum_size = Vector2(96, 0)
	row.add_child(lbl)
	var line := LineEdit.new()
	line.name = "MissionProp_" + field
	line.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(line)
	_props_binder.bind_line(line,
		func(info) -> String: return String(info.get(field, "")),
		func(text: String) -> void: _set_header_string(field, text))
	return line


func _add_props_ref(field: String, kind: String, label: String, tooltip: String = "") -> ResourceRefWidget:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.tooltip_text = tooltip if not tooltip.is_empty() else label
	lbl.clip_text = true
	lbl.custom_minimum_size = Vector2(96, 0)
	row.add_child(lbl)
	var widget := ResourceRefWidget.new()
	widget.name = "MissionProp_" + field
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	widget.configure(kind, label, _ref_services)
	row.add_child(widget)
	_props_binder.bind_link(widget,
		func(info) -> String: return String(info.get(field, "")),
		func(text: String) -> void: _set_header_string(field, text))
	return widget


## Wires the link widgets' resolve/pick/jump Callables (see
## ResourceRefWidget.services_from_shell). Idempotent; safe before or after
## the form is built.
func set_reference_services(services: Dictionary) -> void:
	_ref_services = services
	if _terrain_ref_widget != null and is_instance_valid(_terrain_ref_widget):
		_terrain_ref_widget.configure("terrain", "Terrain", services)
	if _env_ref_widget != null and is_instance_valid(_env_ref_widget):
		_env_ref_widget.configure("environment", "Environment", services)


func _add_props_option(field: String, label: String, choices: Array) -> OptionButton:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.custom_minimum_size = Vector2(96, 0)
	row.add_child(lbl)
	var option := OptionButton.new()
	option.name = "MissionProp_" + field
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for choice in choices:
		var idx := option.item_count
		option.add_item(String(choice[1]))
		option.set_item_id(idx, int(choice[0]))
	row.add_child(option)
	_props_binder.bind_option(option,
		func(info) -> int: return int(info.get(field, 0)),
		func(value: int) -> void: _set_header_int(field, value))
	return option


func _add_props_spin(field: String, label: String, min_value: float, max_value: float) -> SpinBox:
	var spin := ObjectUiHelpers.add_spin_row(_props_box, "MissionProp_" + field, label, min_value, max_value, 1.0)
	_props_binder.bind_spin(spin,
		func(info) -> float: return float(int(info.get(field, 0))),
		func(value: float) -> void: _set_header_int(field, int(value)))
	return spin


func _add_props_flag(bit: int, label: String) -> CheckBox:
	var check := CheckBox.new()
	check.name = "MissionFlag_%d" % bit
	check.text = label
	_props_box.add_child(check)
	_props_binder.bind_checkbox(check,
		func(info) -> bool: return (int(info.get("attrib_flags", 0)) & bit) != 0,
		func(on: bool) -> void: _set_header_flag(bit, on))
	return check


# Engine combobox order [orig: sub_402770 @0x404eff dfx2med.exe]. Index 0 = no mode bits (Single
# Player). The dropdown uses the list INDEX as the item id (Godot ids are 32-bit, but the high modes
# like Search & Destroy = 0x80000000 are not), mapping index <-> attrib_flags bit through this table.
const _GAME_MODE_BITS := [
	0,          # Single player (no mode bits)
	0x1000000,  # Co-op
	0x2000000,  # Deathmatch
	0x20000000, # Team deathmatch
	0x4000000,  # King of the hill
	0x40000000, # Team king of the hill
	0x10000000, # Capture the flag
	0x800000,   # Attack & defend
	0x80000000, # Search & destroy
	0x8000000,  # Flagball
	0x10000,    # Advance & secure
	0x20000,    # Conquer & control
]
const _GAME_MODE_LABELS := [
	"Single player", "Co-op", "Deathmatch", "Team deathmatch", "King of the hill",
	"Team king of the hill", "Capture the flag", "Attack & defend", "Search & destroy",
	"Flagball", "Advance & secure", "Conquer & control",
]


func _add_props_game_mode() -> OptionButton:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.add_child(row)
	var lbl := Label.new()
	lbl.text = "Game mode"
	lbl.custom_minimum_size = Vector2(96, 0)
	row.add_child(lbl)
	var option := OptionButton.new()
	option.name = "MissionProp_game_mode"
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for i in _GAME_MODE_LABELS.size():
		option.add_item(String(_GAME_MODE_LABELS[i]))
		option.set_item_id(i, i)
	row.add_child(option)
	# The mission stores the active mode as one attrib_flags bit; get_game_mode() returns it (0 = SP).
	# Map bit -> index for selection, index -> bit on edit.
	_props_binder.bind_option(option,
		func(info) -> int: return maxi(0, _GAME_MODE_BITS.find(int(info.get("game_mode", 0)))),
		func(id: int) -> void:
			# Bounds-check: the item id equals the list index today (no fallback row), but guard so a
			# future out-of-range fallback id cannot index _GAME_MODE_BITS out of bounds.
			if id >= 0 and id < _GAME_MODE_BITS.size():
				_set_game_mode(int(_GAME_MODE_BITS[id])))
	return option


func _set_game_mode(bit: int) -> void:
	if _controller != null:
		_controller.set_game_mode(bit)


func _set_header_string(field: String, value: String) -> void:
	if _controller != null:
		_controller.set_header_string(field, value)


func _set_header_int(field: String, value: int) -> void:
	if _controller != null:
		_controller.set_header_int(field, value)


func _set_header_flag(bit: int, on: bool) -> void:
	if _controller != null:
		_controller.set_header_flag(bit, on)


func _refresh_props_panel() -> void:
	if _props_toggle == null:
		return
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission == null:
		_props_toggle.visible = false
		_props_box.visible = false
		return
	_props_toggle.visible = true
	_props_binder.sync_from(mission.get_info())


# --- Weapon loadout (mission-global collapsible) ------------------------------

func _build_loadout_panel() -> void:
	_loadout_toggle = CheckButton.new()
	_loadout_toggle.name = "MissionLoadoutToggle"
	_loadout_toggle.text = "Weapon loadout"
	_loadout_toggle.tooltip_text = "Weapons allowed for this mission. An empty list means no restriction (the game uses its default)."
	_loadout_toggle.button_pressed = false
	_loadout_toggle.visible = false
	_mission_content.add_child(_loadout_toggle)

	_loadout_box = VBoxContainer.new()
	_loadout_box.name = "MissionLoadoutBox"
	_loadout_box.add_theme_constant_override("separation", 4)
	_loadout_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_loadout_box.visible = false
	_mission_content.add_child(_loadout_box)
	# Repopulate on expand (the per-`changed` refresh skips the list rebuild while collapsed).
	_loadout_toggle.toggled.connect(func(on: bool) -> void:
		_loadout_box.visible = on
		if on:
			_refresh_loadout_panel())

	_loadout_status = ObjectUiHelpers.add_muted_label(_loadout_box, "")
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
	_loadout_value1 = _add_loadout_line("MissionLoadoutValue1", "Value 1", "First loadout value (usually -1).")
	_loadout_value2 = _add_loadout_line("MissionLoadoutValue2", "Value 2", "Second loadout value (usually -1).")

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
	lbl.custom_minimum_size = Vector2(72, 0)
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
	if _controller == null:
		return
	var entries: Array = _controller.get_weapon_loadout()
	# Seed a non-empty name: an empty name serializes to a leading NUL the loader treats as the chunk
	# terminator (dropping this row and any after it). The user renames it (the name field grabs focus).
	entries.append({ "name": "WPN_NEW", "value1": "-1", "value2": "-1" })
	_loadout_selected = entries.size() - 1
	_controller.set_weapon_loadout(entries)
	_loadout_name.grab_focus()


func _on_loadout_delete_pressed() -> void:
	if _controller == null or _loadout_selected < 0:
		return
	var entries: Array = _controller.get_weapon_loadout()
	if _loadout_selected >= entries.size():
		return
	entries.remove_at(_loadout_selected)
	_loadout_selected = mini(_loadout_selected, entries.size() - 1)
	_controller.set_weapon_loadout(entries)


func _on_loadout_row_selected(row: int) -> void:
	if _loadout_syncing:
		return
	# Flush a pending edit to the previously-selected row before switching (so a click-away keeps it).
	_commit_loadout_editors()
	_loadout_selected = row
	_refresh_loadout_panel()


func _commit_loadout_editors() -> void:
	if _loadout_syncing or _controller == null or _loadout_selected < 0:
		return
	var entries: Array = _controller.get_weapon_loadout()
	if _loadout_selected >= entries.size():
		return
	var entry: Dictionary = entries[_loadout_selected]
	# No-op edits add no undo step.
	if String(entry.get("name", "")) == _loadout_name.text \
			and String(entry.get("value1", "")) == _loadout_value1.text \
			and String(entry.get("value2", "")) == _loadout_value2.text:
		return
	# A weapon cannot be nameless: the .bms loadout chunk uses an empty name as its terminator, so a blank
	# would drop the weapon (set_weapon_loadout rejects "" too — same is_empty() test, so the two layers
	# agree). Reject the whole edit: revert ALL three fields to the stored entry (not just the name, so a
	# simultaneous value edit can't be half-applied or left visually stale) and warn in the panel's own
	# status. Remove deletes a weapon.
	if _loadout_name.text.is_empty():
		_loadout_syncing = true
		_loadout_name.text = String(entry.get("name", ""))
		_loadout_value1.text = String(entry.get("value1", ""))
		_loadout_value2.text = String(entry.get("value2", ""))
		_loadout_syncing = false
		_loadout_status.text = "A weapon needs a name. Use Remove to delete it."
		return
	entry["name"] = _loadout_name.text
	entry["value1"] = _loadout_value1.text
	entry["value2"] = _loadout_value2.text
	entries[_loadout_selected] = entry
	_controller.set_weapon_loadout(entries)


func _refresh_loadout_panel() -> void:
	if _loadout_toggle == null:
		return
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission == null:
		_loadout_toggle.visible = false
		_loadout_box.visible = false
		return
	_loadout_toggle.visible = true
	# Skip the (relatively heavy) list/editor rebuild while collapsed; the toggle handler refreshes
	# on expand, so an open panel is always current.
	if not _loadout_box.visible:
		return

	var entries: Array = _controller.get_weapon_loadout()
	if _loadout_selected >= entries.size():
		_loadout_selected = entries.size() - 1
	_loadout_syncing = true
	_loadout_list.clear()
	for i in entries.size():
		var e := entries[i] as Dictionary
		_loadout_list.add_item("%s   (%s, %s)" % [String(e.get("name", "")), String(e.get("value1", "")), String(e.get("value2", ""))])
		if i == _loadout_selected:
			_loadout_list.select(i)
	_loadout_syncing = false

	var has_sel := _loadout_selected >= 0 and _loadout_selected < entries.size()
	var sel: Dictionary = entries[_loadout_selected] if has_sel else {}
	_loadout_syncing = true
	_sync_line(_loadout_name, String(sel.get("name", "")))
	_sync_line(_loadout_value1, String(sel.get("value1", "")))
	_sync_line(_loadout_value2, String(sel.get("value2", "")))
	_loadout_syncing = false
	_loadout_name.editable = has_sel
	_loadout_value1.editable = has_sel
	_loadout_value2.editable = has_sel
	_loadout_delete.disabled = not has_sel

	if entries.is_empty():
		_loadout_status.text = "No weapons restricted (mission uses the default loadout). Add a weapon to restrict it."
	elif not has_sel:
		_loadout_status.text = "Select a weapon to edit its name and values."
	else:
		_loadout_status.text = "%d weapon%s in the loadout." % [entries.size(), "" if entries.size() == 1 else "s"]


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


# --- Groups (mission-global collapsible) --------------------------------------

func _build_groups_panel() -> void:
	_groups_toggle = CheckButton.new()
	_groups_toggle.name = "MissionGroupsToggle"
	_groups_toggle.text = "Groups"
	_groups_toggle.tooltip_text = "Per-group data (64 groups)."
	_groups_toggle.button_pressed = false
	_groups_toggle.visible = false
	_mission_content.add_child(_groups_toggle)

	_groups_box = VBoxContainer.new()
	_groups_box.name = "MissionGroupsBox"
	_groups_box.add_theme_constant_override("separation", 4)
	_groups_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_groups_box.visible = false
	_mission_content.add_child(_groups_box)
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
	ObjectUiHelpers.add_section_heading(_groups_box, "Fields")
	const INT32_MIN := -2147483648.0
	const INT32_MAX := 2147483647.0
	_group_spins = [
		ObjectUiHelpers.add_spin_row(_groups_box, "MissionGroupFlags", "Flags", 0.0, 3.0, 1.0),
		ObjectUiHelpers.add_spin_row(_groups_box, "MissionGroupValue", "Value", INT32_MIN, INT32_MAX, 1.0),
		ObjectUiHelpers.add_spin_row(_groups_box, "MissionGroupConstant", "Constant", 10.0, 10.0, 1.0),
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
	if _groups_syncing or _controller == null or _groups_selected < 0:
		return
	_controller.set_group(_groups_selected,
		int(_group_spins[0].value), int(_group_spins[1].value), int(_group_spins[2].value))


func _refresh_groups_panel() -> void:
	if _groups_toggle == null:
		return
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission == null:
		_groups_toggle.visible = false
		_groups_box.visible = false
		return
	_groups_toggle.visible = true
	if not _groups_box.visible:
		return

	var groups: Array = _controller.get_groups()
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
	_sync_spin(_group_spins[0], float(int(sel.get("field0", 0))))
	_sync_spin(_group_spins[1], float(int(sel.get("field8", 0))))
	_sync_spin(_group_spins[2], float(int(sel.get("field12", 0))))
	_group_spins[0].editable = has_sel
	_group_spins[1].editable = has_sel
	_group_spins[2].editable = false
	_groups_syncing = false


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
		_add_body("Open a .bms mission to load its terrain, environment, and placed objects.")
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
