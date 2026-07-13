extends "res://modtools/mission/inspectors/inspector_section.gd"


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
	_inspector._root.add_child(_sc_box)

	InspectorForms.add_section_heading(_sc_box, "Mission scripting (events)")
	_sc_status = InspectorForms.add_muted_label(_sc_box, "")
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
	_inspector._sc_detail_box = VBoxContainer.new()
	_inspector._sc_detail_box.add_theme_constant_override("separation", 4)
	_inspector._sc_detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector._sc_detail_box.visible = false
	_inspector._sel_content.add_child(_inspector._sc_detail_box)

	InspectorForms.add_section_heading(_inspector._sc_detail_box, "Event")
	# The flag checkboxes are generated from the engine's bit list on the first refresh (a mission must be
	# loaded for the controller to answer), so a new EventFlags bit appears without touching the UI code.
	_sc_flags_row = HBoxContainer.new()
	_sc_flags_row.name = "MissionScEventFlags"
	_sc_flags_row.add_theme_constant_override("separation", 10)
	_inspector._sc_detail_box.add_child(_sc_flags_row)
	_sc_reset_spin = InspectorForms.add_spin_row(_inspector._sc_detail_box, "MissionScReset", "Reset after", 0.0, SCRIPT_COUNTER_MAX, 1.0)
	_sc_delay_spin = InspectorForms.add_spin_row(_inspector._sc_detail_box, "MissionScDelay", "Delay", 0.0, SCRIPT_COUNTER_MAX, 1.0)
	_sc_reset_spin.tooltip_text = "Ticks before a Reset-after event may fire again (the engine keeps the top 10 bits)."
	_sc_delay_spin.tooltip_text = "Ticks the event waits, after its triggers pass, before running its actions."
	_sc_reset_spin.value_changed.connect(_on_sc_attr_changed)
	_sc_delay_spin.value_changed.connect(_on_sc_attr_changed)

	_inspector._sc_detail_box.add_child(HSeparator.new())
	InspectorForms.add_section_heading(_inspector._sc_detail_box, "Triggers (conditions)")
	_sc_trigger_list = ItemList.new()
	_sc_trigger_list.name = "MissionScTriggers"
	_sc_trigger_list.select_mode = ItemList.SELECT_SINGLE
	_sc_trigger_list.custom_minimum_size = Vector2(0, 76)
	_sc_trigger_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector._sc_detail_box.add_child(_sc_trigger_list)
	_sc_trigger_list.item_selected.connect(_on_sc_trigger_selected)
	var trig_buttons := HBoxContainer.new()
	trig_buttons.add_theme_constant_override("separation", 6)
	_inspector._sc_detail_box.add_child(trig_buttons)
	_sc_trigger_add = _make_sc_button(trig_buttons, "MissionScTrigAdd", "Add", _on_sc_trigger_add)
	_sc_trigger_remove = _make_sc_button(trig_buttons, "MissionScTrigRemove", "Remove", _on_sc_trigger_remove)
	_sc_trigger_up = _make_sc_button(trig_buttons, "MissionScTrigUp", "Up", _on_sc_trigger_up)
	_sc_trigger_down = _make_sc_button(trig_buttons, "MissionScTrigDown", "Down", _on_sc_trigger_down)
	_sc_trigger_main = _add_sc_option(_inspector._sc_detail_box, "MissionScTrigMain", "Type", _on_sc_trigger_main_selected)
	_sc_trigger_sub = _add_sc_option(_inspector._sc_detail_box, "MissionScTrigSub", "Sub-type", _on_sc_trigger_sub_selected)
	_sc_trigger_desc = InspectorForms.add_muted_label(_inspector._sc_detail_box, "")
	_sc_trigger_desc.name = "MissionScTrigDesc"
	var trig_flags := HBoxContainer.new()
	trig_flags.add_theme_constant_override("separation", 10)
	_inspector._sc_detail_box.add_child(trig_flags)
	_sc_trigger_negate = InspectorForms.add_checkbox(trig_flags, "MissionScTrigNeg", "Negate")
	_sc_trigger_or = InspectorForms.add_checkbox(trig_flags, "MissionScTrigOr", "OR")
	_sc_trigger_xor = InspectorForms.add_checkbox(trig_flags, "MissionScTrigXor", "XOR")
	_sc_trigger_negate.tooltip_text = "Invert this condition (condition_flags bit 0)."
	# The engine takes the combine operator from THIS trigger's flags to join the NEXT condition in the
	# chain (left to right). The last trigger's OR/XOR bits are unused. [orig: sub_454050 @0x454050]
	_sc_trigger_or.tooltip_text = "Join the NEXT condition with OR instead of AND (bit 1)."
	_sc_trigger_xor.tooltip_text = "Join the NEXT condition with XOR (bit 2)."
	_sc_trigger_negate.toggled.connect(_on_sc_trigger_flag_toggled)
	_sc_trigger_or.toggled.connect(_on_sc_trigger_flag_toggled)
	_sc_trigger_xor.toggled.connect(_on_sc_trigger_flag_toggled)
	_sc_trigger_params = _build_sc_param_slots("MissionScTrigP", _on_sc_trigger_param_changed)

	_inspector._sc_detail_box.add_child(HSeparator.new())
	InspectorForms.add_section_heading(_inspector._sc_detail_box, "Actions (effects)")
	_sc_action_list = ItemList.new()
	_sc_action_list.name = "MissionScActions"
	_sc_action_list.select_mode = ItemList.SELECT_SINGLE
	_sc_action_list.custom_minimum_size = Vector2(0, 76)
	_sc_action_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector._sc_detail_box.add_child(_sc_action_list)
	_sc_action_list.item_selected.connect(_on_sc_action_selected)
	var act_buttons := HBoxContainer.new()
	act_buttons.add_theme_constant_override("separation", 6)
	_inspector._sc_detail_box.add_child(act_buttons)
	_sc_action_add = _make_sc_button(act_buttons, "MissionScActAdd", "Add", _on_sc_action_add)
	_sc_action_remove = _make_sc_button(act_buttons, "MissionScActRemove", "Remove", _on_sc_action_remove)
	_sc_action_up = _make_sc_button(act_buttons, "MissionScActUp", "Up", _on_sc_action_up)
	_sc_action_down = _make_sc_button(act_buttons, "MissionScActDown", "Down", _on_sc_action_down)
	_sc_action_type = _add_sc_option(_inspector._sc_detail_box, "MissionScActType", "Type", _on_sc_action_type_selected)
	_sc_action_sub = _add_sc_option(_inspector._sc_detail_box, "MissionScActSub", "Sub-type", _on_sc_action_sub_selected)
	_sc_action_desc = InspectorForms.add_muted_label(_inspector._sc_detail_box, "")
	_sc_action_desc.name = "MissionScActDesc"
	_sc_action_params = _build_sc_param_slots("MissionScActP", _on_sc_action_param_changed)

	# Preview row: play this action's part animation on its target model in the editor viewport. Shown
	# only for PLAYPARTANIM (the AI-change "play part anim" sub-type); hidden for every other action.
	_sc_action_preview_row = HBoxContainer.new()
	_sc_action_preview_row.name = "MissionScActPreviewRow"
	_sc_action_preview_row.add_theme_constant_override("separation", 6)
	_sc_action_preview_row.visible = false
	_inspector._sc_detail_box.add_child(_sc_action_preview_row)
	_sc_action_preview = _make_sc_button(_sc_action_preview_row, "MissionScActPreview", "Preview", _on_sc_action_preview)
	_sc_action_preview.tooltip_text = "Play this part animation on the target unit in the viewport."
	_sc_action_preview_stop = _make_sc_button(_sc_action_preview_row, "MissionScActPreviewStop", "Stop", _on_sc_action_preview_stop)
	_sc_action_preview_stop.tooltip_text = "Stop the preview and return the model to rest."

	_inspector._sc_detail_box.add_child(HSeparator.new())
	InspectorForms.add_section_heading(_inspector._sc_detail_box, "Summary")
	_sc_event_summary = InspectorForms.add_muted_label(_inspector._sc_detail_box, "")
	_sc_event_summary.name = "MissionScSummary"
	_sc_event_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	InspectorForms.add_section_heading(_inspector._sc_detail_box, "Diagnostics")
	_sc_diagnostics = InspectorForms.add_muted_label(_inspector._sc_detail_box, "")
	_sc_diagnostics.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART


