extends "res://modtools/music/ui/live_mode_section.gd"

# MusicLiveMode's navigation operations (W4-6a): drill-in / back-to-map,
# the nav location handler that swaps the canvas, breadcrumb trail +
# status, browser-style back/forward input, and the States sidebar.
# Verbatim motion from live_mode.gd; state stays on the mount, reached
# through `_lm` (the mount's _unhandled_input virtual delegates here).


# Drill into a state: navigate there; the nav announces the move and
# _on_nav_location_changed swaps the canvas. A manual drill (double-click,
# "open ▸", Add State, sidebar click) pins (stops live auto-follow) and pushes a
# trail hop; a live-follow drill from _on_section passes pin=false, which both
# keeps following and REPLACES the current trail entry so a transitioning VM
# doesn't flood the history with every state it enters.
func _drill_into(section_name: String, pin: bool = true) -> void:
	if _lm._program_view == null or _lm._document == null or not _lm._document.script_loaded():
		return
	if pin:
		_lm._follow_live = false
	_lm._nav.navigate_to(_lm.MusicNavClass.section_entry(section_name), pin)


# The single place the canvas reacts to navigation. Map: hide the program view and
# restore the map chrome. Section: rebuild its program and swap it in; a stale
# trail entry (the state vanished under the history) is scrubbed, which re-lands
# on the previous surviving location.
func _on_nav_location_changed(entry: Dictionary) -> void:
	if String(entry.get("kind", "")) == "map":
		if _lm._program_view != null:
			_lm._program_view.visible = false
		_lm._logic_section_name = ""
		_set_map_chrome_visible(true)
		if _lm._breadcrumb_label != null:
			_lm._breadcrumb_label.text = ""
	else:
		var section_name := String(entry.get("name", ""))
		if not _populate_program_view(section_name):
			_lm._nav.remove_section(section_name)
			return
		_lm._logic_section_name = section_name
		if _lm._canvas != null:
			_lm._canvas.visible = true
		_lm._program_view.visible = true
		_set_map_chrome_visible(false)
		_update_breadcrumb_status(section_name)
		_refresh_breadcrumb_action_state()
	if _lm._breadcrumb != null and _lm._document != null and _lm._document.script_loaded():
		_lm._breadcrumb.visible = true
	_rebuild_breadcrumb()
	_refresh_states_selection()


# Surface the open state's quirks next to the trail (the map shows the same
# badges on its nodes; without this the drilled-in view gave no hint why a
# state loops in place or can't be reached).
func _update_breadcrumb_status(section_name: String) -> void:
	if _lm._breadcrumb_label == null:
		return
	var parts := PackedStringArray()
	if _section_is_idle_loop(section_name):
		parts.append("↻ loops to itself")
	elif _section_is_unlinked(section_name):
		parts.append("unlinked — nothing points here yet")
	# Caller inputs are explained ON the canvas now (the Inputs card / the
	# engine-events divider), not as a breadcrumb badge.
	_lm._breadcrumb_label.text = "   " + " · ".join(parts) if parts.size() > 0 else ""


# Rebuild the breadcrumb trail row: back/forward enablement, one clickable
# segment per hop (older hops collapse into "…"), and the state-action buttons
# (rename/delete/follow) only while a state is open.
func _rebuild_breadcrumb() -> void:
	if _lm._crumb_segments == null or _lm._nav == null:
		return
	for c in _lm._crumb_segments.get_children():
		_lm._crumb_segments.remove_child(c)
		c.queue_free()
	var t: Array = _lm._nav.trail()
	var start := 0
	if t.size() > _lm.MAX_CRUMBS:
		start = t.size() - _lm.MAX_CRUMBS
		var ell := Label.new()
		ell.text = "…"
		ell.tooltip_text = "%d earlier steps (use ◀ to walk back through them)" % start
		ell.add_theme_color_override("font_color", Color(0.55, 0.58, 0.65))
		_lm._crumb_segments.add_child(ell)
	for i in range(start, t.size()):
		if _lm._crumb_segments.get_child_count() > 0:
			var sep := Label.new()
			sep.text = "▸"
			sep.add_theme_color_override("font_color", Color(0.5, 0.53, 0.6))
			_lm._crumb_segments.add_child(sep)
		var e: Dictionary = t[i]
		var seg := Button.new()
		seg.text = "Map" if String(e.get("kind", "")) == "map" else String(e.get("name", ""))
		seg.flat = true
		seg.focus_mode = Control.FOCUS_NONE
		if i == t.size() - 1:
			seg.disabled = true
			seg.tooltip_text = "You are here."
			seg.add_theme_color_override("font_disabled_color", Color(0.9, 0.94, 1.0))
		else:
			seg.tooltip_text = "Go back to %s." % seg.text
			var idx := i
			seg.pressed.connect(func(): _lm._nav.jump_to(idx))
		_lm._crumb_segments.add_child(seg)
	if _lm._nav_back_btn != null:
		_lm._nav_back_btn.disabled = not _lm._nav.can_go_back()
	if _lm._nav_fwd_btn != null:
		_lm._nav_fwd_btn.disabled = not _lm._nav.can_go_forward()
	var on_section: bool = not _lm._nav.is_on_map()
	if _lm._breadcrumb_rename_btn != null:
		_lm._breadcrumb_rename_btn.visible = on_section
	if _lm._breadcrumb_delete_btn != null:
		_lm._breadcrumb_delete_btn.visible = on_section
	if _lm._follow_btn != null:
		_lm._follow_btn.visible = on_section


