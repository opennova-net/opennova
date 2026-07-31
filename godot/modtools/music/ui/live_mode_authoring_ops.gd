extends "res://modtools/music/ui/live_mode_section.gd"

# MusicLiveMode's authoring operations (W4-6a): the Phase 2 authoring
# intent handlers, Add State + the empty-state welcome installer, the
# level-2 program-view drill-in installer, and the state-level rename /
# delete operations with their dialogs and context menu. Verbatim motion
# from live_mode.gd; state stays on the mount, reached through `_lm`.


# --- Phase 2 authoring intent handlers (route to the document, keep pinned) ---

func _on_inspector_add_statement(section_index: int, lines: PackedStringArray) -> void:
	if _lm._document == null or not _lm._document.has_method("insert_statement"):
		return
	_lm._follow_live = false
	if not _lm._document.insert_statement(section_index, lines):
		_lm._flash_start_warning("Couldn't add that here")


func _on_inspector_replace_statement(section_index: int, ordinal: int, lines: PackedStringArray) -> void:
	if _lm._document == null or not _lm._document.has_method("replace_statement"):
		return
	_lm._follow_live = false
	if not _lm._document.replace_statement(section_index, ordinal, lines):
		_lm._flash_start_warning("Couldn't apply that edit")


func _on_inspector_delete_statement(section_index: int, ordinal: int) -> void:
	if _lm._document == null or not _lm._document.has_method("delete_statement"):
		return
	_lm._follow_live = false
	if not _lm._document.delete_statement(section_index, ordinal):
		_lm._flash_start_warning("Couldn't delete that")


func _on_inspector_reorder_statement(section_index: int, ordinal: int, direction: int) -> void:
	if _lm._document == null or not _lm._document.has_method("reorder_statement"):
		return
	_lm._follow_live = false
	if not _lm._document.reorder_statement(section_index, ordinal, direction):
		_lm._flash_start_warning("Can't move it further")


func _on_program_insert_at(section_index: int, before_ordinal: int, lines: PackedStringArray) -> void:
	if _lm._document == null or not _lm._document.has_method("insert_statement_at"):
		return
	_lm._follow_live = false
	if not _lm._document.insert_statement_at(section_index, before_ordinal, lines):
		_lm._flash_start_warning("Couldn't add it there")


func _on_program_move(section_index: int, ordinal: int, before_ordinal: int) -> void:
	if _lm._document == null or not _lm._document.has_method("move_statement"):
		return
	_lm._follow_live = false
	if not _lm._document.move_statement(section_index, ordinal, before_ordinal):
		_lm._flash_start_warning("Can't move it there")


func _on_program_run_count(section_index: int, start_ordinal: int, old_count: int, new_count: int) -> void:
	if _lm._document == null or not _lm._document.has_method("set_run_count"):
		return
	_lm._follow_live = false
	if not _lm._document.set_run_count(section_index, start_ordinal, old_count, new_count):
		_lm._flash_start_warning("Couldn't resize that run")


# A caller-input label changed on the Inputs card: persist it in the profile
# sidecar (display-only; the script keeps its l_N tokens) and rebuild the
# surfaces that cached the old name.
func _on_input_renamed(section_name: String, input_index: int, label: String) -> void:
	if _lm._document == null or not _lm._document.script_loaded():
		return
	var profile_path := ""
	if _lm._document.has_method("get_var_profile_path"):
		profile_path = _lm._document.get_var_profile_path()
	if profile_path == "":
		return
	var sname := String(_lm._document.mus_script.get_default_script_name())
	if _lm.MusInputNames.set_input_label(profile_path, sname, section_name, input_index, label) == OK:
		_lm._log_typed(_lm.EvType.SYSTEM, "input renamed: %s/%d -> %s" % [section_name, input_index + 1, label])
		if _lm._program_view != null and _lm._program_view.visible and _lm._logic_section_name != "":
			_lm._populate_program_view(_lm._logic_section_name)