# Build 4 typed param slots (label + stacked spinbox/dropdown). One build, never freed; configure() per
# refresh picks the widget from the schema and set_value() syncs focus-guarded. [feature: typed pickers]
func _build_sc_param_slots(prefix: String, on_changed: Callable) -> Array:
	var slots: Array = []
	for i in 4:
		var slot := MissionParamSlot.new()
		slot.setup("%s%d" % [prefix, i + 1], "Param %d" % (i + 1), SCRIPT_PARAM_MIN, SCRIPT_PARAM_MAX)
		_inspector._sc_detail_box.add_child(slot)
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
	var on: bool = _inspector._controller != null and _inspector._controller.is_scripting_mode() and _inspector._controller.get_mission() != null
	_sc_box.visible = on
	_inspector._sc_detail_box.visible = on
	if not on:
		return

	# Build the flag checkboxes once, from the engine's bit list.
	if _sc_flag_checks.is_empty():
		for entry in _inspector._controller.get_event_flag_bits():
			var entry_dict := entry as Dictionary
			var bit := int(entry_dict.get("value", 0))
			var check := InspectorForms.add_checkbox(_sc_flags_row, "MissionScFlag%d" % bit, String(entry_dict.get("name", "")))
			check.toggled.connect(_on_sc_flag_toggled)
			_sc_flag_checks.append({ "bit": bit, "check": check })

	var events: Array = _inspector._controller.get_events()
	var selected := int(_inspector._controller.get_selected_event_index())
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

	_sc_chain = _inspector._controller.get_selected_event_chain()
	var event_dict: Dictionary = _sc_chain.get("event", {})
	var triggers: Array = _sc_chain.get("triggers", [])
	var actions: Array = _sc_chain.get("actions", [])

	_sc_attr_syncing = true
	var flags := int(event_dict.get("flags", 0))
	for flag_entry in _sc_flag_checks:
		var check := flag_entry["check"] as CheckBox
		check.button_pressed = (flags & int(flag_entry["bit"])) != 0
		check.disabled = not has_event
	_inspector._sync_spin(_sc_reset_spin, float(event_dict.get("reset_after", 0)))
	_inspector._sync_spin(_sc_delay_spin, float(event_dict.get("delay", 0)))
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
	var desc := _fill_desc(schema.desc, params)
	return desc if desc != "" else String(t.get("sub_type_name", t.get("main_type_name", "?")))