# Browser-style navigation keys/buttons, active while the workspace is on
# screen: Alt+Left / Alt+Right and the mouse back/forward thumb buttons.
func _unhandled_input(event: InputEvent) -> void:
	if _lm._nav == null or not _lm.is_visible_in_tree():
		return
	if event is InputEventKey and event.pressed and event.alt_pressed:
		if event.keycode == KEY_LEFT:
			_lm._nav.go_back()
			_lm.get_viewport().set_input_as_handled()
		elif event.keycode == KEY_RIGHT:
			_lm._nav.go_forward()
			_lm.get_viewport().set_input_as_handled()
	elif event is InputEventMouseButton and event.pressed:
		if event.button_index == MOUSE_BUTTON_XBUTTON1:
			_lm._nav.go_back()
			_lm.get_viewport().set_input_as_handled()
		elif event.button_index == MOUSE_BUTTON_XBUTTON2:
			_lm._nav.go_forward()
			_lm.get_viewport().set_input_as_handled()


# --- States sidebar ------------------------------------------------------

# Rebuild the always-visible state list (left of the canvas): the map row, then
# one row per state with its badges and the live ▶ marker. Selection mirrors
# the nav location so the user always knows where they are, however deep the
# trail goes.
func _refresh_states_list() -> void:
	if _lm._states_list == null:
		return
	_lm._states_list.clear()
	if _lm._document == null or not _lm._document.script_loaded():
		return
	_lm._states_list.add_item("⌂ Map")
	_lm._states_list.set_item_metadata(0, "")
	_lm._states_list.set_item_tooltip(0, "The whole-program state map.")
	var sn := StringName(_lm._document.mus_script.get_default_script_name())
	var model: Array = _lm.MusicSectionGraphClass.build(_lm._document.mus_script, sn)
	var incoming: Dictionary = _lm._incoming_by_index(model)
	for sec in model:
		var section_name := String(sec.get("name", ""))
		var label := section_name
		var notes := PackedStringArray()
		if bool(sec.get("is_entry", false)):
			label += "  ★"
			notes.append("★ the start state (playback begins here)")
		if bool(sec.get("is_idle_loop", false)):
			label += "  ↻"
			notes.append("↻ loops to itself while waiting for the game")
		if _lm._last_state == _lm.VM_RUNNING and section_name == String(_lm._current_section):
			label = "▶ " + label
			notes.append("▶ playing right now")
		var idx: int = _lm._states_list.add_item(label)
		_lm._states_list.set_item_metadata(idx, section_name)
		if _lm._section_is_unlinked_in_model(sec, incoming):
			_lm._states_list.set_item_custom_fg_color(idx, Color(0.66, 0.62, 0.52))
			notes.append(_lm.UNLINKED_STATE_TOOLTIP)
		var tip := "Open %s's program." % section_name
		if notes.size() > 0:
			tip += "\n" + "\n".join(notes)
		_lm._states_list.set_item_tooltip(idx, tip)
	_refresh_states_selection()


# Highlight the sidebar row for the current location without emitting
# item_selected (select() is signal-less).
func _refresh_states_selection() -> void:
	if _lm._states_list == null or _lm._nav == null:
		return
	var target: String = _lm._nav.current_section()  # "" = the map row
	for i in range(_lm._states_list.item_count):
		if String(_lm._states_list.get_item_metadata(i)) == target:
			_lm._states_list.select(i)
			return
	_lm._states_list.deselect_all()


