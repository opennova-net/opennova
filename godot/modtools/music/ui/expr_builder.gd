extends VBoxContainer

# Compact structured expression builder for the Phase-2 authoring popups (assign
# RHS, if/switch condition, method-call arg). It builds the common MUS expression
# shapes from dropdowns + spinboxes -- a single value, a binary "(A op B)", a
# function call, or a typed escape -- and always emits the canonical names-less
# text via MusExpr.serialize (so it stays byte-stable + compilable). Operands are
# leaves (number / variable / Me / type-it); for anything deeper the "Type it"
# form is the per-expression last resort. Live-validated through the native
# compiler so a bad expression is caught before it reaches the file gate.

const MusExpr = preload("res://modtools/music/mus_expr.gd")
const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")

signal expression_changed(text: String)

enum { F_VALUE, F_COMPARE, F_MATH, F_LOGIC, F_CALL, F_RAW }

var _var_list: Array = []     # [{token:String, label:String}]
var _mus = null               # NovaMusicScript for validation (may be null)
var _suppress := false

var _form_opt: OptionButton
var _body: VBoxContainer
var _valid: Label
# Debounce the compiler-backed validation so fast typing in a raw field doesn't
# compile a probe on every keystroke. The emitted text + get_expression_text() stay
# live; only the ✓/✗ label waits for the typing to settle.
var _validate_timer: Timer

# rebuilt per form
var _leaf_a: HBoxContainer
var _leaf_b: HBoxContainer
var _op_opt: OptionButton
var _intrinsic_opt: OptionButton
var _arg_leaf: HBoxContainer
var _no_arg: CheckBox
var _raw_edit: LineEdit


# A leaf operand: [mode ▾] + (number | variable | nothing | type-it). Tracks its
# own state and exposes get_text(). Built as an inner class so the builder can
# instance several without scene files.
class Leaf extends HBoxContainer:
	signal changed
	enum { L_NUM, L_VAR, L_ME, L_RAW }
	var _mode: OptionButton
	var _num: SpinBox
	var _var: OptionButton
	var _raw: LineEdit
	var _vars: Array

	func _init(vars: Array) -> void:
		_vars = vars
		add_theme_constant_override("separation", 4)
		_mode = OptionButton.new()
		_mode.add_item("Number", L_NUM)
		_mode.add_item("Variable", L_VAR)
		_mode.add_item("This (Me)", L_ME)
		_mode.add_item("Type…", L_RAW)
		add_child(_mode)
		_num = SpinBox.new()
		_num.min_value = -2147483648
		_num.max_value = 2147483647
		_num.step = 1
		_num.custom_minimum_size = Vector2(90, 0)
		add_child(_num)
		_var = OptionButton.new()
		for v in _vars:
			_var.add_item(String(v.get("label", v.get("token", "Var00"))))
		if _vars.is_empty():
			_var.add_item("Var00")
		add_child(_var)
		_raw = LineEdit.new()
		_raw.placeholder_text = "expression"
		_raw.custom_minimum_size = Vector2(120, 0)
		add_child(_raw)
		_mode.item_selected.connect(func(_i): _sync(); changed.emit())
		_num.value_changed.connect(func(_v): changed.emit())
		_var.item_selected.connect(func(_i): changed.emit())
		_raw.text_changed.connect(func(_t): changed.emit())
		_sync()

	func _sync() -> void:
		var m := _mode.get_selected_id()
		_num.visible = (m == L_NUM)
		_var.visible = (m == L_VAR)
		_raw.visible = (m == L_RAW)

	func get_text() -> String:
		match _mode.get_selected_id():
			L_NUM:
				return str(int(_num.value))
			L_VAR:
				var idx := _var.selected
				if idx >= 0 and idx < _vars.size():
					return String(_vars[idx].get("token", "Var00"))
				return "Var00"
			L_ME:
				return "Me"
			L_RAW:
				var t := _raw.text.strip_edges()
				return t if t != "" else "0"
		return "0"

	# Seed a leaf to a variable token if it matches a known var; else type-it.
	func seed_token(token: String) -> void:
		for i in range(_vars.size()):
			if String(_vars[i].get("token", "")) == token:
				_mode.select(L_VAR)
				_var.select(i)
				_sync()
				return
		if token.is_valid_int():
			_mode.select(L_NUM)
			_num.value = int(token)
		else:
			_mode.select(L_RAW)
			_raw.text = token
		_sync()


func setup(var_list: Array, mus = null) -> void:
	_var_list = var_list
	_mus = mus
	if _form_opt == null:
		_build_chrome()
	_rebuild_body()


func _build_chrome() -> void:
	add_theme_constant_override("separation", 4)
	var head := HBoxContainer.new()
	var lbl := Label.new()
	lbl.text = "Expression"
	lbl.add_theme_color_override("font_color", Color(0.7, 0.8, 1.0))
	head.add_child(lbl)
	_form_opt = OptionButton.new()
	_form_opt.add_item("a value", F_VALUE)
	_form_opt.add_item("compare (A op B)", F_COMPARE)
	_form_opt.add_item("math (A op B)", F_MATH)
	_form_opt.add_item("logic (A op B)", F_LOGIC)
	_form_opt.add_item("call a function", F_CALL)
	_form_opt.add_item("type it", F_RAW)
	_form_opt.item_selected.connect(func(_i): _rebuild_body())
	head.add_child(_form_opt)
	add_child(head)
	_body = VBoxContainer.new()
	add_child(_body)
	_valid = Label.new()
	_valid.add_theme_color_override("font_color", Color(0.6, 0.9, 0.6))
	add_child(_valid)
	_validate_timer = Timer.new()
	_validate_timer.one_shot = true
	_validate_timer.wait_time = 0.3
	_validate_timer.timeout.connect(_run_validation)
	add_child(_validate_timer)


