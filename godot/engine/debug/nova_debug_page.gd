class_name NovaDebugPage
extends VBoxContainer
## One page of the F3 debug overlay. Subclasses declare their identity
## (page_id/page_title/page_category), build their UI once in _build(), and
## read live state in refresh() through the shared NovaDebugContext.
##
## Contract:
## - _build() constructs controls only — never a sim read; setup() runs it
##   exactly once and names the node after page_id() so tests keep stable
##   node paths.
## - refresh() runs at the overlay cadence ONLY while this page is active
##   (and immediately on selection), so an idle page costs nothing. Pages
##   resolve _ctx.sim()/runtime()/world() themselves and render their own
##   empty state when a source is gone.
## - set_capture_active(active) is the activity edge — true when the page is
##   active AND the overlay is visible — for pages owning an expensive
##   capture (Stats gates the FrameStatsBoard on it); the default ignores it.
## - Keep the page's minimum content width <= 300 px: it must fit beside the
##   regular sidebar at the responsive-navigation breakpoint.

const CATEGORY_SIM := &"Simulation"
const CATEGORY_WORLD := &"World"
const CATEGORY_PLAYER := &"Player"
const CATEGORY_DIAGNOSTICS := &"Diagnostics"

var _ctx: NovaDebugContext = null
var _debug_controls: Dictionary = {}


## Stable identity: the node name, the select_page() key, and the persisted
## last-page value. Never rename an id casually.
func page_id() -> StringName:
	return &""


## Artist-facing sidebar label.
func page_title() -> String:
	return String(page_id())


func page_category() -> StringName:
	return CATEGORY_SIM


func setup(ctx: NovaDebugContext) -> void:
	_ctx = ctx
	name = String(page_id())
	_build()
	if _ctx != null and _ctx.session != null:
		_ctx.session.control_changed.connect(_on_debug_control_changed)
		refresh_debug_controls()


func _build() -> void:
	pass


func refresh() -> void:
	pass


func set_capture_active(_active: bool) -> void:
	pass


## Re-read every generic control from its public target. Overlay refreshes
## this only on the active page, so live getter readback stays inexpensive.
func refresh_debug_controls() -> void:
	if _ctx == null or _ctx.session == null:
		return
	for id in _debug_controls:
		_apply_control_state(StringName(id), _ctx.session.get_control_state(id))


## Build the standard checkbox for a NovaDebugOptions registry row and wire it
## through the shared option state (single write path: toggling routes into
## set_value, programmatic set_option re-syncs the control). The node is named
## after the option id — the stable test/query path.
func add_option_check(id: StringName) -> CheckBox:
	var option := NovaDebugOptions.find(id)
	var check := CheckBox.new()
	check.name = String(id)
	check.text = String(option.get("label", String(id)))
	check.tooltip_text = String(option.get("tooltip", ""))
	if _ctx != null and _ctx.session != null and _ctx.session.has_control(id):
		var state := _ctx.session.get_control_state(id)
		check.button_pressed = bool(state.value)
		check.toggled.connect(
				func(pressed: bool): _ctx.session.set_control_value(id, pressed))
		_debug_controls[id] = check
		_apply_control_state(id, state)
	elif _ctx != null and _ctx.options != null:
		check.button_pressed = bool(_ctx.options.value(id))
		check.toggled.connect(func(pressed: bool): _ctx.options.set_value(id, pressed))
		_ctx.options.register_control(id, check)
	add_child(check)
	return check


## Build a control directly from the shared typed catalog. The returned node
## is named after the control id so pages, tests and accessibility tools keep a
## stable query path.
func add_debug_control(id: StringName) -> Control:
	if _ctx == null or _ctx.session == null:
		return null
	var definition := _ctx.session.definition(id)
	if definition == null:
		return null
	var control: Control
	match definition.kind:
		NovaDebugControlDef.Kind.CHECK:
			var check := CheckBox.new()
			check.text = definition.label
			check.toggled.connect(
					func(value: bool): _ctx.session.set_control_value(id, value))
			control = check
		NovaDebugControlDef.Kind.SLIDER:
			var slider := HSlider.new()
			slider.min_value = definition.minimum
			slider.max_value = definition.maximum
			slider.step = definition.step
			slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			slider.value_changed.connect(
					func(value: float): _ctx.session.set_control_value(id, value))
			control = slider
		NovaDebugControlDef.Kind.ENUM:
			var option := OptionButton.new()
			for choice in definition.choices:
				option.add_item(choice)
			option.item_selected.connect(
					func(value: int): _ctx.session.set_control_value(id, value))
			control = option
		NovaDebugControlDef.Kind.ACTION:
			var button := Button.new()
			button.text = definition.label
			button.pressed.connect(
					func(): _ctx.session.invoke_control(id))
			control = button
	if control == null:
		return null
	control.name = String(id)
	control.tooltip_text = definition.description
	_debug_controls[id] = control
	add_child(control)
	_apply_control_state(id, _ctx.session.get_control_state(id))
	return control


func _on_debug_control_changed(
		id: StringName,
		state: NovaDebugControlState) -> void:
	if _debug_controls.has(id):
		_apply_control_state(id, state)


func _apply_control_state(id: StringName, state: NovaDebugControlState) -> void:
	var control := _debug_controls.get(id) as Control
	if control == null or not is_instance_valid(control):
		return
	var definition := _ctx.session.definition(id) if _ctx != null \
			and _ctx.session != null else null
	var writable := state.writable
	var available := state.available
	var reason := state.reason
	if control is BaseButton:
		(control as BaseButton).disabled = not available or not writable
	if control is CheckBox:
		(control as CheckBox).set_pressed_no_signal(bool(state.value))
	elif control is Slider:
		(control as Slider).set_value_no_signal(float(state.value))
		(control as Slider).editable = available and writable
	elif control is OptionButton:
		(control as OptionButton).select(int(state.value))
		(control as OptionButton).disabled = not available or not writable
	var base_tooltip := definition.description if definition != null else ""
	control.tooltip_text = base_tooltip
	if not reason.is_empty():
		control.tooltip_text += ("\n\n" if not control.tooltip_text.is_empty() else "") + reason