# Light the drilled-in program-view statement the VM pc is on, but only while the graph
# shows the section that is actually running -- the pc is a global bytecode offset, so
# another section's nodes would mis-bracket it.
func _update_live_highlight(state: int) -> void:
	if _lm._program_view != null and _lm._program_view.visible:
		if state == _lm.VM_RUNNING and _lm._logic_section_name == String(_lm._current_section):
			_lm._program_view.set_active_offset(_lm._director.current_pc())
			if _lm._follow_live and _lm._program_view.has_method("scroll_to_active"):
				_lm._program_view.scroll_to_active()
		else:
			_lm._program_view.set_active_offset(-1)


# --- Add State (visual-first authoring slice) --------------------------

# Mount the "＋ Add State" affordances: one on the map toolbar (hidden while a
# program is open) and one pinned under the always-visible States sidebar --
# a from-scratch project drops the user straight into Begin's program, where
# the map toolbar is hidden, and "make a second state" must never require
# knowing to navigate back first.
func _install_add_state_button() -> void:
	var col: Node = _lm.get_node_or_null("%CenterCol")
	if col == null or _lm._canvas == null:
		return
	var toolbar := HBoxContainer.new()
	toolbar.name = "MapToolbar"
	_lm._add_state_btn = Button.new()
	_lm._add_state_btn.text = "＋ Add State"
	_lm._add_state_btn.tooltip_text = _lm.ADD_STATE_TOOLTIP
	_lm._add_state_btn.pressed.connect(_lm._on_add_state)
	toolbar.add_child(_lm._add_state_btn)
	col.add_child(toolbar)
	col.move_child(toolbar, _lm._canvas.get_index())
	_lm._map_toolbar = toolbar
	_lm._map_header = col.get_node_or_null("MapHeader")
	if _lm._states_list != null and _lm._states_list.get_parent() != null:
		_lm._sidebar_add_state_btn = Button.new()
		_lm._sidebar_add_state_btn.name = "SidebarAddState"
		_lm._sidebar_add_state_btn.text = "＋ Add state"
		_lm._sidebar_add_state_btn.tooltip_text = _lm.ADD_STATE_TOOLTIP
		_lm._sidebar_add_state_btn.focus_mode = Control.FOCUS_NONE
		_lm._sidebar_add_state_btn.pressed.connect(_lm._on_add_state)
		_lm._states_list.get_parent().add_child(_lm._sidebar_add_state_btn)


# Centered welcome shown when nothing is loaded: a primary "create from scratch"
# button + a hint to Open. Without it, New produced a blank canvas with a disabled
# "＋ Add State" and the message "Open a project first" -- and no way to create one.
# Mounted into the map's column; _refresh_empty_state toggles it vs the map chrome.
func _install_empty_state() -> void:
	var col: Node = _lm.get_node_or_null("%CenterCol")
	if col == null:
		return
	var center := CenterContainer.new()
	center.name = "EmptyState"
	center.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	center.size_flags_vertical = Control.SIZE_EXPAND_FILL
	center.visible = false
	var box := VBoxContainer.new()
	box.alignment = BoxContainer.ALIGNMENT_CENTER
	box.add_theme_constant_override("separation", 12)
	center.add_child(box)
	var title := Label.new()
	title.text = "No music project open"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 18)
	box.add_child(title)
	var new_btn := Button.new()
	new_btn.text = "＋ New music program"
	new_btn.tooltip_text = "Create a music program from scratch: a start state plus an empty sound bank. Import tracks in the Tracks dock, then wire up the states."
	new_btn.focus_mode = Control.FOCUS_NONE
	new_btn.pressed.connect(_lm._on_new_project_pressed)
	box.add_child(new_btn)
	var hint := Label.new()
	hint.text = "or use Open to load an existing .sbf / .bin"
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hint.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	box.add_child(hint)
	col.add_child(center)
	_lm._empty_state = center