func _sc_action_phrase(a: Dictionary) -> String:
	var schema := MissionParamSchema.action_slots(int(a.get("action_type", 0)), int(a.get("action_sub_type", 0)))
	var params := [int(a.get("param1", 0)), int(a.get("param2", 0)), int(a.get("param3", 0)), int(a.get("param4", 0))]
	var desc := _fill_desc(schema.desc, params)
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
	var group_count: int = _inspector._controller.get_group_count()
	var path_count: int = _inspector._controller.get_waypoint_summaries().size()
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
			check.call(ts.params[i].kind, tp[i], "Trigger %d" % ti)
	for ai in actions.size():
		var a := actions[ai] as Dictionary
		var as_ := MissionParamSchema.action_slots(int(a.get("action_type", 0)), int(a.get("action_sub_type", 0)))
		var ap := [int(a.get("param1", 0)), int(a.get("param2", 0)), int(a.get("param3", 0)), int(a.get("param4", 0))]
		for i in 4:
			check.call(as_.params[i].kind, ap[i], "Action %d" % ai)
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
	_sc_trigger_add.disabled = _inspector._controller.get_selected_event_index() < 0 or triggers.size() >= 20
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
	_populate_sc_option(_sc_trigger_main, _inspector._controller.get_trigger_main_types(), main_type)
	_populate_sc_option(_sc_trigger_sub, _inspector._controller.get_trigger_sub_types(main_type), sub_type)
	_sc_trigger_negate.button_pressed = bool(trig.get("negated", false))
	_sc_trigger_or.button_pressed = bool(trig.get("logic_or", false))
	_sc_trigger_xor.button_pressed = bool(trig.get("logic_xor", false))
	var params := [int(trig.get("param1", 0)), int(trig.get("param2", 0)), int(trig.get("param3", 0)), int(trig.get("param4", 0))]
	var schema := MissionParamSchema.trigger_slots(main_type, sub_type)
	_sc_trigger_desc.text = schema.desc if schema.desc != "" else "No description yet for this trigger type; parameters are raw values."
	for i in 4:
		var slot_def := schema.params[i]
		_sc_trigger_params[i].configure(slot_def, _sc_param_items(slot_def.kind, slot_def))
		_sc_trigger_params[i].set_value(params[i])
		# Disable slots this trigger type doesn't use (greyed, non-editable). `used` is false only for
		# described types past their param count; unknown / variable types keep all four editable.
		_sc_trigger_params[i].set_editable(slot_def.used)
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
	_sc_action_add.disabled = _inspector._controller.get_selected_event_index() < 0 or actions.size() >= 20
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
	_populate_sc_option(_sc_action_type, _inspector._controller.get_action_types(), action_type)
	_populate_sc_option(_sc_action_sub, _inspector._controller.get_action_sub_types(action_type), action_sub)
	var params := [int(act.get("param1", 0)), int(act.get("param2", 0)), int(act.get("param3", 0)), int(act.get("param4", 0))]
	var schema := MissionParamSchema.action_slots(action_type, action_sub)
	_sc_action_desc.text = schema.desc if schema.desc != "" else "No description yet for this action type; parameters are raw values."
	for i in 4:
		var slot_def := schema.params[i]
		_sc_action_params[i].configure(slot_def, _sc_param_items(slot_def.kind, slot_def))
		_sc_action_params[i].set_value(params[i])
		# Disable slots this action type doesn't use; AI actions (variable) + raw types stay editable.
		_sc_action_params[i].set_editable(slot_def.used)
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
		if _inspector._controller != null:
			_inspector._controller.stop_preview()
		return
	var actions: Array = _sc_chain.get("actions", [])
	var action: Dictionary = actions[_sc_action_selected] if _sc_action_selected >= 0 and _sc_action_selected < actions.size() else {}
	var can: bool = _inspector._controller != null and not action.is_empty() and _inspector._controller.can_preview_part_anim(action)
	_sc_action_preview.disabled = not can
	_sc_action_preview_stop.disabled = not can
	_sc_action_preview.tooltip_text = "Play this part animation on the target unit in the viewport." if can \
		else "Select or target an animated entity to preview this part animation."


