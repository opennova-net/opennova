extends VBoxContainer

# Inline structured expression editor: the node-embedded replacement for the
# dialog-shaped expr_builder. A recursive row of operand cells -- each cell is
# a Number / Variable / This / function call / nested (A op B) expression /
# type-it escape -- backed by MusExpr node dicts and seeded straight from the
# C++ AST's rhs_tree/expr_tree, so an existing expression opens STRUCTURED (the
# old builder could only reopen as raw text). Always serializes through
# MusExpr.serialize, so the emitted text stays canonical + compilable; nesting
# is capped at MAX_DEPTH, past which a subtree opens as type-it text.
#
# Live-validated through the native compiler (the single grammar authority),
# debounced so typing in a raw field doesn't compile a probe per keystroke.

const MusExpr = preload("res://modtools/music/mus_expr.gd")
const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")

signal expr_changed(text: String)

# Deepest cell level that may still open a nested expression / function call.
const MAX_DEPTH := 4

var _var_list: Array = []   # [{token:String, label:String}]
var _mus = null             # NovaMusicScript for validation (may be null)
var _root: ExprCell = null
var _valid_label: Label
var _validate_timer: Timer
var _suppress := false


# One operand cell: [mode ▾] + the mode's controls. Recursive: the expression
# and function modes embed child cells at depth+1.
class ExprCell extends HBoxContainer:
	signal changed

	enum { M_NUM, M_VAR, M_ME, M_EXPR, M_CALL, M_UNOP, M_RAW }

	var _vars: Array
	var _mus = null
	var _depth: int
	var _mode: OptionButton
	var _num: SpinBox
	var _var: OptionButton
	var _raw: LineEdit
	# expression mode (built lazily)
	var _expr_box: HBoxContainer = null
	var _left: ExprCell = null
	var _op: OptionButton = null
	var _right: ExprCell = null
	# function mode (built lazily)
	var _call_box: HBoxContainer = null
	var _fn: OptionButton = null
	var _no_arg: CheckBox = null
	var _arg: ExprCell = null
	# unary mode (built lazily)
	var _unop_box: HBoxContainer = null
	var _unop: OptionButton = null
	var _operand: ExprCell = null

	func _init(vars: Array, mus, depth: int) -> void:
		_vars = vars
		_mus = mus
		_depth = depth
		add_theme_constant_override("separation", 3)
		_mode = OptionButton.new()
		_mode.add_item("Number", M_NUM)
		_mode.add_item("Variable", M_VAR)
		_mode.add_item("This (Me)", M_ME)
		if depth < MAX_DEPTH:
			_mode.add_item("Expression (A · B)", M_EXPR)
			_mode.add_item("Function…", M_CALL)
			_mode.add_item("Not / negate…", M_UNOP)
		_mode.add_item("Type it (advanced)", M_RAW)
		add_child(_mode)
		_num = SpinBox.new()
		_num.min_value = -2147483648
		_num.max_value = 2147483647
		_num.step = 1
		_num.custom_minimum_size = Vector2(86, 0)
		add_child(_num)
		_var = OptionButton.new()
		for v in _vars:
			_var.add_item(String(v.get("label", v.get("token", "Var00"))))
		if _vars.is_empty():
			_var.add_item("Var00")
		_var.visible = false
		add_child(_var)
		_raw = LineEdit.new()
		_raw.placeholder_text = "expression"
		_raw.custom_minimum_size = Vector2(110, 0)
		_raw.visible = false
		add_child(_raw)
		_mode.item_selected.connect(func(_i):
			_sync()
			changed.emit())
		_num.value_changed.connect(func(_v): changed.emit())
		_var.item_selected.connect(func(_i): changed.emit())
		_raw.text_changed.connect(func(_t): changed.emit())
		_sync()

	func _selected_mode() -> int:
		return _mode.get_item_id(_mode.selected) if _mode.selected >= 0 else M_NUM

	func _select_mode(m: int) -> void:
		for i in range(_mode.item_count):
			if _mode.get_item_id(i) == m:
				_mode.select(i)
				break
		_sync()

	# Build the (A op B) sub-row once, on demand.
	func _ensure_expr_box() -> void:
		if _expr_box != null:
			return
		_expr_box = HBoxContainer.new()
		_expr_box.add_theme_constant_override("separation", 3)
		_left = ExprCell.new(_vars, _mus, _depth + 1)
		_left.changed.connect(func(): changed.emit())
		_expr_box.add_child(_left)
		_op = OptionButton.new()
		for e in MusExpr.BINOPS:
			_op.add_item("%s  (%s)" % [String(e["op"]), String(e["label"])])
			_op.set_item_metadata(_op.item_count - 1, String(e["op"]))
		_op.item_selected.connect(func(_i): changed.emit())
		_expr_box.add_child(_op)
		_right = ExprCell.new(_vars, _mus, _depth + 1)
		_right.changed.connect(func(): changed.emit())
		_expr_box.add_child(_right)
		add_child(_expr_box)

	# Build the function-call sub-row once, on demand.
	func _ensure_call_box() -> void:
		if _call_box != null:
			return
		_call_box = HBoxContainer.new()
		_call_box.add_theme_constant_override("separation", 3)
		_fn = OptionButton.new()
		for it in MusExpr.INTRINSICS:
			var stored := String(it["name"])
			_fn.add_item("%s  (%s)" % [MusDisplayNames.intrinsic_label(stored), MusExpr.surface(stored)])
			_fn.set_item_metadata(_fn.item_count - 1, stored)
			_fn.get_popup().set_item_tooltip(_fn.item_count - 1, MusDisplayNames.intrinsic_tooltip(stored))
		_fn.item_selected.connect(func(_i): changed.emit())
		_call_box.add_child(_fn)
		_no_arg = CheckBox.new()
		_no_arg.text = "no value"
		_no_arg.tooltip_text = "Call the function with no value in the parentheses."
		_no_arg.toggled.connect(func(_b):
			_arg.visible = not _no_arg.button_pressed
			changed.emit())
		_call_box.add_child(_no_arg)
		_arg = ExprCell.new(_vars, _mus, _depth + 1)
		_arg.changed.connect(func(): changed.emit())
		_call_box.add_child(_arg)
		add_child(_call_box)

	# Build the unary sub-row (op picker + one operand cell) once, on demand.
	func _ensure_unop_box() -> void:
		if _unop_box != null:
			return
		_unop_box = HBoxContainer.new()
		_unop_box.add_theme_constant_override("separation", 3)
		_unop = OptionButton.new()
		for e in MusExpr.UNOPS:
			_unop.add_item("%s  (%s)" % [String(e["op"]), String(e["label"])])
			_unop.set_item_metadata(_unop.item_count - 1, String(e["op"]))
		_unop.item_selected.connect(func(_i): changed.emit())
		_unop_box.add_child(_unop)
		_operand = ExprCell.new(_vars, _mus, _depth + 1)
		_operand.changed.connect(func(): changed.emit())
		_unop_box.add_child(_operand)
		add_child(_unop_box)

	func _sync() -> void:
		var m := _selected_mode()
		_num.visible = (m == M_NUM)
		_var.visible = (m == M_VAR)
		_raw.visible = (m == M_RAW)
		if m == M_EXPR:
			_ensure_expr_box()
		if _expr_box != null:
			_expr_box.visible = (m == M_EXPR)
		if m == M_CALL:
			_ensure_call_box()
		if _call_box != null:
			_call_box.visible = (m == M_CALL)
		if m == M_UNOP:
			_ensure_unop_box()
		if _unop_box != null:
			_unop_box.visible = (m == M_UNOP)

	# The cell's MusExpr node dict (always serializable to compilable text).
	func get_dict() -> Dictionary:
		match _selected_mode():
			M_NUM:
				return MusExpr.literal(int(_num.value))
			M_VAR:
				var idx := _var.selected
				if idx >= 0 and idx < _vars.size():
					var token := String(_vars[idx].get("token", "Var00"))
					return MusExpr.varref("named", -1, token)
				return MusExpr.varref("named", -1, "Var00")
			M_ME:
				return MusExpr.me()
			M_EXPR:
				var op := "=="
				if _op != null and _op.selected >= 0:
					op = String(_op.get_item_metadata(_op.selected))
				return MusExpr.binop(op, _left.get_dict(), _right.get_dict())
			M_CALL:
				var stored := "GSV"
				if _fn != null and _fn.selected >= 0:
					stored = String(_fn.get_item_metadata(_fn.selected))
				var arg = null if (_no_arg != null and _no_arg.button_pressed) else _arg.get_dict()
				return MusExpr.call_node(stored, arg)
			M_UNOP:
				var uop := "!"
				if _unop != null and _unop.selected >= 0:
					uop = String(_unop.get_item_metadata(_unop.selected))
				return MusExpr.unop(uop, _operand.get_dict())
			M_RAW:
				var t := _raw.text.strip_edges()
				return MusExpr.raw(t if t != "" else "0")
		return MusExpr.literal(0)

	# Seed from a MusExpr node dict (the binding's tree shape). Anything this
	# cell can't mount structurally (unary ops, locals, too-deep nesting) opens
	# as type-it text -- still canonical, still compilable.
	func seed(node) -> void:
		if typeof(node) != TYPE_DICTIONARY:
			_seed_raw_text("0")
			return
		match int(node.get("kind", -1)):
			MusExpr.LITERAL:
				_select_mode(M_NUM)
				_num.value = int(node.get("value", 0))
			MusExpr.ME:
				_select_mode(M_ME)
			MusExpr.VARREF:
				var token := MusExpr.serialize(node)
				if not _seed_var_token(token):
					_seed_raw_text(token)
			MusExpr.BINOP:
				if _depth >= MAX_DEPTH:
					_seed_raw_text(MusExpr.serialize(node))
					return
				_select_mode(M_EXPR)
				var op := String(node.get("op", "=="))
				for i in range(_op.item_count):
					if String(_op.get_item_metadata(i)) == op:
						_op.select(i)
						break
				_left.seed(node.get("left", MusExpr.literal(0)))
				_right.seed(node.get("right", MusExpr.literal(0)))
			MusExpr.CALL:
				if _depth >= MAX_DEPTH:
					_seed_raw_text(MusExpr.serialize(node))
					return
				_select_mode(M_CALL)
				var stored := String(node.get("intrinsic", "GSV"))
				for i in range(_fn.item_count):
					if String(_fn.get_item_metadata(i)) == stored:
						_fn.select(i)
						break
				var arg = node.get("arg", null)
				if arg == null:
					_no_arg.button_pressed = true
					_arg.visible = false
				else:
					_no_arg.button_pressed = false
					_arg.visible = true
					_arg.seed(arg)
			MusExpr.UNOP:
				if _depth >= MAX_DEPTH:
					_seed_raw_text(MusExpr.serialize(node))
					return
				_select_mode(M_UNOP)
				var uop := String(node.get("op", "!"))
				for i in range(_unop.item_count):
					if String(_unop.get_item_metadata(i)) == uop:
						_unop.select(i)
						break
				_operand.seed(node.get("operand", MusExpr.literal(0)))
			_:
				# RAW / unknown: keep the exact canonical text.
				_seed_raw_text(MusExpr.serialize(node))

	func _seed_var_token(token: String) -> bool:
		for i in range(_vars.size()):
			if String(_vars[i].get("token", "")) == token:
				_select_mode(M_VAR)
				_var.select(i)
				return true
		return false

	func _seed_raw_text(text: String) -> void:
		_select_mode(M_RAW)
		_raw.text = text