func _clear_body() -> void:
	_leaf_a = null
	_leaf_b = null
	_op_opt = null
	_intrinsic_opt = null
	_arg_leaf = null
	_no_arg = null
	_raw_edit = null
	for c in _body.get_children():
		c.queue_free()


func _new_leaf() -> HBoxContainer:
	var leaf := Leaf.new(_var_list)
	leaf.changed.connect(_recompute)
	return leaf


func _op_menu(group: String) -> OptionButton:
	var ob := OptionButton.new()
	for e in MusExpr.BINOPS:
		if String(e.get("group", "")) == group:
			ob.add_item("%s  (%s)" % [String(e["op"]), String(e["label"])])
			ob.set_item_metadata(ob.item_count - 1, String(e["op"]))
	ob.item_selected.connect(func(_i): _recompute())
	return ob


func _rebuild_body() -> void:
	if _body == null:
		return
	_clear_body()
	var form := _form_opt.get_selected_id()
	match form:
		F_VALUE:
			_leaf_a = _new_leaf()
			_body.add_child(_leaf_a)
		F_COMPARE, F_MATH, F_LOGIC:
			var row := HBoxContainer.new()
			_leaf_a = _new_leaf()
			row.add_child(_leaf_a)
			var grp := "Compare"
			if form == F_MATH:
				grp = "Math"
			elif form == F_LOGIC:
				grp = "Logic"
			_op_opt = _op_menu(grp)
			row.add_child(_op_opt)
			_leaf_b = _new_leaf()
			row.add_child(_leaf_b)
			_body.add_child(row)
		F_CALL:
			var row2 := HBoxContainer.new()
			_intrinsic_opt = OptionButton.new()
			for it in MusExpr.INTRINSICS:
				var stored := String(it["name"])
				# Friendly name first; the engine's surface mnemonic rides along in
				# parentheses so scripts cross-referenced against originals still match up.
				_intrinsic_opt.add_item("%s  (%s)" % [MusDisplayNames.intrinsic_label(stored), MusExpr.surface(stored)])
				_intrinsic_opt.set_item_metadata(_intrinsic_opt.item_count - 1, stored)
				_intrinsic_opt.get_popup().set_item_tooltip(_intrinsic_opt.item_count - 1, MusDisplayNames.intrinsic_tooltip(stored))
			_intrinsic_opt.item_selected.connect(func(_i): _recompute())
			row2.add_child(_intrinsic_opt)
			_no_arg = CheckBox.new()
			_no_arg.text = "no arg"
			_no_arg.toggled.connect(func(_b): _recompute())
			row2.add_child(_no_arg)
			_arg_leaf = _new_leaf()
			row2.add_child(_arg_leaf)
			_body.add_child(row2)
		F_RAW:
			_raw_edit = LineEdit.new()
			_raw_edit.placeholder_text = "(Var01 == 2)"
			_raw_edit.text_changed.connect(func(_t): _recompute())
			_body.add_child(_raw_edit)
	_recompute()


func _current_text() -> String:
	if _form_opt == null:
		return "0"
	match _form_opt.get_selected_id():
		F_VALUE:
			return _leaf_a.get_text() if _leaf_a else "0"
		F_COMPARE, F_MATH, F_LOGIC:
			var op := "=="
			if _op_opt and _op_opt.selected >= 0:
				op = String(_op_opt.get_item_metadata(_op_opt.selected))
			var a: String = _leaf_a.get_text() if _leaf_a else "0"
			var b: String = _leaf_b.get_text() if _leaf_b else "0"
			return "(%s %s %s)" % [a, op, b]
		F_CALL:
			var stored := "GSV"
			if _intrinsic_opt and _intrinsic_opt.selected >= 0:
				stored = String(_intrinsic_opt.get_item_metadata(_intrinsic_opt.selected))
			var s := MusExpr.surface(stored)
			if _no_arg and _no_arg.button_pressed:
				return "%s()" % s
			return "%s(%s)" % [s, _arg_leaf.get_text() if _arg_leaf else "0"]
		F_RAW:
			var t := _raw_edit.text.strip_edges() if _raw_edit else ""
			return t if t != "" else "0"
	return "0"


func _recompute() -> void:
	if _suppress:
		return
	# The text + signal are cheap and stay live; only the compiler-backed ✓/✗ label
	# is debounced. Before the builder is in the tree (initial build) the timer can't
	# run, so validate inline then.
	expression_changed.emit(_current_text())
	if _validate_timer != null and is_inside_tree():
		_validate_timer.start()
	else:
		_run_validation()


func _run_validation() -> void:
	if _valid == null:
		return
	var text := _current_text()
	var v: Dictionary = MusExpr.validate_expr(text, _mus)
	if bool(v.get("ok", true)):
		_valid.text = "✓ %s" % text
		_valid.add_theme_color_override("font_color", Color(0.6, 0.9, 0.6))
	else:
		_valid.text = "✗ %s" % String(v.get("err", "invalid"))
		_valid.add_theme_color_override("font_color", Color(1.0, 0.5, 0.5))


func get_expression_text() -> String:
	return _current_text()


func is_valid() -> bool:
	return bool(MusExpr.validate_expr(_current_text(), _mus).get("ok", true))


# Seed the builder from existing text. Per the design, editing an existing
# expression opens in the "type it" form pre-filled (we don't parse text back into
# the structured tree); the user can switch forms to rebuild it structurally.
func set_expression_text(text: String) -> void:
	if _form_opt == null:
		_build_chrome()
	_suppress = true
	_form_opt.select(F_RAW)
	_rebuild_body()
	if _raw_edit:
		_raw_edit.text = text
	_suppress = false
	_recompute()