# Build the dropdown items for a picker-kind param slot from the mission's collections. RAW kinds get [].
# An out-of-range stored value is handled by MissionParamSlot.set_value (shows it as a raw "Value N" row).
func _sc_param_items(kind: int, slot_def: MissionParamSlotSpec) -> Array:
	if _inspector._controller == null:
		return []
	match kind:
		MissionParamSchema.Kind.GROUP:
			var groups: Array = []
			for i in _inspector._controller.get_group_count():
				groups.append({ "value": i, "label": "Group %d" % i })
			return groups
		MissionParamSchema.Kind.ENTITY:
			# Cached (see _refresh_option_caches): get_all_entities marshals every entity, so calling it
			# per param slot on every scripting refresh would re-walk the whole scene each keystroke.
			return _inspector._cached_all_entities
		MissionParamSchema.Kind.ZONE:
			var zones: Array = []
			for z in _inspector._controller.get_area_triggers():
				var zd := z as Dictionary
				zones.append({ "value": int(zd.get("index", 0)), "label": "Zone %d (id %d)" % [int(zd.get("index", 0)), int(zd.get("id", 0))] })
			return zones
		MissionParamSchema.Kind.EVENT:
			var evs: Array = []
			for e in _inspector._controller.get_events():
				var ed := e as Dictionary
				evs.append({ "value": int(ed.get("index", 0)), "label": "Event %d" % int(ed.get("index", 0)) })
			return evs
		MissionParamSchema.Kind.WAYPOINT:
			var wps: Array = [{ "value": -1, "label": "-1 (nearest of type)" }]
			for w in _inspector._controller.get_waypoint_summaries():
				var wd := w as Dictionary
				wps.append({ "value": int(wd.get("index", 0)), "label": "Path %d" % int(wd.get("index", 0)) })
			return wps
		MissionParamSchema.Kind.BOOL:
			return [{ "value": 0, "label": "Off (0)" }, { "value": 1, "label": "On (1)" }]
		MissionParamSchema.Kind.ENUM:
			return slot_def.enum_items
	return []


func _on_sc_add_event() -> void:
	if _inspector._controller == null:
		return
	_sc_trigger_selected = -1
	_sc_action_selected = -1
	_inspector._controller.add_event_default()


func _on_sc_delete_event() -> void:
	if _inspector._controller == null:
		return
	_sc_trigger_selected = -1
	_sc_action_selected = -1
	_inspector._controller.delete_selected_event()