# Show the welcome prompt iff no script is loaded (hiding the map chrome behind
# it); otherwise hide it and -- when on the map, not drilled into a state's program --
# restore the map chrome (the program view owns chrome visibility on its own).
func _refresh_empty_state() -> void:
	var empty: bool = _lm._document == null or not _lm._document.script_loaded()
	if _lm._empty_state != null:
		_lm._empty_state.visible = empty
	if empty:
		if _lm._canvas != null:
			_lm._canvas.visible = false
		if _lm._map_toolbar != null:
			_lm._map_toolbar.visible = false
		if _lm._map_header != null:
			_lm._map_header.visible = false
		if _lm._breadcrumb != null:
			_lm._breadcrumb.visible = false
	else:
		if _lm._canvas != null:
			_lm._canvas.visible = true
		if _lm._breadcrumb != null:
			_lm._breadcrumb.visible = true
		if _lm._logic_section_name == "":
			_lm._set_map_chrome_visible(true)


func _on_new_project_pressed() -> void:
	if _lm._document == null or not _lm._document.has_method("new_project"):
		return
	# Emits `changed` -> _on_document_changed -> _refresh_map, which hides this
	# prompt and renders the new start state; then open that start state's
	# program so first-time authors land on the add-step hint immediately.
	var rc: int = _lm._document.new_project()
	if rc != OK:
		_lm._flash_start_warning("Could not create music project")
		return
	_lm._follow_live = false
	_lm._drill_into("Begin")


# Why structured authoring is currently off, or "" when available. Prefers the
# document's specific reason (esp. the multi-chunk read-only case) over a generic
# "must compile" so the user understands a greyed-out edit instead of guessing.
func _authoring_blocked_reason() -> String:
	if _lm._document == null or not _lm._document.script_loaded():
		return "Open a project first"
	if _lm._document.has_method("authoring_blocked_reason"):
		return String(_lm._document.authoring_blocked_reason())
	if not (_lm._document.has_method("can_author") and _lm._document.can_author()):
		return "Script must compile first"
	return ""


func _on_add_state() -> void:
	if _lm._document == null or not _lm._document.script_loaded():
		_lm._flash_start_warning("Open a project first")
		return
	var reason: String = _authoring_blocked_reason()
	if reason != "":
		_lm._flash_start_warning(reason)
		return
	if not _lm._document.has_method("add_section"):
		return
	var new_name := _unique_state_name()
	if not _lm._document.add_section(StringName(new_name)):
		_lm._flash_start_warning("Could not add state")
		return
	_lm._log_typed(_lm.EvType.SYSTEM, "added state %s" % new_name)
	# add_section emitted `changed` -> the map already rebuilt; drill straight into
	# the new state's program so the user can start authoring it.
	_lm._follow_live = false
	_lm._drill_into(new_name)


# Lowest free "State_N" so a fresh state never collides with an existing section.
func _unique_state_name() -> String:
	var existing: Dictionary = {}
	if _lm._document != null and _lm._document.script_loaded():
		var sname := StringName(_lm._document.mus_script.get_default_script_name())
		for s in _lm._document.mus_script.get_section_names(sname):
			existing[String(s)] = true
	var n := 1
	while existing.has("State_%d" % n):
		n += 1
	return "State_%d" % n


# --- Level-2 drill-in: the state's program view ------------------------