func setup(var_list: Array, mus = null) -> void:
	_var_list = var_list
	_mus = mus
	if _root == null:
		_build()


func _build() -> void:
	add_theme_constant_override("separation", 3)
	_root = ExprCell.new(_var_list, _mus, 0)
	_root.changed.connect(_on_changed)
	add_child(_root)
	_valid_label = Label.new()
	_valid_label.add_theme_color_override("font_color", Color(0.6, 0.9, 0.6))
	add_child(_valid_label)
	_validate_timer = Timer.new()
	_validate_timer.one_shot = true
	_validate_timer.wait_time = 0.3
	_validate_timer.timeout.connect(_run_validation)
	add_child(_validate_timer)


# Seed from a structured tree dict (the AST's rhs_tree / expr_tree).
func set_expr(tree: Dictionary) -> void:
	if _root == null:
		_build()
	_suppress = true
	_root.seed(tree)
	_suppress = false
	_on_changed()


# Seed from canonical text only (no tree available): opens as type-it.
func set_expression_text(text: String) -> void:
	set_expr(MusExpr.raw(text))


func get_expr_dict() -> Dictionary:
	return _root.get_dict() if _root != null else MusExpr.literal(0)


func get_expr_text() -> String:
	return MusExpr.serialize(get_expr_dict())


func is_valid() -> bool:
	return bool(MusExpr.validate_expr(get_expr_text(), _mus).get("ok", true))


func _on_changed() -> void:
	if _suppress:
		return
	expr_changed.emit(get_expr_text())
	# Text + signal stay live; only the compiler-probe ✓/✗ label is debounced.
	if _validate_timer != null and is_inside_tree():
		_validate_timer.start()
	else:
		_run_validation()


func _run_validation() -> void:
	if _valid_label == null:
		return
	var text := get_expr_text()
	var v: Dictionary = MusExpr.validate_expr(text, _mus)
	if bool(v.get("ok", true)):
		_valid_label.text = "✓ %s" % MusDisplayNames.pretty_expr(text, _var_list)
		_valid_label.add_theme_color_override("font_color", Color(0.6, 0.9, 0.6))
	else:
		_valid_label.text = "✗ %s" % String(v.get("err", "invalid"))
		_valid_label.add_theme_color_override("font_color", Color(1.0, 0.5, 0.5))
