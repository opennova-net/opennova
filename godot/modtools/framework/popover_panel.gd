class_name PopoverPanel
extends PanelContainer

## Shared component for the editor's top-bar anchored, non-modal popovers
## (camera / environment / settings). It unifies how the three popovers are
## placed and dismissed so they look and behave identically:
##   - one top-right anchor formula (pinned below the shell top bar),
##   - one close affordance (standard glyph, no focus steal) that emits
##     close_requested,
##   - one pop-out affordance (same styling) that emits detach_requested,
##   - one open/close/is_open API.
## The popover's content stays as ordinary scene children; this script only owns
## placement, visibility, and the close/detach signals, so the existing content
## nodes (and their unique names) are untouched.

signal close_requested
signal detach_requested

const ANCHOR_TOP_OFFSET := 56.0
const EDGE_MARGIN := 12.0
const CLOSE_GLYPH := "✕"
const DETACH_GLYPH := "⧉"


# Pin the popover to the top-right of the workstation, `min_width` wide, just
# below the top bar. Replaces the per-popover offset blocks so all three line up.
func apply_anchor(min_width: float) -> void:
	custom_minimum_size.x = min_width
	anchor_left = 1.0
	anchor_top = 0.0
	anchor_right = 1.0
	anchor_bottom = 1.0
	offset_left = -(min_width + EDGE_MARGIN)
	offset_top = ANCHOR_TOP_OFFSET
	offset_right = -EDGE_MARGIN
	offset_bottom = -EDGE_MARGIN
	grow_horizontal = GROW_DIRECTION_BEGIN


# Standardize a popover's close button (glyph + no focus) and route its press
# through close_requested so the shell has a single dismissal path.
func bind_close(button: Button) -> void:
	if button == null:
		return
	button.text = CLOSE_GLYPH
	button.focus_mode = Control.FOCUS_NONE
	if not button.pressed.is_connected(_on_close_pressed):
		button.pressed.connect(_on_close_pressed)


func _on_close_pressed() -> void:
	close_requested.emit()


# Standardize a popover's pop-out button (glyph + no focus) and route its press
# through detach_requested; the shell decides what actually floats.
func bind_detach(button: Button) -> void:
	if button == null:
		return
	button.text = DETACH_GLYPH
	button.focus_mode = Control.FOCUS_NONE
	if not button.pressed.is_connected(_on_detach_pressed):
		button.pressed.connect(_on_detach_pressed)


func _on_detach_pressed() -> void:
	detach_requested.emit()


func open() -> void:
	visible = true


func close() -> void:
	visible = false


func is_open() -> bool:
	return visible