# Mount the navigation bar (back/forward + breadcrumb trail + state actions)
# and the section logic graph. The map and the logic graph share the canvas
# stack; only one is visible at a time. The nav bar stays up whenever a script
# is loaded -- on the map it just reads "Map" -- so back/forward always work.
func _install_program_view() -> void:
	var col: Node = _lm.get_node_or_null("%CenterCol")
	var stack: Node = _lm.get_node_or_null("%CanvasStack")
	if col == null or stack == null:
		return
	_lm._breadcrumb = HBoxContainer.new()
	_lm._breadcrumb.name = "LogicBreadcrumb"
	_lm._nav_back_btn = Button.new()
	_lm._nav_back_btn.text = "◀"
	_lm._nav_back_btn.tooltip_text = "Back (Alt+Left)."
	_lm._nav_back_btn.focus_mode = Control.FOCUS_NONE
	_lm._nav_back_btn.disabled = true
	_lm._nav_back_btn.pressed.connect(func(): _lm._nav.go_back())
	_lm._breadcrumb.add_child(_lm._nav_back_btn)
	_lm._nav_fwd_btn = Button.new()
	_lm._nav_fwd_btn.text = "▶"
	_lm._nav_fwd_btn.tooltip_text = "Forward (Alt+Right)."
	_lm._nav_fwd_btn.focus_mode = Control.FOCUS_NONE
	_lm._nav_fwd_btn.disabled = true
	_lm._nav_fwd_btn.pressed.connect(func(): _lm._nav.go_forward())
	_lm._breadcrumb.add_child(_lm._nav_fwd_btn)
	_lm._crumb_segments = HBoxContainer.new()
	_lm._crumb_segments.name = "CrumbTrail"
	_lm._crumb_segments.add_theme_constant_override("separation", 2)
	_lm._breadcrumb.add_child(_lm._crumb_segments)
	# Status badges for the open state (loops to itself / unlinked), not a title:
	# the trail's last segment already names where you are.
	_lm._breadcrumb_label = Label.new()
	_lm._breadcrumb_label.add_theme_color_override("font_color", Color(0.7, 0.74, 0.82))
	_lm._breadcrumb_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_lm._breadcrumb.add_child(_lm._breadcrumb_label)
	# Rename / delete THIS state -- the state-level operations the right inspector
	# used to own, now on the program view's breadcrumb. They act on the drilled-in
	# section and reuse the document's rename_section / delete_section.
	_lm._breadcrumb_rename_btn = Button.new()
	_lm._breadcrumb_rename_btn.text = "✎ Rename"
	_lm._breadcrumb_rename_btn.tooltip_text = "Rename this state."
	_lm._breadcrumb_rename_btn.focus_mode = Control.FOCUS_NONE
	_lm._breadcrumb_rename_btn.pressed.connect(_lm._on_breadcrumb_rename)
	_lm._breadcrumb.add_child(_lm._breadcrumb_rename_btn)
	_lm._breadcrumb_delete_btn = Button.new()
	_lm._breadcrumb_delete_btn.text = "✕ Delete"
	_lm._breadcrumb_delete_btn.tooltip_text = "Delete this state (only if nothing else points at it)."
	_lm._breadcrumb_delete_btn.focus_mode = Control.FOCUS_NONE
	_lm._breadcrumb_delete_btn.pressed.connect(_lm._on_breadcrumb_delete)
	_lm._breadcrumb.add_child(_lm._breadcrumb_delete_btn)
	# Follow-live toggle: when on, a VM transition re-drills the program view into the
	# entered state. A manual node click pins (turns this off); flipping it back on
	# resumes following -- the explicit, visible control the auto-pin behaviour lacked.
	_lm._follow_btn = CheckButton.new()
	_lm._follow_btn.text = "Follow live"
	_lm._follow_btn.tooltip_text = "Follow the VM into each state it enters. Clicking a state pins the view (turns this off); re-enable to resume following."
	_lm._follow_btn.focus_mode = Control.FOCUS_NONE
	_lm._follow_btn.button_pressed = _lm._follow_live
	_lm._follow_btn.toggled.connect(_lm._on_follow_toggled)
	_lm._breadcrumb.add_child(_lm._follow_btn)
	_lm._breadcrumb.visible = false
	col.add_child(_lm._breadcrumb)
	col.move_child(_lm._breadcrumb, 0)
	_lm._rebuild_breadcrumb()

	_lm._program_view = _lm.MusicSectionProgramViewClass.new()
	_lm._program_view.name = "ProgramView"
	_lm._program_view.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_lm._program_view.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_lm._program_view.visible = false
	_lm._program_view.open_section_requested.connect(func(n): _lm._drill_into(String(n)))
	# The program view's authoring intents route to the document's parity-gated,
	# undoable write path (these handlers keep their _on_inspector_* names from
	# when the inspector shared them; the view is the sole emitter now).
	_lm._program_view.add_statement_requested.connect(_lm._on_inspector_add_statement)
	_lm._program_view.replace_statement_requested.connect(_lm._on_inspector_replace_statement)
	_lm._program_view.delete_statement_requested.connect(_lm._on_inspector_delete_statement)
	_lm._program_view.reorder_statement_requested.connect(_lm._on_inspector_reorder_statement)
	_lm._program_view.insert_statement_at_requested.connect(_lm._on_program_insert_at)
	_lm._program_view.move_statement_requested.connect(_lm._on_program_move)
	_lm._program_view.set_run_count_requested.connect(_lm._on_program_run_count)
	_lm._program_view.add_play_requested.connect(_lm._on_inspector_add_play)
	_lm._program_view.input_renamed.connect(_lm._on_input_renamed)
	_lm._program_view.author_failed.connect(func(msg: String): _lm._flash_start_warning(msg))
	# An open picker/popover must not be yanked away by a VM transition
	# re-drilling the canvas: pin follow-live (re-enable resumes following).
	_lm._program_view.inline_edit_started.connect(func(): _lm._follow_live = false)
	stack.add_child(_lm._program_view)


