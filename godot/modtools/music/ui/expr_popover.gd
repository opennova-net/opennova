class_name MusicExprPopover
extends PanelContainer

# The anchored expression editor for the block-stack canvas: clicking a
# condition / value chip on a row opens THIS, floating next to the row with
# real space -- instead of cramming recursive picker-cells into the row
# itself. Two tabs:
#   Build    the structured ExprRow (mode cells seeded from the C++ expression
#            tree; its own live ✓/✗ + friendly-sentence preview)
#   Type it  the canonical text, for power users / too-deep trees
# Apply hands the active tab's canonical text to the opener's callback (which
# regenerates the statement against the CURRENT AST); Cancel and ✕ close
# quietly. One popover serves the whole view (opening elsewhere re-targets it).
#
# The popover is a child of the VIEW, not of the rows: a document re-render
# rebuilds every row but leaves an open popover's editing state intact -- the
# view re-anchors it to the surviving row (by ordinal/branch key) or closes it
# with a notice when the statement is gone.

const MusExpr = preload("res://modtools/music/mus_expr.gd")
const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")
const ExprRowClass = preload("res://modtools/music/ui/expr_row.gd")

signal applied(text: String)
signal dismissed

var _title: Label = null
var _tabs: TabContainer = null
var _expr_row = null          # ExprRow (Build tab)
var _raw: LineEdit = null     # Type it tab
var _raw_status: Label = null
var _apply: Button = null
var _mus = null
var _key: Dictionary = {}
var _anchor: Control = null


func _ready() -> void:
	visible = false
	z_index = 10
	custom_minimum_size = Vector2(340, 0)
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.10, 0.11, 0.14)
	sb.border_color = Color(0.45, 0.55, 0.75)
	sb.set_border_width_all(1)
	sb.content_margin_left = 10.0
	sb.content_margin_right = 10.0
	sb.content_margin_top = 8.0
	sb.content_margin_bottom = 8.0
	sb.shadow_size = 8
	sb.shadow_color = Color(0, 0, 0, 0.45)
	add_theme_stylebox_override("panel", sb)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	add_child(box)

	var head := HBoxContainer.new()
	box.add_child(head)
	_title = Label.new()
	_title.text = "Edit value"
	_title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_title.add_theme_color_override("font_color", Color(0.75, 0.82, 0.95))
	head.add_child(_title)
	var close := Button.new()
	close.text = "✕"
	close.flat = true
	close.focus_mode = Control.FOCUS_NONE
	close.tooltip_text = "Close without applying"
	close.pressed.connect(cancel)
	head.add_child(close)

	_tabs = TabContainer.new()
	_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(_tabs)

	_reset_build_tab([], null)

	var raw_tab := VBoxContainer.new()
	raw_tab.name = "Type it"
	raw_tab.add_theme_constant_override("separation", 4)
	_tabs.add_child(raw_tab)
	_raw = LineEdit.new()
	_raw.placeholder_text = "canonical expression, e.g. (Var01 != 0)"
	_raw.text_changed.connect(func(_t): _refresh_apply())
	raw_tab.add_child(_raw)
	_raw_status = Label.new()
	_raw_status.add_theme_font_size_override("font_size", 11)
	raw_tab.add_child(_raw_status)

	_tabs.tab_changed.connect(func(_i): _refresh_apply())

	var buttons := HBoxContainer.new()
	buttons.alignment = BoxContainer.ALIGNMENT_END
	buttons.add_theme_constant_override("separation", 6)
	box.add_child(buttons)
	var cancel_btn := Button.new()
	cancel_btn.text = "Cancel"
	cancel_btn.focus_mode = Control.FOCUS_NONE
	cancel_btn.pressed.connect(cancel)
	buttons.add_child(cancel_btn)
	_apply = Button.new()
	_apply.text = "✓ Apply"
	_apply.pressed.connect(_on_apply)
	buttons.add_child(_apply)