func _on_states_item_selected(index: int) -> void:
	if _lm._states_list == null:
		return
	var section_name := String(_lm._states_list.get_item_metadata(index))
	if section_name == "":
		_back_to_map()
	else:
		_drill_into(section_name)


func _refresh_breadcrumb_action_state() -> void:
	var reason: String = _lm._authoring_blocked_reason()
	var blocked := reason != ""
	if _lm._breadcrumb_rename_btn != null:
		_lm._breadcrumb_rename_btn.disabled = blocked
		_lm._breadcrumb_rename_btn.tooltip_text = reason if blocked else "Rename this state."
	if _lm._breadcrumb_delete_btn != null:
		_lm._breadcrumb_delete_btn.disabled = blocked
		_lm._breadcrumb_delete_btn.tooltip_text = reason if blocked else "Delete this state (only if nothing else points at it)."


# Whether a section is a self-loop "idle" state, per the opcode-level section model
# (the AST dict doesn't carry it). Used to badge the drill-in breadcrumb.
func _section_is_idle_loop(section_name: String) -> bool:
	if _lm._document == null or not _lm._document.script_loaded():
		return false
	var ms = _lm._document.mus_script
	if not ms.has_method("get_section_model"):
		return false
	var sn := StringName(ms.get_default_script_name())
	for sec in ms.get_section_model(sn):
		if String(sec.get("name", "")) == section_name:
			return bool(sec.get("is_idle_loop", false))
	return false


func _section_is_unlinked(section_name: String) -> bool:
	if _lm._document == null or not _lm._document.script_loaded():
		return false
	var ms = _lm._document.mus_script
	if not ms.has_method("get_section_model"):
		return false
	var sn := StringName(ms.get_default_script_name())
	var model: Array = _lm.MusicSectionGraphClass.build(ms, sn)
	var incoming_by_index: Dictionary = _lm._incoming_by_index(model)
	for sec in model:
		if String(sec.get("name", "")) == section_name:
			return _lm._section_is_unlinked_in_model(sec, incoming_by_index)
	return false


# Build (or rebuild) the logic graph for `section_name` from the current AST,
# configuring its authoring context FIRST so the per-node tools + ＋Add palette
# match the script's current editability (can_author). Returns false if the
# section no longer exists (e.g. an edit/undo renamed or removed it).
func _populate_program_view(section_name: String) -> bool:
	if _lm._program_view == null or _lm._document == null or not _lm._document.script_loaded():
		return false
	var sn := StringName(_lm._document.mus_script.get_default_script_name())
	var ast: Array = _lm._document.mus_script.get_program_ast(sn)
	# Which states take caller inputs (hidden frame ops' locals counts): the
	# view's call rows show a "hands it values" chip for their targets.
	var inputs_by_section := {}
	for sec in ast:
		var total := 0
		for st in sec.get("statements", []):
			if String((st as Dictionary).get("kind", "")) == "frame_enter":
				total += int((st as Dictionary).get("locals_count", 0))
		if total > 0:
			inputs_by_section[String(sec.get("name", ""))] = total
	var profile_path := ""
	if _lm._document.has_method("get_var_profile_path"):
		profile_path = _lm._document.get_var_profile_path()
	for sec in ast:
		if String(sec.get("name", "")) == section_name:
			var editable: bool = _lm._document.has_method("can_author") and _lm._document.can_author()
			var section_names: PackedStringArray = _lm._document.mus_script.get_section_names(sn)
			_lm._program_view.configure_authoring(section_names, _lm._build_var_list(), _lm._document.mus_script,
				_lm._bank_names(), editable, _lm._authoring_blocked_reason(), profile_path, inputs_by_section)
			_lm._program_view.show_section(sec, _lm._bank_names())
			return true
	return false


# Navigate back to the map (a recorded hop, so forward can return).
func _back_to_map() -> void:
	if _lm._nav != null:
		_lm._nav.navigate_to(_lm.MusicNavClass.map_entry())


func _set_map_chrome_visible(v: bool) -> void:
	if _lm._map != null:
		_lm._map.visible = v
	if _lm._map_header != null:
		_lm._map_header.visible = v
	if _lm._map_toolbar != null:
		_lm._map_toolbar.visible = v