# --- State-level operations (rename / delete), re-homed from the right inspector ---

func _on_breadcrumb_rename() -> void:
	var reason := _authoring_blocked_reason()
	if reason != "":
		_lm._flash_start_warning(reason)
		return
	if _lm._logic_section_name != "":
		_open_rename_dialog(_lm._logic_section_name)


func _on_breadcrumb_delete() -> void:
	var reason := _authoring_blocked_reason()
	if reason != "":
		_lm._flash_start_warning(reason)
		return
	if _lm._logic_section_name != "":
		_open_delete_section_dialog(_lm._logic_section_name)


func _on_follow_toggled(pressed: bool) -> void:
	_lm._follow_live = pressed
	# Re-enabling while the VM is running snaps the program view to the live state now,
	# rather than waiting for the next transition.
	if pressed and _lm._last_state == _lm.VM_RUNNING and String(_lm._current_section) != "":
		_lm._drill_into(String(_lm._current_section), false)


# Right-click a state on the map: open its program, rename it, or delete it.
# Rename + delete need an editable (single-chunk, compiling) script.
func _show_state_context_menu(section_name: String, global_pos: Vector2) -> void:
	var pop := PopupMenu.new()
	pop.add_item("Open program", 0)
	pop.add_item("Rename state…", 1)
	pop.add_item("Delete state", 2)
	var can: bool = _lm._document != null and _lm._document.has_method("can_author") and _lm._document.can_author()
	pop.set_item_disabled(1, not can)
	pop.set_item_disabled(2, not can)
	if not can:
		# Explain the greyed-out items (esp. the multi-chunk read-only case).
		var reason := _authoring_blocked_reason()
		pop.set_item_tooltip(1, reason)
		pop.set_item_tooltip(2, reason)
	_lm.add_child(pop)
	pop.id_pressed.connect(_lm._on_state_menu_id.bind(section_name))
	pop.popup_hide.connect(pop.queue_free)
	pop.position = Vector2i(global_pos)
	pop.reset_size()
	pop.popup()


func _on_state_menu_id(id: int, section_name: String) -> void:
	match id:
		0: _lm._drill_into(section_name)
		1: _open_rename_dialog(section_name)
		2: _open_delete_section_dialog(section_name)