func _on_sc_event_selected(row: int) -> void:
	if _sc_event_syncing or _inspector._controller == null:
		return
	if row < 0 or row >= _sc_event_rows.size():
		return
	# A different event has its own triggers / actions; drop the sub-selections.
	_sc_trigger_selected = -1
	_sc_action_selected = -1
	_inspector._controller.select_event(int(_sc_event_rows[row]))


func _on_sc_flag_toggled(_pressed: bool) -> void:
	if _sc_attr_syncing or _inspector._controller == null:
		return
	_commit_selected_event()


func _on_sc_attr_changed(_value: float) -> void:
	if _sc_attr_syncing or _inspector._controller == null:
		return
	_commit_selected_event()


func _commit_selected_event() -> void:
	if _inspector._controller == null:
		return
	var flags := 0
	for flag_entry in _sc_flag_checks:
		if (flag_entry["check"] as CheckBox).button_pressed:
			flags |= int(flag_entry["bit"])
	_inspector._controller.set_selected_event(flags, int(_sc_reset_spin.value), int(_sc_delay_spin.value))


func _on_sc_trigger_selected(row: int) -> void:
	if _sc_trigger_syncing or _inspector._controller == null:
		return
	_sc_trigger_selected = row
	_refresh_sc_trigger_section(_sc_chain.get("triggers", []))


func _on_sc_trigger_add() -> void:
	if _inspector._controller == null:
		return
	# The appended trigger lands at the end; pre-select that index so the post-add refresh focuses it.
	_sc_trigger_selected = int(_sc_chain.get("triggers", []).size())
	_inspector._controller.add_selected_event_trigger()


func _on_sc_trigger_remove() -> void:
	if _inspector._controller == null or _sc_trigger_selected < 0:
		return
	_inspector._controller.remove_selected_event_trigger(_sc_trigger_selected)


func _on_sc_trigger_up() -> void:
	if _inspector._controller == null or _sc_trigger_selected <= 0:
		return
	var from := _sc_trigger_selected
	_sc_trigger_selected = from - 1
	_inspector._controller.move_selected_event_trigger(from, -1)


func _on_sc_trigger_down() -> void:
	if _inspector._controller == null or _sc_trigger_selected < 0:
		return
	if _sc_trigger_selected >= int(_sc_chain.get("triggers", []).size()) - 1:
		return
	var from := _sc_trigger_selected
	_sc_trigger_selected = from + 1
	_inspector._controller.move_selected_event_trigger(from, 1)


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
	if _inspector._controller == null or _sc_trigger_selected < 0:
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
	_inspector._controller.set_selected_event_trigger(_sc_trigger_selected, trigger)


func _on_sc_action_selected(row: int) -> void:
	if _sc_action_syncing or _inspector._controller == null:
		return
	_sc_action_selected = row
	_refresh_sc_action_section(_sc_chain.get("actions", []))


func _on_sc_action_add() -> void:
	if _inspector._controller == null:
		return
	_sc_action_selected = int(_sc_chain.get("actions", []).size())
	_inspector._controller.add_selected_event_action()


func _on_sc_action_remove() -> void:
	if _inspector._controller == null or _sc_action_selected < 0:
		return
	_inspector._controller.remove_selected_event_action(_sc_action_selected)


func _on_sc_action_up() -> void:
	if _inspector._controller == null or _sc_action_selected <= 0:
		return
	var from := _sc_action_selected
	_sc_action_selected = from - 1
	_inspector._controller.move_selected_event_action(from, -1)


func _on_sc_action_down() -> void:
	if _inspector._controller == null or _sc_action_selected < 0:
		return
	if _sc_action_selected >= int(_sc_chain.get("actions", []).size()) - 1:
		return
	var from := _sc_action_selected
	_sc_action_selected = from + 1
	_inspector._controller.move_selected_event_action(from, 1)


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
	if _inspector._controller == null or _sc_action_selected < 0:
		return
	var actions: Array = _sc_chain.get("actions", [])
	if _sc_action_selected >= actions.size():
		return
	_inspector._controller.preview_part_anim(actions[_sc_action_selected])


func _on_sc_action_preview_stop() -> void:
	if _inspector._controller != null:
		_inspector._controller.stop_preview()


func _commit_selected_action(overrides: Dictionary = {}) -> void:
	if _inspector._controller == null or _sc_action_selected < 0:
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
	_inspector._controller.set_selected_event_action(_sc_action_selected, action)