# Open (or re-target) the popover for one value. `seed` is the statement's
# expression TREE dict when the C++ reconstruction succeeded, else the
# canonical text. `key` identifies the edited slot for re-resolution after a
# re-render ({ordinal, kind, slot, branch_key?}).
func open_for(anchor: Control, title: String, seed, var_list: Array, mus, key: Dictionary) -> void:
	_mus = mus
	_key = key
	_anchor = anchor
	_title.text = title
	# Rebuild the Build tab per open: ExprRow's cells capture the variable
	# list at construction, and the list differs per section (caller inputs).
	_reset_build_tab(var_list, mus)
	if seed is Dictionary and not (seed as Dictionary).is_empty():
		_expr_row.set_expr(seed)
	else:
		_expr_row.set_expression_text(String(seed))
	_raw.text = _expr_row.get_expr_text()
	_tabs.current_tab = 0
	visible = true
	_refresh_apply()
	reposition()


func _reset_build_tab(var_list: Array, mus) -> void:
	if _expr_row != null:
		_tabs.remove_child(_expr_row)
		_expr_row.queue_free()
	_expr_row = ExprRowClass.new()
	_expr_row.name = "Build"
	_expr_row.setup(var_list, mus)
	_tabs.add_child(_expr_row)
	_tabs.move_child(_expr_row, 0)
	_expr_row.expr_changed.connect(func(_t): _refresh_apply())


func is_open() -> bool:
	return visible


func key() -> Dictionary:
	return _key


# Re-attach to a (rebuilt) row after a document re-render; editing state stays.
func reanchor(anchor: Control) -> void:
	_anchor = anchor
	reposition()


# Place under the anchor row, clamped into the parent view.
func reposition() -> void:
	if _anchor == null or not is_instance_valid(_anchor) or get_parent() == null:
		return
	var parent_ctrl := get_parent() as Control
	if parent_ctrl == null:
		return
	reset_size()
	var local := _anchor.global_position - parent_ctrl.global_position
	var pos := local + Vector2(24, _anchor.size.y + 2)
	pos.x = clampf(pos.x, 0.0, maxf(0.0, parent_ctrl.size.x - size.x))
	pos.y = clampf(pos.y, 0.0, maxf(0.0, parent_ctrl.size.y - size.y))
	position = pos


# The active tab's canonical text.
func current_text() -> String:
	if _tabs.current_tab == 1:
		return _raw.text.strip_edges()
	return _expr_row.get_expr_text()


func _current_valid() -> bool:
	var text := current_text()
	if text == "":
		return false
	return bool(MusExpr.validate_expr(text, _mus).get("ok", true))


func _refresh_apply() -> void:
	if _apply == null:
		return
	var ok := _current_valid()
	_apply.disabled = not ok
	_apply.tooltip_text = "" if ok else "Fix the expression first"
	if _raw_status != null:
		var text := _raw.text.strip_edges()
		if _tabs.current_tab == 1 and text != "":
			var v: Dictionary = MusExpr.validate_expr(text, _mus)
			if bool(v.get("ok", true)):
				_raw_status.text = "✓ %s" % MusDisplayNames.pretty_expr(text)
				_raw_status.add_theme_color_override("font_color", Color(0.6, 0.9, 0.6))
			else:
				_raw_status.text = "✗ %s" % String(v.get("err", "invalid"))
				_raw_status.add_theme_color_override("font_color", Color(1.0, 0.5, 0.5))
		else:
			_raw_status.text = ""


func _on_apply() -> void:
	if not _current_valid():
		return
	var text := current_text()
	visible = false
	_key = {}
	_anchor = null
	applied.emit(text)


# Quiet close (Cancel, ✕, takeover, or the view resolving a lost anchor).
func cancel() -> void:
	if not visible:
		return
	visible = false
	_key = {}
	_anchor = null
	dismissed.emit()