func _open_rename_dialog(section_name: String) -> void:
	var dlg := ConfirmationDialog.new()
	dlg.title = "Rename state"
	dlg.min_size = Vector2i(340, 120)
	var box := VBoxContainer.new()
	dlg.add_child(box)
	var lbl := Label.new()
	lbl.text = "New name for '%s':" % section_name
	box.add_child(lbl)
	var edit := LineEdit.new()
	edit.name = "StateNameEdit"
	edit.text = section_name
	box.add_child(edit)
	var hint := Label.new()
	hint.name = "StateNameValidationHint"
	hint.add_theme_color_override("font_color", _lm.COLOR_ERROR)
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(hint)
	_lm.add_child(dlg)
	var ok := dlg.get_ok_button()
	ok.text = "Rename"
	var refresh := func(_text):
		var reason := _section_name_validation_reason(edit.text, section_name)
		hint.text = reason
		hint.tooltip_text = reason
		ok.disabled = reason != ""
		ok.tooltip_text = reason if reason != "" else "Rename state."
	edit.text_changed.connect(refresh)
	dlg.confirmed.connect(func():
		_do_rename_section(section_name, edit.text.strip_edges())
		dlg.queue_free())
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	dlg.popup_centered()
	refresh.call(edit.text)
	edit.select_all()
	edit.grab_focus()


func _section_name_validation_reason(candidate: String, current_name: String = "") -> String:
	var name := candidate.strip_edges()
	if _lm._document != null and _lm._document.has_method("validate_section_name"):
		return String(_lm._document.validate_section_name(StringName(name), StringName(current_name)))
	if current_name != "" and name == current_name:
		return "Type a different state name."
	if name == "":
		return "Enter a state name."
	return ""


func _do_rename_section(old_name: String, new_name: String) -> void:
	if new_name == "" or new_name == old_name:
		return
	if _lm._document == null or not _lm._document.has_method("rename_section"):
		return
	_lm._follow_live = false
	# If we're drilled into this state, re-point the shown-section name BEFORE the
	# rename so the post-change refresh re-populates the program view under it
	# (instead of failing to find the old name and bouncing back to the map).
	var was_drilled: bool = _lm._logic_section_name == old_name
	if was_drilled:
		_lm._logic_section_name = new_name
	if _lm._document.rename_section(StringName(old_name), StringName(new_name)):
		# Rewrite the trail only after the document accepted the rename, so a
		# rejected rename can't corrupt history entries of an unrelated state
		# that already carries the requested name.
		_lm._nav.rename_section(old_name, new_name)
		# Caller-input labels are keyed by section name in the profile sidecar;
		# carry them across the rename.
		if _lm._document.has_method("get_var_profile_path"):
			var profile_path: String = _lm._document.get_var_profile_path()
			if profile_path != "":
				_lm.MusInputNames.rename_section(profile_path,
					String(_lm._document.mus_script.get_default_script_name()), old_name, new_name)
		_lm._rebuild_breadcrumb()
		_lm._refresh_states_list()
		_lm._log_typed(_lm.EvType.SYSTEM, "renamed %s -> %s" % [old_name, new_name])
	else:
		if was_drilled:
			_lm._logic_section_name = old_name
		_lm._flash_start_warning("Rename rejected (name taken/invalid)")


func _open_delete_section_dialog(section_name: String) -> void:
	var dlg := ConfirmationDialog.new()
	dlg.title = "Delete state"
	dlg.dialog_text = "Delete state '%s'?\nStates that other states point at can't be deleted until those links are retargeted." % section_name
	_lm.add_child(dlg)
	dlg.confirmed.connect(func():
		_do_delete_section(section_name)
		dlg.queue_free())
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	dlg.popup_centered()


func _do_delete_section(section_name: String) -> void:
	if _lm._document == null or not _lm._document.has_method("delete_section"):
		return
	_lm._follow_live = false
	# If we're deleting the drilled-in state, drop back to the map first so the
	# post-change refresh doesn't try to re-populate a now-gone section.
	if _lm._logic_section_name == section_name:
		_lm._back_to_map()
	if _lm._document.delete_section(StringName(section_name)):
		# Scrub the dead state from the trail so back/forward can't revisit it.
		_lm._nav.remove_section(section_name)
		_lm._rebuild_breadcrumb()
		_lm._log_typed(_lm.EvType.SYSTEM, "deleted state %s" % section_name)
	else:
		_lm._flash_start_warning("Can't delete: state is still referenced")
