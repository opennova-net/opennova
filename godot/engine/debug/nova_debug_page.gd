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
## - Keep the page's minimum content width <= 360 px: it must fit the panel
##   width floor minus the sidebar.

const CATEGORY_SIM := &"Simulation"
const CATEGORY_WORLD := &"World"
const CATEGORY_PLAYER := &"Player"
const CATEGORY_DIAGNOSTICS := &"Diagnostics"

var _ctx: NovaDebugContext = null


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


func _build() -> void:
	pass


func refresh() -> void:
	pass


func set_capture_active(_active: bool) -> void:
	pass


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
	if _ctx != null and _ctx.options != null:
		check.button_pressed = bool(_ctx.options.value(id))
		check.toggled.connect(func(pressed: bool): _ctx.options.set_value(id, pressed))
		_ctx.options.register_control(id, check)
	add_child(check)
	return check
